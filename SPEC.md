# SPEC.md — C++ 在线判题系统(仿 LeetCode OJ)

> 版本:v1.0  |  状态:需求已冻结
> 后端:cpp-httplib(C++17)  |  前端:原生 HTML/CSS/JS  |  数据库:MySQL

---

## 0. 服务器环境与开发要求(强制)

> **声明:本项目运行于一台 2核2G 的服务器,磁盘 IO 性能有限。**

**开发强制要求(任何阶段均必须遵守):**
- 严禁高频扫描系统盘和项目文件(此前曾因高频扫描导致磁盘 IO 达到上限并最终崩溃)。
- 访问项目文件前先阅读目录结构规划(§11)与已有记录,按需、一次性、批量读取文件,禁止反复递归遍历目录。
- 禁止使用 `find`/`watch`/周期性轮询等高 IO 操作;文件搜索用精确路径或一次性 glob/grep,并尽量减小搜索范围。
- 编译、测试等命令注意控制输出量与频率;评测临时文件用完即清理。
- 日志量保持精简,避免刷盘。
- **内存约束**:系统总内存约 1.6GB,常驻可用约 500~600MB。MySQL 仅在需要集成验证时以**降内存参数临时启动**(如 `--innodb-buffer-pool-size=64M --performance-schema=OFF` 等),验证完成**必须立即关闭**,禁止让 mysqld 常驻;日常开发/审查默认不启动 MySQL。

---

## 1. 需求概述

面向**外部用户公开使用**的在线判题(Online Judge)系统。用户登录后查看题目、在线编写 C++ 代码、提交评测并查看结果;管理员可在后台新增/删除题目。

### 核心功能
- 在线代码编辑、编译、运行、返回判题结果
- 题目列表 / 题目详情(描述 + 测试用例 + 运行限额)
- 管理后台(新增/删除题目,含测试用例管理)
- 角色:普通用户(查看+做题)、管理员(增删题)

---

## 2. 架构设计

### 2.1 总体架构

```
                     ┌─────────────────────────────────────────┐
   Browser (原生      │                OJ 单体 Server (C++)        │
   HTML/CSS/JS)      │  ┌──────────────┐   ┌─────────────────┐   │
     │  HTTP/JSON     │  │  cpp-httplib │   │  JudgeService   │   │
     │  (fetch)       │  │  API 路由层   │   │  串行评测队列     │   │
     ▼                │  │  静态资源     │   │  fork+setrlimit │   │
   ┌───────┐          │  │  鉴权(Session)│   │  沙箱(B档)      │   │
   │ 页面   │          │  └──────┬───────┘   └────────┬────────┘   │
   │ 列表   │◄─────────│         │                    │            │
   │ 详情+  │          │         ▼                    ▼            │
   │  编辑器 │          │  ┌────────────┐      ┌────────────────┐  │
│  后台   │          │  │   MySQL     │      │  临时目录       │  │
└───────┘          │  │  题目/账号/  │      │ (评测时生成用例) │  │
                      │  │  会话/测试用例│      │  用完即清理     │  │
                      │  └────────────┘      └────────────────┘  │
                      └─────────────────────────────────────────┘
```

### 2.2 关键设计决策
- **评测模型**:程序读 stdin、写 stdout,与期望输出**逐字节对比**;支持**特判器**(special judge);**不支持**交互式(一期,标记为未来扩展)。
- **沙箱(B 档)**:以低权限用户运行 + `fork()` + `setrlimit(RLIMIT_CPU/AS)`。⚠️ 隔离较弱,**公开部署已知风险**,归档升级到 A 档(`unshare` namespace + `seccomp` + `cgroup`)的路径见 §7。
- **评测队列**:串行评测队列(单 worker,安全可靠)。
- **运行限额**:限 CPU 时间 + 内存;**全局默认 CPU 500ms**,**管理员建题/改题时可针对单题覆盖**。
- **判题结果类型**(7 类):`Accepted / Wrong Answer(含预期vs实际对比) / Time Limit Exceeded / Memory Limit Exceeded / Runtime Error / Compile Error / System Error`。超时/超内存/RTE 直接终止并返回对应结果。
- **判定响应**:异步 —— 提交返回 `submission_id`,前端轮询结果。
- **鉴权**:Session(服务端存储登录态,Cookie 携带),管理员登录入口与普通用户**分开**,管理员账号由**配置文件**指定。

