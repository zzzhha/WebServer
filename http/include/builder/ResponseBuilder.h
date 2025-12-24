#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/HttpResponse.h"

// 便捷的 HttpResponse 构建器，支持链式设置状态行、头、Body 等。
class ResponseBuilder {
public:
  // 直接创建基础响应
  static std::shared_ptr<HttpResponse> Create(HttpStatusCode code = HttpStatusCode::OK);
  static std::shared_ptr<HttpResponse> WithBody(const std::string& body,
                                                const std::string& contentType = "text/plain");

  // 构建器实例
  static ResponseBuilder New();

  ResponseBuilder& Status(HttpStatusCode code);
  ResponseBuilder& Status(int code, std::string_view reason = "");
  ResponseBuilder& Reason(std::string_view reason);
  ResponseBuilder& Version(HttpVersion version);
  ResponseBuilder& Header(const std::string& key, const std::string& value);
  ResponseBuilder& Headers(const std::vector<std::pair<std::string, std::string>>& headers);
  ResponseBuilder& ContentType(const std::string& contentType);
  ResponseBuilder& ContentEncoding(HttpContentEncoding encoding);
  ResponseBuilder& Body(const std::string& body);

  std::shared_ptr<HttpResponse> Build();

private:
  ResponseBuilder();
  explicit ResponseBuilder(std::shared_ptr<HttpResponse> resp);

  std::shared_ptr<HttpResponse> response_;
};