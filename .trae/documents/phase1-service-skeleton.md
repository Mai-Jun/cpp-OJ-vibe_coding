# Phase 1 — cpp-httplib 服务骨架 + 静态资源托管

## Context（背景）

当前 C++ 在线判题系统（OJ）项目只有 `SPEC.md`、`DEPENDENCIES.md` 和 `third_party/httplib.h`（cpp-httplib 0.18.0 单头文件），没有任何源码与构建配置。系统依赖已全部装齐（g++ 11.4、cmake 3.16、MySQL、oj-runner、/etc/oj）。

本任务按 `SPEC.md` §9 Phase 1 的第一项清单「cpp-httplib 服务骨架 + 静态资源托管」落地：搭建后端服务外壳，用 `set_mount_point` 托管前端静态资源，并预留后续路由接入点。**不做** MySQL 连接、鉴权、注册/登录 API、页面（属后续子项），避免过度设计。

关键环境确认（探索子代理已核对 httplib 0.18 头文件）：
- `httplib 0.18.0`，单一 `class httplib::Server`，`set_mount_point`、`set_payload_max_length`、`listen`、`bind_to_port`、`set_logger`、`stop` 均可用。
- httplib 0.18 的 `routing()` 中 `handle_file_request` 优先于显式路由；挂载点命中 `index.html` 时 `/` 无需额外注册首页路由。`/health` 静态文件命中前提是 `static/` 下不出现同名文件。
- 本头文件默认不启用 `CPPHTTPLIB_ZLIB_SUPPORT`，冲洗不连 zlib；需链接 `Threads::Threads`。

## 目标产物（新建文件）

| 文件 | 说明 |
|---|---|
| `CMakeLists.txt` | C++17，可执行目标 `oj_server` |
| `src/main.cpp` | httplib Server + 挂载 static + `/health` 健康检查 + 请求日志 + SIGINT 优雅退出 |
| `static/index.html` | OJ 主题极简入口页，引用 css/js 占位 |
| `static/css/style.css` | 极简占位全局样式（少量样式） |
| `static/js/api.js` | 占位 fetch 封装（仅注释说明后续 Phase 提供） |

不创建 `config/admin.conf`、`scripts/`、`src/routes|db|auth|judge|util`，留待后续 Phase。

## CMakeLists.txt 要点（cmake 3.16 兼容）

```cmake
cmake_minimum_required(VERSION 3.16)
project(oj_server CXX)
add_compile_options(-Wall -Wextra)
add_executable(oj_server src/main.cpp)
target_include_directories(oj_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_features(oj_server PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(oj_server PRIVATE Threads::Threads)
```

## src/main.cpp 结构要点

- 端口：优先 `OJ_PORT` 环境变量，默认 8080（用 `std::getenv` 解析）。
- 静态目录：优先 `OJ_STATIC_DIR`，默认相对 cwd 的 `"static"`。
- `httplib::Server svr;` `g_svr = &svr;`
- `svr.set_mount_point("/", static_dir)`，失败打印并 `return EXIT_FAILURE`。
- `svr.Get("/health", ...)` 返回 `{"status":"ok"}`，content-type `application/json`。
- `svr.set_logger(...)` 打印 `[METHOD] path -> status`。
- `svr.set_payload_max_length(1024 * 1024)`（防 DoS，预留）。
- `std::signal(SIGINT, [](int){ if (g_svr) g_svr->stop(); });`
- `svr.listen("0.0.0.0", port)` 阻塞，按返回值输出 clean/错误。
- 顶部注释写明运行约定：在仓库根目录执行，或设置 `OJ_STATIC_DIR` 绝对路径。

## static/index.html 要点

- `<!DOCTYPE html>`，`lang="zh"`，标题 `OnlineJudge`。
- `<link rel="stylesheet" href="/css/style.css">`、`<script src="/js/api.js"></script>`。
- 正文说明「服务骨架 + 静态资源托管已就绪」，后续功能待加入。

## 验证方式

```bash
cd /home/mai/cpp-OJ-vibe_coding
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

OJ_PORT=8080 ./build/oj_server &

curl -s http://localhost:8080/ | head -n 5           # index.html 内容
curl -i http://localhost:8080/css/style.css          # 200 + text/css
curl -s http://localhost:8080/health                 # {"status":"ok"}
curl -i http://localhost:8080/nonexistent.html       # 404
kill -INT %1                                          # 打印 server exited, clean
```