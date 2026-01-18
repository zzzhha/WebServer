// user_dao.cpp
#include "User.h"
#include <mysql/mysql.h>
#include <cstring>

// 插入用户：只进行数据库插入操作，不检查用户名是否已存在
// 返回true表示数据库操作成功，false表示数据库操作失败
bool UserDao::InsertUser(const std::string& username, const std::string& password) {
    MYSQL* sql = nullptr;
    // 利用RAII自动获取和释放连接
    SqlConnRAII raii(&sql, SqlConnPool::Instance());
    if (!sql) {
        LOGERROR("InsertUser failed: get connection from pool failed");
        return false;
    }

    // 转义特殊字符，防止SQL注入
    char esc_username[256] = {0};
    mysql_real_escape_string(sql, esc_username, username.c_str(), username.size());
    char esc_password[256] = {0};
    mysql_real_escape_string(sql, esc_password, password.c_str(), password.size());

    // 插入新用户
    std::string insert_sql = "INSERT INTO user(username, password) VALUES('"
        + std::string(esc_username) + "', '" + std::string(esc_password) + "');";
    if (mysql_query(sql, insert_sql.c_str()) != 0) {
        char buf[256];
        sprintf(buf, "InsertUser failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return false;
    }
    
    char buf[256];
    sprintf(buf, "InsertUser success: username = %s", username.c_str());
    LOGINFO(buf);
    return true;
}

// 根据用户名查询用户信息
// 返回std::optional<UserInfo>，用户不存在时返回std::nullopt
std::optional<UserDao::UserInfo> UserDao::QueryUserByUsername(const std::string& username) {
    MYSQL* sql = nullptr;
    SqlConnRAII raii(&sql, SqlConnPool::Instance());
    if (!sql) {
        LOGERROR("QueryUserByUsername failed: get connection from pool failed");
        return std::nullopt;
    }

    // 转义特殊字符
    char esc_username[256] = {0};
    mysql_real_escape_string(sql, esc_username, username.c_str(), username.size());

    // 查询用户信息
    std::string query_sql = "SELECT username, password FROM user WHERE username = '" 
        + std::string(esc_username) + "' LIMIT 1;";
    if (mysql_query(sql, query_sql.c_str()) != 0) {
        char buf[256];
        sprintf(buf, "QueryUserByUsername query failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return std::nullopt;
    }

    MYSQL_RES* res = mysql_store_result(sql);
    if (!res) {
        char buf[256];
        sprintf(buf, "QueryUserByUsername get result failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return std::nullopt;
    }

    // 检查是否有结果
    if (mysql_num_rows(res) == 0) {
        mysql_free_result(res);
        return std::nullopt;
    }

    // 获取用户信息
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0] || !row[1]) {
        mysql_free_result(res);
        return std::nullopt;
    }

    UserInfo user_info;
    user_info.username = row[0];
    user_info.password = row[1];

    mysql_free_result(res);
    return user_info;
}

// 检查用户名是否存在
// 返回true表示用户存在，false表示用户不存在
bool UserDao::UserExists(const std::string& username) {
    MYSQL* sql = nullptr;
    SqlConnRAII raii(&sql, SqlConnPool::Instance());
    if (!sql) {
        LOGERROR("UserExists failed: get connection from pool failed");
        return false;
    }

    // 转义特殊字符
    char esc_username[256] = {0};
    mysql_real_escape_string(sql, esc_username, username.c_str(), username.size());

    // 查询用户名是否存在
    std::string query_sql = "SELECT username FROM user WHERE username = '" 
        + std::string(esc_username) + "' LIMIT 1;";
    if (mysql_query(sql, query_sql.c_str()) != 0) {
        char buf[256];
        sprintf(buf, "UserExists query failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(sql);
    if (!res) {
        char buf[256];
        sprintf(buf, "UserExists get result failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return false;
    }

    bool exists = (mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return exists;
}

// 删除用户：根据用户名删除用户
// 返回true表示数据库操作成功，false表示数据库操作失败
bool UserDao::DeleteUser(const std::string& username) {
    MYSQL* sql = nullptr;
    SqlConnRAII raii(&sql, SqlConnPool::Instance());
    if (!sql) {
        LOGERROR("DeleteUser failed: get connection from pool failed");
        return false;
    }

    // 转义特殊字符
    char esc_username[256] = {0};
    mysql_real_escape_string(sql, esc_username, username.c_str(), username.size());

    // 删除用户
    std::string delete_sql = "DELETE FROM user WHERE username = '" 
        + std::string(esc_username) + "';";
    if (mysql_query(sql, delete_sql.c_str()) != 0) {
        char buf[256];
        sprintf(buf, "DeleteUser failed: %s", mysql_error(sql));
        LOGERROR(buf);
        return false;
    }
    
    // 检查是否有行被删除
    if (mysql_affected_rows(sql) == 0) {
        char buf[256];
        sprintf(buf, "DeleteUser: user not found - %s", username.c_str());
        LOGWARNING(buf);
        return false;
    }
    
    char buf[256];
    sprintf(buf, "DeleteUser success: username = %s", username.c_str());
    LOGINFO(buf);
    return true;
}