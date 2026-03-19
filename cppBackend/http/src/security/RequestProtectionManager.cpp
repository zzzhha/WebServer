#include "security/RequestProtectionManager.h"
#include "../../include/core/HttpRequest.h"
#include "../../include/core/HttpResponse.h"
#include "../../logger/log_fac.h"
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <string>

RequestProtectionManager::RequestProtectionManager() {
    rate_limiter_ = std::make_unique<RequestRateLimiter>();
    deduplicator_ = std::make_unique<RequestDeduplicator>();
    
    // 设置默认配置
    rate_limiter_->SetDefaultConfig(60, 60); // 每分钟60个请求
    deduplicator_->SetConfig(5, 10000); // 5秒窗口，最大缓存10000个请求
}

void RequestProtectionManager::ConfigureRateLimit(int max_requests, int time_window_seconds) {
    rate_limiter_->SetDefaultConfig(max_requests, time_window_seconds);
}

void RequestProtectionManager::ConfigureDeduplication(int time_window_seconds, int max_cache_size) {
    deduplicator_->SetConfig(time_window_seconds, max_cache_size);
}

bool RequestProtectionManager::ShouldBlockRequest(const HttpRequest& request, const std::string& client_ip, HttpResponse& response) {
    // 1. 检查频率限制
    std::string user_identifier = client_ip;
    
    // 如果有用户ID，使用用户ID作为标识符
    auto user_id = request.GetHeader("X-User-ID");
    if (user_id.has_value() && !user_id.value().empty()) {
        user_identifier = user_id.value();
    }
    
    if (!rate_limiter_->CheckRateLimit(user_identifier)) {
        response.SetStatusCodeInt(429); // Too Many Requests
        response.SetStatusReason("Too Many Requests");
        response.SetHeader("Content-Type", "application/json");
        response.SetBody(R"({"error": "Too many requests", "message": "Rate limit exceeded"})");
        LogBlockedRequest("rate_limit", request, client_ip);
        return true;
    }
    
    // 2. 检查请求去重
    std::string request_hash = GenerateRequestHash(request);
    if (deduplicator_->IsDuplicate(request_hash)) {
        response.SetStatusCodeInt(409); // Conflict
        response.SetStatusReason("Conflict");
        response.SetHeader("Content-Type", "application/json");
        response.SetBody(R"({"error": "Duplicate request", "message": "Request already processed"})");
        LogBlockedRequest("duplicate", request, client_ip);
        return true;
    }
    
    return false;
}

std::string RequestProtectionManager::GenerateRequestHash(const HttpRequest& request) {
    std::stringstream hash_input;
    hash_input << request.GetMethodString() << " " << request.GetPath() << " " << request.GetVersionStr();
    
    // 添加请求头
    auto headers = request.GetAllHeaders();
    for (const auto& [key, value] : headers) {
        // 排除可能变化的头
        if (key != "Host" && key != "User-Agent" && key != "Referer" && 
            key != "Accept-Encoding" && key != "Connection" && 
            key != "Content-Length" && key != "X-Forwarded-For") {
            hash_input << key << ":" << value << ";";
        }
    }
    
    // 添加请求体
    hash_input << request.GetBody();
    
    // 使用SHA256计算哈希
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    const EVP_MD* md = EVP_sha256();
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    EVP_DigestInit_ex(mdctx, md, NULL);
    std::string input_str = hash_input.str();
    EVP_DigestUpdate(mdctx, input_str.c_str(), input_str.length());
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    
    // 转换为十六进制字符串
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }
    
    return ss.str();
}

void RequestProtectionManager::LogBlockedRequest(const std::string& reason, const HttpRequest& request, const std::string& client_ip) {
    std::string method = request.GetMethodString();
    std::string path = request.GetPath();
    
    LOGINFO("请求被拦截 - 原因: " + reason + ", IP: " + client_ip + ", 方法: " + method + ", 路径: " + path);
}
