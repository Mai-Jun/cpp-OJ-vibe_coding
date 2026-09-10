# RUNBOOK.md — 启动 / 重启 / 停止指南

> 记录 2026-09-11 实测可用的启动步骤（非 root 用户 `mai`，2 核 2G 服务器，SPEC §0 内存约束）。
> 与 README.md 的区别：本文件为「验证环境」的精确操作步骤，含临时 MySQL 实例的启停。

---

## 0. 当前环境概览

| 组件 | 端口 | 说明 |
|---|---|---|
| MySQL（临时实例） | 33061 | 降内存参数启动，datadir 在 `/tmp/oj-mysql`，验后必须关闭 |
| `oj_server` | 8080 | C++ 服务，静态目录 `static/` |
| 种子题 / 用例 | — | 5 道题 / 23 条用例，已入库 `oj` 库 |

运行身份：`mai`（非 root，无免密 sudo）。

> ⚠️ 临时 MySQL 数据目录在 `/tmp`，重启系统后丢失。数据需长期保存时改用系统 MySQL（见 README §4）。

---

## 1. 快速启动（一条龙）

```bash
# 1) 确认临时 MySQL 未在运行
mysqladmin --socket=/tmp/oj-mysql/mysql.sock -u root ping 2>/dev/null || echo "未运行"

# 2) 启动临时 MySQL（若 /tmp/oj-mysql 不存在，先初始化，见 §2.1）
mysqld --no-defaults --datadir=/tmp/oj-mysql --port=33061 --bind-address=127.0.0.1 \
  --socket=/tmp/oj-mysql/mysql.sock --pid-file=/tmp/oj-mysql/mysql.pid \
  --log-error=/tmp/oj-mysql/mysql.err --innodb-buffer-pool-size=64M \
  --performance-schema=OFF --skip-log-bin --max_connections=20 &
sleep 6 && mysqladmin --socket=/tmp/oj-mysql/mysql.sock -u root ping

# 3) 启动 OJ 服务（关键：setsid 脱离会话，避免被终端超时杀掉）
cd /home/mai/cpp-OJ-vibe_coding
setsid bash -c 'OJ_PORT=8080 OJ_DB_HOST=127.0.0.1 OJ_DB_PORT=33061 OJ_DB_USER=oj \
OJ_DB_PASS=oj_pass_2026 OJ_DB_NAME=oj \
OJ_STATIC_DIR=/home/mai/cpp-OJ-vibe_coding/static \
./build/oj_server > /tmp/oj_server.log 2>&1' < /dev/null &

# 4) 健康检查
curl -s http://127.0.0.1:8080/health   # 期望 {"status":"ok"}
```

---

## 2. 详细步骤

### 2.1 临时 MySQL：首次初始化数据目录

> 仅当 `/tmp/oj-mysql` 不存在时执行；已初始化则直接跳到 2.2。

```bash
rm -rf /tmp/oj-mysql && mkdir -p /tmp/oj-mysql
mysqld --no-defaults --initialize-insecure --datadir=/tmp/oj-mysql \
  --log-error=/tmp/oj-mysql-init.err --innodb-buffer-pool-size=64M \
  --performance-schema=OFF
ls /tmp/oj-mysql   # 出现 mysql/、ibdata1 等即成功
```

> 注意：`--no-defaults` 必须放在第一个，否则会读到系统 `[mysqld]` 配置而尝试写
> `/var/lib/mysql` 或 `/var/log/mysql`（无权限，报错退出）。

### 2.2 临时 MySQL：启动

```bash
mysqld --no-defaults --datadir=/tmp/oj-mysql --port=33061 --bind-address=127.0.0.1 \
  --socket=/tmp/oj-mysql/mysql.sock --pid-file=/tmp/oj-mysql/mysql.pid \
  --log-error=/tmp/oj-mysql/mysql.err --innodb-buffer-pool-size=64M \
  --performance-schema=OFF --skip-log-bin --max_connections=20 &
sleep 6
mysqladmin --socket=/tmp/oj-mysql/mysql.sock -u root ping   # mysqld is alive
```

### 2.3 初始化数据库 + 导入种子题

> 仅在首次建库或数据被清空时执行；已导入则跳过。

```bash
SOCK=/tmp/oj-mysql/mysql.sock
DB_PASS='oj_pass_2026'
cd /home/mai/cpp-OJ-vibe_coding

# 建库 + 低权限账号（root 走本地 socket）
mysql --no-defaults --socket=$SOCK -u root -e \
  "CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
   CREATE USER IF NOT EXISTS 'oj'@'127.0.0.1' IDENTIFIED BY '$DB_PASS';
   GRANT ALL PRIVILEGES ON oj.* TO 'oj'@'127.0.0.1'; FLUSH PRIVILEGES;"

# 导入表结构与种子题
MYSQL_PWD=$DB_PASS mysql --no-defaults --socket=$SOCK -u oj -h127.0.0.1 --port=33061 oj < src/db/schema.sql
MYSQL_PWD=$DB_PASS mysql --no-defaults --socket=$SOCK -u oj -h127.0.0.1 --port=33061 oj < scripts/seed.sql

# 验证
MYSQL_PWD=$DB_PASS mysql --no-defaults --socket=$SOCK -u oj -h127.0.0.1 --port=33061 oj \
  -e "SELECT COUNT(*) AS problems FROM problems; SELECT COUNT(*) AS cases FROM test_cases;"
```

