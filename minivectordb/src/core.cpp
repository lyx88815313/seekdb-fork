#include "core.hpp"

#include <immintrin.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mvdb {

void throw_error(const char* code, const std::string& message) {
  throw DbException(code, message);
}

// ---------------------------------------------------------------------------
// Distance kernel
// ---------------------------------------------------------------------------

namespace {

float l2_distance_scalar(const float* a, const float* b, int dim) {
  float sum = 0.0f;
  for (int i = 0; i < dim; ++i) {
    float difference = a[i] - b[i];
    sum += difference * difference;
  }
  return std::sqrt(sum);
}

__attribute__((target("avx2,fma"))) float l2_distance_avx2(const float* a, const float* b,
                                                           int dim) {
  __m256 sum = _mm256_setzero_ps();
  int i = 0;
  for (; i + 8 <= dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 difference = _mm256_sub_ps(va, vb);
    sum = _mm256_fmadd_ps(difference, difference, sum);
  }
  float lanes[8];
  _mm256_storeu_ps(lanes, sum);
  float total = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
  for (; i < dim; ++i) {
    float difference = a[i] - b[i];
    total += difference * difference;
  }
  return std::sqrt(total);
}

bool cpu_has_avx2() {
  static const bool supported = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return supported;
}

}  // namespace

float l2_distance(const float* a, const float* b, int dim) {
  if (a == nullptr || b == nullptr) return 0.0f;
  if (cpu_has_avx2()) return l2_distance_avx2(a, b, dim);
  return l2_distance_scalar(a, b, dim);
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

namespace {

struct JsonParser {
  const char* p;
  const char* end;

  void skip_space() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }

  [[noreturn]] void fail(const char* what) {
    throw_error("invalid_json", std::string("malformed JSON: ") + what);
  }

  JsonValue parse_value() {
    skip_space();
    if (p >= end) fail("unexpected end of input");
    switch (*p) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': {
        JsonValue value;
        value.kind = JsonValue::Kind::String;
        value.text = parse_string();
        return value;
      }
      case 't':
        expect_literal("true");
        {
          JsonValue value;
          value.kind = JsonValue::Kind::Bool;
          value.boolean = true;
          return value;
        }
      case 'f':
        expect_literal("false");
        {
          JsonValue value;
          value.kind = JsonValue::Kind::Bool;
          value.boolean = false;
          return value;
        }
      case 'n':
        expect_literal("null");
        return JsonValue();
      default: return parse_number();
    }
  }

  void expect_literal(const char* literal) {
    size_t n = std::strlen(literal);
    if (static_cast<size_t>(end - p) < n || std::memcmp(p, literal, n) != 0) {
      fail("unexpected literal");
    }
    p += n;
  }

  JsonValue parse_number() {
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
      ++p;
      digits = true;
    }
    if (p < end && *p == '.') {
      ++p;
      while (p < end && *p >= '0' && *p <= '9') {
        ++p;
        digits = true;
      }
    }
    if (digits && p < end && (*p == 'e' || *p == 'E')) {
      ++p;
      if (p < end && (*p == '-' || *p == '+')) ++p;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (!digits) fail("invalid number");
    JsonValue value;
    value.kind = JsonValue::Kind::Number;
    value.number.assign(start, static_cast<size_t>(p - start));
    return value;
  }

  std::string parse_string() {
    if (p >= end || *p != '"') fail("expected string");
    ++p;
    std::string out;
    while (true) {
      if (p >= end) fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"') {
        ++p;
        return out;
      }
      if (c == '\\') {
        ++p;
        if (p >= end) fail("unterminated escape");
        char esc = *p++;
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            unsigned code = parse_hex4();
            append_utf8(out, code);
            break;
          }
          default: fail("invalid escape");
        }
        continue;
      }
      out.push_back(*p++);
    }
  }

  unsigned parse_hex4() {
    if (end - p < 4) fail("truncated \\u escape");
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p++;
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
      else fail("invalid \\u escape");
    }
    return value;
  }

  static void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  JsonValue parse_array() {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    ++p;  // '['
    skip_space();
    if (p < end && *p == ']') {
      ++p;
      return value;
    }
    while (true) {
      value.array.push_back(parse_value());
      skip_space();
      if (p >= end) fail("unterminated array");
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == ']') {
        ++p;
        return value;
      }
      fail("expected , or ]");
    }
  }

  JsonValue parse_object() {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    ++p;  // '{'
    skip_space();
    if (p < end && *p == '}') {
      ++p;
      return value;
    }
    while (true) {
      skip_space();
      std::string key = parse_string();
      skip_space();
      if (p >= end || *p != ':') fail("expected :");
      ++p;
      value.members.emplace_back(std::move(key), parse_value());
      skip_space();
      if (p >= end) fail("unterminated object");
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == '}') {
        ++p;
        return value;
      }
      fail("expected , or }");
    }
  }
};

}  // namespace

