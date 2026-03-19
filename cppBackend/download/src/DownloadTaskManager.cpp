#include "DownloadTaskManager.h"

/*
 * 简述: 下载任务管理器实现
 * - 维护 task_id -> ChunkedDownloadManager 的映射与 user_id -> [task_id] 索引
 * - 防止同用户重复提交同一 URL（基于 GenerateTaskKey）
 * - 任务启动/查询/移除的线程安全由内部互斥锁保证
 */
#include <algorithm>
#include <functional>
#include <filesystem>

DownloadTaskManager& DownloadTaskManager::Instance() {
  static DownloadTaskManager instance;
  return instance;
}

std::pair<bool, std::string> DownloadTaskManager::SubmitTask(const std::string& user_id, const std::string& url,
                                                             const std::string& filename, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 生成唯一任务Key (用户ID + URL的哈希，防止同一用户重复下载同一URL)
  std::string task_key = GenerateTaskKey(user_id, url);

  // 检查任务是否已存在
  auto it = tasks_.find(task_key);
  if (it != tasks_.end()) {
    // 检查已完成的任务对应的文件是否仍存在
    auto task = it->second;
    if (task->GetState() == DownloadState::Completed) {
      // 构建预期的目标文件路径
      std::string dest_path = ChunkedDownloadManager::BuildDestPath(base_dir, user_id, task_key, filename);
      
      // 如果文件不存在（被用户删除），则从任务列表中移除旧任务，允许重新下载
      if (!std::filesystem::exists(dest_path)) {
         tasks_.erase(it);
         // 同时从 user_tasks_ 中清理（可选，为了严谨）
         auto& u_tasks = user_tasks_[user_id];
         auto t_it = std::find(u_tasks.begin(), u_tasks.end(), task_key);
         if (t_it != u_tasks.end()) u_tasks.erase(t_it);
      } else {
         return {false, task_key};
      }
    } else {
       // 任务正在进行中或暂停，直接返回现有任务
       return {false, task_key};
    }
  }

  // 配置下载参数
  ChunkedDownloadConfig config;
  config.max_concurrency = 4;
  config.chunk_size_bytes = 1024 * 1024; // 1MB chunk

  ChunkedDownloadCallbacks callbacks;
  // 这里可以设置默认的回调，例如记录日志
  // callbacks.on_state = ...

  // 创建新任务
  // 使用 userId 作为 user_key 实现目录隔离: base_dir/userId/taskKey/filename
  // 使用 task_key 作为 task_key
  auto manager = std::make_shared<ChunkedDownloadManager>(url, base_dir, user_id, task_key, filename, config, callbacks);

  // 尝试启动任务
  if (manager->Start()) {
    tasks_[task_key] = manager;
    user_tasks_[user_id].push_back(task_key);
    return {true, task_key};
  }

  return {false, ""};
}

std::shared_ptr<ChunkedDownloadManager> DownloadTaskManager::GetTask(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = tasks_.find(task_id);
  if (it != tasks_.end()) {
    return it->second;
  }
  return nullptr;
}

std::vector<std::string> DownloadTaskManager::GetUserTasks(const std::string& user_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = user_tasks_.find(user_id);
  if (it != user_tasks_.end()) {
    return it->second;
  }
  return {};
}

bool DownloadTaskManager::RemoveTask(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    return false;
  }

  // 停止任务（如果还在运行）
  it->second->Cancel();
  
  // 从 user_tasks_ 中移除
  // 这里需要遍历所有用户找到该任务，或者在 task key 中包含 user id 信息来反查
  // 由于 GenerateTaskKey 包含 user_id，我们可以解析它，或者简单遍历
  // 简单起见，遍历 user_tasks_
  for (auto& [uid, tasks] : user_tasks_) {
    auto tit = std::find(tasks.begin(), tasks.end(), task_id);
    if (tit != tasks.end()) {
      tasks.erase(tit);
      break; 
    }
  }

  tasks_.erase(it);
  return true;
}

std::string DownloadTaskManager::GenerateTaskKey(const std::string& user_id, const std::string& url) {
  std::hash<std::string> hasher;
  size_t url_hash = hasher(url);
  return user_id + "_" + std::to_string(url_hash);
}
