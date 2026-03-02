#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ChunkedDownloadManager.h"

class DownloadTaskManager {
 public:
  static DownloadTaskManager& Instance();

  // 禁止拷贝和赋值
  DownloadTaskManager(const DownloadTaskManager&) = delete;
  DownloadTaskManager& operator=(const DownloadTaskManager&) = delete;

  // 提交下载任务
  // 返回值: pair<是否是新任务, 任务ID>
  // 如果任务已存在（同一用户同一URL），则返回 false 和现有任务ID
  std::pair<bool, std::string> SubmitTask(const std::string& user_id, const std::string& url,
                                          const std::string& filename, const std::string& base_dir);

  // 获取任务状态
  std::shared_ptr<ChunkedDownloadManager> GetTask(const std::string& task_id);

  // 获取用户的所有任务
  std::vector<std::string> GetUserTasks(const std::string& user_id);

  // 移除任务（清理内存，不删除文件）
  bool RemoveTask(const std::string& task_id);

 private:
  DownloadTaskManager() = default;
  ~DownloadTaskManager() = default;

  std::string GenerateTaskKey(const std::string& user_id, const std::string& url);

  std::mutex mutex_;
  // Map: TaskID -> DownloadManager Instance
  std::unordered_map<std::string, std::shared_ptr<ChunkedDownloadManager>> tasks_;
  // Map: UserID -> List of TaskIDs
  std::unordered_map<std::string, std::vector<std::string>> user_tasks_;
};
