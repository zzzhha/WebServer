#include "strategy/ParseStrategy.h"

#include <string_view>

#include "parsers/Http1Parser.h"
#include "parsers/Http2Parser.h"

namespace {
constexpr std::string_view kHttp2Preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
}

std::shared_ptr<IHttpParser> ParseStrategy::CreateByPeek(const char* data, size_t len) const {
  if (data && len >= kHttp2Preface.size()) {
    if (std::string_view(data, kHttp2Preface.size()) == kHttp2Preface) {
      return CreateHttp2();
    }
  }
  return CreateHttp1();
}

std::shared_ptr<IHttpParser> ParseStrategy::CreateByPeek(const std::string& data) const {
  return CreateByPeek(data.data(), data.size());
}

std::shared_ptr<IHttpParser> ParseStrategy::CreateHttp1() const {
  return std::make_shared<Http1Parser>();
}

std::shared_ptr<IHttpParser> ParseStrategy::CreateHttp2() const {
  return std::make_shared<Http2Parser>();
}

