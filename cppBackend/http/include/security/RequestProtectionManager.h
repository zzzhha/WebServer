#pragma once

#include "security/RequestRateLimiter.h"
#include "security/RequestDeduplicator.h"
#include <string>
#include <memory>

class HttpRequest;
class HttpResponse;

/**
 * 请求防护管理器
 * 功能：整合请求频率限制和请求去重功能
 * 提供统一的接口进行请求防护
 */
class RequestProtectionManager {
public:
    RequestProtectionManager();
    ~RequestProtectionManager() = default;

    // 配置频率限制
    void ConfigureRateLimit(int max_requests, int time_window_seconds);

    // 配置请求去重
    void ConfigureDeduplication(int time_window_seconds, int max_cache_size);

    // 检查请求是否应该被拦截
    // 返回true表示请求被拦截，false表示请求通过
    bool ShouldBlockRequest(const HttpRequest& request, const std::string& client_ip, HttpResponse& response);

    // 生成请求哈希
    std::string GenerateRequestHash(const HttpRequest& request);

private:
    std::unique_ptr<RequestRateLimiter> rate_limiter_;
    std::unique_ptr<RequestDeduplicator> deduplicator_;

    // 记录拦截日志
    void LogBlockedRequest(const std::string& reason, const HttpRequest& request, const std::string& client_ip);
};