---

## 3. 数据模型(MySQL)

> 提交记录与用户代码**一期不持久化**(仅内存/临时,便于异步返回)。

### users
| 字段 | 类型 | 说明 |
|---|---|---|
| id | INT PK AUTO | 用户ID |
| username | VARCHAR(64) UNIQUE | 用户名 |
| password_hash | VARCHAR(128) | 密码哈希(建议 bcrypt/argon2) |
| role | ENUM('user','admin') | 角色 |

> 注册仅允许创建 `role='user'` 的账号;管理员账号只能由配置文件指定,不开放注册。

### problems
| 字段 | 类型 | 说明 |
|---|---|---|
| id | INT PK AUTO | 题目ID |
| title | VARCHAR(128) | 标题 |
| description | TEXT | Markdown 描述 |
| difficulty | ENUM('easy','medium','hard') | 难度 |
| tags | VARCHAR(255) | 标签(逗号分隔) |
| time_limit_ms | INT | CPU 限额(毫秒,默认 500,可覆盖) |
| memory_limit_mb | INT | 内存限额(MB) |
| judge_type | ENUM('exact','special') | 'exact'逐字节 / 'special'特判 |
| spj_source | MEDIUMTEXT NULL | 特判器源码(仅 judge_type='special') |
| created_at / updated_at | DATETIME | 时间戳 |

### test_cases(测试用例,表存储)
| 字段 | 类型 | 说明 |
|---|---|---|
| id | INT PK AUTO | 用例ID |
| problem_id | INT FK | 所属题目(级联删除) |
| order_no | INT | 用例顺序(从 1 开始) |
| input | MEDIUMTEXT | 输入内容 |
| expected_output | MEDIUMTEXT | 期望输出(逐字节对比;特判题可空) |
| created_at / updated_at | DATETIME | 时间戳 |

> 输入/期望输出以文本直接入库,单条限制建议 ≤ 64KB,防 DB 膨胀。

### sessions(服务端 Session)
| 字段 | 类型 | 说明 |
|---|---|---|
| id | VARCHAR(64) PK | session_id |
| user_id | INT FK | 关联用户 |
| created_at / expires_at | DATETIME | 创建/过期时间 |

---

## 4. 测试用例存储(表存储)

- 测试用例存入 MySQL **`test_cases` 表**(见 §3),管理员在后台上传/编辑,随题目增删。
- 评测时 JudgeService 从表中拉取用例,写入**独立临时目录**的 `case_N.in` / `case_N.out` 文件供子进程读写,评测结束清理临时目录。
- 特判器:源码存于 `problems.spj_source`(仅特判题),评测时编译为 `spj` 可执行文件,运行接收「输入 + 用户输出 + 标准答案」,返回 0/非0。
- 用例内容以文本入库(单条 ≤ 64KB);超限或二进制用例视为不支持(一期)。

---

## 5. 后端 API 边界

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| POST | /api/auth/register | 公开 | 普通用户注册(用户名 + 密码,默认 role=user) |
| POST | /api/auth/login | 公开 | 普通用户登录(建立 Session) |
| POST | /api/auth/admin/login | 公开 | 管理员登录(入口分离) |
| POST | /api/auth/logout | 登录 | 登出 |
| GET | /api/problems | 登录 | 题目列表 |
| GET | /api/problems/{id} | 登录 | 题目详情 |
| POST | /api/problems | 管理员 | 新增题目 |
| DELETE | /api/problems/{id} | 管理员 | 删除题目 |
| PUT | /api/problems/{id} | 管理员 | 修改题目/限额 |
| POST | /api/submissions | 登录 | 提交代码 → 返回 submission_id |
| GET | /api/submissions/{id} | 登录 | 轮询判题结果(含 WA 对比详情) |
| GET | /api/submissions | 登录 | 我的提交记录(内存态) |

> 静态资源:前端 HTML/CSS/JS 由 cpp-httplib 直接托管。

