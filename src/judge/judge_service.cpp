// judge_service.cpp — 串行评测队列 + 异步 submission（Phase 3）。
// 单后台 worker 线程逐条执行；每任务使用独立临时目录，评测结束清理。
// 结果分类逻辑见 ClassifyRun()：把沙箱原始结果映射到 SPEC 7 类结果。

#include "judge_service.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "comparator.h"
#include "db_conn.h"
#include "sandbox.h"

namespace oj {
namespace judge {

namespace {

constexpr long kMaxOutputBytes = 1024 * 1024;  // 用户程序输出捕获上限 1MB
constexpr long kDisplayTrunc = 4096;           // 单条输出存入结果的截断长度
constexpr long kCompileErrTrunc = 8192;        // 编译错误截断长度
constexpr int kCompileTimeoutSec = 10;
constexpr const char *kBaseWorkDir = "/tmp/oj_judge";
constexpr const char *kRunnerUser = "oj-runner";  // DEPENDENCIES.md 建议的低权限评测用户

std::string Truncate(const std::string &s, long limit) {
  if (static_cast<long>(s.size()) <= limit) return s;
  return s.substr(0, static_cast<size_t>(limit)) + "...[截断]";
}

// 是否像内存不足导致的异常（bad_alloc 等线索）。
bool LooksLikeMemoryError(const std::string &output) {
  if (output.find("bad_alloc") != std::string::npos) return true;
  if (output.find("Cannot allocate memory") != std::string::npos) return true;
  if (output.find("unable to allocate") != std::string::npos) return true;
  return false;
}

// 把一次原始运行结果映射为 7 类结果之一（TLE/MLE/RE/SE/AC/WA 由调用方继续判定）。
// status 与 detail 会写入对应字段；返回 true 表示这是最终判定（无需再对比）。
bool ClassifyRun(const RunResult &r, int mem_limit_mb, std::string &status,
                 std::string &detail) {
  switch (r.kind) {
    case ExitKind::kError:
      status = "SE";
      detail = "系统错误: " + r.error_msg;
      return true;
    case ExitKind::kTimedOut:
      status = "TLE";
      detail = "超出时间限制";
      return true;
    case ExitKind::kOutputLimit:
      status = "RE";
      detail = "运行时错误: 输出过长";
      return true;
    case ExitKind::kCrash:
      if (r.cpu_limit_exceeded) {
        status = "TLE";
        detail = "超出时间限制 (CPU)";
        return true;
      }
      if (r.seccomp_violation) {
        status = "RE";
        detail = "运行时错误: 调用被禁止的系统调用（网络/高危操作被沙箱拦截）";
        return true;
      }
      if (r.signal_no == SIGSEGV && LooksLikeMemoryError(r.output)) {
        status = "MLE";
        detail = "超出内存限制 (" + std::to_string(mem_limit_mb) + "MB)";
        return true;
      }
      if (r.signal_no == SIGSEGV) {
        status = "RE";
        detail = "运行时错误: 段错误 (SIGSEGV)";
        return true;
      }
      if (r.signal_no == SIGABRT && LooksLikeMemoryError(r.output)) {
        status = "MLE";
        detail = "超出内存限制 (" + std::to_string(mem_limit_mb) + "MB)";
        return true;
      }
      status = "RE";
      detail = "运行时错误: 信号 " + std::to_string(r.signal_no) + " (" +
               strsignal(r.signal_no) + ")";
      return true;
    case ExitKind::kFinished:
      if (r.exit_status != 0) {
        status = "RE";
        detail = "运行时错误: 退出码 " + std::to_string(r.exit_status);
        return true;
      }
      return false;  // 正常退出，交由对比逻辑判定 AC/WA
  }
  return true;
}

// 用 g++ 编译源文件到产物路径；成功返回 true，编译输出写入 compile_output。
bool Compile(const std::string &src, const std::string &bin, std::string &compile_output) {
  std::string cmd = "timeout " + std::to_string(kCompileTimeoutSec) +
                    "s g++ -O2 -std=c++17 -o " + bin + " " + src + " 2>&1";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) return false;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) compile_output.append(buf, n);
  int rc = pclose(pipe);
  return rc == 0;
}

void WriteFile(const std::string &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

// 递归删除目录（仅用于评测临时目录，含我们创建的文件）。
void RemoveDir(const std::string &path) {
  DIR *d = opendir(path.c_str());
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    std::string full = path + "/" + name;
    struct stat st {};
    if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RemoveDir(full);
    } else {
      unlink(full.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

// 启动时清理评测工作目录（Phase 5）：删除上次异常退出残留的临时目录。
// 仅处理 /tmp/oj_judge 下以 "oj_" 前缀命名的目录，不递归清空基目录。
void CleanupStaleDirs(const std::string &base) {
  DIR *d = opendir(base.c_str());
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (name.compare(0, 3, "oj_") != 0) continue;
    std::string full = base + "/" + name;
    struct stat st {};
    if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) RemoveDir(full);
  }
  closedir(d);
}

// 确保评测工作基目录存在并清理残留（Phase 5）。
void PrepareWorkDir() {
  struct stat st {};
  if (stat(kBaseWorkDir, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      std::fprintf(stderr, "[judge] %s 存在但不是目录，评测工作目录不可用\n", kBaseWorkDir);
      return;
    }
  } else {
    mkdir(kBaseWorkDir, 0755);
  }
  CleanupStaleDirs(kBaseWorkDir);
}

// 加载题目信息（title/限额/判题方式/spj 源码）。失败返回 false。
bool LoadProblem(MYSQL *db, long long problem_id, std::string &title, int &time_limit_ms,
                 int &mem_limit_mb, std::string &judge_type, std::string &spj_source) {
  std::string q = "SELECT title, time_limit_ms, memory_limit_mb, judge_type, COALESCE(spj_source,'') "
                  "FROM problems WHERE id = " + std::to_string(problem_id);
  if (mysql_query(db, q.c_str()) != 0) return false;
  MYSQL_RES *res = mysql_store_result(db);
  if (!res) return false;
  bool ok = false;
  if (MYSQL_ROW row = mysql_fetch_row(res)) {
    title = row[0] ? row[0] : "";
    time_limit_ms = std::atoi(row[1] ? row[1] : "500");
    mem_limit_mb = std::atoi(row[2] ? row[2] : "256");
    judge_type = row[3] ? row[3] : "exact";
    spj_source = row[4] ? row[4] : "";
    ok = true;
  }
  mysql_free_result(res);
  return ok;
}

struct DbCase {
  int order_no = 0;
  std::string input;
  std::string expected;
};

bool LoadCases(MYSQL *db, long long problem_id, std::vector<DbCase> &out) {
  std::string q = "SELECT order_no, input, COALESCE(expected_output,'') FROM test_cases "
                  "WHERE problem_id = " + std::to_string(problem_id) + " ORDER BY order_no";
  if (mysql_query(db, q.c_str()) != 0) return false;
  MYSQL_RES *res = mysql_store_result(db);
  if (!res) return false;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    DbCase c;
    c.order_no = std::atoi(row[0] ? row[0] : "0");
    c.input = row[1] ? row[1] : "";
    c.expected = row[2] ? row[2] : "";
    out.push_back(std::move(c));
  }
  mysql_free_result(res);
  return true;
}

}  // namespace

