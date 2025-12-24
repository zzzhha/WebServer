#include "DownloadService.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include "../../http/include/handler/AppHandlers.h"  // 使用AppHandlers中的GetContentType工具函数
#include "../../logger/log_fac.h"
#include <fstream>
#include <sys/stat.h>

// 处理下载请求
bool DownloadService::HandleDownload(HttpRequest* request, HttpResponse& response, const std::string& static_path) {
    if (!request) {
        LOGERROR("下载失败：请求对象为空");
        return false;
    }

    // 业务验证：检查请求方法
    if (request->GetMethod() != HttpMethod::GET) {
        LOGWARNING("下载失败：只支持GET方法");
        return false;
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
            
            // 验证文件夹只能是images或video
            if (folder != "images" && folder != "video") {
                LOGWARNING("下载失败：非法的文件夹 - " + folder);
                return false;
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
            return false;
        }
    }

    // 构建文件路径
    std::string file_path = static_path + "/" + folder + "/" + filename;

    // 业务验证：验证文件路径安全性
    if (!ValidateFilePath(file_path)) {
        LOGWARNING("下载失败：文件路径不安全 - " + file_path);
        return false;
    }

    // 业务判断：检查文件是否存在
    if (!FileExists(file_path)) {
        LOGWARNING("下载失败：文件不存在 - " + file_path);
        return false;
    }

    // 读取文件内容
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        LOGERROR("下载失败：无法打开文件 - " + file_path);
        return false;
    }

    std::string file_content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    file.close();

    // 设置下载响应头
    response.SetStatusCode(HttpStatusCode::OK);
    response.SetBody(file_content);
    response.SetHeader("Content-Type", GetContentType(file_path));
    response.SetHeader("Content-Length", std::to_string(file_content.size()));
    response.SetHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");

    LOGINFO("成功下载文件: " + file_path + ", 大小: " + std::to_string(file_content.size()));
    return true;
}

// 验证文件路径安全性
// 安全检查：
// 1. 防止路径遍历攻击（../ 和 ..\\）
// 2. 防止空字符注入
// 3. 注意：绝对路径检查由调用方处理（通过static_path限制）
bool DownloadService::ValidateFilePath(const std::string& file_path) {
    // 防止路径遍历攻击：检查是否包含 ../
    if (file_path.find("../") != std::string::npos) {
        return false;
    }

    // 防止路径遍历攻击：检查是否包含 ..\\（Windows路径）
    if (file_path.find("..\\") != std::string::npos) {
        return false;
    }

    // 验证路径格式：不能包含空字符（防止空字符注入攻击）
    if (file_path.find('\0') != std::string::npos) {
        return false;
    }

    return true;
}

// 检查文件是否存在
bool DownloadService::FileExists(const std::string& file_path) {
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        return false;
    }
    
    // 检查是否是普通文件
    return S_ISREG(file_stat.st_mode);
}

