# C++ Online Judge（仿 LeetCode OJ）

基于 **cpp-httplib(C++17) + 原生 HTML/CSS/JS + MySQL** 的单体在线判题系统。
面向外部用户公开使用：注册登录、浏览题目、在线编写 C++、提交评测（7 类结果）、管理后台增删改题。

> 详细需求与验收标准见 [SPEC.md](SPEC.md)。本文件为部署/开发快速指南。

---

## 1. 功能总览

- 普通用户：自助注册（重名/格式校验）、登录、题目列表（难度/标签过滤）、题目详情（Markdown + 编辑器）、
  提交评测（**异步**返回 `submission_id`，前端轮询结果）、我的提交记录。
- 管理员（配置文件指定，不开放注册）：新增/删除/编辑题目，测试用例管理（文本编辑 / zip 批量导入）、单题限额覆盖。
- 评测：`g++ -O2 -std=c++17` 编译 → `fork + setrlimit + seccomp` 沙箱串行评测 →
  逐字节对比或特判器（spj）。结果 7 类：AC / WA / TLE / MLE / RE / CE / SE。

## 2. 目录结构

```
├── CMakeLists.txt
├── config/admin.conf        # 管理员账号模板（部署时复制到 /etc/oj/admin.conf）
├── src/
│   ├── main.cpp             # 入口：路由注册 + 静态托管 + 安全响应头
│   ├── routes/              # auth / problem / submission 路由
│   ├── auth/                # session、密码哈希（PBKDF2）
│   ├── db/                  # MySQL C API 封装 + schema.sql
│   ├── judge/               # 评测核心：sandbox(seccomp) / comparator(spj) / judge_service
│   └── util/                # json、zip、ratelimit（IP 限流）
├── static/                  # 前端页面 + css + js
├── scripts/
│   ├── init_db.sh           # 建库建账号（root）
│   ├── seed.sql             # 5 道种子题
│   ├── deploy.sh            # 一键部署
│   └── test.sh              # Phase 5 集成测试
└── third_party/httplib.h
```

## 3. 依赖

- g++（C++17）、cmake ≥ 3.16、zlib、OpenSSL
- MySQL 8 + libmysqlclient-dev（编译需 `mysql.h`）
- 评测沙箱低权限用户 `oj-runner`（推荐 root 部署时使用）

```bash
sudo apt install -y build-essential cmake libmysqlclient-dev zlib1g-dev mysql-server mysql-client
sudo useradd -m -s /usr/sbin/nologin oj-runner   # 评测低权限用户
sudo usermod -L oj-runner                        # 锁定登录（可选）
```

## 4. 初始化数据库

```bash
# 1) 建库 + 账号（需 root；账号/库名可用 OJ_DB_* 环境变量覆盖）
OJ_DB_PASS=你的密码 bash scripts/init_db.sh

# 2) 初始化表结构与种子题
MYSQL_PWD=你的密码 mysql -u oj -h127.0.0.1 oj < src/db/schema.sql
MYSQL_PWD=你的密码 mysql -u oj -h127.0.0.1 oj < scripts/seed.sql
```

> 2 核 2G 低内存服务器上若需临时起 MySQL 验证，按 SPEC §0 降内存参数启动（验后关闭）：
> `mysqld --datadir=... --port=33061 --innodb-buffer-pool-size=64M --performance-schema=OFF &`

## 5. 管理员账号

管理员入口与普通用户分开（`/login.html` 顶部切「管理员」）。账号由配置文件指定：

```bash
sudo mkdir -p /etc/oj
sudo cp config/admin.conf /etc/oj/admin.conf
sudo chmod 600 /etc/oj/admin.conf
# 编辑 /etc/oj/admin.conf 修改口令（username= / password=）
```

服务端优先读 `/etc/oj/admin.conf`，不存在则回退仓库内 `config/admin.conf`。

## 6. 构建与启动

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 启动（环境变量均可选）：
OJ_PORT=8080 \
OJ_DB_HOST=127.0.0.1 OJ_DB_PORT=3306 OJ_DB_USER=oj OJ_DB_PASS=你的密码 OJ_DB_NAME=oj \
OJ_STATIC_DIR=static \
./build/oj_server
```

- 默认端口 8080，默认静态目录 `static`（相对 cwd）。
- **以 root 启动**可自动把评测进程降权到 `oj-runner`（SPEC §7 要求）；非 root 则降级为当前用户运行并打印一次提示。
- 生产建议用 systemd 守护，示例：

```ini
# /etc/systemd/system/oj.service
[Unit]
Description=OJ Server
After=network.target mysql.service

[Service]
Type=simple
WorkingDirectory=/opt/cpp-OJ-vibe_coding
ExecStart=/opt/cpp-OJ-vibe_coding/build/oj_server
Environment=OJ_PORT=8080
Environment=OJ_DB_PASS=你的密码
Restart=on-failure
User=root

[Install]
WantedBy=multi-user.target
```

一键部署脚本 `scripts/deploy.sh` 会完成构建、用户创建、数据库初始化的引导。

## 7. 运行测试

`scripts/test.sh` 覆盖 Phase 5 验收：7 类判题结果、沙箱禁网络、IP 限流、并发提交、鉴权与边界用例。

```bash
# 1) 启动服务（含临时 MySQL）后：
bash scripts/test.sh http://127.0.0.1:8080
```

测试通过输出 `PASS=n FAIL=0`。

## 8. 安全说明（Phase 5 加固）

- **沙箱（B 档 + seccomp）**：评测子进程在 `fork + setrlimit(CPU/AS/NPROC) + 关闭 fd` 基础上，
  装载 **seccomp-bpf 白名单过滤器**，拦截所有网络系统调用（`socket/connect/...`）与高危调用
  （`ptrace/mount/bpf/clone3/unshare/...`），违反即以 `SIGSYS` 终止并判为 RE。
  seccomp 在非 root 下同样生效，不依赖降权用户。
- **降权**：root 启动时评测进程自动 `setuid/setgid` 到 `oj-runner`。
- **限流**：注册/登录按 IP 滑动窗口限流（超限 429）。
- **安全响应头**：`X-Content-Type-Options / X-Frame-Options / Referrer-Policy / Content-Security-Policy`。
- **已知风险**：B 档沙箱（无 namespace/cgroup）隔离有限，**公开部署建议升级 A 档**
  （`unshare` namespace + `seccomp` + `cgroup v2`），路径见 SPEC §7。

## 9. 常见问题

- **`mysql.h` not found**：安装 `libmysqlclient-dev`。
- **编译正常但启动报无法连接 MySQL**：确认 `OJ_DB_*` 环境变量、MySQL 已启动、账号有权限。
- **评测失败为 System Error**：多为题目无测试用例或数据库异常，查看服务端日志。
- **非 root 启动日志提示“评测不降权”**：正常现象，部署时建议 root。

---
*本项目按 SPEC.md 分阶段实现（Phase 1-5），验收标准见 SPEC §10。*
