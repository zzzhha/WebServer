#include "security/RequestDeduplicator.h"
#include "../../logger/log_fac.h"
#include <algorithm>

RequestDeduplicator::RequestDeduplicator() : config_{5, 10000}, cleanup_running_{false} {
}

void RequestDeduplicator::SetConfig(int time_window_seconds, int max_cache_size) {
    config_.time_window_seconds = time_window_seconds;
    config_.max_cache_size = max_cache_size;
}

bool RequestDeduplicator::IsDuplicate(const std::string& request_hash) {
    auto now = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 定期清理过期记录（每100个请求清理一次）
    static int cleanup_counter = 0;
    if (++cleanup_counter >= 100) {
        cleanup_counter = 0;
        if (!cleanup_running_.exchange(true)) {
            CleanupExpiredInternal();
            cleanup_running_.store(false);
        }
    }
    
    // 检查缓存大小
    if (request_cache_.size() >= static_cast<size_t>(config_.max_cache_size)) {
        // 缓存已满，清理过期记录
        CleanupExpiredInternal();
        
        // 如果仍然已满，删除最旧的记录
        if (request_cache_.size() >= static_cast<size_t>(config_.max_cache_size)) {
            auto oldest_it = request_cache_.begin();
            for (auto it = request_cache_.begin(); it != request_cache_.end(); ++it) {
                if (it->second.timestamp < oldest_it->second.timestamp) {
                    oldest_it = it;
                }
            }
            request_cache_.erase(oldest_it);
        }
    }
    
    // 检查是否存在相同的哈希
    auto it = request_cache_.find(request_hash);
    if (it != request_cache_.end()) {
        // 检查是否在时间窗口内
        auto cutoff = now - std::chrono::seconds(config_.time_window_seconds);
        if (it->second.timestamp >= cutoff) {
            LOGWARNING("请求去重触发: " + request_hash);
            return true;
        } else {
            // 已过期，更新时间戳
            it->second.timestamp = now;
            return false;
        }
    }
    
    // 添加新的哈希记录
    request_cache_[request_hash] = {now};
    return false;
}

void RequestDeduplicator::CleanupExpired() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    CleanupExpiredInternal();
}

void RequestDeduplicator::CleanupExpiredInternal() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(config_.time_window_seconds);
    
    // 清理所有过期的记录
    for (auto it = request_cache_.begin(); it != request_cache_.end();) {
        if (it->second.timestamp < cutoff) {
            it = request_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

const RequestDeduplicator::DedupConfig& RequestDeduplicator::GetConfig() const {
    return config_;
}
