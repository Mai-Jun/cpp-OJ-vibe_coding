// password.h — 密码加盐哈希（Phase 1）
// 方案：PBKDF2-HMAC-SHA256 + 每用户随机盐（OpenSSL）。
// 存储格式：pbkdf2$<iterations>$<salt_hex>$<hash_hex>
//   迭代次数在验证时从存储串中读取，便于未来升级迭代次数而不破坏旧哈希。

#ifndef OJ_PASSWORD_H
#define OJ_PASSWORD_H

#include <string>

namespace oj {

// 对明文密码生成存储用哈希字符串（含盐与迭代次数）。
std::string HashPassword(const std::string &plain);

// 校验明文是否匹配存储串；返回 true 表示匹配。
bool VerifyPassword(const std::string &plain, const std::string &stored);

}  // namespace oj

#endif  // OJ_PASSWORD_H