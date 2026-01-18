#pragma once

#include <string>
#include <memory>
#include <cstddef>  // for size_t

// SSL 处理结果枚举
enum class SslResult {
  SUCCESS = 0,              // 成功
  NEED_MORE_DATA = 1,       // 需要更多数据（握手阶段）
  ERROR = -1,               // 错误
  HANDSHAKE_FAILED = -2,    // 握手失败
  DECRYPT_FAILED = -3,      // 解密失败
  ENCRYPT_FAILED = -4,      // 加密失败
  NOT_SSL_CONNECTION = -5   // 不是 SSL 连接
};

// SSL 处理接口：定义 SSL/TLS 加密解密的基本操作
class ISslHandler {
public:
  virtual ~ISslHandler() = default;

  // 检测是否为 SSL/TLS 连接（通过端口或数据特征）
  virtual bool IsSslConnection(const char* data, size_t len) const = 0;
  virtual bool IsSslConnection(const std::string& data) const = 0;

  // 执行 SSL/TLS 握手
  // 返回 SUCCESS 表示握手完成，NEED_MORE_DATA 表示需要更多数据
  virtual SslResult Handshake(const char* data, size_t len, std::string& out) = 0;
  virtual SslResult Handshake(const std::string& data, std::string& out) = 0;

  // 解密数据（从加密的 SSL 数据流中解密出明文 HTTP 数据）
  virtual SslResult Decrypt(const char* encrypted_data, size_t len, std::string& decrypted_data) = 0;
  virtual SslResult Decrypt(const std::string& encrypted_data, std::string& decrypted_data) = 0;

  // 加密数据（将明文 HTTP 数据加密为 SSL 数据流）
  virtual SslResult Encrypt(const char* plain_data, size_t len, std::string& encrypted_data) = 0;
  virtual SslResult Encrypt(const std::string& plain_data, std::string& encrypted_data) = 0;

  // 重置 SSL 状态（用于复用）
  virtual void Reset() = 0;

  // 检查握手是否完成
  virtual bool IsHandshakeComplete() const = 0;

  // 设置 SSL 配置
  virtual void SetCertificateFile(const std::string& cert_file) = 0;
  virtual void SetPrivateKeyFile(const std::string& key_file) = 0;
  virtual void SetCaFile(const std::string& ca_file) = 0;
};

