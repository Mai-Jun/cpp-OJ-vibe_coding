// problem_routes.h — 题目 CRUD（管理员）+ 列表/详情（登录）+ 测试用例管理 + zip 导入（Phase 2）

#ifndef OJ_PROBLEM_ROUTES_H
#define OJ_PROBLEM_ROUTES_H

#include <mysql.h>

#include <string>

namespace httplib {
class Server;
}

namespace oj {

// 注册题目相关路由：列表/详情（登录）、增删改（管理员）、测试用例管理（管理员）、zip 导入（管理员）。
void RegisterProblemRoutes(httplib::Server &svr, MYSQL *db);

}  // namespace oj

#endif  // OJ_PROBLEM_ROUTES_H
