#include "AuthService.h"
#include "../../mysql/User.h"
#include "../../logger/log_fac.h"
#include <algorithm>
#include <cctype>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstring>
#include <sstream>
#include <iomanip>

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

    // 比较计算出的哈希和存储的哈希
    return computed_hash == stored_hash;
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
std::optional<std::string> AuthService::HandleLogin(const std::string& username, const std::string& password) {
    // 业务验证：检查用户名和密码是否为空
    if (username.empty() || password.empty()) {
        LOGWARNING("登录失败：用户名或密码为空");
        return std::nullopt;
    }

    // 调用UserDao查询用户
    auto userInfo = UserDao::QueryUserByUsername(username);
    if (!userInfo.has_value()) {
        LOGWARNING("登录失败：用户不存在 - " + username);
        return std::nullopt;
    }

    // 业务判断：验证密码是否匹配
    if (!VerifyPassword(password, userInfo->password_hash)) {
        LOGWARNING("登录失败：密码错误 - " + username);
        return std::nullopt;
    }

    // 登录成功，生成JWT token
    std::string token = JwtUtil::GenerateToken(username, userInfo->id);
    if (token.empty()) {
        LOGERROR("登录成功但生成token失败：用户名 = " + username);
        return std::nullopt;
    }

    LOGINFO("登录成功：用户名 = " + username);
    return token;
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
    for (char c : username) {
        if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }

    // 必须以字母开头
    if (username.empty() || !std::isalpha(username[0])) {
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

