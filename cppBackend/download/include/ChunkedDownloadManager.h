#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ThreadPool;

enum class ChunkStatus { NotStarted = 0, Downloading = 1, Completed = 2, Failed = 3 };

enum class DownloadState { Idle = 0, Downloading = 1, Paused = 2, Completed = 3, Failed = 4, Canceled = 5 };

enum class DownloadError {
  None = 0,
  InvalidUrl,
  NetworkTimeout,
  NetworkError,
  HttpStatusError,
  DiskSpaceInsufficient,
  FileWriteFailed,
  MetadataError,
  MergeFailed,
  Md5Mismatch
};

struct ChunkInfo {
  uint64_t index{0};
  uint64_t start{0};
  uint64_t end{0};
  ChunkStatus status{ChunkStatus::NotStarted};
  int retries{0};
};

struct ChunkedDownloadConfig {
  uint64_t chunk_size_bytes{1024 * 1024};
  int max_concurrency{4};
  int max_retries{3};
  int timeout_ms{3000};
  int progress_interval_ms{200};
};

struct ChunkedDownloadCallbacks {
  std::function<void(DownloadState)> on_state;
  std::function<void(double percent, uint64_t downloaded, uint64_t total, double bytes_per_sec)> on_progress;
  std::function<void(const ChunkInfo&)> on_chunk;
  std::function<void(DownloadError, const std::string&)> on_error;
};

class ChunkedDownloadManager {
 public:
  ChunkedDownloadManager(std::string url, std::string dest_path, ChunkedDownloadConfig config,
                         ChunkedDownloadCallbacks callbacks);
  ChunkedDownloadManager(std::string url, std::string base_dir, std::string user_key, std::string task_key,
                         std::string filename, ChunkedDownloadConfig config, ChunkedDownloadCallbacks callbacks);
  ~ChunkedDownloadManager();

  bool Start();
  void Pause();
  void Resume();
  void Cancel();
  void Wait();

  DownloadState GetState() const;
  uint64_t GetTotalBytes() const;
  uint64_t GetDownloadedBytes() const;
  std::vector<ChunkInfo> GetChunksSnapshot() const;

  static std::string FormatSpeed(double bytes_per_sec);
  static std::vector<ChunkInfo> BuildChunks(uint64_t total_bytes, uint64_t chunk_size_bytes);
  static std::string BuildDestPath(const std::string& base_dir, const std::string& user_key, const std::string& task_key,
                                   const std::string& filename);

 private:
  bool InitOrResume();
  bool FetchRemoteMetadata();
  void SchedulerLoop();
  void ProgressLoop();
  bool FinalizeDownload();
  bool MergeParts(std::string& error);
  bool VerifyMd5(std::string& error);
  void CleanupArtifacts();

  bool LoadMetaFile();
  bool SaveMetaFile() const;
  std::string MetaPath() const;

  bool EnsureDiskSpace(uint64_t bytes_needed, std::string& error) const;
  bool DownloadOneChunk(uint64_t chunk_index);
  void SetState(DownloadState st);
  void ReportError(DownloadError err, const std::string& msg);

  static bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path);

 private:
  std::string url_;
  std::string dest_path_;
  ChunkedDownloadConfig config_;
  ChunkedDownloadCallbacks callbacks_;

  mutable std::mutex mu_;
  std::vector<ChunkInfo> chunks_;
  uint64_t total_bytes_{0};
  std::optional<std::string> expected_md5_hex_;
  std::optional<std::string> actual_md5_hex_;

  std::unique_ptr<ThreadPool> pool_;
  std::thread scheduler_thread_;
  std::thread progress_thread_;

  std::atomic<DownloadState> state_{DownloadState::Idle};
  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<uint64_t> downloaded_bytes_{0};
  std::atomic<uint64_t> bytes_written_since_start_{0};
  std::chrono::steady_clock::time_point start_tp_;
};
