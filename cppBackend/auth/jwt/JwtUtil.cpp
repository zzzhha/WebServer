#include "JwtUtil.h"
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>

// 静态常量初始化
const std::chrono::hours JwtUtil::ACCESS_TOKEN_EXPIRATION = std::chrono::hours(24);
const std::chrono::hours JwtUtil::REFRESH_TOKEN_EXPIRATION = std::chrono::hours(168); // 7天

namespace {
constexpr const char* kJwtSecretEnv = "JWT_SECRET_KEY";
} // namespace

// H6 修复：密钥外部化。
// 优先读取环境变量 JWT_SECRET_KEY；未设置时首次调用生成 32 字节熵的随机密钥
// 并缓存（C++11 magic static 保证线程安全），进程存活期间有效。
// 不再存在硬编码密钥——源码/二进制泄漏无法再离线伪造 token。
// 部署时轮换：更新环境变量并重启进程，旧密钥签发的 token 全部失效。
std::string JwtUtil::GetSecretKey() {
    static const std::string secret = []() -> std::string {
        if (const char* env = std::getenv(kJwtSecretEnv);
            env != nullptr && *env != '\0') {
            if (std::strlen(env) < 32) {
                std::cerr << "[JwtUtil] WARNING: JWT_SECRET_KEY 长度不足 32 字节，"
                             "建议使用 32+ 字节的强随机密钥（HS256）\n";
            }
            return env;
        }
        static std::mt19937_64 gen(std::random_device{}());
        static std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 4; ++i) {
            oss << std::setw(16) << dist(gen);  // 4 × 8 字节 = 32 字节熵
        }
        std::cerr << "[JwtUtil] WARNING: 环境变量 JWT_SECRET_KEY 未设置，"
                     "已生成进程随机密钥（重启后所有已签发 token 失效，"
                     "生产环境请设置该变量）\n";
        return oss.str();
    }();
    return secret;
}

// 生成UUID（用于JWT ID）
std::string JwtUtil::GenerateUUID() {
    // 使用thread_local确保每个线程有自己的随机数生成器实例
    thread_local static std::random_device rd;
    thread_local static std::mt19937 gen(rd());
    thread_local static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(gen);
    uint32_t b = dist(gen);
    uint32_t c = dist(gen);
    uint32_t d = dist(gen);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << a << '-';
    ss << std::setw(4) << (b >> 16) << '-';
    ss << std::setw(4) << ((b & 0xFFFF) | 0x4000) << '-'; // 版本4标记
    ss << std::setw(4) << ((c >> 16) | 0x8000) << '-'; // 变体标记
    ss << std::setw(4) << (c & 0xFFFF) << std::setw(8) << d;

    return ss.str();
}

// 从token中提取claims
template<typename T>
std::optional<JwtUtil::Claims> JwtUtil::ExtractClaims(const T& decoded) {
    try {
        JwtUtil::Claims claims;
        claims.user_id = decoded.get_payload_claim("sub").as_string();
        claims.username = decoded.get_payload_claim("username").as_string();
        claims.issued_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(decoded.get_payload_claim("iat").as_integer())
        );
        claims.expires_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(decoded.get_payload_claim("exp").as_integer())
        );
        claims.jwt_id = decoded.get_payload_claim("jti").as_string();
        // 中危修复：读取 token 类型；缺失（旧 token）默认按 access 处理，RefreshToken 会拒绝
        claims.token_type =
            decoded.has_payload_claim("token_type")
                ? decoded.get_payload_claim("token_type").as_string()
                : "access";
        return claims;
    } catch (const std::exception& e) {
        std::cerr << "ExtractClaims failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// 生成JWT token
std::string JwtUtil::GenerateToken(const std::string& username, const std::string& user_id, TokenType type) {
    try {
        auto now = std::chrono::system_clock::now();
        auto expires_at = now + (type == TokenType::ACCESS ? ACCESS_TOKEN_EXPIRATION : REFRESH_TOKEN_EXPIRATION);
        
        auto token = jwt::create()
            .set_type("JWT")
            .set_subject(user_id)
            .set_payload_claim("username", jwt::claim(username))
            // 中危修复：写入 token 类型，RefreshToken 据此校验，ACCESS 不可续期
            .set_payload_claim("token_type",
                               jwt::claim(std::string(type == TokenType::ACCESS ? "access" : "refresh")))
            .set_issued_at(now)
            .set_expires_at(expires_at)
            .set_id(GenerateUUID())
            .sign(jwt::algorithm::hs256{GetSecretKey()});
        
        return token;
    } catch (const std::exception& e) {
        std::cerr << "GenerateToken failed: " << e.what() << std::endl;
        return "";
    }
}

// 验证JWT token
std::optional<JwtUtil::Claims> JwtUtil::ValidateToken(const std::string& token) {
    try {
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{GetSecretKey()});
        
        verifier.verify(decoded);
        return ExtractClaims(decoded);
    } catch (const std::exception& e) {
        std::cerr << "ValidateToken failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// 刷新JWT token
std::optional<std::string> JwtUtil::RefreshToken(const std::string& token) {
    try {
        auto claims = ValidateToken(token);
        if (!claims) {
            return std::nullopt;
        }
        // 中危修复：仅 REFRESH 类型 token 可续期；ACCESS token（或缺失类型的旧 token）拒绝，
        // 防 ACCESS token 无限续期（原实现任何有效 token 均可刷新）
        if (claims->token_type != "refresh") {
            return std::nullopt;
        }
        
        // 生成新的ACCESS token
        return GenerateToken(claims->username, claims->user_id, TokenType::ACCESS);
    } catch (const std::exception& e) {
        std::cerr << "RefreshToken failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// 从token中提取用户名
std::optional<std::string> JwtUtil::ExtractUsername(const std::string& token) {
    try {
        auto claims = ValidateToken(token);
        if (!claims) {
            return std::nullopt;
        }
        return claims->username;
    } catch (const std::exception& e) {
        std::cerr << "ExtractUsername failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// 从token中提取用户ID
std::optional<std::string> JwtUtil::ExtractUserId(const std::string& token) {
    try {
        auto claims = ValidateToken(token);
        if (!claims) {
            return std::nullopt;
        }
        return claims->user_id;
    } catch (const std::exception& e) {
        std::cerr << "ExtractUserId failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}