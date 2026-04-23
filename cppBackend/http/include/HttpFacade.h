#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "core/IHttpMessage.h"
#include "error/HttpError.h"
#include "observer/IHttpObserver.h"

// 前向声明必要的类
class ISslHandler;
class IHttpParser;
class HandlerChain;
class IRequestHandler;
class HttpResponse;
class Router;

// HTTP服务器处理结果
enum class HttpServerResult {
    SUCCESS = 0,
    SSL_HANDSHAKE_NEED_MORE_DATA = 1,
    SSL_HANDSHAKE_FAILED = 2,
    PARSE_FAILED = 3,
    VALIDATION_FAILED = 4,
    ROUTING_FAILED = 5,
    NEED_MORE_DATA = 6,
    HTTP_VERSION_NOT_SUPPORTED = 7,
    NOT_IMPLEMENTED = 8,
    PAYLOAD_TOO_LARGE = 9,
    REQUEST_TIMEOUT = 10,
    UNKNOWN_ERROR = -1
};

// HTTP服务器外观类：封装所有HTTP处理逻辑
// 遵循外观模式，提供统一的HTTP处理接口
class HttpFacade {
public:
    HttpFacade();
    virtual ~HttpFacade() = default;

    // 核心处理接口：统一的HTTP数据处理入口
  HttpServerResult Process(const std::string& raw_data,
                           std::unique_ptr<IHttpMessage>& out_message,
                           HttpResponse& out_response);

  HttpServerResult Process(const std::string& raw_data,
                           std::unique_ptr<IHttpMessage>& out_message,
                           HttpResponse& out_response,
                           HttpError& out_error);

    void SetParseTimeoutMs(int timeout_ms);

    const HttpError& GetLastError() const { return last_error_; }
    bool HasLastError() const { return has_error_; }

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
    void SetRouter(std::shared_ptr<Router> router);

    // 配置服务器
    void ConfigureServer(bool enable_ssl = false,
                        const std::string& cert_file = "",
                        const std::string& key_file = "");
    
    // 获取已消费的字节数
    size_t GetConsumedBytes() const;

    // Pending 缓冲管理（性能优化：避免每次全量拼接）
    void AppendPending(std::string&& data);
    void AppendPending(const std::string& data);
    void ErasePending(size_t len);
    size_t GetPendingSize() const;
    void ClearPending();
    
    // 缓冲区大小限制配置
    void SetMaxPendingSize(size_t max_size);
    size_t GetMaxPendingSize() const;
    bool IsPendingFull() const;

    // 使用 pending 缓冲的处理接口
    HttpServerResult ProcessPending(std::unique_ptr<IHttpMessage>& out_message,
                                    HttpResponse& out_response);
    HttpServerResult ProcessPending(std::unique_ptr<IHttpMessage>& out_message,
                                    HttpResponse& out_response,
                                    HttpError& out_error);

private:
    // SSL处理阶段
    HttpServerResult ProcessSsl(const std::string& raw_data,
                              std::string& processed_data);

    // HTTP解析阶段
    HttpServerResult ProcessParsing(std::string data,
                                  std::unique_ptr<IHttpMessage>& message);

    // 责任链验证阶段
    HttpServerResult ProcessValidation(IHttpMessage& message, HttpResponse& response);

    // 路由处理阶段
    HttpServerResult ProcessRouting(IHttpMessage& message, HttpResponse& response);

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

    int parse_timeout_ms_{5000};
    bool awaiting_more_data_{false};
    std::chrono::steady_clock::time_point parse_wait_start_{};

    // 责任链相关
    std::shared_ptr<HandlerChain> handler_chain_;

    // 路由处理器
    std::shared_ptr<Router> router_;

    // 观察者列表
    std::vector<std::shared_ptr<IHttpObserver>> observers_;

    HttpError last_error_{};
    bool has_error_{false};

    // Pending 缓冲：累积未解析的数据，避免每次全量拼接
    std::string pending_data_;
    
    // 缓冲区大小限制
    size_t max_pending_size_{10 * 1024 * 1024}; // 默认10MB
};
