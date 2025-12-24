#include "ssl/SslHandler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
// SSL/TLS 握手特征：ClientHello 的第一个字节通常是 0x16 (22)
constexpr uint8_t SSL_HANDSHAKE_RECORD = 0x16;
constexpr uint8_t SSL_CHANGE_CIPHER_SPEC = 0x14;
constexpr uint8_t SSL_ALERT = 0x15;
constexpr uint8_t SSL_APPLICATION_DATA = 0x17;

// 检测是否为 SSL/TLS 握手数据
bool IsSslHandshakeByte(uint8_t byte) {
  return byte == SSL_HANDSHAKE_RECORD || 
         byte == SSL_CHANGE_CIPHER_SPEC || 
         byte == SSL_ALERT ||
         byte == SSL_APPLICATION_DATA;
}

// 获取 OpenSSL 错误信息
std::string GetOpenSSLError() {
  char error_buf[256];
  unsigned long err = ERR_get_error();
  if (err != 0) {
    ERR_error_string_n(err, error_buf, sizeof(error_buf));
    return std::string(error_buf);
  }
  return "Unknown OpenSSL error";
}
}  // namespace

// 静态成员初始化
bool SslHandler::openssl_initialized_ = false;

void SslHandler::InitializeOpenSSL() {
  if (!openssl_initialized_) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    openssl_initialized_ = true;
  }
}

SslHandler::SslHandler(std::shared_ptr<SslContext> context)
    : context_(context ? context : std::make_shared<SslContext>()),
      state_(SslState::INIT),
      handshake_complete_(false),
      ssl_ctx_(nullptr),
      ssl_(nullptr),
      bio_in_(nullptr),
      bio_out_(nullptr) {
  InitializeOpenSSL();
  InitializeSslContext();
}

SslHandler::~SslHandler() {
  CleanupSsl();
}

bool SslHandler::InitializeSslContext() {
  // 创建 SSL 上下文
  const SSL_METHOD* method = context_->IsServerMode() 
      ? TLS_server_method() 
      : TLS_client_method();
  
  ssl_ctx_ = SSL_CTX_new(method);
  if (!ssl_ctx_) {
    return false;
  }

  // 设置 SSL 选项（禁用不安全的 SSLv2 和 SSLv3）
  SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
  
  // 根据配置设置 SSL/TLS 版本
  SslVersion version = context_->GetSslVersion();
  if (version != SslVersion::AUTO) {
    // 设置最小和最大协议版本
    long min_version = 0;
    long max_version = 0;
    
    switch (version) {
      case SslVersion::TLS_1_0:
        min_version = TLS1_VERSION;
        max_version = TLS1_VERSION;
        break;
      case SslVersion::TLS_1_1:
        min_version = TLS1_1_VERSION;
        max_version = TLS1_1_VERSION;
        break;
      case SslVersion::TLS_1_2:
        min_version = TLS1_2_VERSION;
        max_version = TLS1_2_VERSION;
        break;
      case SslVersion::TLS_1_3:
        min_version = TLS1_3_VERSION;
        max_version = TLS1_3_VERSION;
        break;
      case SslVersion::AUTO:
        // AUTO 模式不设置版本限制，使用 OpenSSL 默认行为
        break;
    }
    
    if (min_version > 0 && max_version > 0) {
      // 设置最小协议版本
      if (SSL_CTX_set_min_proto_version(ssl_ctx_, min_version) != 1) {
        return false;
      }
      // 设置最大协议版本
      if (SSL_CTX_set_max_proto_version(ssl_ctx_, max_version) != 1) {
        return false;
      }
    }
  }
  // AUTO 模式：不设置版本限制，允许 OpenSSL 自动协商
  
  // 加载证书和私钥（如果配置了）
  if (!context_->GetCertificateFile().empty() && 
      !context_->GetPrivateKeyFile().empty()) {
    if (SSL_CTX_use_certificate_file(ssl_ctx_, 
                                     context_->GetCertificateFile().c_str(),
                                     SSL_FILETYPE_PEM) != 1) {
      return false;
    }
    
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_,
                                    context_->GetPrivateKeyFile().c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
      return false;
    }
    
    // 验证私钥和证书是否匹配
    if (SSL_CTX_check_private_key(ssl_ctx_) != 1) {
      return false;
    }
  }

  // 加载 CA 证书（如果配置了）
  if (!context_->GetCaFile().empty()) {
    if (SSL_CTX_load_verify_locations(ssl_ctx_,
                                     context_->GetCaFile().c_str(),
                                     nullptr) != 1) {
      return false;
    }
  }

  // 设置验证模式
  if (context_->GetVerifyPeer()) {
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
  } else {
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
  }

  // 创建 SSL 对象
  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    return false;
  }

  // 创建内存 BIO 用于数据交换
  bio_in_ = BIO_new(BIO_s_mem());
  bio_out_ = BIO_new(BIO_s_mem());
  
  if (!bio_in_ || !bio_out_) {
    return false;
  }

  // 将 BIO 附加到 SSL 对象
  SSL_set_bio(ssl_, bio_in_, bio_out_);
  
  // 设置服务器模式或客户端模式
  if (context_->IsServerMode()) {
    SSL_set_accept_state(ssl_);
  } else {
    SSL_set_connect_state(ssl_);
  }

  return true;
}

