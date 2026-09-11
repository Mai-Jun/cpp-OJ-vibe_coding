// Online Judge 服务入口：cpp-httplib 服务骨架 + 静态资源托管。
// Phase 1 第一项：仅提供 HTTP 服务外壳，托管 static/ 前端资源，预留后续路由接入点。
//
// 运行约定：
//   1) 在仓库根目录执行，静态目录缺省为相对 cwd 的 "static"；
//   2) 或通过环境变量 OJ_STATIC_DIR 指定绝对路径，可任意目录运行。
//   端口缺省 8080，可用环境变量 OJ_PORT 覆盖。

#include "auth_routes.h"
#include "db_conn.h"
#include "httplib.h"
#include "judge_service.h"
#include "problem_routes.h"
#include "submission_routes.h"
#include "user_routes.h"

#include <sys/stat.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>

static constexpr int kDefaultPort = 8080;
static httplib::Server *g_svr = nullptr;  // 供信号处理访问

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // 端口：优先 OJ_PORT 环境变量，否则默认 8080
  int port = kDefaultPort;
  if (const char *env_port = std::getenv("OJ_PORT")) {
    int parsed = std::atoi(env_port);
    if (parsed > 0 && parsed <= 65535) {
      port = parsed;
    } else {
      std::fprintf(stderr, "invalid OJ_PORT '%s', fallback to %d\n", env_port, port);
    }
  }

  // 静态目录：优先 OJ_STATIC_DIR，否则相对当前工作目录 "static"
  std::string static_dir = "static";
  if (const char *env_dir = std::getenv("OJ_STATIC_DIR")) {
    static_dir = env_dir;
  }

  httplib::Server svr;
  g_svr = &svr;

  // 挂载静态资源到 "/"；失败（目录不存在等）则直接退出
  if (!svr.set_mount_point("/", static_dir)) {
    std::fprintf(stderr, "static dir not found: %s\n", static_dir.c_str());
    return EXIT_FAILURE;
  }

  // 限制请求体大小，防 DoS（后续注册/提交会用到）
  svr.set_payload_max_length(1024 * 1024);

  // HTTP 安全响应头（Phase 5）：防止点击劫持 / MIME 嗅探 / 限制资源来源。
  // CSP 说明：前端为第一方静态页面，含内联 <script>（非用户可控），故放行内联脚本；
  // 其余资源（图片/样式/连接）限制为同源。若未来将内联脚本外置，可收紧为无 'unsafe-inline'。
  svr.set_default_headers({
      {"X-Content-Type-Options", "nosniff"},
      {"X-Frame-Options", "DENY"},
      {"Referrer-Policy", "no-referrer"},
      {"Content-Security-Policy",
       "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'"},
  });

  // 初始化 MySQL：连不上即 fail-fast，避免带病运行
  MYSQL db;
  oj::DbConfig db_cfg = oj::DbConfigFromEnv();
  if (!oj::DbConnect(&db, db_cfg)) {
    std::fprintf(stderr, "[main] cannot connect to MySQL at %s:%u as %s, giving up\n",
                 db_cfg.host.c_str(), db_cfg.port, db_cfg.user.c_str());
    return EXIT_FAILURE;
  }
  std::printf("[main] MySQL connected: %s@%s/%s\n", db_cfg.user.c_str(),
              db_cfg.host.c_str(), db_cfg.database.c_str());

  // 管理员配置文件：优先 /etc/oj/admin.conf，否则回退仓库内 config/admin.conf
  std::string admin_conf = "/etc/oj/admin.conf";
  {
    struct stat st{};
    if (stat(admin_conf.c_str(), &st) != 0) admin_conf = "config/admin.conf";
  }

  oj::RegisterAuthRoutes(svr, &db, admin_conf);
  oj::RegisterProblemRoutes(svr, &db);

  // 评测服务：后台单 worker 串行评测（其内部使用独立 DB 连接）。
  oj::judge::JudgeService judge(&db);
  oj::RegisterSubmissionRoutes(svr, &db, &judge);
  oj::RegisterUserRoutes(svr, &db, &judge);

  // 健康检查
  svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
    res.set_content("{\"status\":\"ok\"}", "application/json");
  });

  // 请求日志
  svr.set_logger([](const httplib::Request &req, const httplib::Response &res) {
    std::printf("[%s] %s -> %d\n", req.method.c_str(), req.path.c_str(), res.status);
  });

  std::printf("OJ server listening on 0.0.0.0:%d (static dir: %s)\n", port, static_dir.c_str());

  // SIGINT 优雅退出
  std::signal(SIGINT, [](int) {
    if (g_svr) {
      g_svr->stop();
    }
  });

  bool ok = svr.listen("0.0.0.0", port);
  std::printf("server exited, %s\n", ok ? "clean" : std::strerror(errno));
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}