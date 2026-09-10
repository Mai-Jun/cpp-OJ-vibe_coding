// comparator.h — 结果对比与特判器（Phase 3）。
// - 逐字节对比（exact judge）
// - 特判器（special judge）：spj 源码编译为可执行文件，接收 输入+用户输出+标准答案，返回 0/非0

#ifndef OJ_JUDGE_COMPARATOR_H
#define OJ_JUDGE_COMPARATOR_H

#include <string>
#include <vector>

namespace oj {
namespace judge {

enum class CompareResult {
  kMatch,       // 一致（逐字节 / spj 返回 0）
  kMismatch,    // 不一致（WA）
  kJudgeError,  // 对比器自身失败（spj 编译/运行出错）→ System Error
};

// 逐字节对比（SPEJD 精确比对）。所有非空输出行整体比较，结尾换行不影响匹配。
CompareResult CompareExact(const std::string &user, const std::string &expected);

// 将 spj 源码编译为 work_dir/spj 可执行文件；成功返回 true，err 写入失败原因。
bool CompileSpj(const std::string &spj_source, const std::string &work_dir, std::string &err);

// 运行特判器：args = [spj_bin, input_file, user_output_file, answer_file]
// 返回 0 表示通过（AC），非 0 表示不通过（WA）；失败返回 kJudgeError。
CompareResult RunSpj(const std::string &spj_bin, const std::string &input_file,
                     const std::string &user_file, const std::string &answer_file,
                     std::string &err);

}  // namespace judge
}  // namespace oj

#endif  // OJ_JUDGE_COMPARATOR_H
