#pragma once

#include "ErrorHandler.h"
#include <string>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * BadRequestHandler类：400错误处理器
 * 职责：处理400 Bad Request错误
 */
class BadRequestHandler : public ErrorHandler {
public:
    // 构造函数
    explicit BadRequestHandler(const std::string& html_path) 
        : ErrorHandler(html_path) {}
    
    // 处理400错误
    void Handle(HttpRequest* request, HttpResponse& response);
};

