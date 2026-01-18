#pragma once

#include <memory>
#include <string>

#include "parsers/IHttpParser.h"

// 解析策略：根据数据或配置选择具体 HTTP 解析器
class ParseStrategy {
public:
  ParseStrategy() = default;
  ~ParseStrategy() = default;

  // 根据输入数据的特征选择合适的解析器（简单嗅探）
  std::shared_ptr<IHttpParser> CreateByPeek(const char* data, size_t len) const;
  std::shared_ptr<IHttpParser> CreateByPeek(const std::string& data) const;

  // 显式选择
  std::shared_ptr<IHttpParser> CreateHttp1() const;
  std::shared_ptr<IHttpParser> CreateHttp2() const;
};