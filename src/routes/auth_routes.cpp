#include "auth_routes.h"

#include <fstream>
#include <sstream>

#include "db_conn.h"
#include "httplib.h"
#include "json.h"
#include "password.h"
#include "ratelimit.h"
#include "session.h"

namespace oj {

namespace {

// 注册/登录按 IP 限流（Phase 5 防滥用）：同一 IP 窗口内超限返回 429。
// 阈值宽松，正常使用不受影响；后续如需更严可调。
RateLimiter g_register_limit(20, 60);   // 注册：60s 内 20 次
RateLimiter g_login_limit(30, 60);      // 登录：60s 内 30 次

void JsonError(httplib::Response &res, int status, const std::string &msg) {
  res.status = status;
  res.set_content(json::Value(json::Object{{"ok", false}, {"error", msg}}).dump(),
                  "application/json");
}

// 从请求取客户端 IP（REMOTE_ADDR 由 httplib 在 process_request 注入）。
std::string ClientIp(const httplib::Request &req) {
  std::string ip = req.get_header_value("REMOTE_ADDR");
  return ip.empty() ? "unknown" : ip;
}

// 从请求 body 解析 JSON；失败返回 null 并直接响应 400。
json::Value ParseBody(const httplib::Request &req, httplib::Response &res) {
  bool ok = false;
  json::Value v = json::Parse(req.body, &ok);
  if (!ok || !v.is_object()) {
    res.status = 400;
    res.set_content(json::Value(json::Object{{"ok", false}, {"error", "invalid JSON"}}).dump(),
                    "application/json");
  }
  return v;
}

std::string Trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// 读取管理员配置文件：username / password 键值对，支持 # 注释。
bool LoadAdminConf(const std::string &path, std::string &uname, std::string &upass) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(line.substr(0, eq));
    std::string val = Trim(line.substr(eq + 1));
    if (key == "username" && uname.empty()) uname = val;
    else if (key == "password" && upass.empty()) upass = val;
  }
  return !uname.empty() && !upass.empty();
}

void SetCookie(httplib::Response &res, const std::string &token, long max_age_sec, bool http_only) {
  std::string cookie = std::string(kCookieName) + "=" + token
                       + "; Path=/; Max-Age=" + std::to_string(max_age_sec)
                       + "; SameSite=Lax";
  if (http_only) cookie += "; HttpOnly";
  res.set_header("Set-Cookie", cookie);
}

void ClearCookie(httplib::Response &res) {
  std::string cookie = std::string(kCookieName)
                       + "=; Path=/; Max-Age=0; SameSite=Lax; HttpOnly";
  res.set_header("Set-Cookie", cookie);
}

bool ValidUsername(const std::string &u) {
  if (u.size() < 3 || u.size() > 32) return false;
  for (char c : u) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

bool ValidPassword(const std::string &p) { return p.size() >= 6 && p.size() <= 64; }

}  // namespace

