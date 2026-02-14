#pragma once

#include "core/HttpResponse.h"
#include <string>
#include <optional>
#include <map>

/**
 * ResponseFactory类：响应工厂
 * 职责：提供标准化的响应生成方法
 * - 基于ResponseBuilder实现
 * - 提供静态方法生成各种类型的响应
 * - 支持成功、错误、文件下载、静态文件、重定向等响应类型
 */
class ResponseFactory {
public:
    // 成功响应
    static std::shared_ptr<HttpResponse> CreateSuccess(
        const std::string& message = "操作成功",
        HttpStatusCode statusCode = HttpStatusCode::OK
    );

    static std::shared_ptr<HttpResponse> CreateSuccessWithData(
        const std::string& data,
        const std::string& message = "操作成功",
        HttpStatusCode statusCode = HttpStatusCode::OK
    );

    // 错误响应
    static std::shared_ptr<HttpResponse> CreateError(
        HttpStatusCode code,
        const std::string& message
    );

    static std::shared_ptr<HttpResponse> CreateErrorWithDetails(
        HttpStatusCode code,
        const std::string& message,
        const std::map<std::string, std::string>& details
    );

    // 文件下载响应
    static std::shared_ptr<HttpResponse> CreateFileDownload(
        const std::string& filepath,
        const std::string& filename = ""
    );

    // 静态文件响应
    static std::shared_ptr<HttpResponse> CreateStaticFile(
        const std::string& filepath,
        const std::string& contentType = ""
    );

    // 重定向响应
    static std::shared_ptr<HttpResponse> CreateRedirect(
        const std::string& url,
        bool permanent = false
    );

    // HTML响应
    static std::shared_ptr<HttpResponse> CreateHtml(
        const std::string& html,
        HttpStatusCode statusCode = HttpStatusCode::OK
    );

    // 文本响应
    static std::shared_ptr<HttpResponse> CreateText(
        const std::string& text,
        HttpStatusCode statusCode = HttpStatusCode::OK
    );

private:
    ResponseFactory() = delete;
    ~ResponseFactory() = delete;
};
