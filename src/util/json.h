// json.h — 极简 JSON 解析/序列化（header-only，单进程够用），无第三方依赖。
// 支持 null / boolean / number / string / array / object。
// 数值统一存为 double，int 场景用 (long long) 强转；本项目接口体量足够。

#ifndef OJ_JSON_H
#define OJ_JSON_H

#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace oj {
namespace json {

class Value;
using Object = std::map<std::string, Value>;

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() : type_(Type::Null) {}
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(double n) : type_(Type::Number), num_(n) {}
  Value(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Value(const char *s) : type_(Type::String), str_(s) {}
  Value(const std::string &s) : type_(Type::String), str_(s) {}
  Value(Object obj) : type_(Type::Object), obj_(std::move(obj)) {}
  Value(std::vector<Value> arr) : type_(Type::Array), arr_(std::move(arr)) {}

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool() const { return bool_; }
  double as_number() const { return num_; }
  const std::string &as_string() const { return str_; }
  const std::vector<Value> &as_array() const { return arr_; }
  const Object &as_object() const { return obj_; }

  // object 便捷访问；键不存在或未包含该成员时返回缺省构造的 Value。
  const Value &get(const std::string &key) const {
    static const Value kNull;
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
  }
  bool has(const std::string &key) const {
    return type_ == Type::Object && obj_.count(key) != 0;
  }

  std::string dump() const {
    std::ostringstream os;
    Dump(os);
    return os.str();
  }

 private:
  static void EscapeString(std::ostream &os, const std::string &s) {
    os << '"';
    for (char c : s) {
      switch (c) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<int>(c));
            os << buf;
          } else {
            os << c;
          }
      }
    }
    os << '"';
  }

  void Dump(std::ostream &os) const {
    switch (type_) {
      case Type::Null: os << "null"; break;
      case Type::Bool: os << (bool_ ? "true" : "false"); break;
      case Type::Number:
        if (num_ == static_cast<long long>(num_) && std::isfinite(num_)) {
          os << static_cast<long long>(num_);  // 整数不带小数
        } else {
          os << num_;
        }
        break;
      case Type::String: EscapeString(os, str_); break;
      case Type::Array: {
        os << '[';
        for (size_t i = 0; i < arr_.size(); ++i) {
          if (i) os << ',';
          arr_[i].Dump(os);
        }
        os << ']';
        break;
      }
      case Type::Object: {
        os << '{';
        size_t i = 0;
        for (const auto &kv : obj_) {
          if (i++) os << ',';
          EscapeString(os, kv.first);
          os << ':';
          kv.second.Dump(os);
        }
        os << '}';
        break;
      }
    }
  }

  Type type_;
  bool bool_{false};
  double num_{0.0};
  std::string str_;
  std::vector<Value> arr_;
  Object obj_;
};

// 解析失败返回 null；err 可选，写入错误描述。
Value Parse(const std::string &s, bool *ok = nullptr);

}  // namespace json
}  // namespace oj

// ---- 实现 ----
namespace oj {
namespace json {

namespace detail {

struct Parser {
  const std::string &s;
  size_t i = 0;
  bool ok = true;
  std::string err;

  explicit Parser(const std::string &str) : s(str) {}

  void skip_ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }

  void fail(const std::string &msg) {
    if (ok) {
      ok = false;
      err = msg;
    }
  }

  bool match(char c) {
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }

  bool parse_hex4(int &out) {
    if (i + 4 > s.size()) return false;
    int v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i + k];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      v = (v << 4) | d;
    }
    i += 4;
    out = v;
    return true;
  }

  std::string parse_string() {
    // 已消费起始双引号
    std::string out;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return out;
      if (c == '\\') {
        if (i >= s.size()) break;
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            int cp;
            if (!parse_hex4(cp)) { fail("bad \\u"); return out; }
            // 仅处理基本多语言平面（本项目无需代理对）。
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                i += 2;
                int lo;
                if (parse_hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                  fail("bad surrogate");
                  return out;
                }
              } else {
                fail("bad surrogate");
                return out;
              }
            }
            if (cp < 0x80) {
              out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out.push_back(static_cast<char>((cp >> 6) | 0xC0));
              out.push_back(static_cast<char>((cp & 0x3F) | 0x80));
            } else if (cp < 0x10000) {
              out.push_back(static_cast<char>((cp >> 12) | 0xE0));
              out.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
              out.push_back(static_cast<char>((cp & 0x3F) | 0x80));
            } else {
              out.push_back(static_cast<char>((cp >> 18) | 0xF0));
              out.push_back(static_cast<char>(((cp >> 12) & 0x3F) | 0x80));
              out.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
              out.push_back(static_cast<char>((cp & 0x3F) | 0x80));
            }
            break;
          }
          default: fail("bad escape"); return out;
        }
      } else {
        out.push_back(c);
      }
    }
    fail("unterminated string");
    return out;
  }

  Value parse_value() {
    skip_ws();
    if (i >= s.size()) { fail("unexpected end"); return {}; }
    char c = s[i];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') { ++i; return Value(parse_string()); }
    if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; return Value(true); } fail("bad literal"); return {}; }
    if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; return Value(false); } fail("bad literal"); return {}; }
    if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; return Value(); } fail("bad literal"); return {}; }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    fail("unexpected token");
    return {};
  }

  Value parse_number() {
    skip_ws();
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    return Value(std::strtod(s.c_str() + start, nullptr));
  }

  Value parse_array() {
    ++i;  // '['
    std::vector<Value> arr;
    skip_ws();
    if (match(']')) return Value(std::move(arr));
    for (;;) {
      arr.push_back(parse_value());
      if (!ok) return {};
      skip_ws();
      if (match(']')) return Value(std::move(arr));
      if (!match(',')) { fail("expected ',' or ']'"); return {}; }
    }
  }

  Value parse_object() {
    ++i;  // '{'
    Object obj;
    skip_ws();
    if (match('}')) return Value(std::move(obj));
    for (;;) {
      skip_ws();
      if (!match('"')) { fail("expected string key"); return {}; }
      std::string key = parse_string();
      if (!ok) return {};
      skip_ws();
      if (!match(':')) { fail("expected ':'"); return {}; }
      obj[key] = parse_value();
      if (!ok) return {};
      skip_ws();
      if (match('}')) return Value(std::move(obj));
      if (!match(',')) { fail("expected ',' or '}'"); return {}; }
    }
  }
};

}  // namespace detail

inline Value Parse(const std::string &s, bool *ok) {
  detail::Parser p(s);
  bool g = true;
  if (!ok) ok = &g;
  Value v = p.parse_value();
  p.skip_ws();
  if (p.ok && p.i == s.size()) {
    *ok = true;
    return v;
  }
  *ok = false;
  return {};
}

}  // namespace json
}  // namespace oj

#endif  // OJ_JSON_H