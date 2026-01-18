#pragma once

#include <string>
#include "IPageHandler.h"

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * IndexPageHandler类：主页处理器
 * 职责：处理主页的GET请求，返回HTML页面
 */
class IndexPageHandler : public IPageHandler {
private:
    std::string html_path_;  // HTML文件路径

public:
    // 构造函数
    explicit IndexPageHandler(const std::string& html_path) 
        : html_path_(html_path) {}
    
    // 处理主页请求
    void Handle(HttpRequest* request, HttpResponse& response) override;
};