JudgeService::JudgeService(MYSQL *db) : db_(db) {
  worker_ = std::thread(&JudgeService::WorkerLoop, this);
}

JudgeService::~JudgeService() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

long long JudgeService::Submit(long long user_id, long long problem_id, const std::string &code) {
  if (code.empty() || code.size() > 512 * 1024) return 0;  // 内容/大小限制
  {
    std::lock_guard<std::mutex> lk(mu_);
    Task t;
    t.id = next_id_++;
    t.user_id = user_id;
    t.problem_id = problem_id;
    t.code = code;
    queue_.push_back(std::move(t));
    cv_.notify_one();
    return t.id;
  }
}

std::shared_ptr<const SubmissionResult> JudgeService::Get(long long id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = results_.find(id);
  return it == results_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const SubmissionResult>> JudgeService::ListByUser(
    long long user_id, int limit) const {
  std::vector<std::shared_ptr<const SubmissionResult>> out;
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = results_.rbegin(); it != results_.rend() && out.size() < static_cast<size_t>(limit);
       ++it) {
    if (it->second->user_id == user_id) out.push_back(it->second);
  }
  return out;
}

void JudgeService::WorkerLoop() {
  // 评测 worker 使用独立 DB 连接，避免与 HTTP 线程共享句柄产生竞态。
  MYSQL db;
  bool db_ok = false;
  {
    DbConfig cfg = DbConfigFromEnv();
    if (DbConnect(&db, cfg)) db_ok = true;
  }
  PrepareWorkDir();

  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) break;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!db_ok) {
      auto res = std::make_shared<SubmissionResult>();
      res->id = task.id;
      res->user_id = task.user_id;
      res->problem_id = task.problem_id;
      res->status = "SE";
      res->detail = "系统错误: 评测数据库不可用";
      res->created_at = NowStr();
      res->done = true;
      StoreResult(std::move(res));
      continue;
    }
    RunTask(task, &db);
  }

  if (db_ok) mysql_close(&db);
}

