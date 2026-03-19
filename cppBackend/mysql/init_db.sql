-- 创建数据库（如果不存在）
CREATE DATABASE IF NOT EXISTS webserver CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 选择数据库
USE webserver;

-- 创建用户（如果不存在）并设置密码
CREATE USER IF NOT EXISTS 'webuser'@'localhost' IDENTIFIED BY '12589777';

-- 授予用户对 webserver 数据库的所有权限
GRANT ALL PRIVILEGES ON webserver.* TO 'webuser'@'localhost';

-- 刷新权限
FLUSH PRIVILEGES;

-- 创建 user 表（如果不存在）
CREATE TABLE IF NOT EXISTS user (
    id VARCHAR(36) PRIMARY KEY COMMENT '用户ID，UUID格式',
    username VARCHAR(50) NOT NULL UNIQUE COMMENT '用户名，唯一',
    password_hash VARCHAR(255) NOT NULL COMMENT '密码哈希值'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 查看表结构
DESCRIBE user;

-- 查看用户权限
SHOW GRANTS FOR 'webuser'@'localhost';
