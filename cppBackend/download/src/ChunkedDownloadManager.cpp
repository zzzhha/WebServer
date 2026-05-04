#include "ChunkedDownloadManager.h"

#include "Crc64Util.h"
#include "SimpleHttpClient.h"

#include "../../reactor/ThreadPool.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

static std::string PartPath(const std::string& dest, uint64_t index) {
  return dest + ".part" + std::to_string(index);
}

static uint64_t DivideRoundUp(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

static std::string SanitizePathComponent(std::string s) {
  if (s.empty()) return "_";
  for (char& c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.';
    if (!ok) c = '_';
  }
  if (s == "." || s == "..") s = "_";
  return s;
}

std::vector<ChunkInfo> ChunkedDownloadManager::BuildChunks(uint64_t total_bytes, uint64_t chunk_size_bytes) {
  std::vector<ChunkInfo> out;
  if (total_bytes == 0 || chunk_size_bytes == 0) return out;
  uint64_t count = DivideRoundUp(total_bytes, chunk_size_bytes);
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    ChunkInfo ci;
    ci.index = i;
    ci.start = i * chunk_size_bytes;
    uint64_t end = ci.start + chunk_size_bytes - 1;
    if (end >= total_bytes) end = total_bytes - 1;
    ci.end = end;
    ci.status = ChunkStatus::NotStarted;
    ci.retries = 0;
    out.push_back(ci);
  }
  return out;
}

ChunkedDownloadManager::ChunkedDownloadManager(std::string url, std::string dest_path, ChunkedDownloadConfig config,
                                               ChunkedDownloadCallbacks callbacks)
    : url_(std::move(url)), dest_path_(std::move(dest_path)), config_(config), callbacks_(std::move(callbacks)) {
  std::error_code ec;
  fs::path p(dest_path_);
  fs::path dir = p.has_parent_path() ? p.parent_path() : fs::current_path();
  fs::create_directories(dir, ec);
}

ChunkedDownloadManager::ChunkedDownloadManager(std::string url, std::string base_dir, std::string user_key, std::string task_key,
                                               std::string filename, ChunkedDownloadConfig config,
                                               ChunkedDownloadCallbacks callbacks)
    : ChunkedDownloadManager(std::move(url), BuildDestPath(base_dir, user_key, task_key, filename), config,
                             std::move(callbacks)) {}

ChunkedDownloadManager::~ChunkedDownloadManager() {
  Cancel();
  Wait();
}

std::string ChunkedDownloadManager::BuildDestPath(const std::string& base_dir, const std::string& user_key,
                                                  const std::string& task_key, const std::string& filename) {
  fs::path p = fs::path(base_dir) / SanitizePathComponent(user_key) / SanitizePathComponent(task_key) /
               SanitizePathComponent(filename);
  return p.string();
}

bool ChunkedDownloadManager::Start() {
  if (state_.load() == DownloadState::Downloading) return false;
  stop_.store(false);
  paused_.store(false);
  if (!InitOrResume()) {
    SetState(DownloadState::Failed);
    return false;
  }

  start_tp_ = std::chrono::steady_clock::now();
  bytes_written_since_start_.store(0);

  pool_ = std::make_unique<ThreadPool>(static_cast<size_t>(config_.max_concurrency), "DL");
  SetState(DownloadState::Downloading);

  scheduler_thread_ = std::thread([this] { SchedulerLoop(); });
  progress_thread_ = std::thread([this] { ProgressLoop(); });
  return true;
}

void ChunkedDownloadManager::Pause() {
  if (state_.load() != DownloadState::Downloading) return;
  paused_.store(true);
  SetState(DownloadState::Paused);
}

void ChunkedDownloadManager::Resume() {
  if (state_.load() != DownloadState::Paused) return;
  paused_.store(false);
  SetState(DownloadState::Downloading);
}

