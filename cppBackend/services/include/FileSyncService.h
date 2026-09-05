#pragma once

#include <atomic>
#include <string>
#include <thread>

/**
 * FileSyncService：文件系统 → files 表 的同步服务。
 * - 启动时 EnsureSchema + 全量回填（Init）。
 * - 后台线程每 sync_interval(秒) 扫描 images/video，upsert 有变化文件、删除已移除文件（Start/Stop）。
 * 文件系统是事实源；本服务把它变成可查询的 DB 目录。
 */
class FileSyncService {
 public:
  // 初始化：EnsureSchema + 首次全量扫描。返回是否成功。
  static bool Init(const std::string& static_path);

  // 手动同步一次（可被 Init 与后台线程调用）。
  static void ScanOnce(const std::string& static_path);

  // 启动后台同步线程。间隔由 SetInterval 设置（秒），调用需在 Init 之后。
  static void Start();

  // 停止并等待后台线程结束。
  static void Stop();

  static void SetInterval(int seconds);

 private:
  static std::atomic<bool> running_;
  static std::thread worker_;
  static int interval_seconds_;
  static std::string static_path_;
};
