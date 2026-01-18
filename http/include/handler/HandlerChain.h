#pragma once

#include <memory>
#include <vector>

#include "handler/IRequestHandler.h"

// 头部处理责任链，按顺序执行已注册的处理器
// 
// 默认处理器链（按执行顺序）：
// 1. ProtocolValidationHandler - HTTP协议验证
//    - Content-Length与Transfer-Encoding冲突检查
//    - Host头部检查（HTTP/1.1必需）
//    - 方法有效性检查
//    - 路径非空检查
//
// 2. SecurityValidationHandler - 安全检查
//    - HTTP方法白名单验证（防止危险方法如TRACE、CONNECT）
//    - 请求体大小限制（默认10MB，防止DoS攻击）
//    - URL长度限制（默认2KB）
//    - 路径遍历攻击防护（../, %2e%2e等）
//    - Header数量和大小限制（防止Header炸弹攻击）
//    - 可疑模式检测（SQL注入、XSS等简单检测）
//
// 预留接口说明（可扩展的处理器）：
// 1. JWT令牌验证：实现IRequestHandler接口，在Handle方法中验证JWT令牌
//    示例：auto jwt_handler = std::make_shared<JwtValidationHandler>(secret_key);
//          handler_chain->AddHandler(jwt_handler);
//
// 2. Cookie处理：实现IRequestHandler接口，在Handle方法中解析和验证Cookie
//    示例：auto cookie_handler = std::make_shared<CookieHandler>();
//          handler_chain->AddHandler(cookie_handler);
//
// 处理器执行顺序：按照AddHandler的调用顺序依次执行
class HandlerChain : public IRequestHandler {
public:
  HandlerChain();
  ~HandlerChain() override = default;

  // 添加一个处理器到链尾
  // 预留接口：可用于添加JWT、Cookie、Session等认证/授权处理器
  void AddHandler(std::shared_ptr<IRequestHandler> handler);

  // 触发责任链
  bool Handle(IHttpMessage& message) override;

private:
  void AddDefaultHandlers();

  std::vector<std::shared_ptr<IRequestHandler>> handlers_;
  std::shared_ptr<IRequestHandler> head_{};
};