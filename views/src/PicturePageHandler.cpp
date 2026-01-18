#include "PicturePageHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../logger/log_fac.h"
#include <fstream>
#include <sys/stat.h>

// 处理图片浏览页面请求
void PicturePageHandler::Handle(HttpRequest* request, HttpResponse& response) {
    // 构建完整的HTML文件路径
    std::string full_path = html_path_ + "/picture.html";
    
    // 检查文件是否存在
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
            response.SetHeader("Content-Type", "text/html; charset=utf-8");
            response.SetHeader("Content-Length", std::to_string(file_content.size()));
            
            LOGINFO("成功读取图片页面: " + full_path + ", 大小: " + std::to_string(file_content.size()));
        } else {
            response.SetStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
            response.SetHeader("Content-Type", "text/plain");
            response.SetBody("Internal Server Error");
            LOGERROR("无法打开文件: " + full_path);
        }
    } else {
        // 文件不存在，返回404
        response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        response.SetHeader("Content-Type", "text/html");
        response.SetBody("<html><body><h1>404 Not Found</h1><p>The requested resource was not found.</p></body></html>");
        LOGINFO("图片页面文件不存在: " + full_path);
    }
}

