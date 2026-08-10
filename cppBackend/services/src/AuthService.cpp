#include "AuthService.h"
#include "../../mysql/User.h"
#include "../../logger/log_fac.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <iomanip>

namespace {

// 登录限流状态：按用户名记录连续失败次数与锁定到期时间，防暴力破解（审计 H16）
struct LoginThrottleEntry {
    int fail_count{0};
    std::chrono::steady_clock::time_point locked_until{};
};

std::mutex g_login_throttle_mutex;
std::unordered_map<std::string, LoginThrottleEntry> g_login_throttle;

constexpr int kMaxLoginFailures = 5;      // 连续失败锁定阈值
constexpr int kLockoutBaseSeconds = 30;   // 锁定基础时长（秒）

// 用户名当前是否处于锁定状态；锁定过期后顺带清空计数
bool IsLoginLocked(const std::string& username) {
    std::lock_guard<std::mutex> lock(g_login_throttle_mutex);
    auto it = g_login_throttle.find(username);
    if (it == g_login_throttle.end()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < it->second.locked_until) {
        return true;
    }
    if (it->second.fail_count >= kMaxLoginFailures) {
        g_login_throttle.erase(it);
    }
    return false;
}

// 记录一次登录失败；连续失败达阈值后触发指数退避锁定
void RecordLoginFailure(const std::string& username) {
    std::lock_guard<std::mutex> lock(g_login_throttle_mutex);
    auto& entry = g_login_throttle[username];
    entry.fail_count++;
    if (entry.fail_count >= kMaxLoginFailures) {
        // 退避：基础 30s，超过阈值后每次再失败翻倍，封顶约 8 分钟
        const int factor = entry.fail_count - kMaxLoginFailures + 1;
        const int seconds = kLockoutBaseSeconds * std::min(factor, 16);
        entry.locked_until =
            std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }
}

// 登录成功：清除该用户名的失败记录
void ClearLoginThrottle(const std::string& username) {
    std::lock_guard<std::mutex> lock(g_login_throttle_mutex);
    g_login_throttle.erase(username);
}

}  // namespace

// 生成随机盐值
std::string GenerateSalt() {
    const int SALT_SIZE = 16;
    unsigned char salt[SALT_SIZE];
    RAND_bytes(salt, SALT_SIZE);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < SALT_SIZE; ++i) {
        ss << std::setw(2) << static_cast<unsigned int>(salt[i]);
    }
    return ss.str();
}

// 对密码进行哈希处理（使用PBKDF2算法）
std::string HashPassword(const std::string& password, const std::string& salt) {
    const int ITERATIONS = 10000;
    const int KEY_LENGTH = 32;
    unsigned char key[KEY_LENGTH];

    PKCS5_PBKDF2_HMAC(password.c_str(), password.length(),
                      reinterpret_cast<const unsigned char*>(salt.c_str()), salt.length(),
                      ITERATIONS, EVP_sha256(), KEY_LENGTH, key);

    std::stringstream ss;
    ss << salt << ":";
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < KEY_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<unsigned int>(key[i]);
    }
    return ss.str();
}

// 验证密码是否匹配
bool VerifyPassword(const std::string& password, const std::string& stored_hash) {
    // 从存储的哈希中提取盐值
    size_t colon_pos = stored_hash.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    std::string salt = stored_hash.substr(0, colon_pos);

    // 对输入的密码使用相同的盐值进行哈希
    std::string computed_hash = HashPassword(password, salt);

    // 低危修复：改为恒定时间比较，避免逐字节短路比较泄露哈希差异（时序侧信道）
    if (computed_hash.size() != stored_hash.size()) {
        return false;
    }
    return CRYPTO_memcmp(computed_hash.data(), stored_hash.data(), computed_hash.size()) == 0;
}

// 处理注册请求
bool AuthService::HandleRegister(const std::string& username, const std::string& password) {
    // 业务验证：检查用户名和密码是否为空
    if (username.empty() || password.empty()) {
        LOGWARNING("注册失败：用户名或密码为空");
        return false;
    }

    // 业务验证：检查用户名格式
    if (!ValidateUsername(username)) {
        LOGWARNING("注册失败：用户名格式无效 - " + username);
        return false;
    }

    // 业务验证：检查密码长度
    if (!ValidatePassword(password)) {
        LOGWARNING("注册失败：密码格式无效");
        return false;
    }

    // 业务判断：检查用户名是否已存在
    if (UserDao::UserExists(username)) {
        LOGWARNING("注册失败：用户名已存在 - " + username);
        return false;
    }

    // 生成盐值并对密码进行哈希处理
    std::string salt = GenerateSalt();
    std::string password_hash = HashPassword(password, salt);

    // 如果不存在，调用UserDao插入用户
    if (!UserDao::InsertUser(username, password_hash)) {
        LOGERROR("注册失败：数据库插入失败 - " + username);
        return false;
    }

    LOGINFO("注册成功：用户名 = " + username);
    return true;
}

