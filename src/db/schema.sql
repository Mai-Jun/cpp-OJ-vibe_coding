-- schema.sql — OJ 数据库结构（Phase 1：用户 / 会话；Phase 2：题目 / 测试用例）
-- 需以拥有 oj 库权限的账号执行：mysql -u oj -p oj < src/db/schema.sql

CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE oj;

-- 用户表
CREATE TABLE IF NOT EXISTS users (
  id            INT          NOT NULL AUTO_INCREMENT,
  username      VARCHAR(64)  NOT NULL,
  password_hash VARCHAR(128) NOT NULL,
  role          ENUM('user','admin') NOT NULL DEFAULT 'user',
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 会话表（服务端存储登录态，普通用户）
CREATE TABLE IF NOT EXISTS sessions (
  id         VARCHAR(64) NOT NULL,
  user_id    INT         NOT NULL,
  created_at DATETIME    NOT NULL,
  expires_at DATETIME    NOT NULL,
  PRIMARY KEY (id),
  KEY idx_user (user_id),
  KEY idx_expires (expires_at),
  CONSTRAINT fk_sessions_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 题目表
CREATE TABLE IF NOT EXISTS problems (
  id              INT            NOT NULL AUTO_INCREMENT,
  title           VARCHAR(128)   NOT NULL,
  description     TEXT           NOT NULL,
  difficulty      ENUM('easy','medium','hard') NOT NULL DEFAULT 'easy',
  tags            VARCHAR(255)   NOT NULL DEFAULT '',
  time_limit_ms   INT            NOT NULL DEFAULT 500,
  memory_limit_mb INT            NOT NULL DEFAULT 256,
  judge_type      ENUM('exact','special') NOT NULL DEFAULT 'exact',
  spj_source      MEDIUMTEXT     NULL,
  created_at      DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at      DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_difficulty (difficulty)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 测试用例表（表存储；单条输入/期望输出建议 ≤ 64KB，防 DB 膨胀）
CREATE TABLE IF NOT EXISTS test_cases (
  id              INT          NOT NULL AUTO_INCREMENT,
  problem_id      INT          NOT NULL,
  order_no        INT          NOT NULL,
  input           MEDIUMTEXT   NOT NULL,
  expected_output MEDIUMTEXT   NULL,
  created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_problem_order (problem_id, order_no),
  CONSTRAINT fk_test_cases_problem FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;