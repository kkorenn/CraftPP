// Minimal JSON value + parser for vanilla data files. Object key order is preserved.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object };
  Type type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;
  // Special members defined after the class: clang 14 + libstdc++ 12 (Raspberry Pi OS) otherwise instantiates
  // ~vector<pair<string, Json>> while Json is still incomplete.
  Json() = default;
  Json(const Json&);
  Json(Json&&) noexcept;
  Json& operator=(const Json&);
  Json& operator=(Json&&) noexcept;
  ~Json();

  bool isNull() const { return type == Null; }
  bool isBool() const { return type == Bool; }
  bool isNumber() const { return type == Number; }
  bool isString() const { return type == String; }
  bool isArray() const { return type == Array; }
  bool isObject() const { return type == Object; }

  const Json* find(std::string_view key) const {
    for (auto& [k, v] : obj)
      if (k == key) return &v;
    return nullptr;
  }
  bool has(std::string_view key) const { return find(key) != nullptr; }
  const Json& operator[](std::string_view key) const {
    if (auto* v = find(key)) return *v;
    throw std::runtime_error("json: missing key '" + std::string(key) + "'");
  }
  const Json& operator[](size_t i) const { return arr.at(i); }
  size_t size() const { return type == Array ? arr.size() : obj.size(); }

  double asDouble() const { expect(Number); return num; }
  float asFloat() const { expect(Number); return std::strtof(str.c_str(), nullptr); }
  int asInt() const { expect(Number); return (int)num; }
  bool asBool() const { expect(Bool); return b; }
  const std::string& asString() const { expect(String); return str; }
  double getDouble(std::string_view key, double def) const { auto* v = find(key); return v ? v->asDouble() : def; }
  int getInt(std::string_view key, int def) const { auto* v = find(key); return v ? v->asInt() : def; }

  static Json parse(std::string_view text) {
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing characters");
    return v;
  }
  static Json parseFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("json: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
  }

 private:
  void expect(Type t) const {
    if (type != t) throw std::runtime_error("json: wrong value type");
  }

  struct Parser {
    std::string_view s;
    size_t i = 0;

    [[noreturn]] void fail(const char* msg) {
      throw std::runtime_error(std::string("json: ") + msg + " at offset " + std::to_string(i));
    }
    void ws() {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    char peek() { return i < s.size() ? s[i] : '\0'; }
    bool lit(std::string_view w) {
      if (s.substr(i, w.size()) != w) return false;
      i += w.size();
      return true;
    }
    Json value() {
      ws();
      Json v;
      char c = peek();
      if (c == '{') {
        v.type = Object;
        i++;
        ws();
        if (peek() == '}') { i++; return v; }
        for (;;) {
          ws();
          if (peek() != '"') fail("expected key");
          std::string k = string();
          ws();
          if (peek() != ':') fail("expected ':'");
          i++;
          v.obj.emplace_back(std::move(k), value());
          ws();
          if (peek() == ',') { i++; continue; }
          if (peek() == '}') { i++; return v; }
          fail("expected ',' or '}'");
        }
      }
      if (c == '[') {
        v.type = Array;
        i++;
        ws();
        if (peek() == ']') { i++; return v; }
        for (;;) {
          v.arr.push_back(value());
          ws();
          if (peek() == ',') { i++; continue; }
          if (peek() == ']') { i++; return v; }
          fail("expected ',' or ']'");
        }
      }
      if (c == '"') { v.type = String; v.str = string(); return v; }
      if (lit("true")) { v.type = Bool; v.b = true; return v; }
      if (lit("false")) { v.type = Bool; return v; }
      if (lit("null")) return v;
      if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = i;
        while (i < s.size() && (std::string_view("+-.eE").find(s[i]) != std::string_view::npos || (s[i] >= '0' && s[i] <= '9'))) i++;
        std::string tok(s.substr(start, i - start));
        char* end = nullptr;
        v.type = Number;
        v.num = std::strtod(tok.c_str(), &end);  // correctly rounded (C locale assumed)
        if (end != tok.c_str() + tok.size()) fail("bad number");
        v.str = tok;  // raw token: Codec.FLOAT reads Float.parseFloat(token), not (float)double
        return v;
      }
      fail("unexpected character");
    }
    unsigned hex4() {
      if (i + 4 > s.size()) fail("bad \\u escape");
      unsigned u = 0;
      for (int k = 0; k < 4; k++) {
        char h = s[i++];
        u <<= 4;
        if (h >= '0' && h <= '9') u |= h - '0';
        else if (h >= 'a' && h <= 'f') u |= h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') u |= h - 'A' + 10;
        else fail("bad hex digit");
      }
      return u;
    }
    std::string string() {
      i++;  // opening quote
      std::string out;
      for (;;) {
        if (i >= s.size()) fail("unterminated string");
        char c = s[i++];
        if (c == '"') return out;
        if (c != '\\') { out += c; continue; }
        if (i >= s.size()) fail("bad escape");
        char e = s[i++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            uint32_t cp = hex4();
            if (cp >= 0xD800 && cp < 0xDC00 && s.substr(i, 2) == "\\u") {
              i += 2;
              uint32_t lo = hex4();
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
          }
          default: fail("bad escape");
        }
      }
    }
  };
};
inline Json::Json(const Json&) = default;
inline Json::Json(Json&&) noexcept = default;
inline Json& Json::operator=(const Json&) = default;
inline Json& Json::operator=(Json&&) noexcept = default;
inline Json::~Json() = default;

