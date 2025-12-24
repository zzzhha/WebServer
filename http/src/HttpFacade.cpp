#include "HttpFacade.h"

#include <iostream>
#include <algorithm>

#include "ssl/SslFactory.h"
#include "factory/HttpParseFactory.h"
#include "handler/HandlerChain.h"
#include "core/HttpRequest.h"

// 构造函数：初始化服务器组件
HttpFacade::HttpFacade()
    : ssl_enabled_(false)
    , handshake_complete_(false)
    , handler_chain_(std::make_shared<HandlerChain>()) {
}

// 核心处理接口：统一的HTTP数据处理入口
HttpServerResult HttpFacade::Process(const std::string& raw_data,
                                   std::unique_ptr<IHttpMessage>& out_message) {
    std::string processed_data = raw_data;

    // 1. SSL处理阶段
    HttpServerResult ssl_result = ProcessSsl(raw_data, processed_data);
    if (ssl_result != HttpServerResult::SUCCESS) {
        return ssl_result;
    }

    // 2. HTTP解析阶段
    HttpServerResult parse_result = ProcessParsing(processed_data, out_message);
    if (parse_result != HttpServerResult::SUCCESS) {
        return parse_result;
    }

    // 3. 责任链验证阶段
    HttpServerResult validation_result = ProcessValidation(*out_message);
    if (validation_result != HttpServerResult::SUCCESS) {
        return validation_result;
    }

    // 4. 路由处理阶段（预留）
    HttpServerResult routing_result = ProcessRouting(*out_message);
    if (routing_result != HttpServerResult::SUCCESS) {
        return routing_result;
    }

    // 5. 通知观察者
    NotifyObservers(*out_message);

    return HttpServerResult::SUCCESS;
}

// SSL处理阶段：处理SSL/TLS加密解密
HttpServerResult HttpFacade::ProcessSsl(const std::string& raw_data,
                                      std::string& processed_data) {
    if (!ssl_enabled_ || !ssl_handler_) {
        // 不使用SSL，直接返回原始数据
        processed_data = raw_data;
        NotifySslProcess("SSL未启用", "跳过SSL处理");
        return HttpServerResult::SUCCESS;
    }

    // 检测是否为SSL连接
    if (!ssl_handler_->IsSslConnection(raw_data)) {
        processed_data = raw_data;
        NotifySslProcess("非SSL连接", "检测到非SSL连接，跳过处理");
        return HttpServerResult::SUCCESS;
    }

    NotifySslProcess("开始SSL处理", "检测到SSL连接");

    // 执行SSL握手（如果需要）
    if (!handshake_complete_) {
        std::string handshake_output;
        SslResult handshake_result = ssl_handler_->Handshake(raw_data, handshake_output);

        if (handshake_result == SslResult::NEED_MORE_DATA) {
            NotifySslProcess("SSL握手需要更多数据", "等待更多数据完成握手");
            return HttpServerResult::SSL_HANDSHAKE_NEED_MORE_DATA;
        } else if (handshake_result != SslResult::SUCCESS) {
            NotifySslProcess("SSL握手失败", "握手过程出错");
            return HttpServerResult::SSL_HANDSHAKE_FAILED;
        }

        handshake_complete_ = true;
        NotifySslProcess("SSL握手完成", handshake_output);
    }

    // 解密SSL数据
    std::string decrypted_data;
    SslResult decrypt_result = ssl_handler_->Decrypt(raw_data, decrypted_data);

    if (decrypt_result == SslResult::SUCCESS) {
        processed_data = decrypted_data;
        NotifySslProcess("SSL数据解密成功", "数据已解密，准备进行HTTP解析");
        return HttpServerResult::SUCCESS;
    } else if (decrypt_result == SslResult::NOT_SSL_CONNECTION) {
        // 不是SSL连接，继续使用原始数据
        processed_data = raw_data;
        NotifySslProcess("非SSL连接", "跳过解密，使用原始数据");
        return HttpServerResult::SUCCESS;
    } else {
        NotifySslProcess("SSL解密失败", "解密过程出错");
        return HttpServerResult::SSL_HANDSHAKE_FAILED;
    }
}

// HTTP解析阶段：解析HTTP数据
HttpServerResult HttpFacade::ProcessParsing(std::string data,
                                          std::unique_ptr<IHttpMessage>& message) {
    NotifyHttpParse("开始HTTP解析", "准备解析HTTP数据");
    
    // 使用工厂创建解析器（自动嗅探HTTP版本）
    if (!parser_) {
        parser_ = HttpParseFactory::Create(data);
        if (!parser_) {
            NotifyHttpParse("HTTP解析失败", "无法创建HTTP解析器");
            return HttpServerResult::PARSE_FAILED;
        }
        NotifyHttpParse("创建解析器成功", "已根据数据特征创建合适的解析器");
    }

    // 解析HTTP数据
    int parse_result = parser_->Parse(data, message);
    if (parse_result != static_cast<int>(ParseResult::SUCCESS) || !message) {
        std::string error_detail = "解析返回码: " + std::to_string(parse_result);
        NotifyHttpParse("HTTP解析失败", error_detail);
        return HttpServerResult::PARSE_FAILED;
    }

    // 解析成功，提取请求信息用于日志
    std::string parse_detail;
    if (auto* req = dynamic_cast<HttpRequest*>(message.get())) {
        parse_detail = "方法: " + req->GetMethodString() + 
                      ", 路径: " + req->GetPath() + 
                      ", 版本: " + req->GetVersionStr();
    } else {
        parse_detail = "响应消息解析成功";
    }
    NotifyHttpParse("HTTP解析成功", parse_detail);

    return HttpServerResult::SUCCESS;
}

