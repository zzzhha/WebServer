#pragma once

#include <string>
#include "../mysql/User.h"  // 包含UserDao的头文件

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * AuthService类：认证业务处理层
 * 职责：处理用户注册和登录的业务逻辑
 * - 业务验证（用户名/密码格式验证）
 * - 业务判断（用户名是否已存在、密码是否匹配）
 * - 调用数据访问层（UserDao）进行数据库操作
 */
class AuthService {
public:
    // 处理注册请求
    // 返回true表示注册成功，false表示注册失败
    static bool HandleRegister(const std::string& username, const std::string& password);

    // 处理登录请求
    // 返回true表示登录成功，false表示登录失败
    static bool HandleLogin(const std::string& username, const std::string& password);

    // 验证用户名格式（可选）
    // 返回true表示格式有效，false表示格式无效
    static bool ValidateUsername(const std::string& username);

    // 验证密码格式（可选）
    // 返回true表示格式有效，false表示格式无效
    static bool ValidatePassword(const std::string& password);
};

