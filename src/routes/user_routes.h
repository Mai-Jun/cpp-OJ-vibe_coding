// user_routes.h — 个人中心统计接口（登录）

#ifndef OJ_USER_ROUTES_H
#define OJ_USER_ROUTES_H

#include <mysql.h>

namespace httplib {
class Server;
}

namespace oj {
namespace judge {
class JudgeService;
}

// 注册个人中心相关路由（当前：GET /api/users/me/stats）。
void RegisterUserRoutes(httplib::Server &svr, MYSQL *db, judge::JudgeService *judge);

}  // namespace oj

#endif  // OJ_USER_ROUTES_H
