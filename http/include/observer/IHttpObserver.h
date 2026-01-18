#pragma once

#include <memory>
#include <string>

class IHttpMessage;

// HTTP服务器处理阶段枚举
enum class HttpProcessStage {
    SSL_PROCESS,      // SSL解析阶段
    HTTP_PARSE,       // HTTP解析阶段
    VALIDATION,       // 责任链验证阶段
    ROUTING,          // 路由处理阶段
    COMPLETE          // 处理完成
};

// 观察者接口：监听HTTP服务器各个处理阶段的事件
class IHttpObserver{
public:
  virtual ~IHttpObserver() = default;

  // SSL处理阶段事件
  virtual void OnSslProcess(const std::string& event, const std::string& details) {}

  // HTTP解析阶段事件
  virtual void OnHttpParse(const std::string& event, const std::string& details) {}

  // 责任链验证阶段事件
  virtual void OnValidation(const std::string& event, const std::string& details) {}

  // 路由处理阶段事件
  virtual void OnRouting(const std::string& event, const std::string& details) {}

  // 处理完成事件（保留原有接口）
  virtual void OnMessage(const IHttpMessage&) {}

  // 通用事件通知（可选）
  virtual void OnStageEvent(HttpProcessStage stage, const std::string& event, const std::string& details) {}
};