#pragma once

#include <string>
#include <optional>
#include "../../mysql/User.h"  // 包含UserDao的头文件
#include "../../auth/jwt/JwtUtil.h"  // 包含JWT工具类

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * AuthService类：认证业务处理层
 * 职责：处理用户注册和登录的业务逻辑
 * - 业务验证（用户名/密码格式验证）
 * - 业务判断（用户名是否已存在、密码是否匹配）
 * - 调用数据访问层（UserDao）进行数据库操作
 * - JWT token管理
 */
class AuthService {
public:
    // 处理注册请求
    // 返回true表示注册成功，false表示注册失败
    static bool HandleRegister(const std::string& username, const std::string& password);

    // 处理登录请求
    // 返回成功时返回JWT token，失败时返回std::nullopt
    static std::optional<std::string> HandleLogin(const std::string& username, const std::string& password);

    // 验证用户名格式（可选）
    // 返回true表示格式有效，false表示格式无效
    static bool ValidateUsername(const std::string& username);

    // 验证密码格式（可选）
    // 返回true表示格式有效，false表示格式无效
    static bool ValidatePassword(const std::string& password);

    // 验证JWT token
    // 返回true表示token有效，false表示token无效
    static bool ValidateToken(const std::string& token);

    // 从token获取用户信息
    // 返回成功时返回UserInfo，失败时返回std::nullopt
    static std::optional<UserInfo> GetUserFromToken(const std::string& token);
};

