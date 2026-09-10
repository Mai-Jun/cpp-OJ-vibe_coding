# 依赖安装说明（需 root 权限部分）

> 依据 `SPEC.md` 的设计方案整理的依赖清单。
> 非 root 可安装部分（cpp-httplib 单头文件）已完成，位于 `third_party/httplib.h`。

## 一、当前环境已就绪（无需操作）

| 依赖 | 版本/位置 | 用途 |
|---|---|---|
| g++ (C++17) | 11.4.0 | 服务端编译 + 评测编译用户代码 (`-std=c++17`) |
| cmake | 3.16.3 | 构建配置 |
| libmysqlclient21 | 8.0.46 | MySQL C API 运行时库（已装） |
| zlib1g-dev | 1.2.11 | HTTP gzip 压缩支持 |
| 内核对置 | — | fork + setrlimit 沙箱（B 档，系统内置） |
| unzip | — | 测试用例 zip 导入 |

## 二、需要 root 权限安装

以下命令需 `sudo` 执行（Ubuntu 22.04）。

### 1. MySQL 服务器 + 客户端 + C API 开发头文件

`db/db_conn.cpp` 使用 MySQL C API（`mysql.h`），需安装服务器、客户端工具及开发头文件：

```bash
sudo apt update
sudo apt install -y mysql-server mysql-client libmysqlclient-dev
```

> - `mysql-server`：提供 `mysqld` 数据库服务（schema 见 `src/db/schema.sql`）
> - `mysql-client`：提供 `mysql` 命令行工具（供 `scripts/init_db.sh` 初始化数据库）
> - `libmysqlclient-dev`：提供 `/usr/include/mysql/mysql.h` 头文件（编译 `db_conn` 必需）

安装后启动并检查：

```bash
sudo systemctl start mysql
sudo systemctl enable mysql
systemctl status mysql --no-pager
```

设置 root 访问方式（供 `init_db.sh` 使用，建议建专用 OJ 账号）：

```bash
sudo mysql <<'EOF'
CREATE DATABASE IF NOT EXISTS oj DEFAULT CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'oj'@'localhost' IDENTIFIED BY 'change_me_password';
GRANT ALL PRIVILEGES ON oj.* TO 'oj'@'localhost';
FLUSH PRIVILEGES;
EOF
```

### 2. 评测沙箱低权限用户（B 档沙箱运行进程）

`SPEC.md` §7 要求评测以**专用低权限用户**运行，降低越权风险：

```bash
sudo useradd -m -s /usr/sbin/nologin oj-runner
```

> 可选：限制更多权限
> ```bash
> sudo usermod -L oj-runner          # 锁定登录（无需改密码）
> ```

### 3. 管理员账号配置文件目录

管理员账号由配置文件指定（`SPEC.md` §8，默认 `/etc/oj/admin.conf`）：

```bash
sudo mkdir -p /etc/oj
```

> `config/admin.conf` 为项目内模板；部署时将其（或内容）放置到 `/etc/oj/admin.conf`。

---

## 三、安装后验证

```bash
# 1) MySQL
mysql -u oj -p -e "SHOW DATABASES;"

# 2) C API 头文件存在
ls -l /usr/include/mysql/mysql.h

# 3) 低权限用户存在
id oj-runner

# 4) 管理员配置目录存在
ls -l /etc/oj
```

都通过后即可执行 `scripts/init_db.sh` 初始化数据库并开始构建。