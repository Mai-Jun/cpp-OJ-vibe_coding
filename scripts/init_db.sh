#!/usr/bin/env bash
# init_db.sh — 初始化 OJ 数据库：建库、建低权限账号、执行 schema.sql
#
# 用法: bash scripts/init_db.sh
# 需要 root 权限（脚本内部使用 sudo mysql，root 走 unix socket 认证）。
# 数据库名 / 账号可通过环境变量覆盖，与服务端一致：
#   OJ_DB_NAME=oj OJ_DB_USER=oj OJ_DB_PASS=... bash scripts/init_db.sh

set -euo pipefail

DB_NAME="${OJ_DB_NAME:-oj}"
DB_USER="${OJ_DB_USER:-oj}"
DB_PASS="${OJ_DB_PASS:-}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEMA="${ROOT_DIR}/src/db/schema.sql"

if [[ ! -f "$SCHEMA" ]]; then
  echo "[init_db] schema.sql not found: $SCHEMA" >&2
  exit 1
fi

echo "[init_db] 1/3 创建数据库与低权限账号（sudo mysql）..."
# 密码转义单引号，避免特殊字符破坏 SQL
ESC_DB_PASS="${DB_PASS//\'/\'\'}"

sudo mysql <<EOF
CREATE DATABASE IF NOT EXISTS \`$DB_NAME\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '$DB_USER'@'localhost' IDENTIFIED BY '$ESC_DB_PASS';
GRANT ALL PRIVILEGES ON \`$DB_NAME\`.* TO '$DB_USER'@'localhost';
FLUSH PRIVILEGES;
EOF

echo "[init_db] 2/3 执行 schema.sql（作为 $DB_USER）..."
MYSQL_PWD="$DB_PASS" mysql -u "$DB_USER" -h 127.0.0.1 "$DB_NAME" --default-character-set=utf8mb4 < "$SCHEMA"

echo "[init_db] 3/3 验证..."
MYSQL_PWD="$DB_PASS" mysql -u "$DB_USER" -h 127.0.0.1 -e "USE \`$DB_NAME\`; SHOW TABLES;" 2>/dev/null || true

echo "[init_db] 完成。数据库 $DB_NAME，账号 $DB_USER@localhost。"