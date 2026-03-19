#include "security/RequestRateLimiter.h"
#include "../../logger/log_fac.h"
#include <algorithm>

RequestRateLimiter::RequestRateLimiter() : config_{60, 60}, cleanup_running_{false} {
}

void RequestRateLimiter::SetDefaultConfig(int max_requests, int time_window_seconds) {
    config_.max_requests = max_requests;
    config_.time_window_seconds = time_window_seconds;
}

bool RequestRateLimiter::CheckRateLimit(const std::string& identifier) {
    auto now = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(records_mutex_);
    
    // 定期清理过期记录（每100个请求清理一次）
    static int cleanup_counter = 0;
    if (++cleanup_counter >= 100) {
        cleanup_counter = 0;
        if (!cleanup_running_.exchange(true)) {
            CleanupExpiredInternal();
            cleanup_running_.store(false);
        }
    }
    
    auto& record = request_records_[identifier];
    
    // 清理该标识符的过期时间戳
    auto cutoff = now - std::chrono::seconds(config_.time_window_seconds);
    record.timestamps.erase(
        std::remove_if(record.timestamps.begin(), record.timestamps.end(),
                      [cutoff](const auto& ts) { return ts < cutoff; }),
        record.timestamps.end()
    );
    
    // 检查是否超过限制
    if (record.timestamps.size() >= static_cast<size_t>(config_.max_requests)) {
        LOGWARNING("请求频率限制触发: " + identifier + ", 当前请求数: " + 
                  std::to_string(record.timestamps.size()) + ", 限制: " + 
                  std::to_string(config_.max_requests));
        return false;
    }
    
    // 添加当前时间戳
    record.timestamps.push_back(now);
    return true;
}

void RequestRateLimiter::CleanupExpired() {
    std::lock_guard<std::mutex> lock(records_mutex_);
    CleanupExpiredInternal();
}

void RequestRateLimiter::CleanupExpiredInternal() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(config_.time_window_seconds);
    
    // 清理所有过期的记录
    for (auto it = request_records_.begin(); it != request_records_.end();) {
        auto& timestamps = it->second.timestamps;
        timestamps.erase(
            std::remove_if(timestamps.begin(), timestamps.end(),
                          [cutoff](const auto& ts) { return ts < cutoff; }),
            timestamps.end()
        );
        
        // 如果没有时间戳了，删除该记录
        if (timestamps.empty()) {
            it = request_records_.erase(it);
        } else {
            ++it;
        }
    }
}

const RequestRateLimiter::RateLimitConfig& RequestRateLimiter::GetConfig() const {
    return config_;
}
