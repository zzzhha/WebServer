#pragma once

#include <string>
#include "IPageHandler.h"

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * WelcomePageHandler类：欢迎页面处理器
 * 职责：处理欢迎页面的GET请求，返回HTML页面
 */
class WelcomePageHandler : public IPageHandler {
private:
    std::string html_path_;  // HTML文件路径

public:
    // 构造函数
    explicit WelcomePageHandler(const std::string& html_path) 
        : html_path_(html_path) {}
    
    // 处理欢迎页面请求
    void Handle(HttpRequest* request, HttpResponse& response) override;
};

