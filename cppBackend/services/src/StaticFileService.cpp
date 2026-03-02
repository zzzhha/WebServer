#include "StaticFileService.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/handler/AppHandlers.h"  // 使用AppHandlers中的GetContentType工具函数
#include "../../logger/log_fac.h"
#include "FileServeUtil.h"
#include <sys/stat.h>

// 处理静态文件请求
bool StaticFileService::HandleStaticFile(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
    if (!request) {
        LOGERROR("静态文件服务失败：请求对象为空");
        response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Bad Request");
        return true;
    }

    // 业务验证：检查请求方法
    if (request->GetMethod() != HttpMethod::GET && request->GetMethod() != HttpMethod::HEAD) {
        response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Method Not Allowed");
        return true;
    }

    std::string path = request->GetPath();
    
    std::string req_path = path;
    if (req_path.empty() || req_path[req_path.length() - 1] == '/') {
        req_path += "index.html";
    }

    std::string full_path;
    if (!FileServeUtil::ResolvePathUnderRoot(static_path, req_path, full_path)) {
        response.SetStatusCode(HttpStatusCode::FORBIDDEN);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Forbidden");
        return true;
    }

    // 业务判断：检查文件是否存在
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
        uint64_t file_size = 0;
        if (!FileServeUtil::GetFileSize(full_path, file_size)) {
            response.SetStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
            response.SetHeader("Content-Type", "text/plain");
            response.SetBody("Internal Server Error");
            LOGERROR("无法获取文件信息: " + full_path);
            return true;
        }

        std::string last_modified = FileServeUtil::ToHttpDate(file_stat.st_mtime);
        std::string etag = FileServeUtil::BuildWeakEtag(file_stat.st_mtime, file_size);
        response.SetHeader("Last-Modified", last_modified);
        response.SetHeader("ETag", etag);
        response.SetHeader("Cache-Control", "public, max-age=3600");

        auto if_none_match = request->GetHeader("If-None-Match");
        if (if_none_match && *if_none_match == etag) {
            response.SetStatusCode(HttpStatusCode::NOT_MODIFIED);
            response.SetBody("");
            return true;
        }

        auto if_modified_since = request->GetHeader("If-Modified-Since");
        if (if_modified_since) {
            time_t since = 0;
            if (FileServeUtil::ParseHttpDate(*if_modified_since, since)) {
                if (since >= file_stat.st_mtime) {
                    response.SetStatusCode(HttpStatusCode::NOT_MODIFIED);
                    response.SetBody("");
                    return true;
                }
            }
        }

        response.SetHeader("Accept-Ranges", "bytes");
        response.SetHeader("Content-Type", GetContentType(full_path));

        bool want_md5 = false;
        if (request->GetQueryParam("md5") == "1") want_md5 = true;
        auto md5_header = request->GetHeader("X-Request-MD5");
        if (md5_header && *md5_header == "1") want_md5 = true;
        if (want_md5) {
            if (auto md5 = FileServeUtil::ComputeFileMd5Hex(full_path); md5) {
                response.SetHeader("X-File-MD5", *md5);
            }
        }

        FileRange range;
        auto range_value = request->GetHeader("Range");
        if (range_value) {
            if (!FileServeUtil::ParseRangeHeader(*range_value, file_size, range)) {
                response.SetStatusCode(HttpStatusCode::RANGE_NOT_SATISFIABLE);
                response.SetHeader("Content-Range", "bytes */" + std::to_string(file_size));
                response.SetHeader("Content-Type", "text/plain");
                response.SetBody("Range Not Satisfiable");
                return true;
            }
        }
        
        if (request->GetMethod() == HttpMethod::HEAD) {
            if (range.enabled) {
                uint64_t length = range.end - range.start + 1;
                response.SetStatusCode(HttpStatusCode::PARTIAL_CONTENT);
                response.SetHeader("Content-Length", std::to_string(length));
                response.SetHeader("Content-Range",
                                   "bytes " + std::to_string(range.start) + "-" + std::to_string(range.end) + "/" +
                                       std::to_string(file_size));
            } else {
                response.SetStatusCode(HttpStatusCode::OK);
                response.SetHeader("Content-Length", std::to_string(file_size));
            }
            response.SetBody("");
            return true;
        }

        if (range.enabled) {
            uint64_t length = range.end - range.start + 1;
            response.SetStatusCode(HttpStatusCode::PARTIAL_CONTENT);
            response.SetHeader("Content-Length", std::to_string(length));
            response.SetHeader("Content-Range",
                               "bytes " + std::to_string(range.start) + "-" + std::to_string(range.end) + "/" +
                                   std::to_string(file_size));
            response.SetBody("");
            response.SetSendFile(full_path, range.start, length);
            return true;
        }

        response.SetStatusCode(HttpStatusCode::OK);
        response.SetHeader("Content-Length", std::to_string(file_size));
        response.SetBody("");
        response.SetSendFile(full_path, 0, file_size);

        LOGINFO("成功读取静态文件: " + full_path + ", 大小: " + std::to_string(file_size));
        return true;
    } else {
        // 文件不存在，返回404
        response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        response.SetHeader("Content-Type", "text/html");
        response.SetBody("<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
        LOGINFO("静态文件不存在: " + full_path);
        return true;
    }
}