void ChunkedDownloadManager::Cancel() {
  stop_.store(true);
  paused_.store(false);
  DownloadState st = state_.load();
  if (st == DownloadState::Downloading || st == DownloadState::Paused) SetState(DownloadState::Canceled);
  if (pool_) pool_->stop();
}

void ChunkedDownloadManager::Wait() {
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  if (progress_thread_.joinable()) progress_thread_.join();
}

DownloadState ChunkedDownloadManager::GetState() const {
  return state_.load();
}

uint64_t ChunkedDownloadManager::GetTotalBytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  return total_bytes_;
}

uint64_t ChunkedDownloadManager::GetDownloadedBytes() const {
  return downloaded_bytes_.load();
}

std::vector<ChunkInfo> ChunkedDownloadManager::GetChunksSnapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return chunks_;
}

std::string ChunkedDownloadManager::FormatSpeed(double bytes_per_sec) {
  if (bytes_per_sec < 0) bytes_per_sec = 0;
  const double kb = 1024.0;
  const double mb = 1024.0 * 1024.0;
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  if (bytes_per_sec >= mb) {
    oss << (bytes_per_sec / mb) << " MB/s";
  } else if (bytes_per_sec >= kb) {
    oss << (bytes_per_sec / kb) << " KB/s";
  } else {
    oss << bytes_per_sec << " B/s";
  }
  return oss.str();
}

bool ChunkedDownloadManager::InitOrResume() {
  if (!LoadMetaFile()) {
    if (!FetchRemoteMetadata()) return false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      chunks_ = BuildChunks(total_bytes_, config_.chunk_size_bytes);
    }
    downloaded_bytes_.store(0);
    SaveMetaFile();
  }

  uint64_t done = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : chunks_) {
      if (c.status == ChunkStatus::Completed) done += (c.end - c.start + 1);
    }
  }
  downloaded_bytes_.store(done);
  return true;
}

bool ChunkedDownloadManager::FetchRemoteMetadata() {
  std::string host;
  uint16_t port = 0;
  std::string path;
  if (!ParseUrl(url_, host, port, path)) {
    ReportError(DownloadError::InvalidUrl, "invalid url");
    return false;
  }

  HttpResponseData resp;
  std::string error;
  if (!SimpleHttpClient::Head(host, port, path, config_.timeout_ms, resp, error)) {
    ReportError(DownloadError::NetworkError, error);
    return false;
  }

  if (resp.status_code < 200 || resp.status_code >= 300) {
    ReportError(DownloadError::HttpStatusError, "HEAD status " + std::to_string(resp.status_code));
    return false;
  }

  auto it = resp.headers.find("content-length");
  if (it == resp.headers.end()) {
    ReportError(DownloadError::MetadataError, "missing content-length");
    return false;
  }

  uint64_t total = std::strtoull(it->second.c_str(), nullptr, 10);
  if (total == 0) {
    ReportError(DownloadError::MetadataError, "invalid content-length");
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    total_bytes_ = total;
  }

  return true;
}

/**
 * 调度线程
 * - 根据当前并发容量挑选待下载分块（NotStarted 或 Failed 且未超最大重试）
 * - 提交到线程池执行 DownloadOneChunk
 * - 持久化分块状态到 .meta 文件，检测是否全部完成或失败上限
 */
