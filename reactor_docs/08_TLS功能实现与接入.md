# TLS功能实现与接入

## 实际的TLS实现架构

HttpServer中的TLS功能是通过以下方式实现的，与文档中描述的`InitTls()`函数不同。

## 1. TLS上下文创建

### TlsContext::CreateFromEnv()

**位置**: `TlsContext.cpp` 第17-71行

```cpp
std::shared_ptr<TlsContext> TlsContext::CreateFromEnv() {
  // 从环境变量读取证书和私钥路径
  const char* cert = std::getenv("WEBSERVER_TLS_CERT");
  const char* key = std::getenv("WEBSERVER_TLS_KEY");
  
  // 如果没有设置证书和私钥，返回nullptr（不启用TLS）
  if (!cert || !key || std::string(cert).empty() || std::string(key).empty()) {
    return nullptr;
  }

  // 初始化OpenSSL
  OPENSSL_init_ssl(0, nullptr);

  // 创建SSL上下文
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    LOGERROR("SSL_CTX_new failed");
    return nullptr;
  }

  // 设置TLS版本为1.2（强制TLS 1.2）
  SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
  
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) != 1) {
    LOGERROR("SSL_CTX_set_*_proto_version failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  // 设置加密套件列表
  const char* cipher_list =
      "ECDHE-ECDSA-AES128-GCM-SHA256:"
      "ECDHE-RSA-AES128-GCM-SHA256:"
      "ECDHE-ECDSA-AES256-GCM-SHA384:"
      "ECDHE-RSA-AES256-GCM-SHA384";
  if (SSL_CTX_set_cipher_list(ctx, cipher_list) != 1) {
    SSLERROR("SSL_CTX_set_cipher_list failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  // 加载证书和私钥
  if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1) {
    LOGERROR("SSL_CTX_use_certificate_file failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  
  if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
    LOGERROR("SSL_CTX_use_PrivateKey_file failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  
  // 验证私钥与证书匹配
  if (SSL_CTX_check_private_key(ctx) != 1) {
    LOGERROR("SSL_CTX_check_private_key failed");
    SSL_CTX_free(ctx);
    return nullptr;
  }

  // 检查是否启用严格模式
  bool strict = EnvIsOn("WEBSERVER_TLS_STRICT");
  return std::shared_ptr<TlsContext>(new TlsContext(ctx, strict));
}
```

## 2. HttpServer中的TLS接入

### HttpServer构造函数

**位置**: `HttpServer.cpp` 第53行

```cpp
HttpServer::HttpServer(const std::string &ip, uint16_t port, int timeoutMS, bool OptLinger,
                       int sqlPort, const char* sqlUser, const char* sqlPwd, const char* dbName,
                       int subthreadnum, int workthreadnum, int connpoolnum, const std::string& static_path)
  : tcpserver_(ip, port, subthreadnum, timeoutMS / 1000, OptLinger),
    threadpool_(workthreadnum, "work"),
    static_path_(static_path) {
  
  // 设置TcpServer回调
  tcpserver_.setnewconnection(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
  tcpserver_.setcloseconnection(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
  tcpserver_.seterrorconnection(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
  tcpserver_.setonmessage(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1));
  tcpserver_.setsendcomplete(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
  
  // 初始化数据库连接池
  SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
  
  // 设置路由
  router_ = std::make_shared<Router>();
  SetupRoutes(*router_);
  
  // 从环境变量创建TLS上下文
  tls_ctx_ = TlsContext::CreateFromEnv();
  
  // 检查工作线程数量
  if (workthreadnum <= 0) {
    LOGWARNING("workthreadnum<=0，已自动调整为1，避免任务无人消费");
  }
}
```

## 3. 新连接的TLS设置

### HttpServer::HandleNewConnection()

**位置**: `HttpServer.cpp` 第77-84行

