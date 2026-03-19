#include "FileHashUtil.h"

/*
 * 简述: 使用 OpenSSL EVP 计算文件 MD5（流式读取）
 * - 采用 64KB 缓冲增量更新，避免一次性读入大文件
 * - 结果以小写十六进制字符串返回
 * - 失败路径均返回 std::nullopt
 */
#include <fstream>

#include <openssl/evp.h>

static std::string ToHexLower(const unsigned char* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    unsigned char b = data[i];
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::optional<std::string> FileHashUtil::ComputeFileMd5Hex(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return std::nullopt;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return std::nullopt;

  if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::nullopt;
  }

  char buf[64 * 1024];
  while (file) {
    file.read(buf, sizeof(buf));
    std::streamsize n = file.gcount();
    if (n > 0) {
      if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1) {
        EVP_MD_CTX_free(ctx);
        return std::nullopt;
      }
    }
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::nullopt;
  }
  EVP_MD_CTX_free(ctx);
  return ToHexLower(digest, digest_len);
}

