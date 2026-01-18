#pragma once

#include <string>
#include "IPageHandler.h"

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * RegisterPageHandler类：注册页面处理器
 * 职责：处理注册页面的GET请求，返回HTML页面
 * 注意：POST请求的注册处理在AuthService中
 */
class RegisterPageHandler : public IPageHandler {
private:
    std::string html_path_;  // HTML文件路径

public:
    // 构造函数
    explicit RegisterPageHandler(const std::string& html_path) 
        : html_path_(html_path) {}
    
    // 处理注册页面请求
    void Handle(HttpRequest* request, HttpResponse& response) override;
};

