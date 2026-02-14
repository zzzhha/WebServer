/**
 * AppHandlers.cpp：应用处理器工具函数
 * 职责：提供HTTP请求处理的工具函数，不包含业务逻辑
 * - 工具函数：解析表单数据、生成JSON响应、获取Content-Type等
 * - 业务逻辑已迁移到services层（AuthService、DownloadService等）
 * - 静态文件服务已迁移到StaticFileService
 */
#include "handler/AppHandlers.h"
#include "factory/ResponseFactory.h"
#include "logger/log_fac.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <sstream>
#include <algorithm>
#include <cstdlib>

// 解析 POST 表单数据 (application/x-www-form-urlencoded)
std::unordered_map<std::string, std::string> ParseFormData(const std::string& body) {
    std::unordered_map<std::string, std::string> params;
    if (body.empty()) {
        return params;
    }

    size_t pos = 0;
    while (pos < body.length()) {
        size_t amp_pos = body.find('&', pos);
        std::string param_pair;

        if (amp_pos == std::string::npos) {
            param_pair = body.substr(pos);
            pos = body.length();
        } else {
            param_pair = body.substr(pos, amp_pos - pos);
            pos = amp_pos + 1;
        }

        size_t eq_pos = param_pair.find('=');
        std::string key, value;

        if (eq_pos == std::string::npos) {
            key = param_pair;
            value = "";
        } else {
            key = param_pair.substr(0, eq_pos);
            value = param_pair.substr(eq_pos + 1);
        }

        // URL 解码
        // 简单的 URL 解码（将 + 转换为空格，%XX 转换为字符）
        std::string decoded_key, decoded_value;
        for (size_t i = 0; i < key.length(); ++i) {
            if (key[i] == '+') {
                decoded_key += ' ';
            } else if (key[i] == '%' && i + 2 < key.length()) {
                // 简单的十六进制解码
                char hex[3] = {key[i+1], key[i+2], '\0'};
                char c = static_cast<char>(strtol(hex, nullptr, 16));
                decoded_key += c;
                i += 2;
            } else {
                decoded_key += key[i];
            }
        }

        for (size_t i = 0; i < value.length(); ++i) {
            if (value[i] == '+') {
                decoded_value += ' ';
            } else if (value[i] == '%' && i + 2 < value.length()) {
                char hex[3] = {value[i+1], value[i+2], '\0'};
                char c = static_cast<char>(strtol(hex, nullptr, 16));
                decoded_value += c;
                i += 2;
            } else {
                decoded_value += value[i];
            }
        }

        params[decoded_key] = decoded_value;
    }

    return params;
}

// 生成 JSON 响应（向后兼容）
void SetJsonResponse(HttpResponse& response, bool success, const std::string& message, HttpStatusCode statusCode) {
    if (success) {
        SetJsonSuccessResponse(response, message);
    } else {
        SetJsonErrorResponse(response, statusCode, message);
    }
}

// 获取 Content-Type 根据文件扩展名
std::string GetContentType(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string extension = path.substr(dot_pos + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == "html" || extension == "htm") return "text/html";
    if (extension == "css") return "text/css";
    if (extension == "js") return "application/javascript";
    if (extension == "json") return "application/json";
    if (extension == "xml") return "application/xml";
    if (extension == "txt") return "text/plain";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    if (extension == "gif") return "image/gif";
    if (extension == "svg") return "image/svg+xml";
    if (extension == "ico") return "image/x-icon";
    if (extension == "pdf") return "application/pdf";
    if (extension == "zip") return "application/zip";
    if (extension == "mp4") return "video/mp4";
    if (extension == "mp3") return "audio/mpeg";
    if (extension == "webm") return "video/webm";
    if (extension == "avi") return "video/x-msvideo";

    return "application/octet-stream";
}

// JSON成功响应
void SetJsonSuccessResponse(HttpResponse& response, const std::string& message) {
    auto resp = ResponseFactory::CreateSuccess(message);
    response = *resp;
}

void SetJsonSuccessResponseWithData(HttpResponse& response, const std::string& data, const std::string& message) {
    auto resp = ResponseFactory::CreateSuccessWithData(data, message);
    response = *resp;
}

// JSON错误响应
void SetJsonErrorResponse(HttpResponse& response, HttpStatusCode code, const std::string& message) {
    auto resp = ResponseFactory::CreateError(code, message);
    response = *resp;
}

void SetJsonErrorResponseWithDetails(HttpResponse& response, HttpStatusCode code, const std::string& message, const std::map<std::string, std::string>& details) {
    auto resp = ResponseFactory::CreateErrorWithDetails(code, message, details);
    response = *resp;
}

// 文件下载响应
bool SetFileDownloadResponse(HttpResponse& response, const std::string& filepath, const std::string& filename) {
    auto resp = ResponseFactory::CreateFileDownload(filepath, filename);
    if (!resp || resp->getStatusCodeInt() >= 400) {
        return false;
    }
    response = *resp;
    return true;
}

// 静态文件响应
bool SetStaticFileResponse(HttpResponse& response, const std::string& filepath, const std::string& contentType) {
    auto resp = ResponseFactory::CreateStaticFile(filepath, contentType);
    if (!resp || resp->getStatusCodeInt() >= 400) {
        return false;
    }
    response = *resp;
    return true;
}

// 重定向响应
void SetRedirectResponse(HttpResponse& response, const std::string& url, bool permanent) {
    auto resp = ResponseFactory::CreateRedirect(url, permanent);
    response = *resp;
}

// HTML响应
void SetHtmlResponse(HttpResponse& response, const std::string& html, HttpStatusCode statusCode) {
    auto resp = ResponseFactory::CreateHtml(html, statusCode);
    response = *resp;
}

// 文本响应
void SetTextResponse(HttpResponse& response, const std::string& text, HttpStatusCode statusCode) {
    auto resp = ResponseFactory::CreateText(text, statusCode);
    response = *resp;
}


// 注意：业务处理逻辑已迁移到services层
// - 注册/登录逻辑已迁移到AuthService（在HttpServer.cpp中使用）
// - 下载逻辑已迁移到DownloadService（在HttpServer.cpp中使用）
// - 静态文件服务逻辑已迁移到StaticFileService
// 
// 以下函数已不再使用，业务逻辑已迁移：
// - HandleRegister() - 已迁移到HttpServer.cpp中的RegisterRouteHandler()
// - HandleLogin() - 已迁移到HttpServer.cpp中的LoginRouteHandler()
// - HandleDownload() - 已迁移到HttpServer.cpp中的DownloadRouteHandler()
// - HandleStaticFile() - 已迁移到StaticFileService
// - RegisterRouteHandler() - 已迁移到HttpServer.cpp
// - LoginRouteHandler() - 已迁移到HttpServer.cpp
// - DownloadRouteHandler() - 已迁移到HttpServer.cpp
// - StaticFileRouteHandler() - 已迁移到StaticFileService

