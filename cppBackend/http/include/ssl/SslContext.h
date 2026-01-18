#pragma once

#include <string>

// SSL/TLS 版本枚举：提供类型安全的版本表示
// 与 HttpVersion 枚举保持一致的设计风格
enum class SslVersion {
  TLS_1_0,    // TLS 1.0
  TLS_1_1,    // TLS 1.1
  TLS_1_2,    // TLS 1.2（默认）
  TLS_1_3,    // TLS 1.3
  AUTO        // 自动协商（使用 OpenSSL 默认行为）
};

// SSL 版本工具函数：在枚举和字符串之间转换
namespace SslVersionUtil {
  // 将枚举转换为字符串
  std::string ToString(SslVersion version);
  
  // 将字符串转换为枚举（如果无效则返回默认值）
  SslVersion FromString(const std::string& version_str);
  
  // 验证字符串是否为有效的 SSL 版本
  bool IsValidVersion(const std::string& version_str);
}

// SSL 上下文配置类：管理 SSL/TLS 的配置信息
class SslContext {
public:
  SslContext() = default;
  ~SslContext() = default;

  // 设置证书文件路径
  void SetCertificateFile(const std::string& cert_file) { cert_file_ = cert_file; }
  const std::string& GetCertificateFile() const { return cert_file_; }

  // 设置私钥文件路径
  void SetPrivateKeyFile(const std::string& key_file) { key_file_ = key_file; }
  const std::string& GetPrivateKeyFile() const { return key_file_; }

  // 设置 CA 证书文件路径
  void SetCaFile(const std::string& ca_file) { ca_file_ = ca_file; }
  const std::string& GetCaFile() const { return ca_file_; }

  // 设置是否验证对等证书
  void SetVerifyPeer(bool verify) { verify_peer_ = verify; }
  bool GetVerifyPeer() const { return verify_peer_; }

  // 设置 SSL 版本（使用枚举，类型安全）
  void SetSslVersion(SslVersion version) { ssl_version_ = version; }
  SslVersion GetSslVersion() const { return ssl_version_; }
  
  // 设置 SSL 版本（使用字符串，向后兼容）
  void SetSslVersion(const std::string& version_str);
  
  // 获取 SSL 版本字符串表示
  std::string GetSslVersionStr() const;

  // 设置是否为服务端模式
  void SetServerMode(bool server_mode) { server_mode_ = server_mode; }
  bool IsServerMode() const { return server_mode_; }

  // 设置端口号（用于检测是否为 HTTPS）
  void SetPort(int port) { port_ = port; }
  int GetPort() const { return port_; }

private:
  std::string cert_file_;                    // 证书文件路径
  std::string key_file_;                     // 私钥文件路径
  std::string ca_file_;                      // CA 证书文件路径
  SslVersion ssl_version_ = SslVersion::TLS_1_2;  // SSL/TLS 版本（使用枚举）
  bool verify_peer_ = true;                  // 是否验证对等证书
  bool server_mode_ = true;                  // 是否为服务端模式
  int port_ = 443;                           // 端口号（443 为 HTTPS 默认端口）
};

