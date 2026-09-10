// auth_routes.h — 注册 / 登录（普通与管理员分离）/ 登出（Phase 1）

#ifndef OJ_AUTH_ROUTES_H
#define OJ_AUTH_ROUTES_H

#include <mysql.h>

#include <string>

namespace httplib {
class Server;
}

namespace oj {

// admin_conf_path: 管理员账号配置文件路径（/etc/oj/admin.conf 或仓库 config/admin.conf）
void RegisterAuthRoutes(httplib::Server &svr, MYSQL *db, const std::string &admin_conf_path);

}  // namespace oj

#endif  // OJ_AUTH_ROUTES_H