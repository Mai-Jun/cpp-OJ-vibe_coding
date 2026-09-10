// problem_routes.cpp — 题目 CRUD（管理员）+ 列表/详情（登录）+ 测试用例管理 + zip 导入（Phase 2）
// 用例内容以文本入库（单条 ≤ 64KB）；zip 导入格式：case_1.in / case_1.out 配对，或 inputs/、outputs/ 目录。

#include "problem_routes.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "db_conn.h"
#include "httplib.h"
#include "json.h"
#include "session.h"
#include "zip.h"

namespace oj {

namespace {

constexpr long long kMaxCaseBytes = 64 * 1024;          // 单条用例内容上限 64KB
constexpr int kDefaultTimeLimitMs = 500;
constexpr int kDefaultMemoryLimitMb = 256;
constexpr int kMaxCases = 256;                          // 单题用例数上限，防滥用
constexpr int kMaxZipBytes = 8 * 1024 * 1024;           // zip 上传体积上限 8MB

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

std::string Trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool ValidDifficulty(const std::string &d) {
  return d == "easy" || d == "medium" || d == "hard";
}

bool ValidJudgeType(const std::string &t) { return t == "exact" || t == "special"; }

std::string RowStr(MYSQL_ROW row, int idx) { return row[idx] ? row[idx] : ""; }

// 按 order_no 排序读取某题的全部用例。
bool LoadCases(MYSQL *db, long long problem_id, std::vector<json::Object> &out) {
  std::string q = "SELECT id, order_no, input, expected_output FROM test_cases "
                  "WHERE problem_id = " + std::to_string(problem_id) + " ORDER BY order_no";
  if (mysql_query(db, q.c_str()) != 0) return false;
  MYSQL_RES *res = mysql_store_result(db);
  if (!res) return false;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    json::Object o;
    o["id"] = json::Value(std::strtoll(row[0], nullptr, 10));
    o["order_no"] = json::Value(std::strtoll(row[1], nullptr, 10));
    o["input"] = json::Value(RowStr(row, 2));
    o["expected_output"] = json::Value(row[3] ? row[3] : "");
    out.push_back(std::move(o));
  }
  mysql_free_result(res);
  return true;
}

// 清空某题全部用例（不删除题目）。
void DeleteCases(MYSQL *db, long long problem_id) {
  std::string q = "DELETE FROM test_cases WHERE problem_id = " + std::to_string(problem_id);
  if (mysql_query(db, q.c_str()) != 0) {
    std::fprintf(stderr, "[problem] delete cases failed: %s\n", mysql_error(db));
  }
}

// 校验单个用例输入/输出均 ≤ 64KB。
bool ValidCaseContent(const std::string &in, const std::string &out, bool exact) {
  if (in.size() > kMaxCaseBytes) return false;
  if (exact && out.size() > kMaxCaseBytes) return false;
  return true;
}

// 顺序插入一组用例（order 从 first_order 起）。成功返回 true。
bool InsertCases(MYSQL *db, long long problem_id, int first_order,
                 const std::vector<std::pair<std::string, std::string>> &cases) {
  std::string sql = "INSERT INTO test_cases (problem_id, order_no, input, expected_output) VALUES ";
  bool first = true;
  for (size_t i = 0; i < cases.size(); ++i) {
    if (!first) sql += ",";
    first = false;
    int order = first_order + static_cast<int>(i);
    std::string ei = DbEscape(db, cases[i].first);
    std::string eo = DbEscape(db, cases[i].second);
    sql += "(" + std::to_string(problem_id) + "," + std::to_string(order) + ",'" + ei + "','" + eo + "')";
  }
  sql += ";";
  if (mysql_query(db, sql.c_str()) != 0) {
    std::fprintf(stderr, "[problem] insert cases failed: %s\n", mysql_error(db));
    return false;
  }
  return true;
}

// 将 zip 条目配对为 (in, out) 用例列表。
// 兼容两种约定：case_N.in/.out 同级；或 inputs/ 与 outputs/ 两个目录。
// 返回 true 且 cases 非空表示成功；否则 err 描述原因。
bool ParseZipCases(const std::vector<zip::Entry> &entries,
                   std::vector<std::pair<std::string, std::string>> &cases,
                   std::string &err) {
  cases.clear();

  // 提取 basename 与扩展名
  auto base = [](const std::string &name) -> std::string {
    auto pos = name.rfind('/');
    return pos == std::string::npos ? name : name.substr(pos + 1);
  };
  auto dir = [](const std::string &name) -> std::string {
    auto pos = name.rfind('/');
    if (pos == std::string::npos) return "";
    std::string d = name.substr(0, pos);
    // 小写归一化比较用（in/out 目录名大小写不敏感）
    std::string low = d;
    for (auto &c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low;
  };
  auto ext = [](const std::string &name) -> std::string {
    auto pos = name.rfind('.');
    if (pos == std::string::npos) return "";
    std::string e = name.substr(pos + 1);
    for (auto &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
  };
  auto stem = [](const std::string &name) -> std::string {
    auto p = name.rfind('.');
    if (p == std::string::npos) return name;
    auto q = name.rfind('/');
    std::string b = q == std::string::npos ? name : name.substr(q + 1);
    if (b.rfind('.') == std::string::npos) return b;
    return b.substr(0, b.rfind('.'));
  };
  auto to_text = [](const std::vector<uint8_t> &data) -> std::string {
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
  };

  // 方案 A：inputs/ + outputs/ 目录
  std::map<std::string, const zip::Entry *> in_map;
  std::map<std::string, const zip::Entry *> out_map;
  bool has_in_dir = false, has_out_dir = false;
  for (const auto &e : entries) {
    std::string d = dir(e.name);
    if (d == "inputs" || d == "in") {
      in_map[stem(e.name)] = &e;
      has_in_dir = true;
    } else if (d == "outputs" || d == "out" || d == "answers") {
      out_map[stem(e.name)] = &e;
      has_out_dir = true;
    }
  }
  if (has_in_dir && has_out_dir) {
    for (const auto &kv : in_map) {
      auto it = out_map.find(kv.first);
      if (it == out_map.end()) continue;
      cases.emplace_back(to_text(kv.second->data), to_text(it->second->data));
    }
    if (cases.empty()) {
      err = "zip 中 inputs/ 与 outputs/ 没有匹配的用例对";
      return false;
    }
    return true;
  }

  // 方案 B：case_N.in / case_N.out 同级（任意目录层级）
  std::map<std::string, const zip::Entry *> in2;
  std::map<std::string, const zip::Entry *> out2;
  for (const auto &e : entries) {
    std::string x = ext(e.name);
    if (x == "in") in2[base(e.name)] = &e;
    else if (x == "out" || x == "ans") out2[base(e.name)] = &e;
  }
  for (const auto &kv : in2) {
    std::string key = kv.first;
    // 期望输出名：case_N.out（或 case_N.ans 归一为 .out）
    std::string out_key = key;
    if (out_key.size() > 3 && out_key.compare(out_key.size() - 3, 3, ".in") == 0) {
      out_key = out_key.substr(0, out_key.size() - 3) + ".out";
    }
    auto it = out2.find(out_key);
    if (it == out2.end()) {
      // 尝试 .ans 后缀
      it = out2.find(key.substr(0, key.size() - 3) + ".ans");
    }
    if (it == out2.end()) continue;
    cases.emplace_back(to_text(kv.second->data), to_text(it->second->data));
  }
  if (cases.empty()) {
    err = "zip 中未找到 case_N.in / case_N.out 配对（或 inputs/ outputs/ 目录）";
    return false;
  }
  return true;
}

}  // namespace

void RegisterProblemRoutes(httplib::Server &svr, MYSQL *db) {
  // ---- 题目列表（登录）----
  // 可选 query 过滤：?difficulty=xxx &tag=xxx
  svr.Get("/api/problems", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      JsonError(res, 401, "请先登录");
      return;
    }
    std::string q = "SELECT id, title, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type FROM problems";
    std::string where;
    if (req.has_param("difficulty")) {
      std::string d = req.get_param_value("difficulty");
      if (ValidDifficulty(d)) where = " WHERE difficulty = '" + d + "'";
    }
    std::string tag;
    if (req.has_param("tag")) {
      tag = Trim(req.get_param_value("tag"));
      if (!tag.empty()) {
        std::string esc = DbEscape(db, tag);
        std::string clause = " (tags = '" + esc + "' OR tags LIKE '" + esc +
                             ",%' OR tags LIKE '%," + esc + ",%' OR tags LIKE '%," + esc + "')";
        where += (where.empty() ? " WHERE" : " AND") + clause;
      }
    }
    q += where + " ORDER BY id ASC";
    if (mysql_query(db, q.c_str()) != 0) {
      JsonError(res, 500, "数据库查询失败");
      return;
    }
    MYSQL_RES *r = mysql_store_result(db);
    std::vector<json::Value> items;
    if (r) {
      MYSQL_ROW row;
      while ((row = mysql_fetch_row(r))) {
        json::Object o;
        o["id"] = json::Value(std::strtoll(row[0], nullptr, 10));
        o["title"] = json::Value(RowStr(row, 1));
        o["difficulty"] = json::Value(RowStr(row, 2));
        o["tags"] = json::Value(RowStr(row, 3));
        o["time_limit_ms"] = json::Value(std::strtoll(row[4], nullptr, 10));
        o["memory_limit_mb"] = json::Value(std::strtoll(row[5], nullptr, 10));
        o["judge_type"] = json::Value(RowStr(row, 6));
        items.emplace_back(std::move(o));
      }
      mysql_free_result(r);
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["problems"] = json::Value(std::move(items));
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 题目详情（登录）：含描述 + 用例列表 ----
  svr.Get(R"(/api/problems/(\d+))", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess = RequireSession(db, req);
    if (!sess.ok) {
      JsonError(res, 401, "请先登录");
      return;
    }
    long long pid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);
    std::string q = "SELECT id, title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type, spj_source FROM problems WHERE id = " + std::to_string(pid);
    if (mysql_query(db, q.c_str()) != 0) {
      JsonError(res, 500, "数据库查询失败");
      return;
    }
    MYSQL_RES *r = mysql_store_result(db);
    json::Object prob;
    bool found = false;
    if (r) {
      if (MYSQL_ROW row = mysql_fetch_row(r)) {
        found = true;
        prob["id"] = json::Value(std::strtoll(row[0], nullptr, 10));
        prob["title"] = json::Value(RowStr(row, 1));
        prob["description"] = json::Value(RowStr(row, 2));
        prob["difficulty"] = json::Value(RowStr(row, 3));
        prob["tags"] = json::Value(RowStr(row, 4));
        prob["time_limit_ms"] = json::Value(std::strtoll(row[5], nullptr, 10));
        prob["memory_limit_mb"] = json::Value(std::strtoll(row[6], nullptr, 10));
        prob["judge_type"] = json::Value(RowStr(row, 7));
        prob["spj_source"] = json::Value(row[8] ? row[8] : "");
      }
      mysql_free_result(r);
    }
    if (!found) {
      JsonError(res, 404, "题目不存在");
      return;
    }
    std::vector<json::Object> cases;
    if (!LoadCases(db, pid, cases)) {
      JsonError(res, 500, "读取测试用例失败");
      return;
    }
    std::vector<json::Value> case_items;
    for (auto &c : cases) case_items.emplace_back(std::move(c));
    prob["test_cases"] = json::Value(std::move(case_items));
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["problem"] = json::Value(std::move(prob));
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 新增题目（管理员）----
  svr.Post("/api/problems", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess;
    if (!RequireAdmin(db, req, sess)) {
      JsonError(res, 403, "需要管理员权限");
      return;
    }
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;

    std::string title = Trim(body.get("title").as_string());
    std::string description = body.get("description").as_string();
    std::string difficulty = body.get("difficulty").as_string();
    std::string tags = Trim(body.get("tags").as_string());
    long long tlimit = static_cast<long long>(body.get("time_limit_ms").as_number());
    long long mlimit = static_cast<long long>(body.get("memory_limit_mb").as_number());
    std::string judge_type = body.get("judge_type").as_string();
    std::string spj = body.get("spj_source").as_string();

    if (title.empty() || title.size() > 128) {
      JsonError(res, 400, "标题不能为空且不超过 128 字符");
      return;
    }
    if (description.empty()) {
      JsonError(res, 400, "题目描述不能为空");
      return;
    }
    if (!ValidDifficulty(difficulty)) difficulty = "easy";
    if (!ValidJudgeType(judge_type)) judge_type = "exact";
    if (tlimit <= 0) tlimit = kDefaultTimeLimitMs;
    if (mlimit <= 0) mlimit = kDefaultMemoryLimitMb;
    if (judge_type == "special" && spj.empty()) {
      JsonError(res, 400, "特判题必须提供 spj 源码");
      return;
    }
    if (judge_type == "exact") spj.clear();

    std::string e_title = DbEscape(db, title);
    std::string e_desc = DbEscape(db, description);
    std::string e_tags = DbEscape(db, tags);
    std::string e_spj = DbEscape(db, spj);
    std::string sql = "INSERT INTO problems (title, description, difficulty, tags, time_limit_ms, memory_limit_mb, judge_type, spj_source) VALUES ('" +
                      e_title + "', '" + e_desc + "', '" + difficulty + "', '" + e_tags + "', " +
                      std::to_string(tlimit) + ", " + std::to_string(mlimit) + ", '" + judge_type + "', '" +
                      e_spj + "')";
    if (mysql_query(db, sql.c_str()) != 0) {
      std::fprintf(stderr, "[problem] insert problem failed: %s\n", mysql_error(db));
      JsonError(res, 500, "新增题目失败");
      return;
    }
    long long new_id = static_cast<long long>(mysql_insert_id(db));

    // 附带测试用例（JSON 数组）
    std::vector<std::pair<std::string, std::string>> cases;
    const json::Value &arr = body.get("test_cases");
    if (arr.is_array() && !arr.as_array().empty()) {
      if (arr.as_array().size() > static_cast<size_t>(kMaxCases)) {
        JsonError(res, 400, "测试用例数量超限");
        return;
      }
      for (const auto &c : arr.as_array()) {
        std::string in = c.get("input").as_string();
        std::string out = c.get("expected_output").as_string();
        if (!ValidCaseContent(in, out, judge_type == "exact")) {
          JsonError(res, 400, "测试用例内容超限（单条 ≤ 64KB）");
          return;
        }
        cases.emplace_back(in, out);
      }
    }
    if (!cases.empty() && !InsertCases(db, new_id, 1, cases)) {
      // 用例写入失败则回滚题目
      std::string rollback = "DELETE FROM problems WHERE id = " + std::to_string(new_id);
      mysql_query(db, rollback.c_str());
      JsonError(res, 500, "写入测试用例失败");
      return;
    }

    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["id"] = json::Value(new_id);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 修改题目（管理员）：覆盖题目字段，并可选整体替换用例 ----
  svr.Put(R"(/api/problems/(\d+))", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess;
    if (!RequireAdmin(db, req, sess)) {
      JsonError(res, 403, "需要管理员权限");
      return;
    }
    long long pid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;

    std::string title = Trim(body.get("title").as_string());
    std::string description = body.get("description").as_string();
    std::string difficulty = body.get("difficulty").as_string();
    std::string tags = Trim(body.get("tags").as_string());
    long long tlimit = static_cast<long long>(body.get("time_limit_ms").as_number());
    long long mlimit = static_cast<long long>(body.get("memory_limit_mb").as_number());
    std::string judge_type = body.get("judge_type").as_string();
    std::string spj = body.get("spj_source").as_string();

    if (title.empty() || title.size() > 128) {
      JsonError(res, 400, "标题不能为空且不超过 128 字符");
      return;
    }
    if (description.empty()) {
      JsonError(res, 400, "题目描述不能为空");
      return;
    }
    if (!ValidDifficulty(difficulty)) difficulty = "easy";
    if (!ValidJudgeType(judge_type)) judge_type = "exact";
    if (tlimit <= 0) tlimit = kDefaultTimeLimitMs;
    if (mlimit <= 0) mlimit = kDefaultMemoryLimitMb;
    if (judge_type == "special" && spj.empty()) {
      JsonError(res, 400, "特判题必须提供 spj 源码");
      return;
    }
    if (judge_type == "exact") spj.clear();

    // 检查题目存在
    std::string chk = "SELECT id FROM problems WHERE id = " + std::to_string(pid);
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

    std::string e_title = DbEscape(db, title);
    std::string e_desc = DbEscape(db, description);
    std::string e_tags = DbEscape(db, tags);
    std::string e_spj = DbEscape(db, spj);
    std::string sql = "UPDATE problems SET title='" + e_title + "', description='" + e_desc +
                      "', difficulty='" + difficulty + "', tags='" + e_tags +
                      "', time_limit_ms=" + std::to_string(tlimit) +
                      ", memory_limit_mb=" + std::to_string(mlimit) + ", judge_type='" + judge_type +
                      "', spj_source='" + e_spj + "' WHERE id=" + std::to_string(pid);
    if (mysql_query(db, sql.c_str()) != 0) {
      std::fprintf(stderr, "[problem] update failed: %s\n", mysql_error(db));
      JsonError(res, 500, "修改题目失败");
      return;
    }

    // 若带 test_cases 数组则整体替换
    const json::Value &arr = body.get("test_cases");
    if (arr.is_array()) {
      if (arr.as_array().size() > static_cast<size_t>(kMaxCases)) {
        JsonError(res, 400, "测试用例数量超限");
        return;
      }
      std::vector<std::pair<std::string, std::string>> cases;
      for (const auto &c : arr.as_array()) {
        std::string in = c.get("input").as_string();
        std::string out = c.get("expected_output").as_string();
        if (!ValidCaseContent(in, out, judge_type == "exact")) {
          JsonError(res, 400, "测试用例内容超限（单条 ≤ 64KB）");
          return;
        }
        cases.emplace_back(in, out);
      }
      DeleteCases(db, pid);
      if (!cases.empty() && !InsertCases(db, pid, 1, cases)) {
        JsonError(res, 500, "写入测试用例失败");
        return;
      }
    }

    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["id"] = json::Value(pid);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 删除题目（管理员）：test_cases 级联删除 ----
  svr.Delete(R"(/api/problems/(\d+))", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess;
    if (!RequireAdmin(db, req, sess)) {
      JsonError(res, 403, "需要管理员权限");
      return;
    }
    long long pid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);
    std::string sql = "DELETE FROM problems WHERE id = " + std::to_string(pid);
    if (mysql_query(db, sql.c_str()) != 0) {
      std::fprintf(stderr, "[problem] delete failed: %s\n", mysql_error(db));
      JsonError(res, 500, "删除题目失败");
      return;
    }
    if (mysql_affected_rows(db) == 0) {
      JsonError(res, 404, "题目不存在");
      return;
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- 添加测试用例（管理员，追加到末尾）----
  svr.Post(R"(/api/problems/(\d+)/cases)", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess;
    if (!RequireAdmin(db, req, sess)) {
      JsonError(res, 403, "需要管理员权限");
      return;
    }
    long long pid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);
    json::Value body = ParseBody(req, res);
    if (res.status == 400) return;

    const json::Value &arr = body.get("test_cases");
    if (!arr.is_array() || arr.as_array().empty()) {
      JsonError(res, 400, "缺少 test_cases 数组");
      return;
    }

    // 题目须存在；取当前最大 order_no
    std::string q = "SELECT COALESCE(MAX(order_no),0) FROM test_cases WHERE problem_id = " + std::to_string(pid);
    long long next = 1;
    if (mysql_query(db, q.c_str()) != 0) {
      JsonError(res, 500, "数据库查询失败");
      return;
    }
    MYSQL_RES *r = mysql_store_result(db);
    if (r) {
      if (MYSQL_ROW row = mysql_fetch_row(r)) next = std::strtoll(row[0], nullptr, 10) + 1;
      mysql_free_result(r);
    }

    // 确认题目存在
    std::string chk = "SELECT id FROM problems WHERE id = " + std::to_string(pid);
    if (mysql_query(db, chk.c_str()) != 0 || !(r = mysql_store_result(db))) {
      JsonError(res, 500, "数据库查询失败");
      return;
    }
    bool exists = mysql_fetch_row(r);
    mysql_free_result(r);
    if (!exists) {
      JsonError(res, 404, "题目不存在");
      return;
    }

    if (arr.as_array().size() > static_cast<size_t>(kMaxCases)) {
      JsonError(res, 400, "测试用例数量超限");
      return;
    }
    std::vector<std::pair<std::string, std::string>> cases;
    for (const auto &c : arr.as_array()) {
      std::string in = c.get("input").as_string();
      std::string out = c.get("expected_output").as_string();
      if (!ValidCaseContent(in, out, true)) {
        JsonError(res, 400, "测试用例内容超限（单条 ≤ 64KB）");
        return;
      }
      cases.emplace_back(in, out);
    }
    if (!InsertCases(db, pid, static_cast<int>(next), cases)) {
      JsonError(res, 500, "写入测试用例失败");
      return;
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });

  // ---- zip 导入测试用例（管理员，multipart 上传，整体替换该题用例）----
  svr.Post(R"(/api/problems/(\d+)/import)", [db](const httplib::Request &req, httplib::Response &res) {
    SessionInfo sess;
    if (!RequireAdmin(db, req, sess)) {
      JsonError(res, 403, "需要管理员权限");
      return;
    }
    long long pid = std::strtoll(req.matches[1].str().c_str(), nullptr, 10);

    if (!req.has_file("file")) {
      JsonError(res, 400, "缺少 file 文件");
      return;
    }
    const httplib::MultipartFormData &file = req.get_file_value("file");
    std::string fname = file.filename;
    auto ext_pos = fname.rfind('.');
    std::string ext = ext_pos == std::string::npos ? "" : fname.substr(ext_pos + 1);
    for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "zip" || file.content.empty()) {
      JsonError(res, 400, "仅支持 .zip 文件");
      return;
    }
    if (file.content.size() > static_cast<size_t>(kMaxZipBytes)) {
      JsonError(res, 400, "zip 文件过大（上限 8MB）");
      return;
    }

    // 题目须存在
    std::string chk = "SELECT id FROM problems WHERE id = " + std::to_string(pid);
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

    std::vector<zip::Entry> entries;
    std::string err;
    if (!zip::Parse(file.content, entries, err)) {
      JsonError(res, 400, "zip 解析失败: " + err);
      return;
    }

    std::vector<std::pair<std::string, std::string>> cases;
    if (!ParseZipCases(entries, cases, err)) {
      JsonError(res, 400, err);
      return;
    }
    if (cases.size() > static_cast<size_t>(kMaxCases)) {
      JsonError(res, 400, "用例数量超限");
      return;
    }
    for (const auto &c : cases) {
      if (!ValidCaseContent(c.first, c.second, true)) {
        JsonError(res, 400, "存在超过 64KB 的用例内容");
        return;
      }
    }

    DeleteCases(db, pid);
    if (!InsertCases(db, pid, 1, cases)) {
      JsonError(res, 500, "写入测试用例失败");
      return;
    }
    json::Object resp;
    resp["ok"] = json::Value(true);
    resp["count"] = json::Value(static_cast<long long>(cases.size()));
    res.set_content(json::Value(std::move(resp)).dump(), "application/json");
  });
}

}  // namespace oj
