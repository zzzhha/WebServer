#pragma once

/**
 * 模块: DownloadTaskManager
 * 作用: 管理用户维度的下载任务生命周期与索引，避免重复提交并提供任务查询/移除能力
 */
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ChunkedDownloadManager.h"

/**
 * DownloadTaskManager
 * 单例，线程安全：通过内部 mutex 保护任务映射与用户索引
 * 约束：同一用户对同一 URL 仅允许存在一个任务（复用已存在任务）
 */
class DownloadTaskManager {
 public:
  static DownloadTaskManager& Instance();

  // 禁止拷贝和赋值
  DownloadTaskManager(const DownloadTaskManager&) = delete;
  DownloadTaskManager& operator=(const DownloadTaskManager&) = delete;

  /**
 * 提交下载任务
 * 入参:
 * - user_id: 用户标识
 * - url: 下载 URL
 * - filename: 目标文件名
 * - base_dir: 用户下载根目录
 * 返回: pair<是否创建了新任务, 任务ID>；若同用户同 URL 已存在，则返回 {false, 现有任务ID}
 */
std::pair<bool, std::string> SubmitTask(const std::string& user_id, const std::string& url,
                                          const std::string& filename, const std::string& base_dir);

  /**
 * 获取任务管理对象，用于查询状态或进行控制
 */
std::shared_ptr<ChunkedDownloadManager> GetTask(const std::string& task_id);

  /**
 * 获取指定用户的所有任务ID
 */
std::vector<std::string> GetUserTasks(const std::string& user_id);

  /**
 * 移除任务实例（仅释放内存，不删除已下载文件或中间文件）
 */
bool RemoveTask(const std::string& task_id);

 private:
  DownloadTaskManager() = default;
  ~DownloadTaskManager() = default;

  /**
 * 根据 user_id + url 生成稳定的任务键，避免重复提交
 */
std::string GenerateTaskKey(const std::string& user_id, const std::string& url);

  std::mutex mutex_;
  // Map: TaskID -> DownloadManager Instance
  std::unordered_map<std::string, std::shared_ptr<ChunkedDownloadManager>> tasks_;
  // Map: UserID -> List of TaskIDs
  std::unordered_map<std::string, std::vector<std::string>> user_tasks_;
};
