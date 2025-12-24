#pragma once

#include <string>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * StaticFileService类：静态文件业务处理层
 * 职责：处理静态文件服务的业务逻辑
 * - 业务验证（路径验证）
 * - 业务判断（文件是否存在）
 * - 设置静态文件响应
 */
class StaticFileService {
public:
    // 处理静态文件请求
    // request: HTTP请求对象
    // response: HTTP响应对象（用于设置响应）
    // static_path: 静态文件根路径
    // 返回true表示处理成功，false表示处理失败
    static bool HandleStaticFile(HttpRequest* request, HttpResponse& response, const std::string& static_path);
};

