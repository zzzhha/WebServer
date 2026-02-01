#pragma once
#include "sqlconnpool.h"
#include "sqlConnRAII.h"
#include <string>
#include <optional>

/**
 * UserDao类：纯数据访问层
 * 职责：只负责数据库的增删改查操作，不包含任何业务逻辑
 * 业务验证和判断逻辑应放在业务处理层（如AuthService）中
 */
class UserDao {
public:
    // 用户信息结构体
    struct UserInfo {
        std::string id;
        std::string username;
        std::string password_hash;
    };

    // 插入用户：只进行数据库插入操作，不检查用户名是否已存在
    // 返回true表示数据库操作成功，false表示数据库操作失败
    static bool InsertUser(const std::string& username, const std::string& password_hash);

    // 根据用户名查询用户信息
    // 返回std::optional<UserInfo>，用户不存在时返回std::nullopt
    static std::optional<UserInfo> QueryUserByUsername(const std::string& username);

    // 检查用户名是否存在
    // 返回true表示用户存在，false表示用户不存在
    static bool UserExists(const std::string& username);
    
    // 删除用户：根据用户名删除用户
    // 返回true表示数据库操作成功，false表示数据库操作失败
    static bool DeleteUser(const std::string& username);

private:
    // 生成UUID
    static std::string GenerateUUID();
};
