#pragma once

#include <string>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * IPageHandler 接口类：所有页面处理器的基类
 */
class IPageHandler {
public:
    virtual ~IPageHandler() = default;
    
    // 处理页面请求的统一接口
    virtual void Handle(HttpRequest* request, HttpResponse& response) = 0;
};

