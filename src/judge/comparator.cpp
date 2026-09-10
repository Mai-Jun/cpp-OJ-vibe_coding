// comparator.cpp — 逐字节对比 + 特判器编译/运行。

#include "comparator.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "sandbox.h"

namespace oj {
namespace judge {

CompareResult CompareExact(const std::string &user, const std::string &expected) {
  // 特判题无标准答案时不作内容对比。
  if (expected.empty()) return CompareResult::kMatch;
  // 兼容行尾换行差异：末尾至多各剥离一个 '\n' 后再逐字节比较。
  std::string a = user, b = expected;
  if (!a.empty() && a.back() == '\n') a.pop_back();
  if (!b.empty() && b.back() == '\n') b.pop_back();
  return a == b ? CompareResult::kMatch : CompareResult::kMismatch;
}

bool CompileSpj(const std::string &spj_source, const std::string &work_dir, std::string &err) {
  std::string src = work_dir + "/spj.cpp";
  std::string bin = work_dir + "/spj";
  {
    std::ofstream out(src, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "无法写入 spj 源码";
      return false;
    }
    out << spj_source;
    if (!out.good()) {
      err = "写入 spj 源码失败";
      return false;
    }
  }

  // 编译特判器：-O2 -std=c++17，链接到临时目录产物。
  // 编译过程在沙箱外执行（管理员维护的源码，可信度高于用户代码）。
  std::string cmd = "g++ -O2 -std=c++17 -o " + bin + " " + src + " 2>&1";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    err = "无法启动 spj 编译器";
    return false;
  }
  std::string output;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) output.append(buf, n);
  int rc = pclose(pipe);
  if (rc != 0) {
    err = "spj 编译失败: " + output;
    return false;
  }
  return true;
}

CompareResult RunSpj(const std::string &spj_bin, const std::string &input_file,
                     const std::string &user_file, const std::string &answer_file,
                     std::string &err) {
  // spj 读取三个文件路径作为参数；在 work_dir 内运行。
  std::string work_dir;
  {
    auto pos = spj_bin.rfind('/');
    work_dir = pos == std::string::npos ? "." : spj_bin.substr(0, pos);
  }
  std::vector<std::string> args{input_file, user_file, answer_file};
  // spj 运行限额（独立于题目限额，防止恶意 spj 耗资源）。
  RunResult r = RunCommand(spj_bin, work_dir, "", args, 5000, 512, 15000, 64 * 1024, "");
  if (r.kind != ExitKind::kFinished) {
    err = "spj 运行异常: " + r.error_msg;
    return CompareResult::kJudgeError;
  }
  if (r.exit_status == 0) return CompareResult::kMatch;
  err = "spj 判定不通过";
  return CompareResult::kMismatch;
}

}  // namespace judge
}  // namespace oj
