#pragma once
#include <string>

/**
 * UserInfo结构体：用户信息数据实体
 * 职责：只包含用户数据字段，不包含任何业务逻辑
 */
struct UserInfo {
    std::string id;
    std::string username;
    std::string password_hash;
};
