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

  // JSON响应方法
  ResponseBuilder& Json(bool success, const std::string& message);
  ResponseBuilder& Json(bool success, const std::string& message, const std::string& data);
  ResponseBuilder& Json(const std::string& jsonBody);

  // 错误响应方法
  ResponseBuilder& Error(HttpStatusCode code, const std::string& message);
  ResponseBuilder& Error(int code, const std::string& message, const std::string& reason = "");

  // 文件下载响应方法
  ResponseBuilder& FileDownload(const std::string& filename, const std::string& filepath);

  // 静态文件响应方法
  ResponseBuilder& StaticFile(const std::string& filepath, const std::string& contentType);

  // 重定向响应方法
  ResponseBuilder& Redirect(const std::string& url, bool permanent = false);

  std::shared_ptr<HttpResponse> Build();

private:
  ResponseBuilder();
  explicit ResponseBuilder(std::shared_ptr<HttpResponse> resp);

  std::shared_ptr<HttpResponse> response_;
};