```cpp
void HttpServer::HandleNewConnection(spConnection conn) {
  LOGINFO("new connection(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")ok.");
  
  if (conn) {
    // 如果TLS上下文存在，设置到连接上
    if (tls_ctx_) {
      conn->SetTlsContext(tls_ctx_);
    }
    
    // 创建连接级工作上下文
    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->facade = std::make_shared<HttpFacade>();
    
    if (router_) {
      ctx->facade->SetRouter(router_);
    }
    
    // 设置连接上下文
    conn->SetContext(std::move(ctx));
  }
}
```

## 4. 连接级TLS会话管理

### Connection::onmessage() 中的TLS检测和握手

**位置**: `Connection.cpp` 第243-319行

```cpp
void Connection::onmessage() {
  if(disconnect_){
    return;
  }

  // TLS探测阶段：检测首包是TLS握手还是HTTP明文
  if (tls_ctx_ && !tls_decided_) {
    unsigned char probe[8];
    ssize_t n = ::recv(fd(), probe, sizeof(probe), MSG_PEEK);  // 使用MSG_PEEK不消耗数据
    
    if (n == 0) {
      closecallback();
      return;
    }
    
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      errorcallback();
      return;
    }

    // TLS握手检测函数
    auto looks_like_tls = [](const unsigned char* p, size_t len) -> bool {
      if (len < 3) return false;
      unsigned char ct = p[0];  // TLS Content Type
      // 0x14: ChangeCipherSpec, 0x15: Alert, 0x16: Handshake, 0x17: Application Data
      if (ct != 0x14 && ct != 0x15 && ct != 0x16 && ct != 0x17) return false;
      if (p[1] != 0x03) return false;  // TLS version major
      return true;
    };
    
    // HTTP明文检测函数
    auto looks_like_http = [](const unsigned char* p, size_t len) -> bool {
      if (len < 3) return false;
      std::string s(reinterpret_cast<const char*>(p), std::min<size_t>(len, 8));
      for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return s.rfind("GET ", 0) == 0 || s.rfind("POST", 0) == 0 || s.rfind("PUT ", 0) == 0 ||
             s.rfind("HEAD", 0) == 0 || s.rfind("HTTP", 0) == 0 || s.rfind("OPTI", 0) == 0 ||
             s.rfind("DELE", 0) == 0 || s.rfind("PATC", 0) == 0;
    };

    // 根据首包类型决定处理方式
    if (looks_like_tls(probe, static_cast<size_t>(n))) {
      // 检测到TLS握手，创建TLS会话
      tls_ = std::make_unique<TlsSession>(tls_ctx_, fd());
      tls_decided_ = true;
      tls_plaintext_ = false;
    } else if (looks_like_http(probe, static_cast<size_t>(n))) {
      // 检测到HTTP明文，标记为明文模式
      tls_decided_ = true;
      tls_plaintext_ = true;
      
      // 如果启用严格模式且收到明文，关闭连接
      if (tls_ctx_ && tls_ctx_->Strict()) {
        closecallback();
        return;
      }
    } else {
      // 无法识别，等待更多数据
      return;
    }
  }

  // TLS会话处理
  if (tls_) {
    while (true) {
      // 握手阶段
      if (!tls_->HandshakeDone()) {
        TlsIoResult hr = tls_->DriveHandshake();
        if (hr == TlsIoResult::OK) {
          continue;  // 握手进行中，继续处理
        }
        if (hr == TlsIoResult::WANT_WRITE) {
          clientchannel_->enablewriting();  // 需要写事件
          return;
        }
        if (hr == TlsIoResult::WANT_READ) {
          return;  // 需要读事件
        }
        errorcallback();  // 握手失败
        return;
      }

      // 数据读取阶段
      char buf[16384];
      size_t nread = 0;
      TlsIoResult rr = tls_->ReadPlain(buf, sizeof(buf), nread);
      
      if (rr == TlsIoResult::OK) {
        if (nread > 0) {
          inputbuffer_.append(buf, nread);  // 将解密后的明文添加到缓冲区
          continue;
        }
        return;
      }
      
      if (rr == TlsIoResult::WANT_WRITE) {
        clientchannel_->enablewriting();
        return;
      }
      
      if (rr == TlsIoResult::WANT_READ) {
        return;
      }
      
      errorcallback();
      return;
    }
  }

  // 明文HTTP处理...
}
```

