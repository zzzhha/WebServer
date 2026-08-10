# HttpServer 详解

## 概述

`HttpServer` 是 Reactor 模块的业务适配层，将底层的 TCP 网络事件转换为 HTTP 协议处理。它继承自 `TcpServer` 的回调接口，为上层 HTTP 应用提供完整的服务框架。

## 文件信息

- **头文件**: [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)
- **实现文件**: [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)
- **依赖**: `TcpServer`, `HttpFacade`, `Router`, `AuthService`, `DownloadService`
- **被依赖**: `main.cpp`

## 类定义

### 主要成员变量

```cpp
private:
  // 连接工作上下文（Phase2异步架构）
  struct ConnectionWorkContext {
    std::shared_ptr<HttpFacade> facade;
    std::mutex mutex;
    std::deque<PendingChunk> queued_chunks;
    size_t queued_bytes{0};
    bool worker_running{false};
    uint64_t next_enqueue_seq{1};
    uint64_t next_response_seq{1};
    uint64_t last_applied_response_seq{0};
    std::map<uint64_t, WorkResult> pending_results;
  };

  // 工作结果结构
  struct WorkResult {
    uint64_t response_seq{0};
    std::string request_id;
    std::string method;
    std::string path;
    std::string route_bucket;
    bool has_response{false};
    std::string response_data;
    bool close_after_send{false};
    bool has_sendfile{false};
    int sendfile_fd{-1};
    off_t sendfile_offset{0};
    size_t sendfile_length{0};
    bool is_error{false};
    bool is_download{false};
    size_t sendfile_bytes{0};
    long queue_wait_ms{-1};
    long worker_exec_ms{0};
    long parse_route_ms{0};
    long business_ms{0};
    long serialize_ms{0};
    std::chrono::steady_clock::time_point io_enqueue_tp;
  };

  // 路由指标统计
  struct RouteMetric {
    uint64_t requests{0};
    uint64_t errors{0};
    uint64_t total_pipeline_ms{0};
    uint64_t max_pipeline_ms{0};
    uint64_t total_queue_wait_ms{0};
    uint64_t total_worker_exec_ms{0};
    uint64_t total_io_flush_ms{0};
    uint64_t total_parse_route_ms{0};
    uint64_t total_business_ms{0};
    uint64_t total_serialize_ms{0};
    uint64_t sendfile_requests{0};
    uint64_t sendfile_bytes{0};
  };

  TcpServer tcpserver_;                   // TCP服务器实例
  ThreadPool threadpool_;                 // 工作线程池
  std::string static_path_;               // 静态资源路径
  std::shared_ptr<Router> router_;        // HTTP路由器
  std::shared_ptr<TlsContext> tls_ctx_;   // TLS上下文
  std::atomic<uint64_t> request_seq_{0};  // 请求序列号
  std::mutex metrics_mutex_;              // 指标统计互斥锁
  std::unordered_map<std::string, RouteMetric> route_metrics_;  // 路由指标
```

### 构造函数

```cpp
HttpServer::HttpServer(const std::string &ip, uint16_t port, int timeoutMS, bool OptLinger,
                       int sqlPort, const char* sqlUser, const char* sqlPwd, const char* dbName,
                       int subthreadnum, int workthreadnum, int connpoolnum, const std::string& static_path)
  : tcpserver_(ip, port, subthreadnum, timeoutMS / 1000, OptLinger),
    threadpool_(workthreadnum, "work"),
    static_path_(static_path) {
  // 初始化代码...
}
```

## 核心方法详解

### 1. start() - 启动 HTTP 服务器

```cpp
void HttpServer::start() {
  // 初始化数据库连接池
  SqlConnPool::Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
  
  // 设置 TcpServer 回调
  tcpserver_.setnewconnection(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
  tcpserver_.setcloseconnection(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
  tcpserver_.seterrorconnection(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
  tcpserver_.setonmessage(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1));
  tcpserver_.setsendcomplete(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
  
  // 设置路由
  SetupRoutes();
  
  // 初始化 TLS
  InitTls();
  
  // 启动 TCP 服务器
  tcpserver_.start();
}
```

