// user_routes.cpp — 个人中心统计接口（登录）。
// GET /api/users/me/stats：返回当前用户的总提交数 / AC 数 / 通过率 /
// 已解决题目集合与题库总题数（题库完成进度由前端换算展示）。
// 统计来源为评测服务内存态提交记录（与提交记录页一致）。

#include "user_routes.h"

#include <string>
#include <vector>

#include "db_conn.h"
#include "httplib.h"
#include "judge_service.h"
#include "json.h"
#include "session.h"

namespace oj {

void RegisterUserRoutes(httplib::Server &svr, MYSQL *db, judge::JudgeService *judge) {
  svr.Get("/api/users/me/stats", [db, judge](const httplib::Request &req, httplib::Response &res) {
    DbLock lock;  // 共享连接串行化（Phase 5）
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      res.status = 401;
      res.set_content(json::Value(json::Object{{"ok", false}, {"error", "请先登录"}}).dump(),
                      "application/json");
      return;
    }

    // 题库总题数（管理员在 DB 中增删题目，题库以 DB 为准）。
    long long total_problems = 0;
    if (mysql_query(db, "SELECT COUNT(*) FROM problems") == 0) {
      if (MYSQL_RES *r = mysql_store_result(db)) {
        if (MYSQL_ROW row = mysql_fetch_row(r)) {
          total_problems = std::strtoll(row[0], nullptr, 10);
        }
        mysql_free_result(r);
      }
    }

    judge::UserStats st = judge->GetUserStats(sess.user_id);
    std::vector<json::Value> solved;
    for (long long pid : st.solved_problem_ids) solved.emplace_back(pid);

    double ac_rate = st.total > 0
        ? static_cast<double>(st.accepted) / static_cast<double>(st.total) * 100.0
        : 0.0;

    json::Object stats;
    stats["username"] = json::Value(sess.username);
    stats["role"] = json::Value(sess.role);
    stats["total_submissions"] = json::Value(st.total);
    stats["accepted_submissions"] = json::Value(st.accepted);
    stats["ac_rate"] = json::Value(ac_rate);  // 百分数（0-100）
    stats["solved"] = json::Value(static_cast<long long>(st.solved_problem_ids.size()));
    stats["total_problems"] = json::Value(total_problems);
    stats["solved_problem_ids"] = json::Value(std::move(solved));

    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["stats"] = json::Value(std::move(stats));
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });
}

}  // namespace oj
