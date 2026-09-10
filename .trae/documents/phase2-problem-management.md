# Phase 2 — 题目管理（problems / test_cases 表 + CRUD API + 后台页面 + zip 导入 + 种子题）

## Context（背景）

Phase 1 已完成：服务骨架、MySQL 连接层、用户/会话表 + Session 鉴权、注册/登录/登出 API 与页面。
本任务按 `SPEC.md` §9 Phase 2 落地题目管理：`problems` + `test_cases` 表（表存储）、题目 CRUD API、
管理员后台页面（`static/admin.html`）、zip 用例批量导入、以及 5 道种子题（2 easy / 2 medium / 1 hard）。

## 设计决策

- **表存储**：测试用例存 `test_cases` 表（见 `schema.sql`），随题目级联删除（FK `ON DELETE CASCADE`），
  顺序由 `order_no`（UNIQUE(problem_id, order_no)）控制；单条输入/期望输出 ≤ 64KB。
- **API 边界**（`src/routes/problem_routes.cpp`）：
  - 列表 `GET /api/problems`（登录，支持 `?difficulty=` / `?tag=` 过滤）
  - 详情 `GET /api/problems/{id}`（登录，含描述 + 用例数组）
  - 新增 `POST /api/problems`（管理员，可随 body 携带 `test_cases` 数组）
  - 修改 `PUT /api/problems/{id}`（管理员，携带 `test_cases` 数组时整体替换）
  - 删除 `DELETE /api/problems/{id}`（管理员，级联删用例）
  - 追加用例 `POST /api/problems/{id}/cases`（管理员）
  - zip 导入 `POST /api/problems/{id}/import`（管理员，multipart 上传）
- **路由语法**：httplib 0.18 用 `R"(/api/problems/(\d+))"` 正则匹配，`req.matches[1]` 取参数。
- **zip 解析**：`src/util/zip.h`（header-only，zlib inflate），仅支持 store(0)/deflate(8)，拒绝目录项、
  路径穿越（`..`/绝对路径）与未知压缩算法；限制上传 ≤ 8MB、单题 ≤ 256 用例。
- **导入格式**：兼容 `case_N.in` / `case_N.out` 同级配对，或 `inputs/` + `outputs/` 两个目录。
- **特殊题（special）**：`spj_source` 存特判器源码，判题方式为 special 时必填（评测核心 Phase 3 使用）。
- **json.h 修复**：`Parse` 原为跨 TU 重复定义，改为 `inline`（多路由文件包含所需）。

## 目标产物（本次改动）

| 文件 | 说明 |
|---|---|
| `src/db/schema.sql` | 追加 `problems` / `test_cases` 建表（幂等） |
| `src/routes/problem_routes.h/.cpp` | 题目 CRUD + 用例管理 + zip 导入 API |
| `src/util/zip.h` | 极简 ZIP 解析（zlib inflate，安全防护） |
| `src/main.cpp` | 注册 `RegisterProblemRoutes` |
| `CMakeLists.txt` | 加入 `problem_routes.cpp`，链接 `ZLIB::ZLIB` |
| `static/admin.html` | 管理后台：增删改题、用例编辑、zip 导入 |
| `static/css/style.css` | 追加后台表格/表单/徽章等样式 |
| `scripts/seed.sql` | 5 道种子题（A+B、两数之和、最长无重复子串、合并区间、反转链表） |
| `SPEC.md` | Phase 2 清单勾选 |

## 验证方式（集成验证需临时起 MySQL，验后关闭）

```bash
# 1) 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2) 降内存临时启动 MySQL（验后立即关闭，见 SPEC §0）
mysqld --datadir=... --port=33061 ... &
# 3) 初始化 schema + 种子题
mysql -u oj -p oj < src/db/schema.sql
mysql -u oj -p oj < scripts/seed.sql
# 4) 启动服务并 curl 冒烟：登录/管理登录、列表/详情、增删改、zip 导入
# 5) 关闭 mysqld
```

## 验证结果（2026-09-10，临时 MySQL 起停后均已关闭）

- 构建 `-Wall -Wextra` 无警告；`oj_server` 正常启动。
- schema + seed 初始化成功：5 题（2 easy / 2 medium / 1 hard），23 用例。
- 未登录访问题目 API → 401；普通用户新增题目 → 403；管理员登录成功。
- 列表（含 `?difficulty=` / `?tag=` 过滤）、详情、新增（带用例）、修改（整体替换用例）、
  追加用例、删除（级联删用例）全部通过。
- zip 导入：`case_N.in/.out` 同级、`inputs/`+`outputs/` 两种约定均导入成功；
  路径穿越 zip（`../evil.in`）被拒（400 `unsafe path`）。
- special 题缺 spj 源码被拒（400）；`/admin.html` 静态托管 200。
- 验证完成后 `oj_server` 与 `mysqld` 均已停止。
