#!/usr/bin/env bash
# deploy.sh — Phase 5 一键部署脚本（面向 2 核 2G 服务器）。
#
# 说明：
#   - 构建项目（Release）。
#   - 可选：创建低权限评测用户 oj-runner。
#   - 可选：初始化 MySQL（建库 + 账号 + schema + 5 道种子题）。
#   - 可选：生成/部署管理员账号配置 /etc/oj/admin.conf。
#   - 启动服务（前台），建议配合 systemd/后台运行，见 README.md。
#
# 依赖 root 的操作（创建用户、写 /etc/oj、初始化 MySQL）会尝试 sudo；
# 若当前用户无密码 sudo，会提示手动执行。
#
# 环境变量（与 init_db.sh / 服务端一致）：
#   OJ_DB_NAME / OJ_DB_USER / OJ_DB_PASS / OJ_DB_HOST / OJ_DB_PORT
#   OJ_PORT    （服务监听端口，默认 8080）
#   OJ_ADMIN_CONF（管理员配置文件路径，默认 /etc/oj/admin.conf）
#   OJ_SKIP_DB   （非空则跳过数据库初始化）
#   OJ_SKIP_USER （非空则跳过创建 oj-runner）

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PORT="${OJ_PORT:-8080}"
DB_NAME="${OJ_DB_NAME:-oj}"
DB_USER="${OJ_DB_USER:-oj}"
DB_PASS="${OJ_DB_PASS:-}"
ADMIN_CONF="${OJ_ADMIN_CONF:-/etc/oj/admin.conf}"
RUNNER_USER="${OJ_RUNNER_USER:-oj-runner}"

need_sudo() {
  if ! sudo -n true 2>/dev/null; then
    echo "  [deploy] 需要 root 操作但无免密 sudo，请手动执行以下命令："
    echo "    $1"
    return 1
  fi
  return 0
}

echo "== 1/5 构建（Release）=="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo "== 2/5 低权限评测用户 ${RUNNER_USER} =="
if [ -n "${OJ_SKIP_USER:-}" ]; then
  echo "  跳过（OJ_SKIP_USER 已设置）"
elif id "$RUNNER_USER" >/dev/null 2>&1; then
  echo "  已存在，跳过创建。"
else
  if need_sudo "sudo useradd -m -s /usr/sbin/nologin $RUNNER_USER"; then
    sudo useradd -m -s /usr/sbin/nologin "$RUNNER_USER"
    sudo usermod -L "$RUNNER_USER" || true
    echo "  已创建 ${RUNNER_USER} 并锁定登录。"
  fi
fi

echo "== 3/5 数据库初始化 =="
if [ -n "${OJ_SKIP_DB:-}" ]; then
  echo "  跳过（OJ_SKIP_DB 已设置），请确保数据库已就绪。"
elif command -v mysqld >/dev/null 2>&1 && mysqladmin ping -h127.0.0.1 --silent 2>/dev/null; then
  # 已有 MySQL 在运行
  echo "  检测到 MySQL 在运行，执行 schema + 种子题："
  if [ -n "$DB_PASS" ]; then
    mysql -u "$DB_USER" -p"$DB_PASS" -h127.0.0.1 "$DB_NAME" < src/db/schema.sql || \
      { echo "  schema 初始化失败，请先运行 scripts/init_db.sh 建库建账号。"; exit 1; }
    mysql -u "$DB_USER" -p"$DB_PASS" -h127.0.0.1 "$DB_NAME" < scripts/seed.sql
  else
    echo "  未设置 OJ_DB_PASS，无法以非交互方式初始化。"
    echo "  请手动执行：scripts/init_db.sh 后用 mysql 导入 schema.sql 与 seed.sql。"
  fi
else
  echo "  未检测到运行中的 MySQL 或需临时启动；请参考 README.md「启动 MySQL」小节。"
  echo "  快速路径：scripts/init_db.sh（需 root + 系统 mysqld）"
fi

echo "== 4/5 管理员账号配置 =="
if [ -f "$ADMIN_CONF" ]; then
  echo "  $ADMIN_CONF 已存在，跳过。"
else
  if [[ "$ADMIN_CONF" == /etc/oj/* ]]; then
    if need_sudo "sudo mkdir -p /etc/oj && sudo cp config/admin.conf /etc/oj/admin.conf && sudo chmod 600 /etc/oj/admin.conf"; then
      sudo mkdir -p /etc/oj
      sudo cp config/admin.conf "$ADMIN_CONF"
      sudo chmod 600 "$ADMIN_CONF"
      echo "  已从 config/admin.conf 复制管理员配置，请修改其中的口令！"
    fi
  else
    cp config/admin.conf "$ADMIN_CONF"
    chmod 600 "$ADMIN_CONF"
    echo "  已复制管理员配置到 $ADMIN_CONF，请修改其中的口令！"
  fi
fi

echo "== 5/5 启动服务 =="
echo "  监听端口 $PORT，静态目录 static/，数据库 $DB_NAME@$DB_USER"
echo "  启动命令（前台；生产建议用 systemd，见 README.md）："
echo "    OJ_PORT=$PORT OJ_DB_NAME=$DB_NAME OJ_DB_USER=$DB_USER OJ_DB_PASS=... ./build/oj_server"
echo ""
echo "  服务以 root 启动即可自动降权评测到 $RUNNER_USER；"
echo "  非 root 启动则评测不降权（seccomp 仍生效，但隔离弱化，SPEC §7）。"

exit 0
