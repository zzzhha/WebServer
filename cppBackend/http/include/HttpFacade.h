#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/IHttpMessage.h"
#include "observer/IHttpObserver.h"

// 前向声明必要的类
class ISslHandler;
class IHttpParser;
class HandlerChain;
class IRequestHandler;

// HTTP服务器处理结果
enum class HttpServerResult {
    SUCCESS = 0,
    SSL_HANDSHAKE_NEED_MORE_DATA = 1,
    SSL_HANDSHAKE_FAILED = 2,
    PARSE_FAILED = 3,
    VALIDATION_FAILED = 4,
    ROUTING_FAILED = 5,
    NEED_MORE_DATA = 6,
    UNKNOWN_ERROR = -1
};

// HTTP服务器外观类：封装所有HTTP处理逻辑
// 遵循外观模式，提供统一的HTTP处理接口
class HttpFacade {
public:
    HttpFacade();
    virtual ~HttpFacade() = default;

    // 核心处理接口：处理原始HTTP数据
    HttpServerResult Process(const std::string& raw_data,
                           std::unique_ptr<IHttpMessage>& out_message);

    // 添加观察者（用于监听HTTP消息处理过程）
    void AddObserver(std::shared_ptr<IHttpObserver> observer);

    // 移除观察者
    void RemoveObserver(std::shared_ptr<IHttpObserver> observer);

    // 设置SSL处理器（如果需要HTTPS支持）
    void SetSslHandler(std::shared_ptr<ISslHandler> ssl_handler);

    // 启用/禁用SSL处理
    void EnableSsl(bool enable);

    // 添加中间件处理器到责任链
    void AddMiddleware(std::shared_ptr<IRequestHandler> middleware);

    // 设置路由处理器（预留接口）
    void SetRouter(std::shared_ptr<IRequestHandler> router);

    // 配置服务器
    void ConfigureServer(bool enable_ssl = false,
                        const std::string& cert_file = "",
                        const std::string& key_file = "");
    
    // 获取已消费的字节数
    size_t GetConsumedBytes() const;

private:
    // SSL处理阶段
    HttpServerResult ProcessSsl(const std::string& raw_data,
                              std::string& processed_data);

    // HTTP解析阶段
    HttpServerResult ProcessParsing(std::string data,
                                  std::unique_ptr<IHttpMessage>& message);

    // 责任链验证阶段
    HttpServerResult ProcessValidation(IHttpMessage& message);

    // 路由处理阶段（预留）
    HttpServerResult ProcessRouting(IHttpMessage& message);

    // 通知观察者（处理完成事件）
    void NotifyObservers(const IHttpMessage& message);

    // 通知观察者SSL处理事件
    void NotifySslProcess(const std::string& event, const std::string& details);

    // 通知观察者HTTP解析事件
    void NotifyHttpParse(const std::string& event, const std::string& details);

    // 通知观察者责任链验证事件
    void NotifyValidation(const std::string& event, const std::string& details);

    // 通知观察者路由处理事件
    void NotifyRouting(const std::string& event, const std::string& details);

private:
    // SSL相关成员
    std::shared_ptr<ISslHandler> ssl_handler_;
    bool ssl_enabled_;
    bool handshake_complete_;

    // HTTP解析相关
    std::shared_ptr<IHttpParser> parser_;

    // 责任链相关
    std::shared_ptr<HandlerChain> handler_chain_;

    // 路由处理器（预留）
    std::shared_ptr<IRequestHandler> router_;

    // 观察者列表
    std::vector<std::shared_ptr<IHttpObserver>> observers_;
};
