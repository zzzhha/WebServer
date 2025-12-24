#include "ErrorHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../logger/log_fac.h"
#include <fstream>
#include <sys/stat.h>

// 通用错误处理方法
void ErrorHandler::HandleError(HttpRequest* request, HttpResponse& response, 
                                int status_code, const std::string& html_filename, 
                                const std::string& default_body) {
    // 构建完整的HTML文件路径
    std::string full_path = html_path_ + "/" + html_filename;
    
    // 尝试读取错误页面HTML文件
    struct stat file_stat;
    std::string html_content;
    
    if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
        // 文件存在，读取文件内容
        std::ifstream file(full_path, std::ios::binary);
        if (file.is_open()) {
            html_content.assign((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
            file.close();
            LOGINFO("成功读取错误页面: " + full_path);
        } else {
            // 文件无法打开，使用默认响应
            html_content = default_body;
            LOGWARNING("无法打开错误页面文件: " + full_path + "，使用默认响应");
        }
    } else {
        // 文件不存在，使用默认响应
        html_content = default_body;
        LOGINFO("错误页面文件不存在: " + full_path + "，使用默认响应");
    }
    
    // 设置HTTP状态码（将int转换为HttpStatusCode枚举）
    HttpStatusCode statusCode = static_cast<HttpStatusCode>(status_code);
    response.SetStatusCode(statusCode);
    
    // 设置响应头
    response.SetHeader("Content-Type", "text/html; charset=utf-8");
    response.SetHeader("Content-Length", std::to_string(html_content.size()));
    
    // 设置响应体
    response.SetBody(html_content);
}

