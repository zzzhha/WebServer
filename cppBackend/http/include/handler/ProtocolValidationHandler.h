#pragma once

#include "handler/IRequestHandler.h"

// 前向声明
class IHttpMessage;

// HTTP协议验证处理器：检查HTTP协议基本规则
// 
// 验证项：
// 1. Content-Length与Transfer-Encoding冲突检查
// 2. Content-Length数值合法性及与body长度一致性检查
// 3. HTTP/1.1请求必须携带Host头部
// 4. HTTP方法有效性检查（不能为UNKNOWN）
// 5. 请求路径非空检查
class ProtocolValidationHandler : public IRequestHandler {
public:
  ProtocolValidationHandler() = default;
  ~ProtocolValidationHandler() override = default;

  // 处理入口：执行HTTP协议基本规则验证
  bool Handle(IHttpMessage& message) override;
};

