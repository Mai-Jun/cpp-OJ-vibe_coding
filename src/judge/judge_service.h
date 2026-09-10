// judge_service.h — 串行评测队列 + 异步 submission 模型（Phase 3）。
// 提交代码入队，后台单 worker 串行评测；前端轮询 submission_id 获取结果。
// 结果分类（7 类）：Accepted / Wrong Answer / Time Limit Exceeded /
//   Memory Limit Exceeded / Runtime Error / Compile Error / System Error。

#ifndef OJ_JUDGE_JUDGE_SERVICE_H
#define OJ_JUDGE_JUDGE_SERVICE_H

#include <mysql.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oj {
namespace judge {

// 单个用例的评测细节（供前端展示 WA 预期 vs 实际等）。
struct CaseResult {
  long long order_no = 0;
  std::string status;         // AC / WA / TLE / MLE / RE / SE
  std::string detail;         // 说明（RTE 信号、MLE 线索等）
  long long time_ms = 0;
  std::string user_output;    // 用户实际输出（WA 时展示）
  std::string expected_output;// 期望输出（WA 时展示）
};

// 一次提交的最终结果。
struct SubmissionResult {
  long long id = 0;
  long long user_id = 0;
  long long problem_id = 0;
  std::string problem_title;
  std::string status;         // 7 类之一
  std::string detail;         // 人类可读说明（WA 对比、编译错误、RTE 信号等）
  long long time_ms = 0;      // 总耗时（取最坏用例）
  long long memory_mb = 0;    // 峰值内存（取最坏用例）
  std::string compile_error;  // CE 时的编译器输出
  std::vector<CaseResult> cases;
  std::string created_at;
  bool done = true;           // 评测是否完成
};

// 评测服务：持有后台 worker 线程，串行执行内存队列中的评测任务。
class JudgeService {
 public:
  explicit JudgeService(MYSQL *db);
  ~JudgeService();

  JudgeService(const JudgeService &) = delete;
  JudgeService &operator=(const JudgeService &) = delete;

  // 提交一次评测（代码入队）；立即返回 submission_id（>0 成功，0 失败）。
  long long Submit(long long user_id, long long problem_id, const std::string &code);

  // 查询提交结果；不存在返回 nullptr。
  std::shared_ptr<const SubmissionResult> Get(long long id) const;

  // 该用户提交记录（按 id 倒序）。
  std::vector<std::shared_ptr<const SubmissionResult>> ListByUser(long long user_id,
                                                                  int limit) const;

 private:
  struct Task {
    long long id = 0;
    long long user_id = 0;
    long long problem_id = 0;
    std::string code;
  };

  void WorkerLoop();
  void RunTask(const Task &task, MYSQL *db);
  void StoreResult(std::shared_ptr<SubmissionResult> res);
  static std::string NowStr();

  MYSQL *db_;
  std::thread worker_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  long long next_id_ = 1;
  std::deque<Task> queue_;
  std::map<long long, std::shared_ptr<SubmissionResult>> results_;
};

}  // namespace judge
}  // namespace oj

#endif  // OJ_JUDGE_JUDGE_SERVICE_H
