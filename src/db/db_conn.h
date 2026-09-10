// db_conn.h — MySQL C API 连接层（Phase 1）
// 连接信息通过环境变量配置，缺省值与 DEPENDENCIES.md 建议保持一致：
//   OJ_DB_HOST  (默认 localhost)
//   OJ_DB_PORT  (默认 3306)
//   OJ_DB_USER  (默认 oj)
//   OJ_DB_PASS  (默认空)
//   OJ_DB_NAME  (默认 oj)
// 提供：连接管理、SQL 转义、查询执行辅助。客户端需在响应返回前读取结果。

#ifndef OJ_DB_CONN_H
#define OJ_DB_CONN_H

#include <mysql.h>

#include <string>

namespace oj {

struct DbConfig {
  std::string host{"localhost"};
  unsigned    port{3306};
  std::string user{"oj"};
  std::string password;
  std::string database{"oj"};
};

// 从环境变量读取 DbConfig；未设置时使用缺省值。
DbConfig DbConfigFromEnv();

// 对 MYSQL 对象进行一次连接；失败返回非 0 并打印诊断。
bool DbConnect(MYSQL *conn, const DbConfig &cfg);

// 转义字符串内容（不含引号），调用方负责包裹引号；失败返回空串。
std::string DbEscape(MYSQL *conn, const std::string &raw);

}  // namespace oj

#endif  // OJ_DB_CONN_H