---

## 6. 页面(前端,原生三件套)

1. 注册页(用户名 + 密码,校验重名/格式,成功后跳转登录页)
2. 登录页(普通用户 / 管理员分开入口)
3. 题目列表页(含难度/标签过滤)
4. 题目详情 + 代码编辑器 + 提交页(展示结果、WA 预期vs实际)
5. 提交记录页
6. 管理后台(新增/删除/编辑题目,管理测试用例与限额)

---

## 7. 安全、性能与已知风险

- **已实现(Phase 5)**:评测子进程在 B 档基础上装载 **seccomp-bpf 白名单过滤器**,禁止所有网络系统调用
  (`socket/connect/bind/accept/...`)与高危调用(`ptrace/mount/bpf/clone3/unshare/setns/keyctl/...`),
  违反即以 `SIGSYS` 终止并判为 RE;seccomp 在非 root 下同样生效。
- **已实现(Phase 5)**:注册/登录接口按 IP 滑动窗口限流(超限 429);全站安全响应头
  (`X-Content-Type-Options / X-Frame-Options / Referrer-Policy / Content-Security-Policy`);
  评测临时目录启动时清理残留(防磁盘膨胀)。
- **已接受风险**:B 档沙箱(无 namespace/cgroup)存在同机越权/逃逸风险。**发布前必须**:以专用低权限用户
  运行评测进程、内核启用非 root 降权。
  - 实现注意:评测子进程通过 `setrlimit(RLIMIT_CPU, soft < hard)` 使 CPU 超限触发 `SIGXCPU`(soft==hard 时内核直接 SIGKILL,无法区分 CPU 超时);`RLIMIT_AS` 超限表现为 `bad_alloc`/`SIGSEGV`,据此归类 MLE。
  - **降权依赖 root**:`setuid` 切换低权限用户(`oj-runner`)要求服务端以 root 启动;非 root 环境自动降级为当前用户运行(仅打印一次提示),此时隔离减弱,部署时务必 root。
- **升级路径(A 档)**:`unshare(CLONE_NEWPID/NEWNS/NEWNET/NEWIPC)` + `seccomp-bpf` 白名单过滤系统调用 + `cgroup v2` 限制 CPU/内存 + 网络禁用。
- 用户代码**禁止网络访问**(seccomp 已拦截,无需配置网络 namespace)。
- Session 口令用安全随机数生成;密码不得明文存储(注册时同样加盐哈希)。
- 输入长度、提交内容大小限制,防 DoS;注册接口按 IP 限流/防滥用。
- 性能:10 人并发、单 worker 串行评测,单机余量充分。

---

## 8. 种子数据

- 内置 **5 道题**(by 你,手工预置题目 + 测试用例,入库 `problems` / `test_cases` 表),难度分布建议 2 easy / 2 medium / 1 hard。
- 管理员账号由 `/etc/oj/admin.conf`(或 `config` 文件)指定。

---

## 9. TODO 清单(分阶段)

### Phase 1 — 骨架与鉴权
- [X] cpp-httplib 服务骨架 + 静态资源托管
- [X] MySQL 连接层(可选用 mysql-connector-c++ 或直接封装 mysql C API)
- [X] 用户/会话表 + Session 鉴权中间件
- [X] 注册 API + 注册页(重名/格式校验,默认 role=user)
- [X] 登录/登出 API + 登录页(普通用户/管理员分离)

### Phase 2 — 题目管理
- [X] problems 表 + test_cases 测试用例表(表存储)
- [X] 题目 CRUD API + 管理员后台页面
- [X] 导入算法:解压上传.zip → 解析用例写入 test_cases 表
- [X] 5 道种子题

### Phase 3 — 评测核心
- [X] 编译(g++ -O2 -std=c++17 等)到独立临时目录
- [X] fork + setrlimit 沙箱运行(CPU/内存限额、超时 kill)
- [X] 串行评测队列 + 异步 submission 模型
- [X] 逐字节对比 + 特判器(spj)支持
- [X] 结果分类(7 类)+ WA 预期vs实际详情

