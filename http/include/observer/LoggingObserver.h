#pragma once

#include "observer/IHttpObserver.h"
#include <memory>
#include <string>

// 前向声明Logger（假设Logger.h在外部）
// #include "Logger.h"  // 用户说明Logger.h未在本文件夹中，但可以直接引用

class IHttpMessage;

// 日志观察者：使用Logger系统记录HTTP服务器各个处理阶段的事件
class LoggingObserver : public IHttpObserver {
public:
    LoggingObserver() = default;
    ~LoggingObserver() override = default;

    // SSL处理阶段事件
    void OnSslProcess(const std::string& event, const std::string& details) override;

    // HTTP解析阶段事件
    void OnHttpParse(const std::string& event, const std::string& details) override;

    // 责任链验证阶段事件
    void OnValidation(const std::string& event, const std::string& details) override;

    // 路由处理阶段事件
    void OnRouting(const std::string& event, const std::string& details) override;

    // 处理完成事件
    void OnMessage(const IHttpMessage& message) override;

    // 通用事件通知
    void OnStageEvent(HttpProcessStage stage, const std::string& event, const std::string& details) override;

private:
    // 格式化日志消息
    std::string FormatLogMessage(const std::string& stage, const std::string& event, const std::string& details) const;
    
    // 根据事件类型确定日志级别
    void LogWithLevel(const std::string& level, const std::string& message) const;
};