void ChunkedDownloadManager::SchedulerLoop() {
  while (!stop_.load()) {
    if (paused_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (state_.load() != DownloadState::Downloading) break;

    std::vector<uint64_t> to_start;
    bool need_persist = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      int inflight = 0;
      for (const auto& c : chunks_) {
        if (c.status == ChunkStatus::Downloading) ++inflight;
      }
      int capacity = config_.max_concurrency - inflight;
      if (capacity > 0) {
        for (auto& c : chunks_) {
          if (capacity == 0) break;
          if (c.status == ChunkStatus::NotStarted || (c.status == ChunkStatus::Failed && c.retries < config_.max_retries)) {
            c.status = ChunkStatus::Downloading;
            to_start.push_back(c.index);
            need_persist = true;
            --capacity;
          }
        }
      }
    }
    if (need_persist) SaveMetaFile();

    for (uint64_t idx : to_start) {
      if (stop_.load()) break;
      pool_->addtask([this, idx] {
        bool ok = DownloadOneChunk(idx);
        if (!ok) return;
      });
    }

    bool all_done = true;
    bool any_failed = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (const auto& c : chunks_) {
        if (c.status != ChunkStatus::Completed) all_done = false;
        if (c.status == ChunkStatus::Failed && c.retries >= config_.max_retries) any_failed = true;
      }
    }

    if (any_failed) {
      SetState(DownloadState::Failed);
      break;
    }

    if (all_done) {
      if (pool_) pool_->stop();
      if (FinalizeDownload()) {
        SetState(DownloadState::Completed);
      } else {
        SetState(DownloadState::Failed);
      }
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (pool_) pool_->stop();
}

/**
 * 进度线程
 * - 周期性根据 downloaded_bytes_ 与 total_bytes_ 计算百分比
 * - 以启动时间计算平均速度 bytes_per_sec 并回调 on_progress
 * - 在 Paused/Downloading 状态下工作，完成/失败/取消时退出
 */
void ChunkedDownloadManager::ProgressLoop() {
  while (!stop_.load()) {
    DownloadState st = state_.load();
    if (st == DownloadState::Completed || st == DownloadState::Failed || st == DownloadState::Canceled) break;
    if (st != DownloadState::Downloading && st != DownloadState::Paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    uint64_t total = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      total = total_bytes_;
    }

    uint64_t done = downloaded_bytes_.load();
    double percent = 0.0;
    if (total > 0) percent = (static_cast<double>(done) * 100.0) / static_cast<double>(total);
    percent = std::round(percent * 10.0) / 10.0;

    double speed = 0.0;
    auto now = std::chrono::steady_clock::now();
    double sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_tp_).count();
    if (sec > 0.001) speed = static_cast<double>(bytes_written_since_start_.load()) / sec;

    if (callbacks_.on_progress) callbacks_.on_progress(percent, done, total, speed);

    std::this_thread::sleep_for(std::chrono::milliseconds(config_.progress_interval_ms));
  }
}

/**
 * 加载 .meta 元数据文件
 * 验证: url/dest/chunk 大小与当前配置一致；恢复分块状态与重试计数
 * 返回: 成功/失败；失败表示需要走首次初始化流程
 */
