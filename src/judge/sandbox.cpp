// sandbox.cpp — fork + setrlimit + seccomp 沙箱实现（Phase 3 B 档 + Phase 5 加固）。
// 隔离策略（SPEC §2.2/§7）：
//   B 档基础：fork + RLIMIT_CPU/RLIMIT_AS + 低权限 setuid + 关闭继承 fd
//             + 父进程墙钟兜底超时 kill。
//   Phase 5 加固：子进程装载 seccomp-bpf 白名单过滤器，阻止网络访问与高危系统调用，
//             隔离能力在非 root 环境下同样生效（无需 root，也不要求降权用户存在）。
//   A 档（unshare+seccomp+cgroup）见 SPEC §7。

#include "sandbox.h"

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "pwd_util.h"

// seccomp-bpf 相关头文件（Linux 专用；非 Linux 平台不编译本模块）。
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace oj {
namespace judge {

namespace {

constexpr int kReadBufSize = 65536;

// ---- seccomp-bpf 白名单过滤器（Phase 5）----
// 目标：
//   1) 禁止所有网络系统调用（socket/connect/bind/listen/accept/...），
//      子进程一旦调用即被内核以 SIGSYS 终止，无需配置网络 namespace。
//   2) 禁止高危/只读伪装类系统调用，降低逃逸面：
//      ptrace(进程调试)、mount/umount2/pivot_root(挂载)、init_module/
//      finit_module/delete_module(内核模块)、reboot、kexec_load、
//      bpf(动态 BPF)、clone3(绕 RLIMIT_NPROC 的 fork 变体)、setns(切换命名空间)。
//   3) 其余系统调用放行（白名单按需放开），保证用户程序编译产物正常读写文件/内存。
// 被拦截的系统调用使子进程收到 SIGSYS，父进程经 RunResult.seccomp_violation
// 归类为 Runtime Error（"调用了被禁止的系统调用"）。
//
// 说明：评测编译产物均为普通用户态 C/C++ 程序，白名单覆盖其全部需求；
// 特判器（spj）同样受此约束，其读写固定路径文件不受影响。

// 单条 BPF 指令的便捷构造。
struct sock_filter MakeInst(uint16_t code, uint32_t jt, uint32_t jf, uint32_t k) {
  return static_cast<sock_filter>(sock_filter{code, static_cast<uint8_t>(jt),
                                              static_cast<uint8_t>(jf), k});
}

// 用 x86_64 系统调用号构建过滤器（随架构 guard，见下）。
bool LoadSeccompFilter() {
#if defined(__x86_64__)
  // 白名单：所有除被禁 syscall 之外的调用均放行（kill 用户进程默认策略）。
  // 依次对每个禁用 syscall 做 `==` 判断，命中即返回 SECCOMP_RET_KILL_PROCESS。
  static const int kBlocked[] = {
      __NR_socket,       __NR_socketpair,   __NR_bind,       __NR_connect,
      __NR_listen,       __NR_accept,       __NR_accept4,    __NR_sendto,
      __NR_sendmsg,      __NR_sendmmsg,     __NR_recvfrom,   __NR_recvmsg,
      __NR_recvmmsg,     __NR_shutdown,     __NR_setsockopt, __NR_getsockopt,
      __NR_getsockname,  __NR_getpeername,
      __NR_ptrace,       __NR_mount,        __NR_umount2,    __NR_pivot_root,
      __NR_chroot,       __NR_init_module,  __NR_finit_module, __NR_delete_module,
      __NR_reboot,       __NR_kexec_load,   __NR_bpf,        __NR_clone3,
      __NR_setns,        __NR_unshare,      __NR_sethostname, __NR_setdomainname,
      __NR_keyctl,       __NR_add_key,      __NR_request_key,
  };

  // 判断载入的指令数。
  const size_t n = sizeof(kBlocked) / sizeof(kBlocked[0]);
  std::vector<sock_filter> f;
  f.reserve(n * 2 + 3);
  f.push_back(MakeInst(BPF_LD | BPF_W | BPF_ABS, 0, 0,
                       static_cast<uint32_t>(offsetof(struct seccomp_data, nr))));
  for (size_t i = 0; i < n; ++i) {
    f.push_back(MakeInst(BPF_JMP | BPF_JEQ | BPF_K, 0, 1,
                         static_cast<uint32_t>(kBlocked[i])));
    // 命中：kill 整个子进程
    f.push_back(MakeInst(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS));
  }
  f.push_back(MakeInst(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW));

  // prctl(PR_SET_NO_NEW_PRIVS) 后才能安装过滤器（无需 CAP_SYS_ADMIN）。
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;

  // 过滤器需按 8 字节对齐；sock_filter 已是 8 字节结构。
  sock_fprog prog;
  prog.len = static_cast<unsigned short>(f.size());
  prog.filter = f.data();
  if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) return false;
  return true;
#else
  // 非 x86_64：seccomp-bpf 过滤器依赖架构特定的系统调用号表，本环境为 x86_64；
  // 其他架构不启用 seccomp（B 档基础隔离仍生效）。
  std::fprintf(stderr, "[sandbox] 非 x86_64 架构，seccomp 过滤器未启用\n");
  return true;
#endif
}

// 把 input 一次性写入写端并关闭；失败返回 -1。
int WriteAll(int fd, const std::string &input) {
  size_t off = 0;
  while (off < input.size()) {
    ssize_t n = write(fd, input.data() + off, input.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += static_cast<size_t>(n);
  }
  close(fd);
  return 0;
}

// 子进程侧：配置 rlimit 与降权；失败返回 false。
// 降权说明：仅当当前进程是 root 且指定了 run_as_user 时才 setuid/setgid；
// 非 root 环境下无法切换用户，为保证功能可用改为以当前用户运行（打日志提示），
// 部署时服务端应以 root 运行以满足 SPEC §7 的降权要求。
bool SetupChild(const std::string &run_as_user, int cpu_limit_ms, int mem_limit_mb) {
  struct rlimit rl {};
  // RLIMIT_CPU 只接受整秒；取 ceil 且至少 1 秒，避免合法任务被提前误杀。
  // 注意：仅当 soft < hard 时内核才会在超限发送 SIGXCPU（便于精确分类 TLE）；
  // 若 soft == hard，内核直接 SIGKILL，无法区分 CPU 超时与墙钟兜底。
  long cpu_sec = (cpu_limit_ms + 999) / 1000;
  if (cpu_sec < 1) cpu_sec = 1;
  rl.rlim_cur = static_cast<rlim_t>(cpu_sec);
  rl.rlim_max = static_cast<rlim_t>(cpu_sec) + 1;  // hard 比 soft 多 1s 兜底
  if (setrlimit(RLIMIT_CPU, &rl) != 0) return false;

  // 虚拟内存上限（MB → 字节）；RLIMIT_AS 超限会使 new 抛 bad_alloc/被 SIGSEGV 终止。
  if (mem_limit_mb > 0) {
    rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(mem_limit_mb) * 1024 * 1024;
    if (setrlimit(RLIMIT_AS, &rl) != 0) return false;
  }

  // 禁止子进程再开新进程（防 fork 炸弹）；注意必须放在 fork 不会发生的时机。
  rl.rlim_cur = rl.rlim_max = 0;
  if (setrlimit(RLIMIT_NPROC, &rl) != 0) return false;

  // 关闭所有 ≥ 3 的 fd（防泄露网络/DB 连接、文件句柄给用户程序）。
  for (int fd = 3; fd < 1024; ++fd) close(fd);

  // 降权为低权限评测用户（先 gid 再 uid，之后不可恢复）。
  // 非 root 时无法 setuid，降级为当前用户运行（开发/单测场景）。
  if (!run_as_user.empty() && geteuid() == 0) {
    uid_t uid;
    gid_t gid;
    if (!ResolveUser(run_as_user, uid, gid)) return false;
    if (setgid(gid) != 0 || setuid(uid) != 0) return false;
  }
  return true;
}

}  // namespace

RunResult RunCommand(const std::string &binary_path, const std::string &cwd,
                     const std::string &input, const std::vector<std::string> &args,
                     int cpu_limit_ms, int mem_limit_mb, int wall_timeout_ms,
                     long max_output_bytes, const std::string &run_as_user,
                     bool apply_seccomp) {
  RunResult result;

  // 非 root 且要求降权时，在父进程侧提示一次（不要写入子进程 stdout，避免污染用户输出）。
  static bool warned_not_root = false;
  if (!run_as_user.empty() && geteuid() != 0 && !warned_not_root) {
    std::fprintf(stderr, "[sandbox] 非 root 运行，评测不降权（部署时请以 root 启动以获得 %s 隔离）\n",
                 run_as_user.c_str());
    warned_not_root = true;
  }

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    result.error_msg = "pipe() failed";
    return result;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    result.error_msg = "fork() failed";
    return result;
  }

  if (pid == 0) {
    // ---- 子进程 ----
    close(in_pipe[1]);
    close(out_pipe[0]);
    if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
    dup2(STDOUT_FILENO, STDERR_FILENO);  // stderr 并入 stdout，一并捕获 bad_alloc 等线索
    close(in_pipe[0]);
    close(out_pipe[1]);

    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    // 先完成 chdir/setrlimit/降权，再装载 seccomp（此时才关闭外部访问）。
    if (!SetupChild(run_as_user, cpu_limit_ms, mem_limit_mb)) _exit(125);
    if (apply_seccomp && !LoadSeccompFilter()) _exit(124);  // seccomp 装载失败按沙箱错误退出

    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>(binary_path.c_str()));
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execv(binary_path.c_str(), argv.data());
    _exit(127);  // exec 失败
  }

  // ---- 父进程 ----
  close(in_pipe[0]);
  close(out_pipe[1]);

  std::thread writer([&]() {
    if (WriteAll(in_pipe[1], input) != 0) close(in_pipe[1]);
  });

  std::string out;
  bool output_limit_hit = false;
  {
    std::vector<char> buf(kReadBufSize);
    ssize_t n;
    while ((n = read(out_pipe[0], buf.data(), buf.size())) > 0) {
      out.append(buf.data(), static_cast<size_t>(n));
      if (max_output_bytes > 0 && static_cast<long>(out.size()) > max_output_bytes) {
        output_limit_hit = true;
        kill(pid, SIGKILL);
        break;
      }
    }
    close(out_pipe[0]);
  }
  writer.join();

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(std::max(wall_timeout_ms, 100));
  struct rusage ru {};
  int status = 0;
  for (;;) {
    pid_t r = wait4(pid, &status, WNOHANG, &ru);
    if (r == pid) {
      result.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
      result.peak_rss_kb = ru.ru_maxrss;
      result.cpu_ms = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000LL +
                      (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
      if (output_limit_hit) {
        result.kind = ExitKind::kOutputLimit;
      } else if (WIFEXITED(status)) {
        result.kind = ExitKind::kFinished;
        result.exit_status = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        result.kind = ExitKind::kCrash;
        result.signal_no = WTERMSIG(status);
        // RLIMIT_CPU 超限由 SIGXCPU 表达。
        if (result.signal_no == SIGXCPU) result.cpu_limit_exceeded = true;
        // seccomp 拦截的系统调用由 SIGSYS 表达。
        if (result.signal_no == SIGSYS) result.seccomp_violation = true;
      }
      result.output = std::move(out);
      return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      wait4(pid, &status, 0, &ru);
      result.kind = ExitKind::kTimedOut;
      result.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
      result.peak_rss_kb = ru.ru_maxrss;
      result.cpu_ms = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000LL +
                      (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
      result.output = std::move(out);
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace judge
}  // namespace oj
