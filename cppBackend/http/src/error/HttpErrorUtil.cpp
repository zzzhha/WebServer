#include "error/HttpErrorUtil.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <vector>

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#include <cstdlib>
#endif

std::string ToString(HttpErrc code) {
  switch (code) {
    case HttpErrc::OK: return "OK";
    case HttpErrc::SSL_HANDSHAKE_FAILED: return "SSL_HANDSHAKE_FAILED";
    case HttpErrc::PARSE_EMPTY_INPUT: return "PARSE_EMPTY_INPUT";
    case HttpErrc::PARSE_TIMEOUT: return "PARSE_TIMEOUT";
    case HttpErrc::PARSE_INVALID_START_LINE: return "PARSE_INVALID_START_LINE";
    case HttpErrc::PARSE_INVALID_HEADER: return "PARSE_INVALID_HEADER";
    case HttpErrc::PARSE_UNSUPPORTED_VERSION: return "PARSE_UNSUPPORTED_VERSION";
    case HttpErrc::PARSE_HEADER_TOO_LARGE: return "PARSE_HEADER_TOO_LARGE";
    case HttpErrc::PARSE_BODY_TOO_LARGE: return "PARSE_BODY_TOO_LARGE";
    case HttpErrc::PARSE_INVALID_CHUNKED_ENCODING: return "PARSE_INVALID_CHUNKED_ENCODING";
    case HttpErrc::PARSE_FAILED: return "PARSE_FAILED";
    case HttpErrc::VALIDATION_CONFLICTING_LENGTH: return "VALIDATION_CONFLICTING_LENGTH";
    case HttpErrc::VALIDATION_CONTENT_LENGTH_INVALID: return "VALIDATION_CONTENT_LENGTH_INVALID";
    case HttpErrc::VALIDATION_CONTENT_LENGTH_MISMATCH: return "VALIDATION_CONTENT_LENGTH_MISMATCH";
    case HttpErrc::VALIDATION_MISSING_HOST: return "VALIDATION_MISSING_HOST";
    case HttpErrc::VALIDATION_UNKNOWN_METHOD: return "VALIDATION_UNKNOWN_METHOD";
    case HttpErrc::VALIDATION_METHOD_NOT_ALLOWED: return "VALIDATION_METHOD_NOT_ALLOWED";
    case HttpErrc::VALIDATION_EMPTY_PATH: return "VALIDATION_EMPTY_PATH";
    case HttpErrc::VALIDATION_URL_TOO_LONG: return "VALIDATION_URL_TOO_LONG";
    case HttpErrc::VALIDATION_HEADERS_TOO_MANY: return "VALIDATION_HEADERS_TOO_MANY";
    case HttpErrc::VALIDATION_HEADER_VALUE_TOO_LARGE: return "VALIDATION_HEADER_VALUE_TOO_LARGE";
    case HttpErrc::VALIDATION_PATH_UNSAFE: return "VALIDATION_PATH_UNSAFE";
    case HttpErrc::VALIDATION_SUSPICIOUS_PATTERN: return "VALIDATION_SUSPICIOUS_PATTERN";
    case HttpErrc::VALIDATION_BODY_TOO_LARGE: return "VALIDATION_BODY_TOO_LARGE";
    case HttpErrc::VALIDATION_FAILED: return "VALIDATION_FAILED";
    case HttpErrc::ROUTE_NOT_FOUND: return "ROUTE_NOT_FOUND";
    case HttpErrc::INTERNAL_ERROR: return "INTERNAL_ERROR";
  }
  return "UNKNOWN";
}