// 处理登录请求
std::optional<AuthService::LoginResult> AuthService::HandleLogin(const std::string& username, const std::string& password) {
    // 业务验证：检查用户名和密码是否为空
    if (username.empty() || password.empty()) {
        LOGWARNING("登录失败：用户名或密码为空");
        return std::nullopt;
    }

    // 业务验证：登录路径复用注册时的用户名格式校验
    // 防止超长/非法用户名（如 >=256 字节）进入 DAO 层的转义逻辑
    if (!ValidateUsername(username)) {
        LOGWARNING("登录失败：用户名格式无效 - " + username);
        return std::nullopt;
    }

    // 限流检查放在 DAO 查询之前：锁定期间直接拒绝，既防暴力破解也避免被锁定账号继续消耗 DB（审计 H16）
    if (IsLoginLocked(username)) {
        LOGWARNING("登录失败：尝试过于频繁，请稍后再试");
        return std::nullopt;
    }

    // 调用UserDao查询用户
    auto userInfo = UserDao::QueryUserByUsername(username);
    if (!userInfo.has_value()) {
        // 用户不存在与密码错误返回同一日志，避免通过日志区分账号是否存在 → 账号枚举（审计 H16）
        LOGWARNING("登录失败：用户名或密码错误");
        RecordLoginFailure(username);
        return std::nullopt;
    }

    // 业务判断：验证密码是否匹配
    if (!VerifyPassword(password, userInfo->password_hash)) {
        LOGWARNING("登录失败：用户名或密码错误");
        RecordLoginFailure(username);
        return std::nullopt;
    }

    // 登录成功：清除该用户名的失败计数
    ClearLoginThrottle(username);

    // 登录成功，生成JWT token
    std::string access_token = JwtUtil::GenerateToken(username, userInfo->id, JwtUtil::TokenType::ACCESS);
    if (access_token.empty()) {
        LOGERROR("登录成功但生成token失败：用户名 = " + username);
        return std::nullopt;
    }

    // 生成refresh_token
    std::string refresh_token = JwtUtil::GenerateToken(username, userInfo->id, JwtUtil::TokenType::REFRESH);
    if (refresh_token.empty()) {
        LOGERROR("登录成功但生成refresh_token失败：用户名 = " + username);
        return std::nullopt;
    }

    LOGINFO("登录成功：用户名 = " + username);
    return LoginResult{access_token, refresh_token};
}

// 验证JWT token
bool AuthService::ValidateToken(const std::string& token) {
    if (token.empty()) {
        LOGWARNING("ValidateToken失败：token为空");
        return false;
    }

    auto claims = JwtUtil::ValidateToken(token);
    if (!claims) {
        LOGWARNING("ValidateToken失败：token无效");
        return false;
    }

    return true;
}

// 从token获取用户信息
std::optional<UserInfo> AuthService::GetUserFromToken(const std::string& token) {
    if (token.empty()) {
        LOGWARNING("GetUserFromToken失败：token为空");
        return std::nullopt;
    }

    auto claims = JwtUtil::ValidateToken(token);
    if (!claims) {
        LOGWARNING("GetUserFromToken失败：token无效");
        return std::nullopt;
    }

    // 从数据库查询用户信息
    auto userInfo = UserDao::QueryUserByUsername(claims->username);
    if (!userInfo.has_value()) {
        LOGWARNING("GetUserFromToken失败：用户不存在 - " + claims->username);
        return std::nullopt;
    }

    return userInfo;
}

// 验证用户名格式
// 规则：
// 1. 长度：3-20个字符
// 2. 字符：只能包含字母、数字、下划线
// 3. 首字符：必须以字母开头
bool AuthService::ValidateUsername(const std::string& username) {
    // 检查长度（3-20个字符）
    if (username.length() < 3 || username.length() > 20) {
        return false;
    }

    // 检查字符：只能包含字母、数字、下划线
    // 低危修复：std::isalnum/isalpha 入参为负值（UTF-8 多字节高位 char）时是 UB，
    // 先转 unsigned char 再传入
    for (char c : username) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }

    // 必须以字母开头
    if (username.empty() || !std::isalpha(static_cast<unsigned char>(username[0]))) {
        return false;
    }

    return true;
}

// 验证密码格式
// 规则：
// 1. 长度：至少6位
// 2. 可扩展：可以添加更多复杂度要求（如必须包含字母和数字等）
bool AuthService::ValidatePassword(const std::string& password) {
    // 检查长度（至少6位）
    if (password.length() < 6) {
        return false;
    }

    // 可以添加更多密码复杂度要求
    // 例如：必须包含字母和数字、特殊字符等

    return true;
}

