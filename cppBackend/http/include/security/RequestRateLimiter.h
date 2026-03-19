#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <mutex>
#include <atomic>

/**
 * 请求频率限制器
 * 功能：限制同一用户ID或IP地址在单位时间内的请求次数
 */
class RequestRateLimiter {
public:
    struct RateLimitConfig {
        int max_requests;      // 单位时间内最大请求数
        int time_window_seconds; // 时间窗口（秒）
    };

    RequestRateLimiter();
    ~RequestRateLimiter() = default;

    // 设置默认配置
    void SetDefaultConfig(int max_requests, int time_window_seconds);

    // 检查请求是否超过频率限制
    // 返回true表示允许请求，false表示拒绝请求
    bool CheckRateLimit(const std::string& identifier);

    // 清理过期的请求记录
    void CleanupExpired();

    // 获取当前配置
    const RateLimitConfig& GetConfig() const;

private:
    struct RequestRecord {
        std::vector<std::chrono::steady_clock::time_point> timestamps;
    };

    RateLimitConfig config_;
    std::unordered_map<std::string, RequestRecord> request_records_;
    std::mutex records_mutex_;
    std::atomic<bool> cleanup_running_;

    // 清理过期记录的内部方法
    void CleanupExpiredInternal();
};