const JsonValue* JsonValue::find(const char* key) const {
  if (kind != Kind::Object) return nullptr;
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

JsonValue json_parse(const std::string& document) {
  JsonParser parser{document.data(), document.data() + document.size()};
  JsonValue value = parser.parse_value();
  parser.skip_space();
  if (parser.p != parser.end) {
    throw_error("invalid_json", "trailing content after JSON document");
  }
  return value;
}

// ---------------------------------------------------------------------------
// JSON writing
// ---------------------------------------------------------------------------

void json_write_string(const std::string& value, std::string& out) {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void json_write_int64(int64_t value, std::string& out) {
  char buffer[32];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, static_cast<size_t>(result.ptr - buffer));
}

void json_write_number(double value, std::string& out) {
  if (!std::isfinite(value)) {
    // The protocol forbids NaN and Infinity in the response stream.
    out += "0";
    return;
  }
  double integral = 0.0;
  if (std::modf(value, &integral) == 0.0 && integral >= -9007199254740992.0 &&
      integral <= 9007199254740992.0) {
    json_write_int64(static_cast<int64_t>(integral), out);
    return;
  }
  char buffer[40];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, static_cast<size_t>(result.ptr - buffer));
}

void json_write_cell(const Cell& cell, std::string& out) {
  switch (cell.kind) {
    case Cell::Kind::Null: out += "null"; break;
    case Cell::Kind::Int: json_write_int64(cell.integer, out); break;
    case Cell::Kind::Double: json_write_number(cell.number, out); break;
    case Cell::Kind::Str: json_write_string(cell.text, out); break;
    case Cell::Kind::Vector: {
      out.push_back('[');
      for (size_t i = 0; i < cell.vector.size(); ++i) {
        if (i) out.push_back(',');
        json_write_number(static_cast<double>(cell.vector[i]), out);
      }
      out.push_back(']');
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Schema helpers
// ---------------------------------------------------------------------------

int TableDef::index_of(const std::string& name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

const ColumnDef* TableDef::column_by_name(const std::string& name) const {
  int index = index_of(name);
  return index < 0 ? nullptr : &columns[index];
}

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------

size_t utf8_length(const std::string& value) {
  size_t count = 0;
  for (unsigned char c : value) {
    if ((c & 0xC0) != 0x80) ++count;  // count lead bytes only
  }
  return count;
}

int utf8_compare(const std::string& left, const std::string& right) {
  return utf8_compare(left.data(), left.size(), right.data(), right.size());
}

int utf8_compare(const char* left, size_t left_length, const char* right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int diff = std::memcmp(left, right, common);
  if (diff != 0) return diff < 0 ? -1 : 1;
  if (left_length == right_length) return 0;
  return left_length < right_length ? -1 : 1;
}

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, used to detect torn WAL records)
// ---------------------------------------------------------------------------

namespace {
struct Crc32Table {
  uint32_t entries[256];
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      entries[i] = c;
    }
  }
};
const Crc32Table kCrcTable;
}  // namespace

uint32_t crc32_update(uint32_t crc, const void* data, size_t length) {
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  crc = ~crc;
  for (size_t i = 0; i < length; ++i) {
    crc = kCrcTable.entries[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

uint32_t crc32_of(const void* data, size_t length) {
  return crc32_update(0, data, length);
}

}  // namespace mvdb
