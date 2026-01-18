#pragma once

#include <memory>
#include <string>

#include "strategy/ParseStrategy.h"

// 工厂：对外提供解析器创建接口，可基于策略嗅探或显式指定
class HttpParseFactory {
public:
  // 嗅探数据，自动选择 HTTP/1 或 HTTP/2 解析器
  static std::shared_ptr<IHttpParser> Create(const char* data, size_t len);
  static std::shared_ptr<IHttpParser> Create(const std::string& data);

  // 显式指定
  static std::shared_ptr<IHttpParser> CreateHttp1();
  static std::shared_ptr<IHttpParser> CreateHttp2();

};