## 5. TLS数据发送

### Connection::writecallback() 中的TLS加密发送

```cpp
void Connection::writecallback() {
  if (tls_) {
    // TLS发送路径
    if (tls_->IsKTlsTxEnabled()) {
      // kTLS已启用：使用用户态writev并发送，由内核加密
      auto iovecs = outputbuffer_.getIOVecs();
      ssize_t nwritten = ::writev(fd(), iovecs.data(), iovecs.size());
    } else {
      // kTLS未启用：使用SSL_write加密后发送
      size_t remaining = outputbuffer_.avail();
      size_t offset = 0;
      
      while (remaining > 0) {
        const char* data = outputbuffer_.data() + offset;
        size_t to_write = std::min(remaining, static_cast<size_t>(16384));
        
        int nwritten = tls_->WriteCipher(data, to_write);
        if (nwritten > 0) {
          offset += nwritten;
          remaining -= nwritten;
        } else if (nwritten == 0) {
          break;
        } else {
          if (tls_->GetLastError() == SSL_ERROR_WANT_WRITE) {
            clientchannel_->enablewriting();
            return;
          }
          errorcallback();
          return;
        }
      }
    }
  } else {
    // 明文发送路径
    auto iovecs = outputbuffer_.getIOVecs();
    ssize_t nwritten = ::writev(fd(), iovecs.data(), iovecs.size());
  }
}
```

## 6. 环境变量配置

### 必需环境变量

```bash
# SSL证书文件路径
export WEBSERVER_TLS_CERT="/path/to/cert.pem"

# SSL私钥文件路径
export WEBSERVER_TLS_KEY="/path/to/key.pem"
```

### 可选环境变量

```bash
# TLS严格模式（只允许TLS，拒绝HTTP明文）
# "1", "true", "TRUE" → 启用严格模式
# 其他值或不设置 → 允许TLS和HTTP明文共存
export WEBSERVER_TLS_STRICT="1"
```

## 7. TLS工作流程总结

### 完整的TLS连接建立流程

```
1. 服务器启动
   HttpServer::HttpServer()
   → TlsContext::CreateFromEnv()
   → 创建共享的TLS上下文

2. 新连接建立
   Acceptor::newconnection()
   → TcpServer::newconnection()
   → HttpServer::HandleNewConnection()
   → conn->SetTlsContext(tls_ctx_)

3. 客户端首次发送数据
   Connection::onmessage()
   → recv(..., MSG_PEEK) 探测首包类型
   
4a. 首包是TLS握手
   → looks_like_tls() 检测成功
   → 创建 TlsSession(tls_ctx_, fd_)
   → 标记 tls_plaintext_ = false
   → 开始TLS握手过程

4b. 首包是HTTP明文
   → looks_like_http() 检测成功
   → 标记 tls_plaintext_ = true
   → 如果严格模式启用 → 关闭连接
   → 否则按HTTP明文处理

5. TLS握手完成
   → tls_->HandshakeDone() = true
   → 开始接收加密的HTTP数据

6. HTTP数据传输
   客户端 → 发送加密数据
           → Network [encrypted]
           → Connection::onmessage()
           → tls_->ReadPlain() [解密]
           → inputbuffer_ [明文]
           → HttpServer::HandleMessage()
           
   服务器 → 生成HTTP响应
           → outputbuffer_ [明文]
           → Connection::send()
           → tls_->WriteCipher() [加密]
           → Network [encrypted]
           → 客户端
```