void SslHandler::CleanupSsl() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  
  // 注意：bio_in_ 和 bio_out_ 会被 SSL_free() 自动释放
  
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  
  state_ = SslState::INIT;
  handshake_complete_ = false;
}

bool SslHandler::IsSslConnection(const char* data, size_t len) const {
  if (!data || len < 1) {
    return false;
  }

  // 方法1: 检测端口号（如果是 443 端口，通常是 HTTPS）
  if (context_ && context_->GetPort() == 443) {
    return true;
  }

  // 方法2: 检测 SSL/TLS 握手特征
  return DetectSslHandshake(data, len);
}

bool SslHandler::IsSslConnection(const std::string& data) const {
  return IsSslConnection(data.data(), data.size());
}

bool SslHandler::DetectSslHandshake(const char* data, size_t len) const {
  if (!data || len < 1) {
    return false;
  }

  // SSL/TLS 记录的第一个字节应该是握手记录类型
  uint8_t first_byte = static_cast<uint8_t>(data[0]);
  
  // 检查是否为 SSL/TLS 记录类型
  if (IsSslHandshakeByte(first_byte)) {
    return true;
  }

  // 检查是否为 HTTP 请求行（如果是 HTTP，则不是 SSL）
  // HTTP 请求通常以 "GET", "POST", "PUT" 等开头
  if (len >= 3) {
    std::string prefix(data, std::min(len, size_t(4)));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);
    if (prefix.find("GET ") == 0 || prefix.find("POST") == 0 || 
        prefix.find("PUT ") == 0 || prefix.find("HEAD") == 0 ||
        prefix.find("HTTP") == 0) {
      return false;  // 这是 HTTP，不是 SSL
    }
  }

  return false;
}

SslResult SslHandler::Handshake(const char* data, size_t len, std::string& out) {
  if (!data || len == 0) {
    return SslResult::NEED_MORE_DATA;
  }

  // 检查是否为 SSL 连接
  if (!DetectSslHandshake(data, len)) {
    return SslResult::NOT_SSL_CONNECTION;
  }

  // 确保 SSL 上下文已初始化
  if (!ssl_ || !bio_in_ || !bio_out_) {
    if (!InitializeSslContext()) {
      return SslResult::ERROR;
    }
  }

  // 更新状态
  if (state_ == SslState::INIT) {
    state_ = SslState::HANDSHAKING;
  }

  // 将接收到的数据写入输入 BIO
  int written = BIO_write(bio_in_, data, static_cast<int>(len));
  if (written <= 0) {
    return SslResult::ERROR;
  }

  // 执行 SSL 握手
  int ret = SSL_do_handshake(ssl_);
  
  if (ret == 1) {
    // 握手成功
    state_ = SslState::ESTABLISHED;
    handshake_complete_ = true;
    out = "SSL handshake completed successfully";
    return SslResult::SUCCESS;
  } else {
    // 检查错误类型
    int ssl_error = SSL_get_error(ssl_, ret);
    
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      // 需要更多数据或需要发送数据
      // 检查是否有需要发送的数据
      int pending = BIO_pending(bio_out_);
      if (pending > 0) {
        std::vector<char> buffer(pending);
        int read_bytes = BIO_read(bio_out_, buffer.data(), pending);
        if (read_bytes > 0) {
          out.assign(buffer.data(), read_bytes);
        }
      }
      return SslResult::NEED_MORE_DATA;
    } else {
      // 握手失败
      state_ = SslState::ERROR;
      out = "SSL handshake failed: " + GetOpenSSLError();
      return SslResult::HANDSHAKE_FAILED;
    }
  }
}