std::string ToString(HttpErrorStage stage) {
  switch (stage) {
    case HttpErrorStage::UNKNOWN: return "unknown";
    case HttpErrorStage::SSL: return "ssl";
    case HttpErrorStage::PARSING: return "parsing";
    case HttpErrorStage::VALIDATION: return "validation";
    case HttpErrorStage::ROUTING: return "routing";
  }
  return "unknown";
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::ostringstream oss;
          oss << "\\u" << std::hex << std::uppercase
              << static_cast<int>(static_cast<unsigned char>(c));
          out += oss.str();
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string CaptureStackTrace(size_t max_frames) {
#if __has_include(<execinfo.h>)
  std::vector<void*> frames;
  frames.resize(std::max<size_t>(1, max_frames));
  int n = ::backtrace(frames.data(), static_cast<int>(frames.size()));
  if (n <= 0) return {};
  char** syms = ::backtrace_symbols(frames.data(), n);
  if (!syms) return {};
  std::string out;
  for (int i = 0; i < n; ++i) {
    out.append(syms[i]);
    out.push_back('\n');
  }
  std::free(syms);
  return out;
#else
  (void)max_frames;
  return {};
#endif
}

static std::string NowIso8601Utc() {
  using clock = std::chrono::system_clock;
  auto now = clock::now();
  std::time_t t = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

std::string BuildHttpErrorJson(const HttpError& err,
                               std::string_view request_id,
                               bool include_context) {
  std::ostringstream json;
  const std::string ts = NowIso8601Utc();
  const std::string code = ToString(err.code);
  json << "{";
  json << "\"success\":false";
  json << ",\"code\":\"" << JsonEscape(code) << "\"";
  json << ",\"message\":\"" << JsonEscape(err.message) << "\"";
  json << ",\"timestamp\":\"" << JsonEscape(ts) << "\"";
  json << ",\"details\":{";
  json << "\"http_status\":" << static_cast<int>(err.status);
  json << ",\"stage\":\"" << JsonEscape(ToString(err.ctx.stage)) << "\"";
  json << ",\"request_id\":\"" << JsonEscape(request_id) << "\"";

  if (include_context) {
    json << ",\"context\":{";
    bool first = true;
    auto add_kv = [&](std::string_view k, std::string_view v) {
      if (v.empty()) return;
      if (!first) json << ",";
      json << "\"" << JsonEscape(k) << "\":\"" << JsonEscape(v) << "\"";
      first = false;
    };
    auto add_num = [&](std::string_view k, long long v, bool emit) {
      if (!emit) return;
      if (!first) json << ",";
      json << "\"" << JsonEscape(k) << "\":" << v;
      first = false;
    };

    add_kv("method", err.ctx.method);
    add_kv("url", err.ctx.url);
    add_kv("path", err.ctx.path);
    add_kv("version", err.ctx.version);
    add_kv("header", err.ctx.header_key);
    add_kv("detail", err.ctx.detail);
    add_num("parser_result", err.ctx.parser_result, err.ctx.parser_result != 0);
    add_num("consumed_bytes", static_cast<long long>(err.ctx.consumed_bytes), err.ctx.consumed_bytes != 0);
    add_num("received_bytes", static_cast<long long>(err.ctx.received_bytes), err.ctx.received_bytes != 0);

    json << "}";
  }

  json << "}";

  json << ",\"error\":{";
  json << "\"code\":\"" << JsonEscape(code) << "\"";
  json << ",\"message\":\"" << JsonEscape(err.message) << "\"";
  json << ",\"details\":{";
  json << "\"http_status\":" << static_cast<int>(err.status);
  json << ",\"stage\":\"" << JsonEscape(ToString(err.ctx.stage)) << "\"";
  json << ",\"request_id\":\"" << JsonEscape(request_id) << "\"";
  if (include_context) {
    json << ",\"context\":{";
    bool first2 = true;
    auto add_kv2 = [&](std::string_view k, std::string_view v) {
      if (v.empty()) return;
      if (!first2) json << ",";
      json << "\"" << JsonEscape(k) << "\":\"" << JsonEscape(v) << "\"";
      first2 = false;
    };
    auto add_num2 = [&](std::string_view k, long long v, bool emit) {
      if (!emit) return;
      if (!first2) json << ",";
      json << "\"" << JsonEscape(k) << "\":" << v;
      first2 = false;
    };
    add_kv2("method", err.ctx.method);
    add_kv2("url", err.ctx.url);
    add_kv2("path", err.ctx.path);
    add_kv2("version", err.ctx.version);
    add_kv2("header", err.ctx.header_key);
    add_kv2("detail", err.ctx.detail);
    add_num2("parser_result", err.ctx.parser_result, err.ctx.parser_result != 0);
    add_num2("consumed_bytes", static_cast<long long>(err.ctx.consumed_bytes), err.ctx.consumed_bytes != 0);
    add_num2("received_bytes", static_cast<long long>(err.ctx.received_bytes), err.ctx.received_bytes != 0);
    json << "}";
  }
  json << "}";
  json << ",\"timestamp\":\"" << JsonEscape(ts) << "\"";
  json << "}";

  json << "}";
  return json.str();
}
