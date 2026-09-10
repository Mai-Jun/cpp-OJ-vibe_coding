// pwd_util.h — 低权限评测用户解析（sandbox 用，B 档沙箱降权）。
// 通过 /etc/passwd 一次性解析 uid/gid，避免依赖额外库。

#ifndef OJ_JUDGE_PWD_UTIL_H
#define OJ_JUDGE_PWD_UTIL_H

#include <string>

#include <sys/types.h>

namespace oj {
namespace judge {

// 按用户名查询 uid/gid；成功返回 true。
bool ResolveUser(const std::string &name, uid_t &uid, gid_t &gid);

}  // namespace judge
}  // namespace oj

#endif  // OJ_JUDGE_PWD_UTIL_H
