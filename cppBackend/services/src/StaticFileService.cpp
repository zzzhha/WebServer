#include "StaticFileService.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/handler/AppHandlers.h"  // 使用AppHandlers中的GetContentType工具函数
#include "../../logger/log_fac.h"
#include <fstream>
#include <sys/stat.h>

// 处理静态文件请求
bool StaticFileService::HandleStaticFile(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
    if (!request) {
        LOGERROR("静态文件服务失败：请求对象为空");
        return false;
    }

    // 业务验证：检查请求方法
    if (request->GetMethod() != HttpMethod::GET) {
        response.SetStatusCode(HttpStatusCode::METHOD_NOT_ALLOWED);
        response.SetHeader("Content-Type", "text/plain");
        response.SetBody("Method Not Allowed");
        return false;
    }

    std::string path = request->GetPath();
    
    // 构建完整的文件路径
    std::string full_path = static_path + path;

    // 如果路径以 / 结尾，添加默认的 index.html
    if (path.empty() || path[path.length() - 1] == '/') {
        full_path += "index.html";
    }

    // 业务判断：检查文件是否存在
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
        // 文件存在，读取文件内容
        std::ifstream file(full_path, std::ios::binary);
        if (file.is_open()) {
            std::string file_content((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
            file.close();
            
            // 设置响应
            response.SetStatusCode(HttpStatusCode::OK);
            response.SetBody(file_content);
            response.SetHeader("Content-Type", GetContentType(full_path));
            response.SetHeader("Content-Length", std::to_string(file_content.size()));
            
            LOGINFO("成功读取静态文件: " + full_path + ", 大小: " + std::to_string(file_content.size()));
            return true;
        } else {
            response.SetStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
            response.SetHeader("Content-Type", "text/plain");
            response.SetBody("Internal Server Error");
            LOGERROR("无法打开文件: " + full_path);
            return false;
        }
    } else {
        // 文件不存在，返回404
        response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        response.SetHeader("Content-Type", "text/html");
        response.SetBody("<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
        LOGINFO("静态文件不存在: " + full_path);
        return false;
    }
}

