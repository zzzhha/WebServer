#include "FileSyncService.h"

#include "../../http/include/handler/AppHandlers.h"  // GetContentType
#include "../../logger/log_fac.h"
#include "../../mysql/dao/FileDao.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

std::atomic<bool> FileSyncService::running_{false};
std::thread FileSyncService::worker_;
int FileSyncService::interval_seconds_ = 300;  // 默认 5 分钟（300s）
std::string FileSyncService::static_path_;

namespace {

const char* kSyncFolders[] = {"images", "video"};

bool IsSafeName(const std::string& v) {
  if (v.empty()) return false;
  if (v.find('/') != std::string::npos) return false;
  if (v.find('\\') != std::string::npos) return false;
  if (v.find("..") != std::string::npos) return false;
  if (v.find('\0') != std::string::npos) return false;
  return true;
}

std::string LowerExt(const std::string& name) {
  auto pos = name.find_last_of('.');
  if (pos == std::string::npos) return "";
  std::string e = name.substr(pos + 1);
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return e;
}

bool IsAllowedInFolder(const std::string& folder, const std::string& name) {
  const std::string ext = LowerExt(name);
  if (folder == "images")
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "svg";
  if (folder == "video")
    return ext == "mp4" || ext == "webm" || ext == "avi";
  return false;
}

}  // namespace

bool FileSyncService::Init(const std::string& static_path) {
  if (!FileDao::EnsureSchema()) {
    LOGERROR("FileSyncService::Init: EnsureSchema failed");
    return false;
  }
  static_path_ = static_path;
  ScanOnce(static_path);
  return true;
}

void FileSyncService::ScanOnce(const std::string& static_path) {
  for (const char* folder : kSyncFolders) {
    const std::string dir = static_path + "/" + folder;
    DIR* d = opendir(dir.c_str());
    if (!d) {
      LOGWARNING("FileSyncService: cannot open dir - " + dir);
      // 目录不存在：清空该 folder 的 DB 行（一致性）。
      FileDao::DeleteMissing(folder, {});
      continue;
    }

    std::vector<std::string> present;
    while (auto* ent = readdir(d)) {
      std::string name = ent->d_name;
      if (name == "." || name == "..") continue;
      if (!IsSafeName(name)) continue;
      if (!IsAllowedInFolder(folder, name)) continue;

      const std::string full = dir + "/" + name;
      struct stat st;
      if (stat(full.c_str(), &st) != 0) continue;
      if (!S_ISREG(st.st_mode)) continue;

      present.push_back(name);
      FileMeta m;
      m.folder = folder;
      m.name = name;
      m.size = static_cast<long long>(st.st_size);
      m.mime_type = GetContentType(full);  // AppHandlers
      m.updated_at = static_cast<long long>(st.st_mtime);
      m.thumb_state = "none";
      m.thumb_width = 0;
      m.poster_state = "none";
      FileDao::Upsert(m);
    }
    closedir(d);

    // 删除磁盘上已不存在的行。
    FileDao::DeleteMissing(folder, present);
  }
  LOGINFO("FileSyncService::ScanOnce done");
}

void FileSyncService::Start() {
  if (running_.exchange(true)) return;
  worker_ = std::thread([]() {
    long sec = interval_seconds_ > 0 ? interval_seconds_ : 300;
    while (running_.load()) {
      // 用 1s 小步睡眠，保证 Stop() 能快速退出。
      for (long i = 0; i < sec && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (!running_.load()) break;
      ScanOnce(static_path_);
    }
  });
}

void FileSyncService::Stop() {
  running_.store(false);
  if (worker_.joinable()) worker_.join();
}

void FileSyncService::SetInterval(int seconds) {
  interval_seconds_ = seconds > 0 ? seconds : 300;
}