bool ChunkedDownloadManager::LoadMetaFile() {
  std::string mp = MetaPath();
  if (!fs::exists(mp)) return false;

  std::ifstream in(mp);
  if (!in.is_open()) return false;

  std::string line;
  std::string url;
  std::string dest;
  uint64_t total = 0;
  uint64_t chunk = 0;
  std::optional<std::string> md5;
  uint64_t chunk_count = 0;

  std::vector<int> states;
  std::vector<int> retries;
  while (std::getline(in, line)) {
    if (line.rfind("url=", 0) == 0) url = line.substr(4);
    else if (line.rfind("dest=", 0) == 0) dest = line.substr(5);
    else if (line.rfind("total=", 0) == 0) total = std::strtoull(line.substr(6).c_str(), nullptr, 10);
    else if (line.rfind("chunk=", 0) == 0) chunk = std::strtoull(line.substr(6).c_str(), nullptr, 10);
    else if (line.rfind("crc64=", 0) == 0) {
      std::string v = line.substr(6);
      if (!v.empty()) md5 = v;
    } else if (line.rfind("chunk_count=", 0) == 0) {
      chunk_count = std::strtoull(line.substr(12).c_str(), nullptr, 10);
      states.resize(static_cast<size_t>(chunk_count), 0);
      retries.resize(static_cast<size_t>(chunk_count), 0);
    } else if (line.size() > 2 && line[0] == 'c') {
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      uint64_t idx = std::strtoull(line.substr(1, eq - 1).c_str(), nullptr, 10);
      std::string v = line.substr(eq + 1);
      size_t comma = v.find(',');
      int st = 0;
      int rt = 0;
      if (comma == std::string::npos) {
        st = std::atoi(v.c_str());
      } else {
        st = std::atoi(v.substr(0, comma).c_str());
        rt = std::atoi(v.substr(comma + 1).c_str());
      }
      if (idx < states.size()) {
        states[static_cast<size_t>(idx)] = st;
        retries[static_cast<size_t>(idx)] = rt;
      }
    }
  }

  if (url != url_ || dest != dest_path_) return false;
  if (chunk != config_.chunk_size_bytes) return false;
  if (total == 0 || chunk_count == 0) return false;

  total_bytes_ = total;
  if (md5) {
    expected_crc64_ = std::strtoull(md5->c_str(), nullptr, 16);
  }

  std::vector<ChunkInfo> cs;
  cs.reserve(static_cast<size_t>(chunk_count));
  for (uint64_t i = 0; i < chunk_count; ++i) {
    ChunkInfo ci;
    ci.index = i;
    ci.start = i * chunk;
    uint64_t end = ci.start + chunk - 1;
    if (end >= total) end = total - 1;
    ci.end = end;
    int st = states.empty() ? 0 : states[static_cast<size_t>(i)];
    if (st == 2) ci.status = ChunkStatus::Completed;
    else if (st == 3) ci.status = ChunkStatus::Failed;
    else ci.status = ChunkStatus::NotStarted;
    ci.retries = retries.empty() ? 0 : retries[static_cast<size_t>(i)];
    cs.push_back(ci);
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    chunks_ = std::move(cs);
  }
  return true;
}

/**
 * 保存 .meta 元数据文件（原子写入：先写 tmp，再重命名）
 * 内容: url/dest/total/chunk/md5/chunk_count 以及每个分块的状态与重试次数
 */
