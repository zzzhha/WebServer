#pragma once

#include <string>
#include <fstream>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * ErrorHandler类：错误处理器基类
 * 职责：提供统一的错误页面处理逻辑
 * 子类可以继承此类并实现特定的错误处理
 */
class ErrorHandler {
protected:
    std::string html_path_;  // HTML文件路径

public:
    // 构造函数
    explicit ErrorHandler(const std::string& html_path) : html_path_(html_path) {}
    
    virtual ~ErrorHandler() = default;

    // 通用错误处理方法
    // request: HTTP请求对象
    // response: HTTP响应对象（用于设置响应）
    // status_code: HTTP状态码
    // html_filename: HTML文件名（相对于html_path_）
    // default_body: 如果HTML文件不存在，使用的默认响应体
    void HandleError(HttpRequest* request, HttpResponse& response, 
                     int status_code, const std::string& html_filename, 
                     const std::string& default_body);
};

