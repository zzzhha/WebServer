#include "HttpFacade.h"

#include <iostream>
#include <algorithm>
#include <chrono>

#include "factory/HttpParseFactory.h"
#include "handler/HandlerChain.h"
#include "router/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "error/HttpErrorUtil.h"

// 构造函数：初始化服务器组件
HttpFacade::HttpFacade()
    : handler_chain_(std::make_shared<HandlerChain>()) {
}

void HttpFacade::SetParseTimeoutMs(int timeout_ms) {
  parse_timeout_ms_ = timeout_ms;
}

// 核心处理接口：统一的HTTP数据处理入口
HttpServerResult HttpFacade::Process(const std::string& raw_data,
                                   std::unique_ptr<IHttpMessage>& out_message,
                                   HttpResponse& out_response) {
  HttpError ignored;
  return Process(raw_data, out_message, out_response, ignored);
}

HttpServerResult HttpFacade::Process(const std::string& raw_data,
                                   std::unique_ptr<IHttpMessage>& out_message,
                                   HttpResponse& out_response,
                                   HttpError& out_error) {
    has_error_ = false;
    last_error_ = HttpError{};

    // 检查输入数据是否为空
    if (raw_data.empty()) {
        last_error_.code = HttpErrc::PARSE_EMPTY_INPUT;
        last_error_.status = HttpStatusCode::BAD_REQUEST;
        last_error_.message = "Bad Request";
        last_error_.ctx.stage = HttpErrorStage::PARSING;
        last_error_.ctx.detail = "empty input";
        has_error_ = true;
        out_error = last_error_;
        return HttpServerResult::PARSE_FAILED;
    }
    
    std::string processed_data = raw_data;

    // 1. HTTP解析阶段
    HttpServerResult parse_result = ProcessParsing(processed_data, out_message);
    if (parse_result != HttpServerResult::SUCCESS || !out_message) {
        out_error = last_error_;
        return parse_result;
    }

    // 2. 责任链验证阶段
    if (!handler_chain_) {
        last_error_.code = HttpErrc::INTERNAL_ERROR;
        last_error_.status = HttpStatusCode::INTERNAL_SERVER_ERROR;
        last_error_.message = "Internal Server Error";
        last_error_.ctx.stage = HttpErrorStage::VALIDATION;
        last_error_.stack = CaptureStackTrace();
        has_error_ = true;
        out_error = last_error_;
        return HttpServerResult::VALIDATION_FAILED;
    }
    HttpServerResult validation_result = ProcessValidation(*out_message, out_response);
    if (validation_result != HttpServerResult::SUCCESS) {
        out_error = last_error_;
        return validation_result;
    }

    // 3. 路由处理阶段
    HttpServerResult routing_result = ProcessRouting(*out_message, out_response);
    if (routing_result != HttpServerResult::SUCCESS) {
        out_error = last_error_;
        return routing_result;
    }

    // 4. 通知观察者
    NotifyObservers(*out_message);

    out_error = last_error_;
    return HttpServerResult::SUCCESS;
}

void HttpFacade::SetMaxPendingSize(size_t max_size) {
    max_pending_size_ = max_size;
}

size_t HttpFacade::GetMaxPendingSize() const {
    return max_pending_size_;
}

