#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>

/**
 * 请求去重器
 * 功能：对用户提交的请求内容进行哈希计算，建立短期缓存机制
 * 对于相同哈希值的重复请求在指定时间窗口内只处理一次
 */
class RequestDeduplicator {
public:
    struct DedupConfig {
        int time_window_seconds; // 时间窗口（秒）
        int max_cache_size;      // 最大缓存大小
    };

    RequestDeduplicator();
    ~RequestDeduplicator() = default;

    // 设置配置
    void SetConfig(int time_window_seconds, int max_cache_size);

    // 检查请求是否重复
    // 返回true表示请求重复，false表示请求不重复
    bool IsDuplicate(const std::string& request_hash);

    // 清理过期的缓存记录
    void CleanupExpired();

    // 获取当前配置
    const DedupConfig& GetConfig() const;

private:
    struct DeduplicateRecord {
        std::chrono::steady_clock::time_point timestamp;
    };

    DedupConfig config_;
    std::unordered_map<std::string, DeduplicateRecord> request_cache_;
    std::mutex cache_mutex_;
    std::atomic<bool> cleanup_running_;

    // 清理过期记录的内部方法
    void CleanupExpiredInternal();
};
