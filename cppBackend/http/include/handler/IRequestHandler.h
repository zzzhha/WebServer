#pragma once

#include <memory>

class IHttpMessage;

// 责任链节点接口：处理 HTTP 消息并可将处理权交给下一个节点
class IRequestHandler {
public:
  IRequestHandler() = default;
  virtual ~IRequestHandler();

  // 设置后继节点
  void SetNext(std::shared_ptr<IRequestHandler> next) { next_ = std::move(next); }

  // 处理入口，返回 false 表示校验或处理失败
  virtual bool Handle(IHttpMessage& message) = 0;

protected:
  // 将消息传递给后继节点
  bool CallNext(IHttpMessage& message);

private:
  std::shared_ptr<IRequestHandler> next_{};
};