bool HttpFacade::IsPendingFull() const {
    return pending_data_.size() >= max_pending_size_;
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
            last_error_.code = HttpErrc::INTERNAL_ERROR;
            last_error_.status = HttpStatusCode::INTERNAL_SERVER_ERROR;
            last_error_.message = "Internal Server Error";
            last_error_.ctx.stage = HttpErrorStage::PARSING;
            last_error_.ctx.received_bytes = data.size();
            last_error_.stack = CaptureStackTrace();
            has_error_ = true;
            return HttpServerResult::PARSE_FAILED;
        }
        NotifyHttpParse("创建解析器成功", "已根据数据特征创建合适的解析器");
    }

    // 解析HTTP数据
    int parse_result = parser_->Parse(data, message);
    if (parse_result == static_cast<int>(ParseResult::NEEDMOREDATA)) {
        NotifyHttpParse("HTTP解析需要更多数据", "等待更多数据完成解析");
        if (!awaiting_more_data_) {
            awaiting_more_data_ = true;
            parse_wait_start_ = std::chrono::steady_clock::now();
        } else if (parse_timeout_ms_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - parse_wait_start_);
            if (elapsed.count() > parse_timeout_ms_) {
                last_error_.code = HttpErrc::PARSE_TIMEOUT;
                last_error_.status = HttpStatusCode::REQUEST_TIMEOUT;
                last_error_.message = "Request Timeout";
                last_error_.ctx.stage = HttpErrorStage::PARSING;
                last_error_.ctx.received_bytes = data.size();
                last_error_.ctx.consumed_bytes = parser_->GetConsumeBytes();
                last_error_.ctx.detail = "incomplete request";
                has_error_ = true;
                parser_->Reset();
                awaiting_more_data_ = false;
                return HttpServerResult::REQUEST_TIMEOUT;
            }
        }
        return HttpServerResult::NEED_MORE_DATA;
    }
    if (parse_result == static_cast<int>(ParseResult::UNSUPPORTEDVERSION)) {
        NotifyHttpParse("HTTP版本不支持", "解析到不支持的HTTP版本");
        last_error_.code = HttpErrc::PARSE_UNSUPPORTED_VERSION;
        last_error_.status = HttpStatusCode::HTTP_VERSION_NOT_SUPPORTED;
        last_error_.message = "HTTP Version Not Supported";
        last_error_.ctx.stage = HttpErrorStage::PARSING;
        last_error_.ctx.parser_result = parse_result;
        last_error_.ctx.received_bytes = data.size();
        last_error_.ctx.consumed_bytes = parser_->GetConsumeBytes();
        has_error_ = true;
        parser_->Reset();
        awaiting_more_data_ = false;
        return HttpServerResult::HTTP_VERSION_NOT_SUPPORTED;
    }
    if (parse_result == static_cast<int>(ParseResult::HEADERTOOLONG) ||
        parse_result == static_cast<int>(ParseResult::BODYTOOLONG) ||
        parse_result == static_cast<int>(ParseResult::LINE_TOO_LONG)) {
        NotifyHttpParse("HTTP报文过大", "Header或Body超过限制");
        if (parse_result == static_cast<int>(ParseResult::LINE_TOO_LONG) ||
            parse_result == static_cast<int>(ParseResult::HEADERTOOLONG)) {
            last_error_.code = HttpErrc::PARSE_HEADER_TOO_LARGE;
            last_error_.status = HttpStatusCode::REQUEST_HEADER_FIELDS_TOO_LARGE;
            last_error_.message = "Request Header Fields Too Large";
        } else {
            last_error_.code = HttpErrc::PARSE_BODY_TOO_LARGE;
            last_error_.status = HttpStatusCode::PAYLOAD_TOO_LARGE;
            last_error_.message = "Payload Too Large";
        }
        last_error_.ctx.stage = HttpErrorStage::PARSING;
        last_error_.ctx.parser_result = parse_result;
        last_error_.ctx.received_bytes = data.size();
        last_error_.ctx.consumed_bytes = parser_->GetConsumeBytes();
        has_error_ = true;
        parser_->Reset();
        awaiting_more_data_ = false;
        return HttpServerResult::PAYLOAD_TOO_LARGE;
    }
    if (parse_result != static_cast<int>(ParseResult::SUCCESS) || !message) {
        std::string error_detail = "解析返回码: " + std::to_string(parse_result);
        NotifyHttpParse("HTTP解析失败", error_detail);
        if (parse_result == static_cast<int>(ParseResult::INVALIDSTARTLINE)) {
            last_error_.code = HttpErrc::PARSE_INVALID_START_LINE;
        } else if (parse_result == static_cast<int>(ParseResult::INVALIDHEADER)) {
            last_error_.code = HttpErrc::PARSE_INVALID_HEADER;
        } else if (parse_result == static_cast<int>(ParseResult::ERROR)) {
            last_error_.code = HttpErrc::PARSE_INVALID_CHUNKED_ENCODING;
        } else {
            last_error_.code = HttpErrc::PARSE_FAILED;
        }
        last_error_.status = HttpStatusCode::BAD_REQUEST;
        last_error_.message = "Bad Request";
        last_error_.ctx.stage = HttpErrorStage::PARSING;
        last_error_.ctx.parser_result = parse_result;
        last_error_.ctx.received_bytes = data.size();
        last_error_.ctx.consumed_bytes = parser_->GetConsumeBytes();
        last_error_.ctx.detail = error_detail;
        has_error_ = true;
        parser_->Reset();
        awaiting_more_data_ = false;
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
    awaiting_more_data_ = false;

    return HttpServerResult::SUCCESS;
}

