#pragma once

#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "router/Router.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <map>

/**
 * AppHandlers.h：应用处理器工具函数
 * 职责：提供HTTP请求处理的工具函数，不包含业务逻辑
 * - 工具函数：解析表单数据、生成JSON响应、获取Content-Type等
 * - 业务逻辑已迁移到services层（AuthService、DownloadService等）
 * - 静态文件服务已迁移到StaticFileService
 */

// 解析 POST 表单数据 (application/x-www-form-urlencoded)
std::unordered_map<std::string, std::string> ParseFormData(const std::string& body);

// 生成 JSON 响应
void SetJsonResponse(HttpResponse& response, bool success, const std::string& message, HttpStatusCode statusCode = HttpStatusCode::OK);

// 获取 Content-Type 根据文件扩展名
std::string GetContentType(const std::string& path);

// JSON成功响应
void SetJsonSuccessResponse(HttpResponse& response, const std::string& message = "操作成功");
void SetJsonSuccessResponseWithData(HttpResponse& response, const std::string& data, const std::string& message = "操作成功");

// JSON错误响应
void SetJsonErrorResponse(HttpResponse& response, HttpStatusCode code, const std::string& message);
void SetJsonErrorResponseWithDetails(HttpResponse& response, HttpStatusCode code, const std::string& message, const std::map<std::string, std::string>& details);

// 文件下载响应
bool SetFileDownloadResponse(HttpResponse& response, const std::string& filepath, const std::string& filename = "");

// 静态文件响应
bool SetStaticFileResponse(HttpResponse& response, const std::string& filepath, const std::string& contentType = "");

// 重定向响应
void SetRedirectResponse(HttpResponse& response, const std::string& url, bool permanent = false);

// HTML响应
void SetHtmlResponse(HttpResponse& response, const std::string& html, HttpStatusCode statusCode = HttpStatusCode::OK);

// 文本响应
void SetTextResponse(HttpResponse& response, const std::string& text, HttpStatusCode statusCode = HttpStatusCode::OK);



