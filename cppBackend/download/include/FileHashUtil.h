#pragma once

/**
 * 模块: FileHashUtil
 * 作用: 提供文件哈希计算能力，当前实现为 MD5，用于下载完整性校验
 */
#include <optional>
#include <string>

/**
 * FileHashUtil
 * 注意: 对大文件计算 MD5 可能耗时，建议在后台线程执行
 */
class FileHashUtil {
 public:
  /**
 * 计算文件 MD5 的十六进制字符串
 * path: 文件路径
 * 返回: 成功返回 32 位小写十六进制字符串；失败返回 std::nullopt
 */
static std::optional<std::string> ComputeFileMd5Hex(const std::string& path);
};

