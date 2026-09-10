// sandbox.h — fork + setrlimit + seccomp 沙箱（Phase 3 B 档 + Phase 5 加固）
// 评测子进程通过管道接收输入、向管道写出输出；父进程负责超时 kill。
// Phase 5：子进程装载 seccomp-bpf 白名单过滤器，禁止网络访问与高危系统调用
// （被拦截时以 SIGSYS 终止，judge_service 据此分类为 RE）。
// 提供原始运行信息（信号、峰值内存、CPU/墙钟耗时、输出），
// 由 judge_service 据此做 7 类结果分类。

#ifndef OJ_JUDGE_SANDBOX_H
#define OJ_JUDGE_SANDBOX_H

#include <string>
#include <vector>

#include <sys/types.h>

namespace oj {
namespace judge {

// 子进程退出原因（原始事实，不做业务分类）。
enum class ExitKind {
  kFinished,    // 正常退出（exit_status 可用）
  kTimedOut,    // 父进程墙钟兜底超时，已 SIGKILL
  kCrash,       // 信号终止（signal_no 可用）
  kOutputLimit, // 输出超过上限，父进程 kill
  kError,       // 沙箱自身错误（fork/管道/setrlimit/exec 失败）
};

// 一次运行的完整结果。
struct RunResult {
  ExitKind kind = ExitKind::kError;
  int exit_status = 0;   // WEXITSTATUS（仅 kFinished）
  int signal_no = 0;     // WTERMSIG（仅 kCrash）
  bool cpu_limit_exceeded = false;  // 被 SIGXCPU 终止（RLIMIT_CPU）
  bool seccomp_violation = false;   // 被 SIGSYS 终止（seccomp 拦截的系统调用，Phase 5）
  long long cpu_ms = 0;            // 子进程 CPU 时间（utime+stime）
  long long wall_ms = 0;           // 父进程墙钟耗时
  long peak_rss_kb = 0;            // 子进程峰值 RSS（wait4 rusage）
  std::string output;              // 捕获到的 stdout（含 stderr 重定向）
  std::string error_msg;           // 沙箱/启动错误描述
};

// 运行 binary_path（execv，argv[0]=binary_path，argv[1..]=args）。
// - cwd 非空时先 chdir；stdin 写入 input；stdout 捕获到 result.output；
// - cpu_limit_ms: RLIMIT_CPU（秒级取整，最小 1s）；
// - mem_limit_mb: RLIMIT_AS 上限（MB→字节，>0 生效）；
// - wall_timeout_ms: 父进程兜底超时（含编译等待），必须 ≥ cpu_limit_ms；
// - max_output_bytes: 输出捕获上限（0=不限；超限 kill 子进程并置 kOutputLimit）；
// - run_as_user: 非空时子进程 setgid/setuid 降权（需服务器以 root 运行）；
// - apply_seccomp: true 时子进程装载 seccomp-bpf 过滤器（白名单），拦截
//   网络访问与高危系统调用（Phase 5 安全加固，见 sandbox.cpp ApplySeccomp）。
RunResult RunCommand(const std::string &binary_path, const std::string &cwd,
                     const std::string &input, const std::vector<std::string> &args,
                     int cpu_limit_ms, int mem_limit_mb, int wall_timeout_ms,
                     long max_output_bytes, const std::string &run_as_user,
                     bool apply_seccomp);

}  // namespace judge
}  // namespace oj

#endif  // OJ_JUDGE_SANDBOX_H
