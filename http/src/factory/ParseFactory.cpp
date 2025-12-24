#include "factory/HttpParseFactory.h"

#include "parsers/IHttpParser.h"

namespace {
ParseStrategy& StrategyInstance() {
  static ParseStrategy strategy;
  return strategy;
}
}  // namespace

std::shared_ptr<IHttpParser> HttpParseFactory::Create(const char* data, size_t len) {
  return StrategyInstance().CreateByPeek(data, len);
}

std::shared_ptr<IHttpParser> HttpParseFactory::Create(const std::string& data) {
  return StrategyInstance().CreateByPeek(data);
}

std::shared_ptr<IHttpParser> HttpParseFactory::CreateHttp1() {
  return StrategyInstance().CreateHttp1();
}

std::shared_ptr<IHttpParser> HttpParseFactory::CreateHttp2() {
  return StrategyInstance().CreateHttp2();
}