bool ChunkedDownloadManager::SaveMetaFile() const {
  std::string tmp = MetaPath() + ".tmp";
  std::ofstream out(tmp, std::ios::trunc);
  if (!out.is_open()) return false;

  std::vector<ChunkInfo> cs;
  uint64_t total = 0;
  std::optional<uint64_t> crc64;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cs = chunks_;
    total = total_bytes_;
    crc64 = expected_crc64_;
  }

  out << "v=1\n";
  out << "url=" << url_ << "\n";
  out << "dest=" << dest_path_ << "\n";
  out << "total=" << total << "\n";
  out << "chunk=" << config_.chunk_size_bytes << "\n";
  out << "crc64=" << (crc64 ? Crc64Util::ToHex(*crc64) : "") << "\n";
  out << "chunk_count=" << cs.size() << "\n";

  for (const auto& c : cs) {
    int st = 0;
    if (c.status == ChunkStatus::Downloading) st = 1;
    else if (c.status == ChunkStatus::Completed) st = 2;
    else if (c.status == ChunkStatus::Failed) st = 3;
    out << "c" << c.index << "=" << st << "," << c.retries << "\n";
  }
  out.close();

  std::error_code ec;
  fs::rename(tmp, MetaPath(), ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string ChunkedDownloadManager::MetaPath() const {
  return dest_path_ + ".meta";
}

/**
 * 磁盘空间校验
 * - 使用 statvfs 查询可用字节数，与预写入长度比较
 * - 不足时返回错误并阻止下载
 */
bool ChunkedDownloadManager::EnsureDiskSpace(uint64_t bytes_needed, std::string& error) const {
  fs::path p(dest_path_);
  fs::path dir = p.has_parent_path() ? p.parent_path() : fs::current_path();
  struct statvfs vfs;
  if (::statvfs(dir.c_str(), &vfs) != 0) {
    error = "statvfs failed";
    return false;
  }
  uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
  if (free_bytes < bytes_needed) {
    error = "disk space insufficient";
    return false;
  }
  return true;
}

/**
 * 下载单个分块
 * 流程:
 * - 解析 URL，检查暂停/取消与磁盘空间
 * - 发送 Range GET，校验状态码与返回体长度
 * - 将数据写入 .part 文件，更新分块状态并累计进度
 * - 出错时递增重试计数并持久化
 */
bool ChunkedDownloadManager::DownloadOneChunk(uint64_t chunk_index) {
  ChunkInfo ci;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (chunk_index >= chunks_.size()) return false;
    ci = chunks_[static_cast<size_t>(chunk_index)];
  }

  if (stop_.load()) return false;
  if (paused_.load()) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (chunk_index < chunks_.size() && chunks_[static_cast<size_t>(chunk_index)].status == ChunkStatus::Downloading) {
        chunks_[static_cast<size_t>(chunk_index)].status = ChunkStatus::NotStarted;
        changed = true;
      }
    }
    if (changed) SaveMetaFile();
    return false;
  }

  std::string host;
  uint16_t port = 0;
  std::string path;
  if (!ParseUrl(url_, host, port, path)) {
    ReportError(DownloadError::InvalidUrl, "invalid url");
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries = config_.max_retries;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  uint64_t length = ci.end - ci.start + 1;
  std::string disk_error;
  if (!EnsureDiskSpace(length, disk_error)) {
    ReportError(DownloadError::DiskSpaceInsufficient, disk_error);
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.status = ChunkStatus::Failed;
      c.retries = config_.max_retries;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  HttpResponseData resp;
  std::string error;
  if (!SimpleHttpClient::GetRange(host, port, path, ci.start, ci.end, config_.timeout_ms, resp, error)) {
    if (error == "timeout") ReportError(DownloadError::NetworkTimeout, "timeout");
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries += 1;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  if (resp.status_code != 206 && resp.status_code != 200) {
    ReportError(DownloadError::HttpStatusError, "status " + std::to_string(resp.status_code));
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries += 1;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  if (resp.body.size() != static_cast<size_t>(length)) {
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries += 1;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  std::string part = PartPath(dest_path_, chunk_index);
  std::ofstream out(part, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    ReportError(DownloadError::FileWriteFailed, "open part file failed");
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries += 1;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }

  out.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
  if (!out) {
    out.close();
    ReportError(DownloadError::FileWriteFailed, "write part file failed");
    ChunkInfo snap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& c = chunks_[static_cast<size_t>(chunk_index)];
      c.retries += 1;
      c.status = ChunkStatus::Failed;
      snap = c;
    }
    SaveMetaFile();
    if (callbacks_.on_chunk) callbacks_.on_chunk(snap);
    return false;
  }
  out.close();

  {
    uint64_t chunk_crc = Crc64Util::ComputeChunkCrc64(resp.body.data(), resp.body.size());
    std::lock_guard<std::mutex> lk(mu_);
    if (chunk_crc64s_.size() <= static_cast<size_t>(chunk_index)) {
      chunk_crc64s_.resize(static_cast<size_t>(chunk_index) + 1, 0);
    }
    chunk_crc64s_[static_cast<size_t>(chunk_index)] = chunk_crc;
  }

  ChunkInfo snap;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto& c = chunks_[static_cast<size_t>(chunk_index)];
    c.status = ChunkStatus::Completed;
    snap = c;
  }
  SaveMetaFile();
  if (callbacks_.on_chunk) callbacks_.on_chunk(snap);

  downloaded_bytes_.fetch_add(length);
  bytes_written_since_start_.fetch_add(length);
  return true;
}

void ChunkedDownloadManager::SetState(DownloadState st) {
  DownloadState prev = state_.exchange(st);
  if (prev == st) return;
  if (callbacks_.on_state) callbacks_.on_state(st);
}

void ChunkedDownloadManager::ReportError(DownloadError err, const std::string& msg) {
  if (callbacks_.on_error) callbacks_.on_error(err, msg);
}

/**
 * 完成收尾
 * - 合并所有 .part 到临时输出，再重命名到最终文件
 * - 若配置/响应提供了期望 MD5，则进行校验
 * - 清理中间文件（.meta 等）
 */
bool ChunkedDownloadManager::FinalizeDownload() {
  std::string error;
  if (!MergeParts(error)) {
    ReportError(DownloadError::MergeFailed, error);
    return false;
  }

  if (!VerifyCrc64(error)) {
    ReportError(DownloadError::Crc64Mismatch, error);
    return false;
  }

  CleanupArtifacts();
  return true;
}

/**
 * 合并分块
 * - 按分块顺序读取并写入到临时文件（缓冲 64KB）
 * - 写入失败或分块缺失时中止并返回错误
 */
bool ChunkedDownloadManager::MergeParts(std::string& error) {
  std::vector<ChunkInfo> cs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cs = chunks_;
  }
  if (cs.empty()) {
    error = "no chunks";
    return false;
  }

  std::string tmp = dest_path_ + ".merge_tmp";
  std::error_code ec;
  fs::remove(tmp, ec);

  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    error = "open output failed";
    return false;
  }

  char buf[64 * 1024];
  for (uint64_t i = 0; i < cs.size(); ++i) {
    std::string part = PartPath(dest_path_, i);
    std::ifstream in(part, std::ios::binary);
    if (!in.is_open()) {
      out.close();
      error = "open part failed";
      return false;
    }

    while (in) {
      in.read(buf, sizeof(buf));
      std::streamsize n = in.gcount();
      if (n > 0) {
        out.write(buf, n);
        if (!out) {
          in.close();
          out.close();
          error = "write output failed";
          return false;
        }
      }
    }
    in.close();
    fs::remove(part, ec);
  }

  out.close();
  if (!out) {
    error = "close output failed";
    return false;
  }

  fs::remove(dest_path_, ec);
  fs::rename(tmp, dest_path_, ec);
  if (ec) {
    error = "rename output failed";
    return false;
  }
  return true;
}