## 8. TLS特性

### 安全特性

1. **强制TLS 1.2**：禁用SSLv2、SSLv3，只支持TLS 1.2
2. **强加密套件**：使用AES-GCM强加密算法
3. **严格模式**：可配置强制TLS，拒绝明文HTTP连接

### 性能特性

1. **kTLS支持**：优先使用内核TLS硬件加速
2. **连接级会话**：每个连接独立的TLS会话，避免连接间干扰
3. **非阻塞握手**：使用非阻塞I/O进行TLS握手
4. **零拷贝加密**：kTLS模式下实现用户态零拷贝

### 兼容性特性

1. **自动检测**：自动识别TLS和HTTP明文连接
2. **灵活配置**：通过环境变量轻松配置HTTPS
3. **降级支持**：在不启用严格模式下支持HTTP和HTTPS共存

## 9. 与文档中的不准确描述对比

### 文档中的错误描述

文档05_HttpServer详解.md中第300-311行描述的`InitTls()`函数实际上并不存在：

```cpp
// ❌ 文档中不存在的代码
void HttpServer::InitTls() {
  const char* cert_path = std::getenv("WEBSERVER_TLS_CERT");
  const char* key_path = std::getenv("WEBSERVER_TLS_KEY");
  const char* strict_str = std::getenv("WEBSERVER_TLS_STRICT");
  
  if (cert_path && key_path) {
    tls_ctx_ = std::make_shared<TlsContext>(cert_path, key_path);
    // ...
  }
}
```

### 实际的实现方式

实际代码中TLS是通过以下方式实现的：

```cpp
// ✅ 实际的实现方式
// 1. 在构造函数中创建TLS上下文
HttpServer::HttpServer(...) {
  // ...
  tls_ctx_ = TlsContext::CreateFromEnv();  // 第53行
}

// 2. 在新连接时设置TLS上下文
void HttpServer::HandleNewConnection(spConnection conn) {
  if (conn) {
    if (tls_ctx_) {
      conn->SetTlsContext(tls_ctx_);  // 第79行
    }
    // ...
  }
}

// 3. 在连接中处理TLS检测和握手
void Connection::onmessage() {
  if (tls_ctx_ && !tls_decided_) {
    // TLS检测逻辑（第243-285行）
  }
  if (tls_) {
    // TLS会话处理（第287-319行）
  }
}
```

## 10. 使用示例

### 启用HTTPS服务

```bash
# 设置环境变量
export WEBSERVER_TLS_CERT="/etc/ssl/certs/server.crt"
export WEBSERVER_TLS_KEY="/etc/ssl/private/server.key"

# 启动服务器
./webserver

# 连接测试
curl -k https://localhost:8080/  # 使用HTTPS
```

### 启用严格模式（仅HTTPS）

```bash
export WEBSERVER_TLS_CERT="/etc/ssl/certs/server.crt"
export WEBSERVER_TLS_KEY="/etc/ssl/private/server.key"
export WEBSERVER_TLS_STRICT="1"  # 启用严格模式

./webserver

# 此时HTTP连接会被拒绝，只允许HTTPS
curl http://localhost:8080/     # 连接会被服务器关闭
curl -k https://localhost:8080/  # 正常工作
```

### HTTP和HTTPS共存（默认）

```bash
# 不设置WEBSERVER_TLS_STRICT或不设置为"1"
export WEBSERVER_TLS_CERT="/etc/ssl/certs/server.crt" 
export WEBSERVER_TLS_KEY="/etc/ssl/private/server.key"

./webserver

# HTTP和HTTPS都能工作
curl http://localhost:8080/     # 明文HTTP工作
curl -k https://localhost:8080/  # 加密HTTPS也工作
```

这个实现提供了完整的TLS/HTTPS支持，具有安全性、性能和兼容性的优势，是现代HTTP服务器的标准实现方式。