// 责任链验证阶段：执行中间件和基础校验
HttpServerResult HttpFacade::ProcessValidation(IHttpMessage& message) {
    NotifyValidation("开始责任链验证", "执行中间件和基础校验");
    
    if (!handler_chain_->Handle(message)) {
        NotifyValidation("责任链验证失败", "某个中间件或验证器拒绝了请求");
        return HttpServerResult::VALIDATION_FAILED;
    }
    
    NotifyValidation("责任链验证成功", "所有验证器和中间件通过");
    return HttpServerResult::SUCCESS;
}

// 路由处理阶段：预留接口，目前直接返回成功
HttpServerResult HttpFacade::ProcessRouting(IHttpMessage& message) {
    if (router_) {
        NotifyRouting("开始路由处理", "使用配置的路由器处理请求");
        if (!router_->Handle(message)) {
            NotifyRouting("路由处理失败", "路由器无法处理该请求");
            return HttpServerResult::ROUTING_FAILED;
        }
        NotifyRouting("路由处理成功", "请求已路由到对应的处理器");
    } else {
        NotifyRouting("跳过路由处理", "未配置路由器，使用默认处理");
    }
    // 预留：这里可以添加路由逻辑（JWT令牌、Cookie、Session等）
    return HttpServerResult::SUCCESS;
}

// 添加观察者
void HttpFacade::AddObserver(std::shared_ptr<IHttpObserver> observer) {
    if (observer) {
        observers_.push_back(observer);
    }
}

// 移除观察者
void HttpFacade::RemoveObserver(std::shared_ptr<IHttpObserver> observer) {
    if (observer) {
        auto it = std::find(observers_.begin(), observers_.end(), observer);
        if (it != observers_.end()) {
            observers_.erase(it);
        }
    }
}

// 通知所有观察者（处理完成事件）
void HttpFacade::NotifyObservers(const IHttpMessage& message) {
    for (auto& observer : observers_) {
        if (observer) {
            observer->OnMessage(message);
        }
    }
}

// 通知观察者SSL处理事件
void HttpFacade::NotifySslProcess(const std::string& event, const std::string& details) {
    for (auto& observer : observers_) {
        if (observer) {
            observer->OnSslProcess(event, details);
        }
    }
}

// 通知观察者HTTP解析事件
void HttpFacade::NotifyHttpParse(const std::string& event, const std::string& details) {
    for (auto& observer : observers_) {
        if (observer) {
            observer->OnHttpParse(event, details);
        }
    }
}

// 通知观察者责任链验证事件
void HttpFacade::NotifyValidation(const std::string& event, const std::string& details) {
    for (auto& observer : observers_) {
        if (observer) {
            observer->OnValidation(event, details);
        }
    }
}

// 通知观察者路由处理事件
void HttpFacade::NotifyRouting(const std::string& event, const std::string& details) {
    for (auto& observer : observers_) {
        if (observer) {
            observer->OnRouting(event, details);
        }
    }
}

// 设置SSL处理器
void HttpFacade::SetSslHandler(std::shared_ptr<ISslHandler> ssl_handler) {
    ssl_handler_ = ssl_handler;
    handshake_complete_ = false;  // 重置握手状态
}

// 启用/禁用SSL处理
void HttpFacade::EnableSsl(bool enable) {
    ssl_enabled_ = enable;
    if (!enable) {
        ssl_handler_.reset();
        handshake_complete_ = false;
    }
}

// 添加中间件到责任链
void HttpFacade::AddMiddleware(std::shared_ptr<IRequestHandler> middleware) {
    if (middleware && handler_chain_) {
        handler_chain_->AddHandler(middleware);
    }
}

// 设置路由处理器
void HttpFacade::SetRouter(std::shared_ptr<IRequestHandler> router) {
    router_ = router;
}

// 配置服务器
void HttpFacade::ConfigureServer(bool enable_ssl,
                               const std::string& cert_file,
                               const std::string& key_file) {
    EnableSsl(enable_ssl);

    if (enable_ssl) {
        if (!cert_file.empty() && !key_file.empty()) {
            ssl_handler_ = SslFactory::CreateWithCert(cert_file, key_file);
        } else {
            ssl_handler_ = SslFactory::CreateServer();
        }
    }
}
