// submission_routes.h — 提交 / 轮询结果 / 我的提交记录（Phase 3）。

#ifndef OJ_SUBMISSION_ROUTES_H
#define OJ_SUBMISSION_ROUTES_H

#include <mysql.h>

namespace httplib {
class Server;
}

namespace oj {
namespace judge {
class JudgeService;
}

// judge: 由主程序持有的评测服务单例（生命周期长于路由）。
void RegisterSubmissionRoutes(httplib::Server &svr, MYSQL *db,
                              judge::JudgeService *judge);

}  // namespace oj

#endif  // OJ_SUBMISSION_ROUTES_H
