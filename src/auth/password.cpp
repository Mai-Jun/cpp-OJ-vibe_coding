#include "password.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace oj {

namespace {

constexpr int kIterations = 210000;  // OWASP 2023 对 SHA256-PBKDF2 的建议下限附近
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;  // SHA-256 输出

std::string HexEncode(const unsigned char *data, int len) {
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<size_t>(len) * 2);
  for (int i = 0; i < len; ++i) {
    out.push_back(hex[(data[i] >> 4) & 0xF]);
    out.push_back(hex[data[i] & 0xF]);
  }
  return out;
}

bool HexDecode(const std::string &in, std::vector<unsigned char> &out) {
  if (in.size() % 2 != 0) return false;
  out.reserve(in.size() / 2);
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < in.size(); i += 2) {
    int hi = hexval(in[i]);
    int lo = hexval(in[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

}  // namespace

std::string HashPassword(const std::string &plain) {
  unsigned char salt[kSaltBytes];
  unsigned char hash[kHashBytes];
  if (RAND_bytes(salt, kSaltBytes) != 1) {
    std::fprintf(stderr, "[password] RAND_bytes failed\n");
    return {};
  }
  if (PKCS5_PBKDF2_HMAC(plain.c_str(), static_cast<int>(plain.size()), salt, kSaltBytes,
                        kIterations, EVP_sha256(), kHashBytes, hash) != 1) {
    std::fprintf(stderr, "[password] PBKDF2 failed\n");
    return {};
  }
  std::ostringstream os;
  os << "pbkdf2$" << kIterations << "$" << HexEncode(salt, kSaltBytes) << "$"
     << HexEncode(hash, kHashBytes);
  return os.str();
}

bool VerifyPassword(const std::string &plain, const std::string &stored) {
  // 解析 pbkdf2$iter$salt_hex$hash_hex
  std::string prefix = "pbkdf2$";
  if (stored.compare(0, prefix.size(), prefix) != 0) return false;
  size_t p1 = stored.find('$', prefix.size());
  if (p1 == std::string::npos) return false;
  long iters = std::strtol(stored.c_str() + prefix.size(), nullptr, 10);
  if (iters <= 0) return false;

  size_t p2 = stored.find('$', p1 + 1);
  if (p2 == std::string::npos) return false;
  std::string salt_hex = stored.substr(p1 + 1, p2 - p1 - 1);
  std::string hash_hex = stored.substr(p2 + 1);

  std::vector<unsigned char> salt, expect;
  if (!HexDecode(salt_hex, salt) || salt.empty()) return false;
  if (!HexDecode(hash_hex, expect) || expect.empty()) return false;

  std::vector<unsigned char> got(kHashBytes);
  if (PKCS5_PBKDF2_HMAC(plain.c_str(), static_cast<int>(plain.size()), salt.data(),
                        static_cast<int>(salt.size()), static_cast<int>(iters),
                        EVP_sha256(), static_cast<int>(got.size()), got.data()) != 1) {
    return false;
  }
  // 常数时间比较。
  if (got.size() != expect.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < got.size(); ++i) diff |= got[i] ^ expect[i];
  return diff == 0;
}

}  // namespace oj