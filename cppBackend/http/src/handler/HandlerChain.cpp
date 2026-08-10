#include "handler/HandlerChain.h"
#include "handler/ProtocolValidationHandler.h"
#include "handler/SecurityValidationHandler.h"
#include "error/HttpError.h"

#include <memory>
#include <utility>

HandlerChain::HandlerChain() {
  AddDefaultHandlers();
}

void HandlerChain::AddHandler(std::shared_ptr<IRequestHandler> handler) {
  if (!handler) return;
  // 低危修复：加锁注册，防止运行期并发 AddHandler 与 Handle 遍历的数据竞争
  std::lock_guard<std::mutex> lock(mutex_);
  if (!head_) {
    head_ = handler;
  } else if (!handlers_.empty()) {
    handlers_.back()->SetNext(handler);
  }
  handlers_.push_back(std::move(handler));
}

bool HandlerChain::Handle(IHttpMessage& message, HttpError& error) {
  if (!head_) return true;
  return head_->Handle(message, error);
}

void HandlerChain::AddDefaultHandlers() {
  // 1. HTTP协议验证：检查HTTP协议基本规则（Content-Length、Host等）
  AddHandler(std::make_shared<ProtocolValidationHandler>());
  
  // 2. 安全检查：防止常见Web攻击（DoS、路径遍历、注入攻击等）
  AddHandler(std::make_shared<SecurityValidationHandler>());
}
