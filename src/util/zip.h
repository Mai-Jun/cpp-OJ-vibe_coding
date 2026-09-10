// zip.h — 极简 ZIP 解析（header-only，zlib inflate），仅支持 store(0)/deflate(8)。
// 用途：管理后台上传测试用例 .zip（case_N.in / case_N.out 配对）时解析。
// 安全：拒绝目录项、路径穿越（..）、绝对路径与未知压缩算法。

#ifndef OJ_ZIP_H
#define OJ_ZIP_H

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace oj {
namespace zip {

struct Entry {
  std::string name;
  std::vector<uint8_t> data;
};

namespace detail {

inline uint16_t RdU16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t RdU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace detail

// 原始 deflate（windowBits=-15）解压，按需扩容，防止声明尺寸过小被截断。
inline bool InflateRaw(const uint8_t *src, size_t src_len, size_t expected,
                       std::vector<uint8_t> &out, std::string &err) {
  z_stream zs;
  std::memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, -15) != Z_OK) {
    err = "zlib init failed";
    return false;
  }
  zs.next_in = const_cast<Bytef *>(src);
  zs.avail_in = static_cast<uInt>(src_len);
  out.clear();
  uint8_t buf[16384];
  int ret = Z_OK;
  while (ret == Z_OK) {
    zs.next_out = buf;
    zs.avail_out = sizeof buf;
    ret = inflate(&zs, Z_NO_FLUSH);
    size_t got = sizeof buf - zs.avail_out;
    out.insert(out.end(), buf, buf + got);
  }
  inflateEnd(&zs);
  if (ret != Z_STREAM_END) {
    err = "inflate failed";
    return false;
  }
  if (expected != 0 && out.size() != expected) {
    err = "inflate size mismatch";
    return false;
  }
  return true;
}

// 解析 ZIP 字节流为条目列表；失败返回 false 并写入 err。
inline bool Parse(const std::string &bytes, std::vector<Entry> &out, std::string &err) {
  out.clear();
  const uint8_t *p = reinterpret_cast<const uint8_t *>(bytes.data());
  size_t n = bytes.size();
  if (n < 22) {
    err = "not a zip (too small)";
    return false;
  }

  // 从末尾向前搜索 EOCD 签名（至多扫描最后 64KB+22B）。
  size_t eocd = n;
  size_t start = n > 65557 ? n - 65557 : 0;
  for (size_t i = n; i-- > start;) {
    if (i + 4 <= n && p[i] == 0x50 && p[i + 1] == 0x4b && p[i + 2] == 0x05 && p[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd == n || eocd + 22 > n) {
    err = "not a zip (EOCD not found)";
    return false;
  }
  uint16_t total = detail::RdU16(p + eocd + 10);
  uint32_t cd_size = detail::RdU32(p + eocd + 12);
  uint32_t cd_off = detail::RdU32(p + eocd + 16);
  if (cd_off > n || cd_size > n || cd_off + cd_size > n) {
    err = "bad central directory";
    return false;
  }

  size_t pos = cd_off;
  size_t end = cd_off + cd_size;
  for (uint16_t k = 0; k < total; ++k) {
    if (pos + 46 > end) {
      err = "truncated central directory entry";
      return false;
    }
    if (detail::RdU32(p + pos) != 0x02014b50u) {
      err = "bad central directory signature";
      return false;
    }
    uint16_t method = detail::RdU16(p + pos + 10);
    uint32_t comp_size = detail::RdU32(p + pos + 20);
    uint32_t uncomp_size = detail::RdU32(p + pos + 24);
    uint16_t name_len = detail::RdU16(p + pos + 28);
    uint16_t extra_len = detail::RdU16(p + pos + 30);
    uint16_t comment_len = detail::RdU16(p + pos + 32);
    uint32_t local_off = detail::RdU32(p + pos + 42);

    size_t name_pos = pos + 46;
    if (name_pos + name_len > end) {
      err = "truncated central directory name";
      return false;
    }
    std::string name(reinterpret_cast<const char *>(p + name_pos), name_len);
    pos = name_pos + name_len + extra_len + comment_len;

    if (name.empty() || name.back() == '/') continue;  // 目录项跳过
    // 路径穿越防护：拒绝绝对路径与含 ".." 的路径
    if (name[0] == '/' || name.find("..") != std::string::npos) {
      err = "unsafe path in zip: " + name;
      return false;
    }

    if (local_off + 30 > n || detail::RdU32(p + local_off) != 0x04034b50u) {
      err = "bad local file header for " + name;
      return false;
    }
    uint16_t lname_len = detail::RdU16(p + local_off + 26);
    uint16_t lextra_len = detail::RdU16(p + local_off + 28);
    size_t data_off = local_off + 30 + lname_len + lextra_len;
    if (data_off + comp_size > n) {
      err = "truncated data for " + name;
      return false;
    }

    Entry e;
    e.name = name;
    const uint8_t *data = p + data_off;
    if (method == 0) {  // stored
      e.data.assign(data, data + comp_size);
    } else if (method == 8) {  // deflate
      if (!InflateRaw(data, comp_size, uncomp_size, e.data, err)) return false;
    } else {
      err = "unsupported compression method for " + name;
      return false;
    }
    out.push_back(std::move(e));
  }
  return true;
}

}  // namespace zip
}  // namespace oj

#endif  // OJ_ZIP_H
