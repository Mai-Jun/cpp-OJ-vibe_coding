// submission_routes.cpp — 提交 / 轮询结果 / 我的提交记录（Phase 3）。
// 提交为异步：POST 立即返回 submission_id，GET /api/submissions/{id} 轮询最终结果。

#include "submission_routes.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "db_conn.h"
#include "httplib.h"
#include "judge_service.h"
#include "json.h"
#include "session.h"

namespace oj {

namespace {

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

void JsonError(httplib::Response &res, int status, const std::string &msg) {
  res.status = status;
  res.set_content(json::Value(json::Object{{"ok", false}, {"error", msg}}).dump(),
                  "application/json");
}

// 把单条 CaseResult 转成 JSON。
json::Value CaseToJson(const judge::CaseResult &c) {
  json::Object o;
  o["order_no"] = json::Value(c.order_no);
  o["status"] = json::Value(c.status);
  o["detail"] = json::Value(c.detail);
  o["time_ms"] = json::Value(c.time_ms);
  o["user_output"] = json::Value(c.user_output);
  o["expected_output"] = json::Value(c.expected_output);
  return json::Value(std::move(o));
}

// 把完整结果转成 JSON。
json::Value ResultToJson(const judge::SubmissionResult &s) {
  json::Object o;
  o["id"] = json::Value(s.id);
  o["user_id"] = json::Value(s.user_id);
  o["problem_id"] = json::Value(s.problem_id);
  o["problem_title"] = json::Value(s.problem_title);
  o["status"] = json::Value(s.status);
  o["detail"] = json::Value(s.detail);
  o["time_ms"] = json::Value(s.time_ms);
  o["memory_mb"] = json::Value(s.memory_mb);
  o["compile_error"] = json::Value(s.compile_error);
  o["created_at"] = json::Value(s.created_at);
  o["done"] = json::Value(s.done);
  std::vector<json::Value> cases;
  for (const auto &c : s.cases) cases.push_back(CaseToJson(c));
  o["cases"] = json::Value(std::move(cases));
  return json::Value(std::move(o));
}

}  // namespace

void RegisterSubmissionRoutes(httplib::Server &svr, MYSQL *db,
                              judge::JudgeService *judge) {
  // ---- 提交代码（登录；异步返回 submission_id）----
  svr.Post("/api/submissions", [db, judge](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      JsonError(res, 401, "请先登录");
      return;
    }
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;

    long long problem_id = static_cast<long long>(body.get("problem_id").as_number());
    std::string code = body.get("code").as_string();
    if (problem_id <= 0) {
      JsonError(res, 400, "缺少有效的 problem_id");
      return;
    }
    if (code.empty()) {
      JsonError(res, 400, "代码不能为空");
      return;
    }
    if (code.size() > 512 * 1024) {
      JsonError(res, 400, "代码过长（上限 512KB）");
      return;
    }

    // 校验题目存在（顺便避免对不存在题目的垃圾提交）。
    std::string chk = "SELECT id FROM problems WHERE id = " + std::to_string(problem_id);
    if (mysql_query(db, chk.c_str()) != 0) {
      JsonError(res, 500, "数据库查询失败");
      return;
    }
    MYSQL_RES *r = mysql_store_result(db);
    bool exists = r && mysql_fetch_row(r);
    if (r) mysql_free_result(r);
    if (!exists) {
      JsonError(res, 404, "题目不存在");
      return;
    }

    long long sid = judge->Submit(sess.user_id, problem_id, code);
    if (sid <= 0) {
      JsonError(res, 400, "提交失败");
      return;
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["submission_id"] = json::Value(sid);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 轮询结果（登录；仅本人可见）----
  svr.Get(R"(/api/submissions/(\d+))", [db, judge](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      JsonError(res, 401, "请先登录");
      return;
    }
    long long sid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);
    auto sub = judge->Get(sid);
    if (!sub) {
      JsonError(res, 404, "提交不存在");
      return;
    }
    if (sub->user_id != sess.user_id) {
      JsonError(res, 403, "无权查看他人提交");
      return;
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["submission"] = ResultToJson(*sub);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 我的提交记录（登录；内存态，按 id 倒序）----
  svr.Get("/api/submissions", [db, judge](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      JsonError(res, 401, "请先登录");
      return;
    }
    int limit = 50;
    if (req.has_param("limit")) {
      int v = std::atoi(req.get_param_value("limit").c_str());
      if (v > 0 && v <= 200) limit = v;
    }
    auto list = judge->ListByUser(sess.user_id, limit);
    std::vector<json::Value> items;
    for (const auto &s : list) {
      json::Object o;
      o["id"] = json::Value(s->id);
      o["problem_id"] = json::Value(s->problem_id);
      o["problem_title"] = json::Value(s->problem_title);
      o["status"] = json::Value(s->status);
      o["detail"] = json::Value(s->detail);
      o["time_ms"] = json::Value(s->time_ms);
      o["memory_mb"] = json::Value(s->memory_mb);
      o["created_at"] = json::Value(s->created_at);
      o["done"] = json::Value(s->done);
      items.emplace_back(std::move(o));
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["submissions"] = json::Value(std::move(items));
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });
}

}  // namespace oj
