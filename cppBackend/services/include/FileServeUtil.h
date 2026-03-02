#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

struct FileRange {
  bool enabled{false};
  uint64_t start{0};
  uint64_t end{0};
};

class FileServeUtil {
 public:
  static bool GetFileSize(const std::string& path, uint64_t& size);

  static bool ParseRangeHeader(const std::string& range_value, uint64_t file_size, FileRange& out);

  static bool ReadFileRange(const std::string& path, uint64_t start, uint64_t length, std::string& out);

  static std::optional<std::string> ComputeFileMd5Hex(const std::string& path);

  static std::string ToHttpDate(time_t t);
  static bool ParseHttpDate(const std::string& s, time_t& out);
  static std::string BuildWeakEtag(time_t mtime, uint64_t size);
  static bool ResolvePathUnderRoot(const std::string& root, const std::string& request_path, std::string& out_full_path);
};
