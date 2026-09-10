# Phase 5 — 加固与收尾（安全加固 + 测试 + README/部署脚本）

## Context（背景）

Phase 4 已完成：前端完整（题目列表/详情/编辑器/提交/提交记录/后台）。本任务按 `SPEC.md` §9 Phase 5
收尾：安全加固（§7）、并发/异常/边界测试、README + 部署脚本。

## 关键发现（并发 bug）

集成测试（并发提交 10 个）暴露出**共享 MySQL 连接竞态**：
- cpp-httplib 默认线程池（8+ 线程）并发处理请求，而所有路由复用同一个 `MYSQL*` 连接。
- 多线程同时在连接上 `mysql_query/store_result`，报 `Commands out of sync; you can't run this command now`，
  会话校验随机失败（表现为登录 200 后请求 401 / 数据库查询失败）。
- **修复**：`src/db/db_conn.h/.cpp` 新增进程级 `DbMutex()` + `DbLock`（RAII），所有使用共享连接的
  路由处理器入口加锁，使对同一连接的查询/释放原子化（评测 worker 独立连接不受影响）。
  修复后 10 并发提交全部 Accepted，无丢失、无串扰。

## 安全加固（Phase 5）

1. **seccomp-bpf 白名单**（`src/judge/sandbox.cpp`）：评测子进程装载 BPF 过滤器，
  拦截所有网络系统调用（`socket/connect/bind/accept/sendto/recvmsg/...`）与高危调用
  （`ptrace/mount/pivot_root/chroot/init_module/reboot/kexec_load/bpf/clone3/setns/unshare/keyctl/...`），
  违反即以 `SIGSYS` 终止。judge_service 据此分类为 RE（"调用被禁止的系统调用"）。
  - 非 x86_64 架构自动禁用（syscall 号表依赖架构）。
  - 无需 root、无需降权用户即可生效（`PR_SET_NO_NEW_PRIVS` + `seccomp()`）。
  - 特判器（spj）同样运行在 seccomp 沙箱内。
  - 验证：普通程序正常运行、文件读写放行、`socket()` 触发 SIGSYS。
2. **评测工作目录**：`mkdir 0700 + chmod 0755`，使 root 启动降权 oj-runner 后可读写；
  启动时清理 `/tmp/oj_judge/oj_*` 残留（`PrepareWorkDir`）。
3. **IP 限流**（`src/util/ratelimit.h`）：注册 20 次/60s、登录 30 次/60s，超限 429。
4. **HTTP 安全响应头**（`src/main.cpp`）：`X-Content-Type-Options / X-Frame-Options / Referrer-Policy /
   Content-Security-Policy`（CSP 放行第一方内联脚本，`script-src 'self' 'unsafe-inline'`）。

## 测试（scripts/test.sh，28 项）

覆盖：健康检查、安全响应头、鉴权边界（401/403）、注册/重名/格式/错误密码、注册限流（429）、
题目列表/详情、7 类判题结果（AC/WA/CE/TLE/MLE/RE/SE-404）、沙箱禁网（socket→RE）、
10 并发提交全部 AC、边界（空代码/600KB 超长/非法 JSON）、提交记录。

## 验证结果（2026-09-11，临时 MySQL 起停后均已关闭）

- `-Wall -Wextra` 构建零警告。
- `scripts/test.sh http://127.0.0.1:8081` → **PASS=28 FAIL=0**。
- seccomp 独立单测：正常程序 AC、文件读写放行、`socket()` 被 SIGSYS 拦截。
- 共享 DB 连接竞态修复后：10 并发提交全部 Accepted（修复前随机 401 / Commands out of sync）。
- 验证完成后 `oj_server` 与 `mysqld` 均已停止，临时数据目录已清理。

## 目标产物

| 文件 | 说明 |
|---|---|
| `src/judge/sandbox.h/.cpp` | seccomp-bpf 白名单过滤器 + SIGSYS 归类 |
| `src/db/db_conn.h/.cpp` | 共享连接互斥锁（DbLock） |
| `src/routes/*` | 各路由入口加 DbLock；auth 加 IP 限流 |
| `src/main.cpp` | HTTP 安全响应头 |
| `src/util/ratelimit.h` | 滑动窗口限流 |
| `scripts/test.sh` | 28 项集成测试 |
| `scripts/deploy.sh` / `README.md` | 部署脚本 + 文档 |
| `SPEC.md` | Phase 5 清单勾选、§7 更新 |
