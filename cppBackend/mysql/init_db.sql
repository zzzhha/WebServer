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

-- 创建 files 表（文件元数据目录；文件系统是事实源，服务启动/定时扫描回填）
-- url / downloadUrl 不入库，由代码用 folder+name 拼接。
CREATE TABLE IF NOT EXISTS files (
    id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    folder        VARCHAR(32)  NOT NULL COMMENT '目录：images|video',
    name          VARCHAR(255) NOT NULL COMMENT '原始文件名',
    size          BIGINT       NOT NULL DEFAULT 0 COMMENT '字节大小',
    mime_type     VARCHAR(64),
    updated_at    BIGINT       NOT NULL COMMENT '文件系统mtime（epoch秒），事实源',
    thumb_state   ENUM('none','ok','fail') NOT NULL DEFAULT 'none',
    thumb_width   INT          NOT NULL DEFAULT 0 COMMENT '已生成缩略图宽',
    poster_state  ENUM('none','ok','fail') NOT NULL DEFAULT 'none' COMMENT '视频封面状态',
    created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_row_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_folder_name (folder, name),
    KEY idx_folder_mtime (folder, updated_at DESC),
    KEY idx_folder_name_sort (folder, name),
    KEY idx_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 查看表结构
DESCRIBE user;

-- 查看用户权限
SHOW GRANTS FOR 'webuser'@'localhost';
