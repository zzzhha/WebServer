#pragma once

#include "ErrorHandler.h"
#include <string>
#include <memory>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * NotFoundHandler类：404错误处理器
 * 职责：处理404 Not Found错误
 */
class NotFoundHandler : public ErrorHandler {
public:
    // 构造函数
    explicit NotFoundHandler(const std::string& html_path) 
        : ErrorHandler(html_path) {}
    
    // 处理404错误
    void Handle(HttpRequest* request, HttpResponse& response);
};

