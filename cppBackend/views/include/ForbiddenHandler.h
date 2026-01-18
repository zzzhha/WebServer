#pragma once

#include "ErrorHandler.h"
#include <string>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * ForbiddenHandler类：403错误处理器
 * 职责：处理403 Forbidden错误
 */
class ForbiddenHandler : public ErrorHandler {
public:
    // 构造函数
    explicit ForbiddenHandler(const std::string& html_path) 
        : ErrorHandler(html_path) {}
    
    // 处理403错误
    void Handle(HttpRequest* request, HttpResponse& response);
};

