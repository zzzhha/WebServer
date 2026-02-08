// UserDao.cpp
#include "UserDao.h"
#include <mysql/mysql.h>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

// 生成UUID v4
std::string UserDao::GenerateUUID() {
    // 使用thread_local确保每个线程有自己的随机数生成器实例
    // 避免线程间的竞争条件
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

// 插入用户：只进行数据库插入操作，不检查用户名是否已存在
// 返回true表示数据库操作成功，false表示数据库操作失败
bool UserDao::InsertUser(const std::string& username, const std::string& password_hash) {
    MYSQL* sql = nullptr;
    // 利用RAII自动获取和释放连接
    SqlConnRAII raii(&sql, SqlConnPool::Instance());
    if (!sql) {
        LOGERROR("InsertUser failed: get connection from pool failed");
        return false;
    }

    // 生成UUID
    std::string uuid = GenerateUUID();

    // 转义特殊字符，防止SQL注入
    char esc_id[64] = {0};
    mysql_real_escape_string(sql, esc_id, uuid.c_str(), uuid.size());
    char esc_username[256] = {0};
    mysql_real_escape_string(sql, esc_username, username.c_str(), username.size());
    char esc_password_hash[256] = {0};
    mysql_real_escape_string(sql, esc_password_hash, password_hash.c_str(), password_hash.size());

    // 插入新用户
    std::string insert_sql = "INSERT INTO user(id, username, password_hash) VALUES('"
        + std::string(esc_id) + "', '" + std::string(esc_username) + "', '" + std::string(esc_password_hash) + "');";
    if (mysql_query(sql, insert_sql.c_str()) != 0) {
        char buf[256];
        sprintf(buf, "InsertUser failed: %s", mysql_error(sql));
        LOGERROR(buf);
        // 检查是否是用户名重复错误
        std::string error_msg = mysql_error(sql);
        if (error_msg.find("Duplicate entry") != std::string::npos && error_msg.find("username") != std::string::npos) {
            LOGWARNING("InsertUser failed: username already exists - " + username);
        }
        return false;
    }
    
    char buf[256];
    sprintf(buf, "InsertUser success: username = %s, id = %s", username.c_str(), uuid.c_str());
    LOGINFO(buf);
    return true;
}

// 根据用户名查询用户信息
// 返回std::optional<UserInfo>，用户不存在时返回std::nullopt
std::optional<UserInfo> UserDao::QueryUserByUsername(const std::string& username) {
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
    std::string query_sql = "SELECT id, username, password_hash FROM user WHERE username = '" 
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
    if (!row || !row[0] || !row[1] || !row[2]) {
        mysql_free_result(res);
        return std::nullopt;
    }

    UserInfo user_info;
    user_info.id = row[0];
    user_info.username = row[1];
    user_info.password_hash = row[2];

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
