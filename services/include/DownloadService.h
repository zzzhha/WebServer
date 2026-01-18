#pragma once

#include <string>

// 前向声明
class HttpRequest;
class HttpResponse;

/**
 * DownloadService类：下载业务处理层
 * 职责：处理文件下载的业务逻辑
 * - 业务验证（参数验证、路径安全性验证）
 * - 业务判断（文件是否存在）
 * - 设置下载响应
 */
class DownloadService {
public:
    // 处理下载请求
    // request: HTTP请求对象
    // response: HTTP响应对象（用于设置响应）
    // static_path: 静态文件根路径
    // 返回true表示处理成功，false表示处理失败
    static bool HandleDownload(HttpRequest* request, HttpResponse& response, const std::string& static_path);

    // 验证文件路径安全性
    // 防止路径遍历攻击（检查 ../ 等）
    // 返回true表示路径安全，false表示路径不安全
    static bool ValidateFilePath(const std::string& file_path);

    // 检查文件是否存在
    // 返回true表示文件存在，false表示文件不存在
    static bool FileExists(const std::string& file_path);
};