**功能**: 初始化并启动 HTTP 服务器
**调用位置**: `main.cpp` 中调用

### 2. HandleNewConnection() - 处理新连接

```cpp
void HttpServer::HandleNewConnection(spConnection conn) {
  LOGDEBUG("新的连接建立，fd=" + std::to_string(conn->fd()));
  
  // 创建连接级工作上下文
  auto ctx = std::make_shared<ConnectionWorkContext>();
  
  // 创建 HttpFacade 并初始化
  ctx->facade = std::make_shared<HttpFacade>();
  ctx->facade->Init(std::make_shared<HttpRequest>(), std::make_shared<HttpResponse>());
  ctx->facade->SetRouter(router_);
  ctx->facade->SetStaticPath(static_path_);
  
  // 设置连接上下文
  conn->SetContext(ctx);
  
  // 记录连接建立
  // ...
}
```

**功能**: 在新连接建立时创建连接级上下文和 HttpFacade
**调用位置**: `TcpServer::newconnection` 回调

### 3. HandleMessage() - 处理消息（Phase2异步入口）

```cpp
void HttpServer::HandleMessage(spConnection conn) {
  // 获取连接上下文
  auto ctx_ptr = conn->GetContext();
  if (!ctx_ptr.has_value()) return;
  
  auto* ctx = std::any_cast<std::shared_ptr<ConnectionWorkContext>>(&ctx_ptr);
  if (!ctx) return;
  
  // 读取输入缓冲区数据
  std::string data = conn->inputbuffer().toString();
  conn->inputbuffer().clear();
  
  // 检查背压
  if (threadpool_.queue_size() > max_work_queue_depth_ ||
      (*ctx)->queued_bytes + data.size() > max_conn_pending_bytes_) {
    // 返回 503 服务不可用
    SendErrorResponse(conn, 503, "Service Unavailable", true);
    return;
  }
  
  // 创建任务块
  PendingChunk chunk;
  chunk.data = std::move(data);
  chunk.enqueue_seq = (*ctx)->next_enqueue_seq++;
  chunk.enqueue_tp = std::chrono::steady_clock::now();
  
  // 添加到队列
  {
    std::lock_guard<std::mutex> lock((*ctx)->mutex);
    (*ctx)->queued_chunks.push_back(std::move(chunk));
    (*ctx)->queued_bytes += chunk.data.size();
  }
  
  // 如果 worker 未运行，启动 worker
  if (!(*ctx)->worker_running) {
    (*ctx)->worker_running = true;
    threadpool_.addtask(std::bind(&HttpServer::HandleMessageInWorker, this, conn));
  }
}
```

**功能**: Phase2 异步架构的 IO 收包阶段，收集数据并触发 worker 处理
**调用位置**: `Connection::onmessage` 回调

### 4. HandleMessageInWorker() - Worker处理阶段

```cpp
void HttpServer::HandleMessageInWorker(spConnection conn) {
  // 获取连接上下文
  auto ctx_ptr = conn->GetContext();
  // ...
  
  // 处理所有排队的数据块
  while (true) {
    PendingChunk chunk;
    {
      std::lock_guard<std::mutex> lock((*ctx)->mutex);
      if ((*ctx)->queued_chunks.empty()) {
        (*ctx)->worker_running = false;
        break;
      }
      chunk = std::move((*ctx)->queued_chunks.front());
      (*ctx)->queued_chunks.pop_front();
      (*ctx)->queued_bytes -= chunk.data.size();
    }
    
    // 处理 HTTP 请求
    WorkResult result;
    ProcessHttpRequest(conn, chunk, result);
    
    // 投递结果回 IO 线程
    PostResultToIoLoop(conn, std::move(result));
  }
}
```

**功能**: Phase2 异步架构的 Worker 处理阶段，解析 HTTP 请求并执行业务逻辑
**调用位置**: `HandleMessage` 中通过线程池调用

