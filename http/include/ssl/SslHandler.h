#pragma once

#include "ssl/ISslHandler.h"
#include "ssl/SslContext.h"
#include <string>
#include <memory>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

// SSL 处理实现类：使用 OpenSSL 实现 SSL/TLS 的加密解密功能
class SslHandler : public ISslHandler {
public:
  explicit SslHandler(std::shared_ptr<SslContext> context = nullptr);
  ~SslHandler() override;

  // ISslHandler 接口实现
  bool IsSslConnection(const char* data, size_t len) const override;
  bool IsSslConnection(const std::string& data) const override;

  SslResult Handshake(const char* data, size_t len, std::string& out) override;
  SslResult Handshake(const std::string& data, std::string& out) override;

  SslResult Decrypt(const char* encrypted_data, size_t len, std::string& decrypted_data) override;
  SslResult Decrypt(const std::string& encrypted_data, std::string& decrypted_data) override;

  SslResult Encrypt(const char* plain_data, size_t len, std::string& encrypted_data) override;
  SslResult Encrypt(const std::string& plain_data, std::string& encrypted_data) override;

  void Reset() override;
  bool IsHandshakeComplete() const override;

  void SetCertificateFile(const std::string& cert_file) override;
  void SetPrivateKeyFile(const std::string& key_file) override;
  void SetCaFile(const std::string& ca_file) override;

  // 获取 SSL 上下文
  std::shared_ptr<SslContext> GetContext() const { return context_; }
  void SetContext(std::shared_ptr<SslContext> context) { context_ = context; }

private:
  // 检测 SSL/TLS 握手特征（ClientHello）
  bool DetectSslHandshake(const char* data, size_t len) const;

  // 初始化 OpenSSL 上下文
  bool InitializeSslContext();
  
  // 清理 OpenSSL 资源
  void CleanupSsl();

  // SSL 状态
  enum class SslState {
    INIT,           // 初始状态
    HANDSHAKING,    // 握手中
    ESTABLISHED,    // 已建立连接
    ERROR           // 错误状态
  };

  std::shared_ptr<SslContext> context_;  // SSL 配置上下文
  SslState state_;                       // 当前 SSL 状态
  bool handshake_complete_;              // 握手是否完成
  
  // OpenSSL 相关对象
  SSL_CTX* ssl_ctx_;                     // SSL 上下文
  SSL* ssl_;                             // SSL 对象
  BIO* bio_in_;                          // 输入 BIO（用于接收加密数据）
  BIO* bio_out_;                         // 输出 BIO（用于发送加密数据）
  
  // OpenSSL 初始化标志（静态）
  static bool openssl_initialized_;
  static void InitializeOpenSSL();
};

