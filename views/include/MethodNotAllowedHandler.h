#pragma once

#include "ErrorHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include <string>
#include <vector>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * MethodNotAllowedHandler类：405错误处理器
 * 职责：处理405 Method Not Allowed错误
 */
class MethodNotAllowedHandler : public ErrorHandler {
public:
    // 构造函数
    explicit MethodNotAllowedHandler(const std::string& html_path) 
        : ErrorHandler(html_path) {}
    
    // 处理405错误
    // allowedMethods: 允许的HTTP方法列表
    void Handle(HttpRequest* request, HttpResponse& response, 
                const std::vector<HttpMethod>& allowedMethods);
};

