#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/HttpRequest.h"

// 便捷的 HttpRequest 构建器，支持链式设置方法/URL/头/查询参数/Body。
class RequestBuilder {
public:
  // 创建空请求（默认 GET / HTTP/1.1）
  static std::shared_ptr<HttpRequest> CreateEmpty();

  // 直接将原始字符串作为 body（用于简易场景）
  static std::shared_ptr<HttpRequest> FromRaw(const std::string& raw);

  // 构建器实例
  static RequestBuilder New();

  RequestBuilder& Method(HttpMethod method);
  RequestBuilder& Method(std::string_view methodStr); // 自动大小写规整
  RequestBuilder& Url(std::string_view url);
  RequestBuilder& Path(std::string_view path);
  RequestBuilder& Version(HttpVersion version);
  RequestBuilder& Header(const std::string& key, const std::string& value);
  RequestBuilder& Headers(const std::vector<std::pair<std::string, std::string>>& headers);
  RequestBuilder& QueryParam(const std::string& key, const std::string& value);
  RequestBuilder& Body(const std::string& body);

  std::shared_ptr<HttpRequest> Build();

private:
  RequestBuilder();
  explicit RequestBuilder(std::shared_ptr<HttpRequest> req);

  std::shared_ptr<HttpRequest> request_;
};