void RegisterAuthRoutes(httplib::Server &svr, MYSQL *db, const std::string &admin_conf_path) {
  // ---- 注册（普通用户，公开）----
  svr.Post("/api/auth/register", [db](const httplib::Request &req, httplib::Response &res) {
    DbLock lock;  // 共享连接串行化（Phase 5）
    if (!g_register_limit.Allow(ClientIp(req))) {
      JsonError(res, 429, "注册过于频繁，请稍后再试");
      return;
    }
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;

    std::string username = body.get("username").as_string();
    std::string password = body.get("password").as_string();

    if (!ValidUsername(username)) {
      res.status = 400;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "用户名需为 3-32 位字母/数字/下划线"}}).dump(), "application/json");
      return;
    }
    if (!ValidPassword(password)) {
      res.status = 400;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "密码需为 6-64 位"}}).dump(), "application/json");
      return;
    }

    // 查重
    std::string esc_u = DbEscape(db, username);
    std::string chk = "SELECT id FROM users WHERE username = '" + esc_u + "'";
    if (mysql_query(db, chk.c_str()) != 0) {
      res.status = 500;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "数据库查询失败"}}).dump(), "application/json");
      return;
    }
    bool exists = false;
    if (MYSQL_RES *r = mysql_store_result(db)) {
      exists = mysql_fetch_row(r) != nullptr;
      mysql_free_result(r);
    }
    if (exists) {
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "用户名已存在"}}).dump(), "application/json");
      return;
    }

    std::string hash = HashPassword(password);
    if (hash.empty()) {
      res.status = 500;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "密码处理失败"}}).dump(), "application/json");
      return;
    }
    std::string esc_h = DbEscape(db, hash);
    std::string ins = "INSERT INTO users (username, password_hash, role) VALUES ('" + esc_u + "', '" + esc_h + "', 'user')";
    if (mysql_query(db, ins.c_str()) != 0) {
      // 并发下可能仍撞重名，兜底
      std::string e = mysql_error(db);
      bool dup = e.find("Duplicate") != std::string::npos;
      res.status = dup ? 200 : 500;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", dup ? "用户名已存在" : "注册失败"}}).dump(), "application/json");
      return;
    }
    res.set_content(json::Value(json::Object{{"ok", true}}).dump(), "application/json");
  });

  // ---- 普通用户登录（公开）----
  svr.Post("/api/auth/login", [db](const httplib::Request &req, httplib::Response &res) {
    DbLock lock;  // 共享连接串行化（Phase 5）
    if (!g_login_limit.Allow(ClientIp(req))) {
      JsonError(res, 429, "登录尝试过于频繁，请稍后再试");
      return;
    }
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;
    std::string username = body.get("username").as_string();
    std::string password = body.get("password").as_string();

    std::string esc_u = DbEscape(db, username);
    std::string q = "SELECT id, username, password_hash, role FROM users WHERE username = '" + esc_u + "'";
    if (mysql_query(db, q.c_str()) != 0) {
      res.status = 500;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "数据库查询失败"}}).dump(), "application/json");
      return;
    }
    long long uid = -1;
    std::string db_uname, db_hash, db_role;
    bool found = false;
    if (MYSQL_RES *r = mysql_store_result(db)) {
      if (MYSQL_ROW row = mysql_fetch_row(r)) {
        found = true;
        uid = std::strtoll(row[0], nullptr, 10);
        db_uname = row[1];
        db_hash = row[2];
        db_role = row[3];
      }
      mysql_free_result(r);
    }
    if (!found || !VerifyPassword(password, db_hash)) {
      res.status = 401;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "用户名或密码错误"}}).dump(), "application/json");
      return;
    }
    std::string token = CreateUserSession(db, uid);
    if (token.empty()) {
      res.status = 500;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "创建会话失败"}}).dump(), "application/json");
      return;
    }
    SetCookie(res, token, kUserSessionTtlSec, true);
    json::Object data;
    data["ok"] = json::Value(true);
    data["role"] = json::Value(db_role);
    data["username"] = json::Value(db_uname);
    res.set_content(json::Value(std::move(data)).dump(), "application/json");
  });

  // ---- 管理员登录（入口分离，公开）----
  svr.Post("/api/auth/admin/login",
           [admin_conf_path, db](const httplib::Request &req, httplib::Response &res) {
             (void)db;
             if (!g_login_limit.Allow(ClientIp(req))) {
               JsonError(res, 429, "登录尝试过于频繁，请稍后再试");
               return;
             }
             json::Value body = ParseBody(req, res);
             if (res.status == 400) return;
             std::string cfg_uname, cfg_upass;
             if (!LoadAdminConf(admin_conf_path, cfg_uname, cfg_upass)) {
               res.status = 500;
               res.set_content(json::Value(json::Object{{"ok", false}, {"error", "管理员配置缺失"}}).dump(), "application/json");
               return;
             }
             std::string username = body.get("username").as_string();
             std::string password = body.get("password").as_string();
             if (username != cfg_uname || password != cfg_upass) {
               res.status = 401;
               res.set_content(json::Value(json::Object{{"ok", false}, {"error", "用户名或密码错误"}}).dump(), "application/json");
               return;
             }
             std::string token = CreateAdminSession(cfg_uname);
             if (token.empty()) {
               res.status = 500;
               res.set_content(json::Value(json::Object{{"ok", false}, {"error", "创建会话失败"}}).dump(), "application/json");
               return;
             }
             SetCookie(res, token, kAdminSessionTtlSec, true);
             res.set_content(json::Value(json::Object{{"ok", true}, {"role", "admin"}, {"username", cfg_uname}}).dump(), "application/json");
           });

  // ---- 登出（登录态可调用）----
  svr.Post("/api/auth/logout", [db](const httplib::Request &req, httplib::Response &res) {
    DbLock lock;  // 共享连接串行化（Phase 5）
    std::string token = ExtractToken(req);
    if (!token.empty()) DestroySession(db, token);
    ClearCookie(res);
    res.set_content(json::Value(json::Object{{"ok", true}}).dump(), "application/json");
  });
}

}  // namespace oj