### Phase 4 — 前端完整
- [X] 题目列表 + 详情 + 编辑器(自带简单高亮)
- [X] 提交 + 轮询结果 UI
- [X] 提交记录页
- [X] 后台题目管理 UI

### Phase 5 — 加固与收尾
- [X] 安全加固(见 §7):seccomp 禁网/高危 syscall + 评测降权 + 注册/登录限流 + 安全响应头 + 临时目录清理
- [X] 并发/异常/边界用例测试(scripts/test.sh:7 类结果、禁网、限流、并发、边界)
- [X] README + 部署脚本(README.md / scripts/deploy.sh)

---

## 10. 验收标准

1. 普通用户可自助注册(重名/格式校验),注册后可用账号密码登录。
2. 管理员可新增/删除/编辑题目(含上传测试用例、设置单题限额)。
3. 普通用户可浏览题目、提交 C++ 代码、得到 7 类判题结果之一;WA 时展示「预期输出 vs 实际输出」。
4. 超时/超内存/运行时错误直接终止并返回对应结果。
5. 未登录不能进入功能页或调用受限 API;管理员入口与普通用户隔离。
6. 提交为**异步**返回 submission_id,前端可轮询到最终结果。
7. 种子 5 题可完整走通「列表→详情→提交→判题→结果」闭环。
8. 任意用户代码无法读写系统关键文件、无法访问网络、无法波及服务器进程。

---

## 11. 项目目录结构(规划)

```
cpp-OJ-vibe_coding/
├── SPEC.md                        # 需求规格文档
├── CMakeLists.txt                 # 构建配置(C++17)
├── config/
│   └── admin.conf                 # 管理员账号配置(用户名/口令,不开放注册)
├── src/                           # C++ 后端源码
│   ├── main.cpp                   # 入口:cpp-httplib 服务 + 路由注册 + 静态资源托管
│   ├── routes/                    # API 路由层
│   │   ├── auth_routes.cpp/.h     # 注册 / 登录(普通/管理员) / 登出
│   │   ├── problem_routes.cpp/.h  # 题目 CRUD(管理员)+ 列表/详情(登录)
│   │   └── submission_routes.cpp/.h # 提交 / 轮询结果 / 我的提交记录
│   ├── db/
│   │   ├── db_conn.cpp/.h         # MySQL 连接层(mysql C API 封装)
│   │   └── schema.sql             # 建表脚本(users / problems / test_cases / sessions)
│   ├── auth/
│   │   ├── session.cpp/.h         # Session 创建/校验/过期(服务端存储)
│   │   └── password.cpp/.h        # 密码加盐哈希
│   ├── judge/                     # 评测核心
│   │   ├── judge_service.cpp/.h   # 串行评测队列 + 异步 submission 模型
│   │   ├── sandbox.cpp/.h         # fork + setrlimit(CPU/内存限额、超时 kill)
│   │   └── comparator.cpp/.h      # 逐字节对比 + 特判器(spj)编译与运行
│   └── util/                      # 通用工具(随机数、JSON 解析等)
├── static/                        # 前端静态资源(由 cpp-httplib 直接托管)
│   ├── index.html                 # 入口页(按登录态跳转)
│   ├── login.html                 # 登录页(普通用户/管理员分开入口)
│   ├── register.html              # 注册页
│   ├── problems.html              # 题目列表页
│   ├── problem.html               # 题目详情 + 代码编辑器 + 提交
│   ├── submissions.html           # 提交记录页
│   ├── admin.html                 # 管理后台(增删改题 + 测试用例管理)
│   ├── css/
│   │   └── style.css              # 全局样式
│   └── js/
│       ├── api.js                 # fetch 封装(携带 Cookie)
│       ├── auth.js                # 登录态检查 / 登出
│       └── editor.js              # 简单代码高亮
├── scripts/
│   ├── init_db.sh                 # 初始化数据库(执行 schema.sql)
│   └── seed.sql                   # 5 道种子题(problems + test_cases 表数据)
└── third_party/
    └── httplib.h                  # cpp-httplib 单头文件
```

> 说明:测试用例已改为**表存储**(§3/§4),无 `/data/problems` 文件目录;文件系统仅用于评测时的独立临时目录,由 JudgeService 创建并清理。