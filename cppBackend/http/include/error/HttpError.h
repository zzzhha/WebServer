#pragma once

#include <cstdint>
#include <string>

#include "core/HttpResponse.h"

enum class HttpErrorStage : uint8_t {
  UNKNOWN = 0,
  SSL = 1,
  PARSING = 2,
  VALIDATION = 3,
  ROUTING = 4
};

enum class HttpErrc : uint16_t {
  OK = 0,

  SSL_HANDSHAKE_FAILED = 1000,

  PARSE_EMPTY_INPUT = 1100,
  PARSE_TIMEOUT = 1101,
  PARSE_INVALID_START_LINE = 1102,
  PARSE_INVALID_HEADER = 1103,
  PARSE_UNSUPPORTED_VERSION = 1104,
  PARSE_HEADER_TOO_LARGE = 1105,
  PARSE_BODY_TOO_LARGE = 1106,
  PARSE_INVALID_CHUNKED_ENCODING = 1107,
  PARSE_FAILED = 1199,

  VALIDATION_CONFLICTING_LENGTH = 1200,
  VALIDATION_CONTENT_LENGTH_INVALID = 1201,
  VALIDATION_CONTENT_LENGTH_MISMATCH = 1202,
  VALIDATION_MISSING_HOST = 1203,
  VALIDATION_UNKNOWN_METHOD = 1204,
  VALIDATION_METHOD_NOT_ALLOWED = 1205,
  VALIDATION_EMPTY_PATH = 1206,
  VALIDATION_URL_TOO_LONG = 1207,
  VALIDATION_HEADERS_TOO_MANY = 1208,
  VALIDATION_HEADER_VALUE_TOO_LARGE = 1209,
  VALIDATION_PATH_UNSAFE = 1210,
  VALIDATION_SUSPICIOUS_PATTERN = 1211,
  VALIDATION_BODY_TOO_LARGE = 1212,
  VALIDATION_FAILED = 1299,

  ROUTE_NOT_FOUND = 1300,

  INTERNAL_ERROR = 2000
};

struct HttpErrorContext {
  HttpErrorStage stage{HttpErrorStage::UNKNOWN};
  int parser_result{0};
  size_t consumed_bytes{0};
  size_t received_bytes{0};

  std::string method;
  std::string url;
  std::string path;
  std::string version;

  std::string header_key;
  std::string detail;
};

struct HttpError {
  HttpErrc code{HttpErrc::OK};
  HttpStatusCode status{HttpStatusCode::OK};
  std::string message;
  HttpErrorContext ctx;
  std::string stack;

  bool IsOk() const { return code == HttpErrc::OK; }
  int HttpStatus() const { return static_cast<int>(status); }
  bool IsClientError() const { return HttpStatus() >= 400 && HttpStatus() < 500; }
  bool IsServerError() const { return HttpStatus() >= 500 && HttpStatus() < 600; }
};
