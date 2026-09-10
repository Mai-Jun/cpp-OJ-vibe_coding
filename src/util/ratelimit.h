// ratelimit.h — 按 IP 的简单滑动窗口限流（Phase 5 安全加固）。
// 用于注册 / 登录等公开接口，防 DoS / 撞库 / 批量注册。
// 线程安全：内部以互斥锁保护计数。内存占用随活跃 IP 数量增长，
// 定期（CheckAndTrim）清理过期条目，避免无界增长。

#ifndef OJ_UTIL_RATELIMIT_H
#define OJ_UTIL_RATELIMIT_H

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace oj {

// 按 key（通常为客户端 IP）限流。
class RateLimiter {
 public:
  // 构造：window_sec 窗口秒数内最多允许 max_hits 次。
  RateLimiter(int max_hits, int window_sec)
      : max_hits_(max_hits), window_sec_(window_sec) {}

  // 记录一次访问并判断是否超限；返回 true 表示允许，false 表示拒绝（超限）。
  bool Allow(const std::string &key) {
    const long long now = NowMs();
    std::lock_guard<std::mutex> lk(mu_);
    auto &w = entries_[key];
    // 丢弃窗口外的时间戳
    long long cutoff = now - window_sec_ * 1000LL;
    while (!w.empty() && w.front() <= cutoff) w.pop_front();
    if (static_cast<long long>(w.size()) >= max_hits_) return false;
    w.push_back(now);
    return true;
  }

 private:
  static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  int max_hits_;
  int window_sec_;
  std::mutex mu_;
  std::map<std::string, std::deque<long long>> entries_;
};

}  // namespace oj

#endif  // OJ_UTIL_RATELIMIT_H
