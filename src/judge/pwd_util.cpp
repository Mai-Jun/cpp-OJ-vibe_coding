// pwd_util.cpp — 读取 /etc/passwd 解析低权限评测用户。

#include "pwd_util.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace oj {
namespace judge {

bool ResolveUser(const std::string &name, uid_t &uid, gid_t &gid) {
  if (name.empty()) return false;
  std::ifstream in("/etc/passwd");
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    // user:x:uid:gid:...
    size_t c1 = line.find(':');
    if (c1 == std::string::npos) continue;
    size_t c2 = line.find(':', c1 + 1);
    if (c2 == std::string::npos) continue;
    size_t c3 = line.find(':', c2 + 1);
    if (c3 == std::string::npos) continue;
    size_t c4 = line.find(':', c3 + 1);
    if (c4 == std::string::npos) continue;
    if (line.compare(0, name.size(), name) != 0 || c1 != name.size()) continue;
    uid = static_cast<uid_t>(std::strtoul(line.substr(c2 + 1, c3 - c2 - 1).c_str(), nullptr, 10));
    gid = static_cast<gid_t>(std::strtoul(line.substr(c3 + 1, c4 - c3 - 1).c_str(), nullptr, 10));
    return true;
  }
  return false;
}

}  // namespace judge
}  // namespace oj
