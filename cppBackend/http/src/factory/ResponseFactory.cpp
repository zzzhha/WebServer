#include "factory/ResponseFactory.h"
#include "builder/ResponseBuilder.h"
#include "handler/AppHandlers.h"
#include "logger/log_fac.h"
#include <fstream>
#include <sstream>

/**
 * 创建成功响应
 * @param message 成功消息
 * @param statusCode HTTP状态码
 * @return 成功响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateSuccess(
    const std::string& message,
    HttpStatusCode statusCode) {
    LOGINFO("创建成功响应: " + message);
    return ResponseBuilder::New()
        .Status(statusCode)
        .Json(true, message)
        .Build();
}

/**
 * 创建带数据的成功响应
 * @param data 业务数据（JSON格式字符串）
 * @param message 成功消息
 * @param statusCode HTTP状态码
 * @return 带数据的成功响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateSuccessWithData(
    const std::string& data,
    const std::string& message,
    HttpStatusCode statusCode) {
    LOGINFO("创建带数据的成功响应: " + message);
    return ResponseBuilder::New()
        .Status(statusCode)
        .Json(true, message, data)
        .Build();
}

/**
 * 创建错误响应
 * @param code HTTP状态码
 * @param message 错误消息
 * @return 错误响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateError(
    HttpStatusCode code,
    const std::string& message) {
    LOGWARNING("创建错误响应: " + std::to_string(static_cast<int>(code)) + " - " + message);
    return ResponseBuilder::New()
        .Error(code, message)
        .Build();
}

/**
 * 创建带详细信息的错误响应
 * @param code HTTP状态码
 * @param message 错误消息
 * @param details 详细错误信息
 * @return 带详细信息的错误响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateErrorWithDetails(
    HttpStatusCode code,
    const std::string& message,
    const std::map<std::string, std::string>& details) {
    std::ostringstream json;
    json << "{\"success\":false,\"error\":{\"code\":" << static_cast<int>(code)
         << ",\"message\":\"" << message << "\"";
    
    if (!details.empty()) {
        json << ",\"details\":{";
        bool first = true;
        for (const auto& [key, value] : details) {
            if (!first) json << ",";
            json << "\"" << key << ":\"" << value << "\"";
            first = false;
        }
        json << "}";
    }
    json << "}}";

    LOGWARNING("创建详细错误响应: " + std::to_string(static_cast<int>(code)) + " - " + message);
    return ResponseBuilder::New()
        .Status(code)
        .Header("Content-Type", "application/json; charset=utf-8")
        .Body(json.str())
        .Build();
}

/**
 * 创建文件下载响应
 * @param filepath 文件路径
 * @param filename 文件名（为空时从filepath提取）
 * @return 文件下载响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateFileDownload(
    const std::string& filepath,
    const std::string& filename) {
    std::string actual_filename = filename.empty() ? 
        filepath.substr(filepath.find_last_of("/\\") + 1) : filename;
    
    LOGINFO("创建文件下载响应: " + actual_filename);
    return ResponseBuilder::New()
        .FileDownload(actual_filename, filepath)
        .Build();
}

/**
 * 创建静态文件响应
 * @param filepath 文件路径
 * @param contentType 内容类型（为空时自动检测）
 * @return 静态文件响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateStaticFile(
    const std::string& filepath,
    const std::string& contentType) {
    LOGINFO("创建静态文件响应: " + filepath);
    return ResponseBuilder::New()
        .StaticFile(filepath, contentType)
        .Build();
}

/**
 * 创建重定向响应
 * @param url 重定向目标URL
 * @param permanent 是否为永久重定向
 * @return 重定向响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateRedirect(
    const std::string& url,
    bool permanent) {
    LOGINFO("创建重定向响应: " + url + (permanent ? " (永久)" : " (临时)"));
    return ResponseBuilder::New()
        .Redirect(url, permanent)
        .Build();
}

/**
 * 创建HTML响应
 * @param html HTML内容
 * @param statusCode HTTP状态码
 * @return HTML响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateHtml(
    const std::string& html,
    HttpStatusCode statusCode) {
    LOGINFO("创建HTML响应: 长度=" + std::to_string(html.size()));
    return ResponseBuilder::New()
        .Status(statusCode)
        .Header("Content-Type", "text/html; charset=utf-8")
        .Body(html)
        .Build();
}

/**
 * 创建文本响应
 * @param text 文本内容
 * @param statusCode HTTP状态码
 * @return 文本响应的智能指针
 */
std::shared_ptr<HttpResponse> ResponseFactory::CreateText(
    const std::string& text,
    HttpStatusCode statusCode) {
    LOGINFO("创建文本响应: " + text);
    return ResponseBuilder::New()
        .Status(statusCode)
        .Header("Content-Type", "text/plain; charset=utf-8")
        .Body(text)
        .Build();
}
