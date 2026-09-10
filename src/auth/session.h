// session.h — 会话与鉴权中间件（Phase 1）
// 用户会话存 MySQL `sessions` 表；管理员会话（配置账号、无 DB 用户）存进程内 map。
// Cookie 名：oj_session。token 为 64 位十六进制随机串。

#ifndef OJ_SESSION_H
#define OJ_SESSION_H

#include <mysql.h>

#include <string>

namespace httplib {
struct Request;
}

namespace oj {

inline constexpr const char *kCookieName = "oj_session";

// cookie 存放期限（秒）
inline constexpr int kUserSessionTtlSec = 7 * 24 * 3600;
// 管理员会话过期（秒），比普通用户短，降低泄露风险
inline constexpr int kAdminSessionTtlSec = 8 * 3600;

struct SessionInfo {
  bool ok = false;          // 是否有效
  long long user_id = 0;    // 普通用户 id（管理员为 0）
  std::string username;
  std::string role;         // "user" / "admin"
};

// 由 Cookie 头解析出 token；无则返回空串。
std::string ExtractToken(const httplib::Request &req);

// 生成 64 字符十六进制随机 token；失败返回空串。
std::string GenerateToken();

// 为用户创建会话（写入 sessions 表）；返回 token，失败返回空串。
std::string CreateUserSession(MYSQL *db, long long user_id);

// 创建管理员会话（进程内）；成功返回 token。
std::string CreateAdminSession(const std::string &username);

// 校验 token：先查管理员 map，再查 DB 用户表（含过期判断与表清理）。
// 返回 isOk=false 表示无效/过期。
SessionInfo GetSession(MYSQL *db, const std::string &token);

// 销毁会话（用户删 DB 行，管理员删 map 项）。
void DestroySession(MYSQL *db, const std::string &token);

// 便捷：从请求 Cookie 校验得到会话信息；失败时 ok=false。
SessionInfo RequireSession(MYSQL *db, const httplib::Request &req);

// 便捷：要求管理员身份。
bool RequireAdmin(MYSQL *db, const httplib::Request &req, SessionInfo &out);

}  // namespace oj

#endif  // OJ_SESSION_H