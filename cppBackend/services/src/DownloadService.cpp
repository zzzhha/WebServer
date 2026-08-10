#include "DownloadService.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/handler/AppHandlers.h"  // 使用AppHandlers中的GetContentType工具函数
#include "../../logger/log_fac.h"
#include "FileServeUtil.h"
#include <algorithm>
#include <sys/stat.h>

namespace {

// 中危修复：日志注入防护 —— 用户可控值写入日志前剥离 CR/LF
std::string SanitizeForLog(const std::string& v) {
  std::string out = v;
  out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
  out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
  return out;
}

// 中危修复：If-None-Match 支持逗号分隔多值 / 弱 ETag（W/"..."）前缀比较
bool IfNoneMatchMatches(const std::string& header, const std::string& etag) {
  auto stripWeak = [](std::string v) {
    if (v.size() >= 3 && (v[0] == 'W' || v[0] == 'w') && v[1] == '/') v = v.substr(2);
    return v;
  };
  const std::string target = stripWeak(etag);
  size_t pos = 0;
  while (pos <= header.size()) {
    size_t comma = header.find(',', pos);
    std::string tok =
        header.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t b = tok.find_first_not_of(" \t");
    const size_t e = tok.find_last_not_of(" \t");
    if (b != std::string::npos) {
      tok = tok.substr(b, e - b + 1);
      if (tok == "*") return true;
      if (stripWeak(tok) == target) return true;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return false;
}

}  // namespace

// 处理下载请求
bool DownloadService::HandleDownload(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
    if (!request) {
        LOGERROR("下载失败：请求对象为空");
        response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Bad Request");
        return true;
    }

    // 业务验证：检查请求方法
    if (request->GetMethod() != HttpMethod::GET && request->GetMethod() != HttpMethod::HEAD) {
        LOGWARNING("下载失败：只支持GET/HEAD方法");
        response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Method Not Allowed");
        return true;
    }

    // 从路径或查询参数获取文件名和文件夹
    std::string path = request->GetPath();
    std::string filename;
    std::string folder = "video";  // 默认文件夹为video
    
    // 解析下载路径（格式：/download/[folder]/[filename]）
    size_t download_pos = path.find("/download/");
    if (download_pos != std::string::npos) {
        size_t start = download_pos + 10; // "/download/" 的长度
        
        // 查找下一个斜杠，确定文件夹名称
        size_t folder_slash = path.find('/', start);
        if (folder_slash != std::string::npos) {
            folder = path.substr(start, folder_slash - start);
            
            // 验证文件夹只能是images、video或uploads
            if (folder != "images" && folder != "video" && folder != "uploads") {
                LOGWARNING("下载失败：非法的文件夹 - " + SanitizeForLog(folder));
                response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
                response.SetHeader("Content-Type", "text/plain");
                response.SetBody("Bad Request");
                return true;
            }
            
            // 提取文件名
            if (folder_slash + 1 < path.length()) {
                filename = path.substr(folder_slash + 1);
            }
        }
    }
    
    // 如果路径中没有获取到文件名，尝试从查询参数获取
    if (filename.empty()) {
        auto query_file = request->GetQueryParam("file");
        if (!query_file.empty()) {
            filename = query_file;
        } else {
            // 如果仍未获取到文件名，返回错误
            LOGWARNING("下载失败：未指定文件名");
            response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
            response.SetHeader("Content-Type", "text/plain");
            response.SetBody("Bad Request");
            return true;
        }
    }
    
    if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos || filename.find('\0') != std::string::npos) {
        LOGWARNING("下载失败：非法的文件名 - " + SanitizeForLog(filename));
        response.SetStatusCode(HttpStatusCode::BAD_REQUEST);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Bad Request");
        return true;
    }

    // 构建文件路径
    // H10 修复：统一走 ResolvePathUnderRoot（weakly_canonical 解析符号链接 + 前缀比对），
    // 替代仅字符串过滤的 ValidateFilePath——后者拦不住静态目录内指向根目录外
    // （如 /etc/passwd）的符号链接，stat/SetSendFile 会跟随链接实现任意文件读取。
    std::string rel_path = folder + "/" + filename;
    std::string file_path;
    if (!FileServeUtil::ResolvePathUnderRoot(static_path, rel_path, file_path)) {
        LOGWARNING("下载失败：文件路径不安全 - " + SanitizeForLog(rel_path));
        response.SetStatusCode(HttpStatusCode::FORBIDDEN);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Forbidden");
        return true;
    }

    // 业务判断：检查文件是否存在
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        LOGWARNING("下载失败：文件不存在 - " + SanitizeForLog(file_path));
        response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Not Found");
        return true;
    }

    // 1. 设置 HTTP 缓存头 (Last-Modified / Cache-Control)
    uint64_t file_size = 0;
    if (!FileServeUtil::GetFileSize(file_path, file_size)) {
        LOGERROR("下载失败：无法获取文件信息 - " + SanitizeForLog(file_path));
        response.SetStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Internal Server Error");
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
    response.SetHeader("Content-Type", GetContentType(file_path));
    auto sanitize_filename = [](std::string v) {
        v.erase(std::remove(v.begin(), v.end(), '\r'), v.end());
        v.erase(std::remove(v.begin(), v.end(), '\n'), v.end());
        for (auto& c : v) {
            if (c == '"') c = '_';
        }
        if (v.empty()) v = "download";
        return v;
    };
    // 低危修复：默认 inline，允许 <video>/<img> 内嵌播放；
    // 仅当客户端显式携带 ?download=1 时才返回 attachment（强制下载）。
    // 前端下载按钮通过 <a download> 属性即可触发下载，不依赖服务端 attachment。
    bool force_download = request->GetQueryParam("download") == "1";
    std::string disposition = force_download ? "attachment" : "inline";
    response.SetHeader("Content-Disposition", disposition + "; filename=\"" + sanitize_filename(filename) + "\"");

    FileRange range;
    auto range_value = request->GetHeader("Range");
    if (range_value) {
        if (!FileServeUtil::ParseRangeHeader(*range_value, file_size, range)) {
            response.SetStatusCode(HttpStatusCode::RANGE_NOT_SATISFIABLE);
            response.SetHeader("Content-Range", "bytes */" + std::to_string(file_size));
            response.SetHeader("Content-Type", "text/plain");
            // 中危修复：HEAD 请求按规范不得返回 body
            if (request->GetMethod() != HttpMethod::HEAD) {
                response.SetBody("Range Not Satisfiable");
            }
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
        response.SetSendFile(file_path, range.start, length);
        return true;
    }

    response.SetStatusCode(HttpStatusCode::OK);
    response.SetHeader("Content-Length", std::to_string(file_size));
    response.SetBody("");
    response.SetSendFile(file_path, 0, file_size);

    LOGINFO("成功下载文件: " + file_path + ", 大小: " + std::to_string(file_size));
    return true;
}

// 验证文件路径安全性
// 安全检查：
// 检查文件是否存在
bool DownloadService::FileExists(const std::string& file_path) {
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        return false;
    }
    
    // 检查是否是普通文件
    return S_ISREG(file_stat.st_mode);
}