### 5. PostResultToIoLoop() - 结果回写阶段

```cpp
void HttpServer::PostResultToIoLoop(spConnection conn, WorkResult result) {
  auto ctx_ptr = conn->GetContext();
  // ...
  
  // 设置响应序列号
  result.response_seq = (*ctx)->next_response_seq++;
  
  // 存储结果
  {
    std::lock_guard<std::mutex> lock((*ctx)->mutex);
    (*ctx)->pending_results[result.response_seq] = std::move(result);
  }
  
  // 投递到 IO 线程按序回写
  conn->loop()->queueinloop(std::bind(&HttpServer::ApplyPendingResults, this, conn));
}
```

**功能**: Phase2 异步架构的 IO 回写阶段，将处理结果投递回 IO 线程
**调用位置**: `HandleMessageInWorker` 中调用

## Phase2 异步架构特点

### 三段式处理流程
1. **IO收包阶段** (`HandleMessage`): 在IO线程收集数据到 `queued_chunks`
2. **Worker处理阶段** (`HandleMessageInWorker`): 在工作线程解析、路由、业务处理
3. **IO回写阶段** (`PostResultToIoLoop`): 结果回IO线程，按序写入 `outputbuffer`

### 背压控制机制
- **全局工作队列背压**: `threadpool_.queue_size() > max_work_queue_depth_` 时返回503
- **单连接待处理字节背压**: `queued_bytes + data.size() > max_conn_pending_bytes_` 时返回503

### 乱序回写与过期丢弃
- 支持worker处理结果乱序到达
- 按 `response_seq` 顺序回写
- 过期结果自动丢弃
- 确保HTTP响应按请求顺序发送

## 路由设置

```cpp
void HttpServer::SetupRoutes() {
  router_ = std::make_shared<Router>();
  
  // 注册页面处理器
  router_->Register("/", std::make_shared<WelcomeHandler>());
  router_->Register("/login", std::make_shared<LoginHandler>());
  router_->Register("/register", std::make_shared<RegisterHandler>());
  router_->Register("/dashboard", std::make_shared<DashboardHandler>());
  router_->Register("/upload", std::make_shared<UploadHandler>());
  router_->Register("/download", std::make_shared<DownloadHandler>());
  
  // 注册API路由
  // ...
}
```

**功能**: 设置HTTP路由表，将URL路径映射到对应的处理器

## TLS支持

```cpp
void HttpServer::InitTls() {
  const char* cert_path = std::getenv("WEBSERVER_TLS_CERT");
  const char* key_path = std::getenv("WEBSERVER_TLS_KEY");
  const char* strict_str = std::getenv("WEBSERVER_TLS_STRICT");
  
  if (cert_path && key_path) {
    tls_ctx_ = std::make_shared<TlsContext>(cert_path, key_path);
    if (strict_str && (strcmp(strict_str, "1") == 0 || strcasecmp(strict_str, "true") == 0)) {
      tls_ctx_->SetStrictMode(true);
    }
  }
}
```

**功能**: 初始化TLS上下文，支持HTTPS连接

## 指标统计

### 路由级指标
- 请求数量、错误数量
- 各阶段处理时间统计
- 文件传输统计
- 慢请求检测

### 统计采样
- 每 `metrics_snapshot_every_` 个请求采样一次
- 慢请求阈值 `slow_request_ms_threshold_`
- 线程安全的指标更新

## 关联组件

### 1. TcpServer
- HttpServer 持有 TcpServer 实例
- 通过回调接口处理网络事件
- 提供底层网络通信能力

### 2. HttpFacade
- 每个连接拥有独立的 HttpFacade
- 处理HTTP协议解析和构造
- 连接级状态隔离

### 3. Router
- 路由HTTP请求到对应处理器
- 支持页面处理器和API路由
- 路径匹配和参数解析

### 4. 业务服务
- `AuthService`: 用户认证服务
- `DownloadService`: 文件下载服务
- 其他业务逻辑服务