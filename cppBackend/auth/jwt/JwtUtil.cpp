#include "JwtUtil.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>

// 静态常量初始化
const std::string JwtUtil::SECRET_KEY = "your-super-secret-key-for-jwt-token-generation-2026";
const std::chrono::hours JwtUtil::ACCESS_TOKEN_EXPIRATION = std::chrono::hours(24);
const std::chrono::hours JwtUtil::REFRESH_TOKEN_EXPIRATION = std::chrono::hours(168); // 7天

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
        return claims;
    } catch (const std::exception& e) {
        std::cerr << "ExtractClaims failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// 生成JWT token
std::string JwtUtil::GenerateToken(const std::string& username, const std::string& user_id) {
    try {
        auto now = std::chrono::system_clock::now();
        auto expires_at = now + ACCESS_TOKEN_EXPIRATION;
        
        auto token = jwt::create()
            .set_type("JWT")
            .set_subject(user_id)
            .set_payload_claim("username", jwt::claim(username))
            .set_issued_at(now)
            .set_expires_at(expires_at)
            .set_id(GenerateUUID())
            .sign(jwt::algorithm::hs256{SECRET_KEY});
        
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
            .allow_algorithm(jwt::algorithm::hs256{SECRET_KEY});
        
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
        
        // 生成新的token
        return GenerateToken(claims->username, claims->user_id);
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