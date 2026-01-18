#include "handler/SecurityValidationHandler.h"

#include "core/HttpRequest.h"
#include "core/IHttpMessage.h"
#include <algorithm>
#include <cctype>
#include <string>

SecurityValidationHandler::SecurityValidationHandler(
    size_t max_body_size,
    size_t max_url_length,
    size_t max_header_count,
    size_t max_header_value_length)
  : maxBodySize_(max_body_size)
  , maxUrlLength_(max_url_length)
  , maxHeaderCount_(max_header_count)
  , maxHeaderValueLength_(max_header_value_length)
{
  // 默认允许的HTTP方法白名单
  allowedMethods_ = {
    HttpMethod::GET,
    HttpMethod::POST,
    HttpMethod::PUT,
    HttpMethod::DELETE,
    HttpMethod::PATCH,
    HttpMethod::HEAD,
    HttpMethod::OPTIONS
  };
}

bool SecurityValidationHandler::Handle(IHttpMessage& message) {
  if (!message.IsRequest()) {
    return CallNext(message);
  }

  auto* request = dynamic_cast<HttpRequest*>(&message);
  if (!request) {
    return false;
  }

  // 1. 检查HTTP方法是否在白名单中
  if (!IsMethodAllowed(request->GetMethod())) {
    return false;
  }

  // 2. 检查请求体大小
  if (!CheckBodySize(message)) {
    return false;
  }

  // 3. 检查URL长度
  if (!CheckUrlLength(*request)) {
    return false;
  }

  // 4. 检查路径安全性（防止路径遍历攻击）
  if (!CheckPathSecurity(request->GetPath())) {
    return false;
  }

  // 5. 检查Header数量和大小
  if (!CheckHeaders(message)) {
    return false;
  }

  // 6. 检查可疑字符和模式
  if (!CheckSuspiciousPatterns(*request)) {
    return false;
  }

  return CallNext(message);
}

bool SecurityValidationHandler::IsMethodAllowed(HttpMethod method) const {
  return std::find(allowedMethods_.begin(), allowedMethods_.end(), method) 
         != allowedMethods_.end();
}

bool SecurityValidationHandler::CheckBodySize(const IHttpMessage& message) const {
  // 检查Content-Length声明的body大小
  auto contentLength = message.GetHeader("Content-Length");
  if (contentLength) {
    try {
      size_t len = std::stoull(*contentLength);
      if (len > maxBodySize_) {
        return false;
      }
    } catch (...) {
      return false;
    }
  }

  // 检查实际接收到的body大小
  if (message.GetBodyLength() > maxBodySize_) {
    return false;
  }

  return true;
}

bool SecurityValidationHandler::CheckUrlLength(const HttpRequest& request) const {
  const std::string& url = request.GetUrl();
  if (url.length() > maxUrlLength_) {
    return false;
  }
  return true;
}

bool SecurityValidationHandler::CheckPathSecurity(const std::string& path) const {
  if (path.empty()) {
    return false;
  }

  // 检查路径遍历攻击：../, ..\, %2e%2e, %2e%2e%2f等
  std::string lower_path = path;
  std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

  // 检查常见的路径遍历模式
  if (lower_path.find("../") != std::string::npos ||
      lower_path.find("..\\") != std::string::npos ||
      lower_path.find("%2e%2e%2f") != std::string::npos ||
      lower_path.find("%2e%2e%5c") != std::string::npos ||
      lower_path.find("..%2f") != std::string::npos ||
      lower_path.find("..%5c") != std::string::npos) {
    return false;
  }

  // 检查绝对路径（Windows和Unix）
  if (path.length() > 0 && (path[0] == '/' || 
      (path.length() > 1 && path[1] == ':'))) {
    // 允许根路径 /，但拒绝其他绝对路径模式
    if (path != "/" && path.find("://") == std::string::npos) {
      // 这里可以根据需要调整策略
    }
  }

  // 检查空字节注入
  if (path.find('\0') != std::string::npos) {
    return false;
  }

  return true;
}

bool SecurityValidationHandler::CheckHeaders(const IHttpMessage& message) const {
  const auto& headers = message.GetAllHeaders();
  
  // 检查Header数量
  if (headers.size() > maxHeaderCount_) {
    return false;
  }

  // 检查每个Header值的大小
  for (const auto& header : headers) {
    if (header.second.length() > maxHeaderValueLength_) {
      return false;
    }
  }

  return true;
}

bool SecurityValidationHandler::CheckSuspiciousPatterns(const HttpRequest& request) const {
  const std::string& path = request.GetPath();
  const std::string& url = request.GetUrl();

  // 检查SQL注入常见模式（简单检测）
  std::string lower_path = path;
  std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
  
  // 检查常见的SQL注入关键词（简单检测，实际应该更复杂）
  std::vector<std::string> suspicious_patterns = {
    "union select", "drop table", "delete from", 
    "insert into", "update set", "exec(", "script>"
  };

  for (const auto& pattern : suspicious_patterns) {
    if (lower_path.find(pattern) != std::string::npos ||
        url.find(pattern) != std::string::npos) {
      return false;
    }
  }

  // 检查XSS常见模式
  if (lower_path.find("<script") != std::string::npos ||
      lower_path.find("javascript:") != std::string::npos ||
      lower_path.find("onerror=") != std::string::npos ||
      lower_path.find("onload=") != std::string::npos) {
    return false;
  }

  return true;
}

