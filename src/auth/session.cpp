#include "session.h"

#include <openssl/rand.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "db_conn.h"
#include "httplib.h"

namespace oj {

namespace {

// ---- Cookie 解析 ----
std::string GetCookieValue(const std::string &cookie_header, const std::string &name) {
  size_t pos = 0;
  while (pos <= cookie_header.size()) {
    size_t end = cookie_header.find(';', pos);
    if (end == std::string::npos) end = cookie_header.size();
    std::string part = cookie_header.substr(pos, end - pos);
    // 去掉首尾空白
    size_t b = part.find_first_not_of(" \t");
    size_t e = part.find_last_not_of(" \t");
    if (b == std::string::npos) b = 0, e = part.size() > 0 ? part.size() - 1 : 0;
    part = part.substr(b, e - b + 1);
    if (part.compare(0, name.size(), name) == 0 && part.size() > name.size() &&
        part[name.size()] == '=') {
      return part.substr(name.size() + 1);
    }
    if (end == cookie_header.size()) break;
    pos = end + 1;
  }
  return {};
}

// ---- 管理员会话（进程内）----
struct AdminSession {
  std::string username;
  std::chrono::steady_clock::time_point expire;
};

std::mutex g_admin_mu;
std::map<std::string, AdminSession> g_admin_sessions;

std::string RndHex32() {
  unsigned char buf[32];
  if (RAND_bytes(buf, sizeof buf) != 1) {
    std::fprintf(stderr, "[session] RAND_bytes failed\n");
    return {};
  }
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (size_t i = 0; i < sizeof buf; ++i) {
    out.push_back(hex[(buf[i] >> 4) & 0xF]);
    out.push_back(hex[buf[i] & 0xF]);
  }
  return out;
}

long long NowUnixSec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::string ExtractToken(const httplib::Request &req) {
  std::string cookie = req.get_header_value("Cookie");
  return GetCookieValue(cookie, kCookieName);
}

std::string GenerateToken() { return RndHex32(); }

std::string CreateAdminSession(const std::string &username) {
  std::string token = RndHex32();
  if (token.empty()) return {};
  std::lock_guard<std::mutex> lk(g_admin_mu);
  g_admin_sessions[token] = AdminSession{
      username,
      std::chrono::steady_clock::now() + std::chrono::seconds(kAdminSessionTtlSec)};
  return token;
}

std::string CreateUserSession(MYSQL *db, long long user_id) {
  std::string token = RndHex32();
  if (token.empty()) return {};
  long long now = NowUnixSec();
  long long exp = now + kUserSessionTtlSec;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld", exp);
  std::string exp_str = buf;
  std::string esc_token = DbEscape(db, token);
  std::string sql =
      "INSERT INTO sessions (id, user_id, created_at, expires_at) VALUES ('" + esc_token +
      "', " + std::to_string(user_id) + ", FROM_UNIXTIME(" + std::to_string(now) + "), FROM_UNIXTIME(" + exp_str + "))";
  if (mysql_query(db, sql.c_str()) != 0) {
    std::fprintf(stderr, "[session] create user session failed: %s\n", mysql_error(db));
    return {};
  }
  return token;
}

SessionInfo GetSession(MYSQL *db, const std::string &token) {
  if (token.empty()) return {};
  // 1) 管理员会话
  {
    std::lock_guard<std::mutex> lk(g_admin_mu);
    auto it = g_admin_sessions.find(token);
    if (it != g_admin_sessions.end()) {
      if (std::chrono::steady_clock::now() < it->second.expire) {
        SessionInfo info;
        info.ok = true;
        info.user_id = 0;
        info.username = it->second.username;
        info.role = "admin";
        return info;
      }
      g_admin_sessions.erase(it);
      return {};
    }
  }

  // 2) 数据库用户会话
  if (db == nullptr) return {};  // 防御：DB 未初始化时仅管理员内存会话可用
  std::string esc = DbEscape(db, token);
  std::string sql =
      "SELECT s.user_id, u.username, u.role FROM sessions s "
      "JOIN users u ON u.id = s.user_id "
      "WHERE s.id = '" + esc + "' AND s.expires_at > NOW()";
  if (mysql_query(db, sql.c_str()) != 0) {
    std::fprintf(stderr, "[session] query failed: %s\n", mysql_error(db));
    return {};
  }
  MYSQL_RES *res = mysql_store_result(db);
  SessionInfo info;
  if (res != nullptr) {
    MYSQL_ROW row;
    if ((row = mysql_fetch_row(res)) != nullptr && row[0] && row[1] && row[2]) {
      info.ok = true;
      info.user_id = std::strtoll(row[0], nullptr, 10);
      info.username = row[1];
      info.role = row[2];
    }
    mysql_free_result(res);
  }
  // 顺带清理过期会话，避免 sessions 表无界增长。
  const char *cleanup = "DELETE FROM sessions WHERE expires_at <= NOW()";
  mysql_query(db, cleanup);
  return info;
}

void DestroySession(MYSQL *db, const std::string &token) {
  if (token.empty()) return;
  {
    std::lock_guard<std::mutex> lk(g_admin_mu);
    g_admin_sessions.erase(token);
  }
  if (db == nullptr) return;  // 防御：同 GetSession
  std::string esc = DbEscape(db, token);
  std::string sql = "DELETE FROM sessions WHERE id = '" + esc + "'";
  if (mysql_query(db, sql.c_str()) != 0) {
    std::fprintf(stderr, "[session] delete failed: %s\n", mysql_error(db));
  }
}

SessionInfo RequireSession(MYSQL *db, const httplib::Request &req) {
  return GetSession(db, ExtractToken(req));
}

bool RequireAdmin(MYSQL *db, const httplib::Request &req, SessionInfo &out) {
  out = RequireSession(db, req);
  return out.ok && out.role == "admin";
}

}  // namespace oj