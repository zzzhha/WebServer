#pragma once

/**
 * 模块: ChunkedDownloadManager
 * 作用: 提供多线程分块下载与断点续传能力，负责进度管理、错误重试、合并校验与资源清理。
 * 特性:
 * - Range 请求：对每个分块使用 HTTP Range 进行按字节段下载
 * - 续传机制：通过 .meta 元数据文件记录分块状态，重启后恢复
 * - 并发控制：线程池调度，限制最大并发与超时重试
 * - 完整性校验：支持 MD5 校验，确保合并后的文件正确
 * 线程模型:
 * - scheduler_thread 负责调度待下载分块
 * - progress_thread 周期性上报总体进度与速度
 * - pool_ 中的工作线程执行具体分块下载
 */
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

/**
 * 分块状态
 * - NotStarted: 尚未开始
 * - Downloading: 正在下载
 * - Completed: 分块下载完成且持久化成功
 * - Failed: 下载或写入失败，待重试
 */
enum class ChunkStatus { NotStarted = 0, Downloading = 1, Completed = 2, Failed = 3 };

/**
 * 任务整体状态
 * - Idle: 初始化但未开始
 * - Downloading: 正在下载
 * - Paused: 已暂停，保留进度
 * - Completed: 所有分块完成并合并校验通过
 * - Failed: 发生不可恢复错误
 * - Canceled: 用户取消，停止后清理中间文件
 */
enum class DownloadState { Idle = 0, Downloading = 1, Paused = 2, Completed = 3, Failed = 4, Canceled = 5 };

/**
 * 错误类别
 * - InvalidUrl: URL 解析失败或不合法
 * - NetworkTimeout/NetworkError: 网络连接/读写异常
 * - HttpStatusError: 响应状态码异常（非 2xx/206）
 * - DiskSpaceInsufficient: 磁盘空间不足
 * - FileWriteFailed: 文件写入失败
 * - MetadataError: 元数据文件读写失败
 * - MergeFailed: 分块合并失败
 * - Md5Mismatch: 合并后 MD5 校验不一致
 */
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

/**
 * 分块信息
 * index: 分块序号，从 0 开始
 * start/end: 字节范围，闭区间 [start, end]
 * status: 当前分块状态
 * retries: 已重试次数（不含当前进行中的尝试）
 */
struct ChunkInfo {
  uint64_t index{0};
  uint64_t start{0};
  uint64_t end{0};
  ChunkStatus status{ChunkStatus::NotStarted};
  int retries{0};
};

/**
 * 下载配置
 * chunk_size_bytes: 分块大小（字节），建议与服务端 sendfile/缓冲策略协调
 * max_concurrency: 最大并发下载线程数
 * max_retries: 每个分块最大重试次数
 * timeout_ms: 单次请求超时时间
 * progress_interval_ms: 进度上报间隔（毫秒）
 */
struct ChunkedDownloadConfig {
  uint64_t chunk_size_bytes{1024 * 1024};
  int max_concurrency{4};
  int max_retries{3};
  int timeout_ms{3000};
  int progress_interval_ms{200};
};

/**
 * 回调接口
 * on_state: 状态变化通知
 * on_progress: 进度通知（百分比、已下载、总大小、速度）
 * on_chunk: 分块级事件（开始/完成/失败）
 * on_error: 错误通知（包含可读消息）
 */
struct ChunkedDownloadCallbacks {
  std::function<void(DownloadState)> on_state;
  std::function<void(double percent, uint64_t downloaded, uint64_t total, double bytes_per_sec)> on_progress;
  std::function<void(const ChunkInfo&)> on_chunk;
  std::function<void(DownloadError, const std::string&)> on_error;
};

/**
 * ChunkedDownloadManager
 * 用法:
 * - 传入 URL 与目标文件路径或基准目录/用户键/任务键/文件名组合
 * - 调用 Start() 启动下载；Pause()/Resume()/Cancel() 控制任务生命周期
 * - Wait() 阻塞直到任务结束
 * 交互:
 * - 使用 SimpleHttpClient 发起 HEAD/Range 请求
 * - 通过 .meta/.part 文件实现续传与临时存储
 * - 合并完成后可选进行 MD5 校验
 */
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
  /**
 * 根据总字节数与分块大小构建分块列表；最后一个分块可能小于 chunk_size_bytes
 */
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

  /**
 * 解析 HTTP URL，支持 http://host[:port]/path 格式；默认端口 80
 */
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
