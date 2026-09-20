-- 机械臂官网 · 留言板数据库
-- 执行： mysql -u root < server/schema.sql

-- ---------------------------------------------------------------
-- 数据库
-- ---------------------------------------------------------------
CREATE DATABASE IF NOT EXISTS robot_arm
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE robot_arm;

-- ---------------------------------------------------------------
-- 应用账号（最小权限：只给业务需要的增删改查，不给 DROP/ALTER）
-- 生产环境请改掉默认密码
-- ---------------------------------------------------------------
CREATE USER IF NOT EXISTS 'arm_app'@'localhost' IDENTIFIED BY 'arm_dev_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON robot_arm.* TO 'arm_app'@'localhost';

-- 压测/演示专用库：explain-demo.js 要反复建表删表，
-- 所以给它单独一个库放开权限；业务库 robot_arm 依然只给增删改查
CREATE DATABASE IF NOT EXISTS robot_arm_demo
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;
GRANT ALL PRIVILEGES ON robot_arm_demo.* TO 'arm_app'@'localhost';

FLUSH PRIVILEGES;

-- ---------------------------------------------------------------
-- 账号表
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS accounts (
  id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  username      VARCHAR(32)  NOT NULL,
  nickname      VARCHAR(32)  NOT NULL,
  -- 只存哈希，永不存明文。算法见 server/accounts.js（scrypt）
  password_hash VARCHAR(255) NOT NULL,
  -- 第一个注册的账号自动成为管理员
  role          ENUM('admin','user') NOT NULL DEFAULT 'user',
  created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------
-- 会话表
--
-- 放在数据库而不是进程内存里，是为了让 Python 的图书管理服务
-- 也能校验同一个 token —— 这样全站就是一套账号，不用服务间调用。
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sessions (
  token      CHAR(48)     NOT NULL,
  account_id INT UNSIGNED NOT NULL,
  expire_at  DATETIME(3)  NOT NULL,
  PRIMARY KEY (token),
  KEY idx_expire (expire_at),
  KEY idx_account (account_id),
  CONSTRAINT fk_sessions_account
    FOREIGN KEY (account_id) REFERENCES accounts (id)
    ON DELETE CASCADE          -- 删账号时自动清掉它的会话
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------
-- 留言表
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS messages (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  nickname   VARCHAR(50)     NOT NULL,
  content    VARCHAR(500)    NOT NULL,
  created_at DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (id),
  -- 列表页固定按时间倒序，加索引避免全表扫描 + filesort
  KEY idx_created_at (created_at DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------
-- 回复表
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS replies (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  message_id BIGINT UNSIGNED NOT NULL,
  content    VARCHAR(500)    NOT NULL,
  created_at DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (id),
  -- 按留言查回复是最高频的查询，且外键也需要索引
  KEY idx_message_id (message_id),
  CONSTRAINT fk_replies_message
    FOREIGN KEY (message_id) REFERENCES messages (id)
    ON DELETE CASCADE        -- 删留言时自动删掉它的回复，不会留孤儿数据
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
