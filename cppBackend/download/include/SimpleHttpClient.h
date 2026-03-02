#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

struct HttpResponseData {
  int status_code{0};
  std::string reason;
  std::map<std::string, std::string> headers;
  std::string body;
};

class SimpleHttpClient {
 public:
  static bool Head(const std::string& host, uint16_t port, const std::string& path, int timeout_ms,
                   HttpResponseData& out, std::string& error);

  static bool GetRange(const std::string& host, uint16_t port, const std::string& path, uint64_t start, uint64_t end,
                       int timeout_ms, HttpResponseData& out, std::string& error);
};