### 2.4 启动 OJ 服务

```bash
cd /home/mai/cpp-OJ-vibe_coding
setsid bash -c 'OJ_PORT=8080 OJ_DB_HOST=127.0.0.1 OJ_DB_PORT=33061 OJ_DB_USER=oj \
OJ_DB_PASS=oj_pass_2026 OJ_DB_NAME=oj \
OJ_STATIC_DIR=/home/mai/cpp-OJ-vibe_coding/static \
./build/oj_server > /tmp/oj_server.log 2>&1' < /dev/null &
```

**必须用 `setsid`**：后台进程若挂在当前 shell 会话下，会被终端/工具超时机制连带杀掉。
启动后立即返回，进程独立于会话运行。

### 2.5 确认运行

```bash
ps aux | grep oj_server | grep -v grep
curl -s http://127.0.0.1:8080/health          # {"status":"ok"}
cat /tmp/oj_server.log                        # 启动日志
```

---

## 3. 停止

```bash
# 停止 OJ 服务
pkill -f 'build/oj_server'

# 停止临时 MySQL（SPEC §0：验证完成必须立即关闭）
mysqladmin --no-defaults --socket=/tmp/oj-mysql/mysql.sock -u root shutdown
```

---

## 4. 验收

### 4.1 自动化测试（28 项）

```bash
cd /home/mai/cpp-OJ-vibe_coding
bash scripts/test.sh http://127.0.0.1:8080    # 期望 PASS=28 FAIL=0
```

### 4.2 浏览器手动验收进度

> 记录时间：2026-09-11。以下为按 SPEC §10 的逐项验收状态。
> 云服务器公网 8080 已放行，`http://<公网IP>:8080/login.html` 主页面访问成功 ✅。

| # | 验收项（SPEC §10） | 状态 |
|---|---|---|
| 1 | 普通用户自助注册（重名/格式校验），注册后登录 | ⬜ 未做 |
| 2 | 管理员新增/删除/编辑题目（含测试用例上传、单题限额） | ⬜ 未做 |
| 3 | 浏览题目、提交 C++ 代码，得到 7 类判题结果之一；WA 显示「预期 vs 实际」 | ⬜ 未做 |
| 4 | 超时/超内存/运行时错误直接终止并返回对应结果 | ⬜ 未做 |
| 5 | 未登录不能进功能页或调受限 API；管理员入口与普通用户隔离 | ⬜ 未做 |
| 6 | 提交异步返回 submission_id，前端轮询到最终结果 | ⬜ 未做 |
| 7 | 种子 5 题完整走通「列表→详情→提交→判题→结果」闭环 | ⬜ 未做 |
| 8 | 用户代码无法读写系统关键文件、无法访问网络、无法波及服务器进程 | ⬜ 未做 |

> 说明：以上 8 项已由自动化测试（§4.1，PASS=28 FAIL=0）在服务端覆盖验证；
> 下表为**浏览器端手工走查**，验证 UI 交互与端到端体验。

### 4.3 浏览器手工验收清单（下次继续）

1. **注册**：`/register.html` 注册新用户 → 重名再注册应被拒 → 成功后跳登录
2. **登录**：`/login.html` 普通用户登录 → 进入题目列表
3. **题目列表**：`/problems.html` 看到 5 道种子题，按难度/标签过滤
4. **题目详情 + 提交**：打开题 1 → 编辑器粘贴以下代码 → 提交 → 轮询到 **Accepted**：
   ```cpp
   #include <cstdio>
   int main(){ long long a,b; scanf("%lld%lld",&a,&b); printf("%lld\n", a+b); return 0; }
   ```
5. **WA 展示**：把上面代码 `a+b` 改为 `a-b` 提交 → 应显示 Wrong Answer，且含「预期输出 vs 实际输出」对比
6. **TLE**：提交死循环 `int main(){ while(true){} }` → Time Limit Exceeded
7. **提交记录**：`/submissions.html` 能看到上述提交历史
8. **管理员后台**：登录页切「管理员」→ `admin` / `CHANGE_ME_ADMIN_PASSWORD`（`config/admin.conf`）→
   新增/删除/编辑题目、管理测试用例与限额
9. **未登录拦截**：退出登录后直接访问 `/problems.html` 或 `/api/problems` 应被拒（跳登录/401）
10. **沙箱验证**：提交含 `socket()` 的代码应被 seccomp 拦截 → Runtime Error

---

## 5. 故障排查

| 现象 | 处理 |
|---|---|
| `[main] cannot connect to MySQL` | 检查 MySQL 是否启动：`mysqladmin --socket=/tmp/oj-mysql/mysql.sock -u root ping` |
| `static dir not found` | 以仓库根目录为 cwd 运行，或设置 `OJ_STATIC_DIR` 绝对路径 |
| `/var/log/mysql/error.log: Permission denied` | 初始化/启动 MySQL 时漏了 `--no-defaults` |
| 服务启动后 curl 无响应 | 用 `setsid` 重启（§2.4），避免后台进程被杀 |
| 评测 System Error | 查看 `/tmp/oj_server.log`；多为题目无用例或数据库异常 |
| 端口被占用 | 换 `OJ_PORT`，并同步修改验收脚本的 BASE 地址 |