/**
 * CRC64 校验
 * - 组合各分块的 CRC64 值（利用 CRC64 的线性可分性），无需重新读盘
 * - 若无期望值，则视为校验通过
 */
bool ChunkedDownloadManager::VerifyCrc64(std::string& error) {
  if (chunk_crc64s_.empty()) {
    error = "no chunk crc64";
    return false;
  }
  uint64_t combined = chunk_crc64s_[0];
  for (size_t i = 1; i < chunk_crc64s_.size(); ++i) {
    uint64_t chunk_len = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (i < chunks_.size()) {
        chunk_len = chunks_[i].end - chunks_[i].start + 1;
      }
    }
    combined = Crc64Util::Combine(combined, chunk_crc64s_[i], chunk_len);
  }
  actual_crc64_ = combined;
  if (!expected_crc64_) return true;
  if (actual_crc64_.value() != expected_crc64_.value()) {
    error = "crc64 mismatch";
    return false;
  }
  return true;
}

void ChunkedDownloadManager::CleanupArtifacts() {
  std::error_code ec;
  fs::remove(MetaPath(), ec);
}

/**
 * 解析 HTTP URL
 * 格式: http://host[:port]/path
 * 返回: 成功填充 host/port/path；无端口时默认 80
 */
bool ChunkedDownloadManager::ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  host.clear();
  path.clear();
  port = 0;

  const std::string kHttp = "http://";
  if (url.rfind(kHttp, 0) != 0) return false;
  std::string rest = url.substr(kHttp.size());

  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (hostport.empty()) return false;

  size_t colon = hostport.find(':');
  if (colon == std::string::npos) {
    host = hostport;
    port = 80;
    return true;
  }

  host = hostport.substr(0, colon);
  std::string port_str = hostport.substr(colon + 1);
  if (host.empty() || port_str.empty()) return false;
  uint64_t p = std::strtoull(port_str.c_str(), nullptr, 10);
  if (p == 0 || p > 65535) return false;
  port = static_cast<uint16_t>(p);
  return true;
}
