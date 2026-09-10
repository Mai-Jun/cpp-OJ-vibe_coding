#include "db_conn.h"

#include <cstdio>
#include <cstdlib>

namespace oj {

DbConfig DbConfigFromEnv() {
  DbConfig cfg;
  if (const char *v = std::getenv("OJ_DB_HOST")) cfg.host = v;
  if (const char *v = std::getenv("OJ_DB_PORT")) cfg.port = static_cast<unsigned>(std::atoi(v));
  if (const char *v = std::getenv("OJ_DB_USER")) cfg.user = v;
  if (const char *v = std::getenv("OJ_DB_PASS")) cfg.password = v;
  if (const char *v = std::getenv("OJ_DB_NAME")) cfg.database = v;
  return cfg;
}

bool DbConnect(MYSQL *conn, const DbConfig &cfg) {
  if (mysql_init(conn) == nullptr) {
    std::fprintf(stderr, "[db] mysql_init failed\n");
    return false;
  }
  // 显式走 UTF-8，避免中文/输入内容误判字符集。
  if (mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
    std::fprintf(stderr, "[db] set charset utf8mb4 failed\n");
  }
  if (mysql_real_connect(conn, cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(),
                         cfg.database.c_str(), cfg.port, nullptr, 0) == nullptr) {
    std::fprintf(stderr, "[db] connect failed: %s\n", mysql_error(conn));
    mysql_close(conn);
    return false;
  }
  mysql_set_character_set(conn, "utf8mb4");
  return true;
}

std::string DbEscape(MYSQL *conn, const std::string &raw) {
  std::string out;
  out.resize(raw.size() * 2 + 1);
  unsigned long len = mysql_real_escape_string(
      conn, &out[0], raw.data(), static_cast<unsigned long>(raw.size()));
  out.resize(len);
  return out;
}

}  // namespace oj