void JudgeService::RunTask(const Task &task, MYSQL *db) {
  auto res = std::make_shared<SubmissionResult>();
  res->id = task.id;
  res->user_id = task.user_id;
  res->problem_id = task.problem_id;
  res->status = "SE";
  res->created_at = NowStr();
  res->done = true;

  // 独立工作目录：mkdir 0700 + chmod 0755。
  // chmod 0755 是 root 启动 + 降权 oj-runner 运行所需的：子进程降权后仍需
  // 读入 main.cpp / 写出输出到该目录。目录本身无敏感数据（评测用例由 worker
  // 写入，属公开题目内容），受 seccomp + rlimit 约束的用户代码读不到系统文件。
  std::string work = std::string(kBaseWorkDir) + "/oj_" + std::to_string(task.id);
  mkdir(work.c_str(), 0700);
  chmod(work.c_str(), 0755);

  // 1) 读取题目信息
  int time_limit_ms = 500, mem_limit_mb = 256;
  std::string judge_type = "exact", spj_source;
  if (!LoadProblem(db, task.problem_id, res->problem_title, time_limit_ms, mem_limit_mb,
                   judge_type, spj_source)) {
    res->detail = "系统错误: 题目不存在或数据异常";
    RemoveDir(work);
    StoreResult(res);
    return;
  }

  // 2) 编译用户代码
  {
    std::string src = work + "/main.cpp";
    std::string bin = work + "/main";
    WriteFile(src, task.code);
    std::string co;
    if (!Compile(src, bin, co)) {
      res->status = "CE";
      res->detail = "编译错误";
      res->compile_error = Truncate(co, kCompileErrTrunc);
      RemoveDir(work);
      StoreResult(res);
      return;
    }
  }

  // 3) 特判器编译（special 题）
  std::string spj_bin;
  if (judge_type == "special") {
    std::string err;
    if (!CompileSpj(spj_source, work, err)) {
      res->status = "SE";
      res->detail = "系统错误: " + err;
      RemoveDir(work);
      StoreResult(res);
      return;
    }
    spj_bin = work + "/spj";
  }

  // 4) 加载用例
  std::vector<DbCase> cases;
  if (!LoadCases(db, task.problem_id, cases) || cases.empty()) {
    res->status = "SE";
    res->detail = "系统错误: 测试用例缺失";
    RemoveDir(work);
    StoreResult(res);
    return;
  }

  // 5) 逐个运行用例（遇首个非 AC 即停止）
  long long max_time = 0, max_mem_kb = 0;
  bool all_ac = true;
  for (const auto &c : cases) {
    std::string in_file = work + "/case_" + std::to_string(c.order_no) + ".in";
    std::string out_file = work + "/case_" + std::to_string(c.order_no) + ".out";
    WriteFile(in_file, c.input);
    WriteFile(out_file, c.expected);

    RunResult r = RunCommand(work + "/main", work, c.input, {}, time_limit_ms, mem_limit_mb,
                             time_limit_ms + 5000, kMaxOutputBytes, kRunnerUser, true);

    CaseResult cr;
    cr.order_no = c.order_no;
    cr.time_ms = std::max<long long>(r.cpu_ms, 0);
    cr.user_output = Truncate(r.output, kDisplayTrunc);
    cr.expected_output = Truncate(c.expected, kDisplayTrunc);
    max_time = std::max(max_time, cr.time_ms);
    max_mem_kb = std::max(max_mem_kb, static_cast<long long>(r.peak_rss_kb));

    std::string status, detail;
    bool final_verdict = ClassifyRun(r, mem_limit_mb, status, detail);
    if (final_verdict) {
      cr.status = status;
      cr.detail = detail;
    } else {
      // 正常退出：对比输出
      if (judge_type == "special") {
        std::string user_file = work + "/user.out";
        WriteFile(user_file, r.output);
        std::string err;
        CompareResult cmp = RunSpj(spj_bin, in_file, user_file, out_file, err);
        if (cmp == CompareResult::kMatch) {
          cr.status = "AC";
        } else if (cmp == CompareResult::kMismatch) {
          cr.status = "WA";
          cr.detail = "输出不符合要求";
        } else {
          res->status = "SE";
          res->detail = "系统错误: " + err;
          RemoveDir(work);
          StoreResult(res);
          return;
        }
      } else {
        if (CompareExact(r.output, c.expected) == CompareResult::kMatch) {
          cr.status = "AC";
        } else {
          cr.status = "WA";
          cr.detail = "输出与期望不符";
        }
      }
    }

    res->cases.push_back(std::move(cr));
    const CaseResult &last = res->cases.back();
    res->time_ms = max_time;
    res->memory_mb = (max_mem_kb + 1023) / 1024;
    if (last.status != "AC") {
      all_ac = false;
      res->status = last.status;
      res->detail = last.detail;
      break;
    }
  }

  if (all_ac && !res->cases.empty()) {
    res->status = "AC";
    res->detail = "";
  }
  if (res->cases.empty()) {
    res->detail = "系统错误: 无可用测试用例";
  }

  RemoveDir(work);
  StoreResult(std::move(res));
}

void JudgeService::StoreResult(std::shared_ptr<SubmissionResult> res) {
  std::lock_guard<std::mutex> lk(mu_);
  results_[res->id] = std::move(res);
}

std::string JudgeService::NowStr() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  return buf;
}

}  // namespace judge
}  // namespace oj
