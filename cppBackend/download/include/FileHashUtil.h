#pragma once

#include <optional>
#include <string>

class FileHashUtil {
 public:
  static std::optional<std::string> ComputeFileMd5Hex(const std::string& path);
};

