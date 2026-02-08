#pragma once
#include <string>
#include <optional>
#include <chrono>
#include <jwt-cpp/jwt.h>

/**
 * JwtUtil类：JWT工具类
 * 职责：处理JWT token的生成、验证、刷新等操作
 */
class JwtUtil {
public:
    // Claims结构，用于存储JWT的声明信息
    struct Claims {
        std::string user_id;
        std::string username;
        std::chrono::system_clock::time_point issued_at;
        std::chrono::system_clock::time_point expires_at;
        std::string jwt_id;
    };

    // 生成JWT token
    // 参数：username - 用户名，user_id - 用户ID
    // 返回值：生成的JWT token字符串
    static std::string GenerateToken(const std::string& username, const std::string& user_id);

    // 验证JWT token
    // 参数：token - 要验证的JWT token
    // 返回值：验证成功返回Claims，验证失败返回std::nullopt
    static std::optional<Claims> ValidateToken(const std::string& token);

    // 刷新JWT token
    // 参数：token - 要刷新的JWT token
    // 返回值：刷新成功返回新的token，刷新失败返回std::nullopt
    static std::optional<std::string> RefreshToken(const std::string& token);

    // 从token中提取用户名
    // 参数：token - JWT token
    // 返回值：提取成功返回用户名，提取失败返回std::nullopt
    static std::optional<std::string> ExtractUsername(const std::string& token);

    // 从token中提取用户ID
    // 参数：token - JWT token
    // 返回值：提取成功返回用户ID，提取失败返回std::nullopt
    static std::optional<std::string> ExtractUserId(const std::string& token);

private:
    // 固定的开发环境密钥
    static const std::string SECRET_KEY;
    
    // 访问令牌过期时间（24小时）
    static const std::chrono::hours ACCESS_TOKEN_EXPIRATION;
    
    // 刷新令牌过期时间（7天）
    static const std::chrono::hours REFRESH_TOKEN_EXPIRATION;
    
    // 生成UUID（用于JWT ID）
    static std::string GenerateUUID();
    
    // 从token中提取claims
    template<typename T>
    static std::optional<Claims> ExtractClaims(const T& decoded);
};