// 责任链验证阶段：执行中间件和基础校验
HttpServerResult HttpFacade::ProcessValidation(IHttpMessage& message, HttpResponse& response) {
    NotifyValidation("开始责任链验证", "执行中间件和基础校验");

    if (!handler_chain_->Handle(message, last_error_)) {
        if (auto* request = dynamic_cast<HttpRequest*>(&message)) {
            last_error_.ctx.method = request->GetMethodString();
            last_error_.ctx.url = request->GetUrl();
            last_error_.ctx.path = request->GetPath();
            last_error_.ctx.version = request->GetVersionStr();
        }
        has_error_ = true;
        NotifyValidation("责任链验证失败", "某个中间件或验证器拒绝了请求");
        if (last_error_.status == HttpStatusCode::NOT_IMPLEMENTED) {
            return HttpServerResult::NOT_IMPLEMENTED;
        }
        if (last_error_.status == HttpStatusCode::PAYLOAD_TOO_LARGE ||
            last_error_.status == HttpStatusCode::REQUEST_HEADER_FIELDS_TOO_LARGE) {
            return HttpServerResult::PAYLOAD_TOO_LARGE;
        }
        return HttpServerResult::VALIDATION_FAILED;
    }
    
    NotifyValidation("责任链验证成功", "所有验证器和中间件通过");
    return HttpServerResult::SUCCESS;
}