SslResult SslHandler::Handshake(const std::string& data, std::string& out) {
  return Handshake(data.data(), data.size(), out);
}

SslResult SslHandler::Decrypt(const char* encrypted_data, size_t len, std::string& decrypted_data) {
  if (!encrypted_data || len == 0) {
    return SslResult::ERROR;
  }

  // 检查握手是否完成
  if (!handshake_complete_) {
    return SslResult::HANDSHAKE_FAILED;
  }

  if (!ssl_ || !bio_in_) {
    return SslResult::ERROR;
  }

  // 将加密数据写入输入 BIO
  int written = BIO_write(bio_in_, encrypted_data, static_cast<int>(len));
  if (written <= 0) {
    return SslResult::ERROR;
  }

  // 从 SSL 对象读取解密后的数据
  std::vector<char> buffer(4096);  // 4KB 缓冲区
  int total_read = 0;
  
  while (true) {
    int read_bytes = SSL_read(ssl_, buffer.data() + total_read, 
                              static_cast<int>(buffer.size() - total_read));
    
    if (read_bytes > 0) {
      total_read += read_bytes;
      // 如果缓冲区满了，扩展它
      if (total_read >= static_cast<int>(buffer.size())) {
        buffer.resize(buffer.size() * 2);
      }
    } else {
      int ssl_error = SSL_get_error(ssl_, read_bytes);
      
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        // 需要更多数据，但我们已经读取了一些数据
        break;
      } else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        // 连接关闭
        break;
      } else {
        // 解密失败
        return SslResult::DECRYPT_FAILED;
      }
    }
  }

  if (total_read > 0) {
    decrypted_data.assign(buffer.data(), total_read);
    return SslResult::SUCCESS;
  } else {
    // 如果没有读取到数据，可能是需要更多加密数据
    return SslResult::NEED_MORE_DATA;
  }
}

SslResult SslHandler::Decrypt(const std::string& encrypted_data, std::string& decrypted_data) {
  return Decrypt(encrypted_data.data(), encrypted_data.size(), decrypted_data);
}

SslResult SslHandler::Encrypt(const char* plain_data, size_t len, std::string& encrypted_data) {
  if (!plain_data || len == 0) {
    return SslResult::ERROR;
  }

  // 检查握手是否完成
  if (!handshake_complete_) {
    return SslResult::HANDSHAKE_FAILED;
  }

  if (!ssl_ || !bio_out_) {
    return SslResult::ERROR;
  }

  // 使用 SSL_write 加密数据
  int written = SSL_write(ssl_, plain_data, static_cast<int>(len));
  
  if (written <= 0) {
    int ssl_error = SSL_get_error(ssl_, written);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      return SslResult::NEED_MORE_DATA;
    } else {
      return SslResult::ENCRYPT_FAILED;
    }
  }

  // 从输出 BIO 读取加密后的数据
  int pending = BIO_pending(bio_out_);
  if (pending > 0) {
    std::vector<char> buffer(pending);
    int read_bytes = BIO_read(bio_out_, buffer.data(), pending);
    if (read_bytes > 0) {
      encrypted_data.assign(buffer.data(), read_bytes);
      return SslResult::SUCCESS;
    }
  }

  return SslResult::SUCCESS;
}

SslResult SslHandler::Encrypt(const std::string& plain_data, std::string& encrypted_data) {
  return Encrypt(plain_data.data(), plain_data.size(), encrypted_data);
}

void SslHandler::Reset() {
  CleanupSsl();
  state_ = SslState::INIT;
  handshake_complete_ = false;
  InitializeSslContext();
}

bool SslHandler::IsHandshakeComplete() const {
  return handshake_complete_;
}

void SslHandler::SetCertificateFile(const std::string& cert_file) {
  if (context_) {
    context_->SetCertificateFile(cert_file);
    // 重新初始化 SSL 上下文以加载新证书
    CleanupSsl();
    InitializeSslContext();
  }
}

void SslHandler::SetPrivateKeyFile(const std::string& key_file) {
  if (context_) {
    context_->SetPrivateKeyFile(key_file);
    // 重新初始化 SSL 上下文以加载新私钥
    CleanupSsl();
    InitializeSslContext();
  }
}

void SslHandler::SetCaFile(const std::string& ca_file) {
  if (context_) {
    context_->SetCaFile(ca_file);
    // 重新初始化 SSL 上下文以加载新 CA 证书
    CleanupSsl();
    InitializeSslContext();
  }
}
