#pragma once

#include "handler/IRequestHandler.h"
#include "core/HttpRequest.h"  // 需要HttpMethod枚举定义
#include <vector>
#include <cstddef>
#include <string>

// 前向声明
class IHttpMessage;

// 增强安全检查处理器：防止常见Web攻击
// 
// 安全检查项：
// 1. HTTP方法白名单验证（防止危险方法如TRACE、CONNECT）
// 2. 请求体大小限制（防止DoS攻击）
// 3. URL长度限制
// 4. 路径遍历攻击防护（../, %2e%2e等）
// 5. Header数量和大小限制（防止Header炸弹攻击）
// 6. 可疑模式检测（SQL注入、XSS等简单检测）
class SecurityValidationHandler : public IRequestHandler {
public:
  // 构造函数：可配置各项限制参数
  SecurityValidationHandler(
    size_t max_body_size = 10 * 1024 * 1024,        // 默认10MB
    size_t max_url_length = 2048,                    // 默认2KB
    size_t max_header_count = 50,                    // 默认50个Header
    size_t max_header_value_length = 8192             // 默认8KB
  );

  // 处理入口：执行所有安全检查
  bool Handle(IHttpMessage& message) override;

private:
  // 检查HTTP方法是否在白名单中
  bool IsMethodAllowed(HttpMethod method) const;

  // 检查请求体大小
  bool CheckBodySize(const IHttpMessage& message) const;

  // 检查URL长度
  bool CheckUrlLength(const HttpRequest& request) const;

  // 检查路径安全性（防止路径遍历攻击）
  bool CheckPathSecurity(const std::string& path) const;

  // 检查Header数量和大小
  bool CheckHeaders(const IHttpMessage& message) const;

  // 检查可疑字符和模式
  bool CheckSuspiciousPatterns(const HttpRequest& request) const;

private:
  size_t maxBodySize_;
  size_t maxUrlLength_;
  size_t maxHeaderCount_;
  size_t maxHeaderValueLength_;
  std::vector<HttpMethod> allowedMethods_;
};