// 路由处理阶段：处理路由并返回响应
HttpServerResult HttpFacade::ProcessRouting(IHttpMessage& message, HttpResponse& response) {
    if (router_) {
        NotifyRouting("开始路由处理", "使用配置的路由器处理请求");
        
        // 检查是否是请求消息
        if (message.IsRequest()) {
            auto* request = dynamic_cast<HttpRequest*>(&message);
            if (request) {
                // 设置响应版本
                response.SetVersion(request->GetVersion());
                
                // 调用路由器处理请求
                if (!router_->Handle(message, response)) {
                    NotifyRouting("路由处理失败", "路由器无法处理该请求");
                    last_error_.code = HttpErrc::ROUTE_NOT_FOUND;
                    last_error_.status = HttpStatusCode::NOT_FOUND;
                    last_error_.message = "Not Found";
                    last_error_.ctx.stage = HttpErrorStage::ROUTING;
                    last_error_.ctx.method = request->GetMethodString();
                    last_error_.ctx.url = request->GetUrl();
                    last_error_.ctx.path = request->GetPath();
                    last_error_.ctx.version = request->GetVersionStr();
                    has_error_ = true;
                    return HttpServerResult::ROUTING_FAILED;
                }
                
                NotifyRouting("路由处理成功", "请求已路由到对应的处理器");
            }
        }
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

// 设置路由处理器
void HttpFacade::SetRouter(std::shared_ptr<Router> router) {
    router_ = router;
}

// 获取已消费的字节数
size_t HttpFacade::GetConsumedBytes() const {
    if (parser_) {
        return parser_->GetConsumeBytes();
    }
    return 0;
}

// Pending 缓冲管理方法实现
void HttpFacade::AppendPending(std::string&& data) {
    // 检查缓冲区大小限制
    if (pending_data_.size() + data.size() > max_pending_size_) {
        // 缓冲区已满，清除旧数据以腾出空间
        if (data.size() > max_pending_size_) {
            // 如果单次数据就超过最大限制，只保留最新数据
            pending_data_.clear();
            pending_data_.append(data.substr(0, max_pending_size_));
        } else {
            // 计算需要清除的旧数据量
            size_t overflow = (pending_data_.size() + data.size()) - max_pending_size_;
            if (overflow > 0) {
                pending_data_.erase(0, overflow);
            }
            pending_data_.append(std::move(data));
        }
    } else {
        pending_data_.append(std::move(data));
    }
}

void HttpFacade::AppendPending(const std::string& data) {
    // 检查缓冲区大小限制
    if (pending_data_.size() + data.size() > max_pending_size_) {
        // 缓冲区已满，清除旧数据以腾出空间
        if (data.size() > max_pending_size_) {
            // 如果单次数据就超过最大限制，只保留最新数据
            pending_data_.clear();
            pending_data_.append(data.substr(0, max_pending_size_));
        } else {
            // 计算需要清除的旧数据量
            size_t overflow = (pending_data_.size() + data.size()) - max_pending_size_;
            if (overflow > 0) {
                pending_data_.erase(0, overflow);
            }
            pending_data_.append(data);
        }
    } else {
        pending_data_.append(data);
    }
}

void HttpFacade::ErasePending(size_t len) {
    if (len >= pending_data_.size()) {
        pending_data_.clear();
    } else {
        pending_data_.erase(0, len);
    }
}

size_t HttpFacade::GetPendingSize() const {
    return pending_data_.size();
}

void HttpFacade::ClearPending() {
    pending_data_.clear();
}

HttpServerResult HttpFacade::ProcessPending(std::unique_ptr<IHttpMessage>& out_message,
                                            HttpResponse& out_response) {
    HttpError ignored;
    return ProcessPending(out_message, out_response, ignored);
}

HttpServerResult HttpFacade::ProcessPending(std::unique_ptr<IHttpMessage>& out_message,
                                            HttpResponse& out_response,
                                            HttpError& out_error) {
    has_error_ = false;
    last_error_ = HttpError{};

    if (pending_data_.empty()) {
        last_error_.code = HttpErrc::PARSE_EMPTY_INPUT;
        last_error_.status = HttpStatusCode::BAD_REQUEST;
        last_error_.message = "Bad Request";
        last_error_.ctx.stage = HttpErrorStage::PARSING;
        last_error_.ctx.detail = "empty pending buffer";
        has_error_ = true;
        out_error = last_error_;
        return HttpServerResult::NEED_MORE_DATA;
    }

    // 1. HTTP解析阶段（直接在 pending_data_ 上解析）
    HttpServerResult parse_result = ProcessParsing(pending_data_, out_message);
    if (parse_result != HttpServerResult::SUCCESS || !out_message) {
        out_error = last_error_;
        return parse_result;
    }

    // 2. 责任链验证阶段
    if (!handler_chain_) {
        last_error_.code = HttpErrc::INTERNAL_ERROR;
        last_error_.status = HttpStatusCode::INTERNAL_SERVER_ERROR;
        last_error_.message = "Internal Server Error";
        last_error_.ctx.stage = HttpErrorStage::VALIDATION;
        last_error_.stack = CaptureStackTrace();
        has_error_ = true;
        out_error = last_error_;
        return HttpServerResult::VALIDATION_FAILED;
    }
    HttpServerResult validation_result = ProcessValidation(*out_message, out_response);
    if (validation_result != HttpServerResult::SUCCESS) {
        out_error = last_error_;
        return validation_result;
    }

    // 3. 路由处理阶段
    HttpServerResult routing_result = ProcessRouting(*out_message, out_response);
    if (routing_result != HttpServerResult::SUCCESS) {
        out_error = last_error_;
        return routing_result;
    }

    // 4. 通知观察者
    NotifyObservers(*out_message);

    out_error = last_error_;
    return HttpServerResult::SUCCESS;
}
