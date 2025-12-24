#pragma once

#include <string>
#include "IPageHandler.h"

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * VideoPageHandler类：视频浏览页面处理器
 * 职责：处理视频浏览页面的GET请求，返回HTML页面
 */
class VideoPageHandler : public IPageHandler {
private:
    std::string html_path_;  // HTML文件路径

public:
    // 构造函数
    explicit VideoPageHandler(const std::string& html_path) 
        : html_path_(html_path) {}
    
    // 处理视频浏览页面请求
    void Handle(HttpRequest* request, HttpResponse& response) override;
};

