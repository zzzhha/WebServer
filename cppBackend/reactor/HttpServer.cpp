#include"HttpServer.h"
#include"../mysql/sqlconnpool.h"
#include"../http/include/handler/AppHandlers.h"
#include"../http/include/factory/ResponseFactory.h"
#include"../http/include/error/HttpErrorUtil.h"
#include"../http/include/util/HttpHeadersUtil.h"
#include"../http/include/util/HttpStringUtil.h"
#include"../http/include/router/Router.h"
#include"TlsContext.h"
#include"../services/include/AuthService.h"
#include"../auth/jwt/JwtUtil.h"
#include"../services/include/DownloadService.h"
#include"../services/include/StaticFileService.h"
#include"../services/include/FileApiService.h"
#include"../services/include/UploadService.h"
#include"../views/include/IndexPageHandler.h"
#include"../views/include/WelcomePageHandler.h"
#include"../views/include/LoginPageHandler.h"
#include"../views/include/RegisterPageHandler.h"
#include"../views/include/PicturePageHandler.h"
#include"../views/include/VideoPageHandler.h"
#include"../views/include/IPageHandler.h"
#include"RouteMetricsUtil.h"
#include"../proactor/UringConnection.h"
#include"../proactor/UringServer.h"
#include<arpa/inet.h>
#include<algorithm>
#include<cerrno>
#include<cstring>
#include<fstream>
#include<fcntl.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<sstream>
#include<atomic>
#include<chrono>
#include<cstdio>
#include<cstdlib>
#include<stdexcept>

std::atomic<bool> HttpServer::dump_requested{false};

namespace {

std::string DebugJsonEscapeHttp(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 16);
  for (char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += '?';
        } else {
          out += ch;
        }
        break;
    }
  }
  return out;
}

void DebugReportHttp(const char* hypothesis_id,
                     const std::string& location,
                     const std::string& msg,
                     const std::string& data_json,
                     const std::string& trace_id = "") {
  std::string url = "http://127.0.0.1:7777/event";
  std::string session_id = "high-concurrency-hang";
  std::ifstream env(".dbg/high-concurrency-hang.env");
  std::string line;
  while (std::getline(env, line)) {
    if (line.rfind("DEBUG_SERVER_URL=", 0) == 0) {
      url = line.substr(std::strlen("DEBUG_SERVER_URL="));
    } else if (line.rfind("DEBUG_SESSION_ID=", 0) == 0) {
      session_id = line.substr(std::strlen("DEBUG_SESSION_ID="));
    }
  }

  const std::string prefix = "http://";
  if (url.rfind(prefix, 0) != 0) return;
  std::string remainder = url.substr(prefix.size());
  const size_t slash = remainder.find('/');
  const std::string host_port = slash == std::string::npos ? remainder : remainder.substr(0, slash);
  const std::string path = slash == std::string::npos ? "/event" : remainder.substr(slash);
  const size_t colon = host_port.rfind(':');
  if (colon == std::string::npos) return;
  const std::string host = host_port.substr(0, colon);
  const int port = std::stoi(host_port.substr(colon + 1));

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return;
  }

  std::string body = "{\"sessionId\":\"" + DebugJsonEscapeHttp(session_id) +
                     "\",\"runId\":\"pre-fix\",\"hypothesisId\":\"" + DebugJsonEscapeHttp(hypothesis_id) +
                     "\",\"location\":\"" + DebugJsonEscapeHttp(location) +
                     "\",\"msg\":\"[DEBUG] " + DebugJsonEscapeHttp(msg) +
                     "\",\"data\":" + data_json;
  if (!trace_id.empty()) {
    body += ",\"traceId\":\"" + DebugJsonEscapeHttp(trace_id) + "\"";
  }
  body += "}";

  const std::string request =
      "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  ::send(fd, request.data(), request.size(), 0);
  char resp[256];
  while (::recv(fd, resp, sizeof(resp), 0) > 0) {
  }
  ::close(fd);
}

}  // namespace

HttpServer::HttpServer(std::unique_ptr<INetServer> net_server,
                       int sqlPort,const char*sqlUser,const char*sqlPwd,const char*dbName,
                       int workthreadnum,int connpoolnum,const std::string&static_path)
      :net_server_(std::move(net_server)),
       threadpool_(static_cast<size_t>(std::max(1, workthreadnum)), "WORKS"),
       static_path_(static_path)
{
  if (!net_server_) {
    throw std::invalid_argument("HttpServer requires a valid INetServer backend");
  }
  // 以下代码不是必须的，业务关心什么事件，就指定相应的回调函数。
  net_server_->setnewconnectioncb(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
  net_server_->setcloseconnectioncb(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
  net_server_->seterrorconnectioncb(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
  net_server_->setonmessagecb(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1/*, std::placeholders::_2*/));
  net_server_->setsendcompletecb(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
  SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
  
  router_ = std::make_shared<Router>();
  SetupRoutes(*router_);
  tls_ctx_ = TlsContext::CreateFromEnv();
  if (workthreadnum <= 0) {
    LOGWARNING("workthreadnum<=0，已自动调整为1，避免任务无人消费");
  }

  const size_t effective_work_threads = static_cast<size_t>(std::max(1, workthreadnum));
  concurrencpp::runtime_options runtime_options;
  runtime_options.max_cpu_threads = effective_work_threads;
  runtime_options.max_background_threads = std::max<size_t>(effective_work_threads, 4);
  cc_runtime_ = std::make_shared<concurrencpp::runtime>(runtime_options);
  works_executor_ = cc_runtime_->make_executor<concurrencpp::thread_pool_executor>(
      "WORKS",
      effective_work_threads,
      std::chrono::milliseconds(15000));

  if (const char* blocking_env = std::getenv("WEBSERVER_ENABLE_BLOCKING_EXECUTOR");
      blocking_env && std::string(blocking_env) == "1") {
    const size_t blocking_threads = std::max<size_t>(2, effective_work_threads);
    blocking_executor_ = cc_runtime_->make_executor<concurrencpp::thread_pool_executor>(
        "WORKS-BLOCKING",
        blocking_threads,
        std::chrono::milliseconds(30000));
  }

  // 协程化：向 Proactor 后端注入 coroutine executor
  if (auto* uring_server = dynamic_cast<UringServer*>(net_server_.get())) {
    uring_server->SetCoroutineExecutor(works_executor_);
    LOGINFO("HttpServer injected coroutine executor to UringServer");
  }

  if (const char* coroutine_env = std::getenv("WEBSERVER_COROUTINE");
      coroutine_env && std::string(coroutine_env) == "1") {
    coroutine_enabled_ = true;
    LOGINFO("HttpServer coroutine path enabled by WEBSERVER_COROUTINE=1");
  }
}
HttpServer::~HttpServer(){
  if (blocking_executor_) {
    blocking_executor_->shutdown();
    blocking_executor_.reset();
  }
  if (works_executor_) {
    works_executor_->shutdown();
    works_executor_.reset();
  }
  cc_runtime_.reset();
}
void HttpServer::start(){
  LOGINFO("Http服务器启动");

  // 诊断：SIGUSR1 触发连接状态转储（排查高并发挂死）
  std::thread dump_thread([this]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (dump_requested.exchange(false)) {
        DumpConnectionStates();
      }
    }
  });
  dump_thread.detach();

  net_server_->start();
}

void HttpServer::DumpConnectionStates() {
  FILE* f = fopen("/tmp/conn_dump.txt", "w");
  if (!f) return;
  fprintf(f, "=== conn dump t=%ld ===\n", static_cast<long>(time(nullptr)));

  // 1) proactor 连接层状态
  if (auto* uring_server = dynamic_cast<UringServer*>(net_server_.get())) {
    uring_server->DumpConnections(f);
  }

  // 2) 业务层连接上下文状态
  fprintf(f, "--- HttpServer ctxs ---\n");
  std::vector<std::weak_ptr<ConnectionWorkContext>> snapshot;
  {
    std::lock_guard<std::mutex> lock(tracked_ctxs_mutex_);
    snapshot = tracked_ctxs_;
  }
  for (const auto& weak : snapshot) {
    auto ctx = weak.lock();
    if (!ctx) continue;
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      auto conn = ctx->conn.lock();
      fd = conn ? conn->fd() : -1;
      fprintf(f,
              "  ctx fd=%d worker_running=%d active_workers=%zu draining=%d "
              "coroutine_running=%d queued_chunks=%zu queued_bytes=%zu "
              "pending_results=%zu next_seq=%llu last_applied=%llu\n",
              fd, ctx->worker_running ? 1 : 0, ctx->active_worker_count,
              ctx->draining ? 1 : 0, ctx->coroutine_running ? 1 : 0,
              ctx->queued_chunks.size(), ctx->queued_bytes,
              ctx->pending_results.size(),
              static_cast<unsigned long long>(ctx->next_response_seq),
              static_cast<unsigned long long>(ctx->last_applied_response_seq));
    }
  }
  fflush(f);
  fclose(f);
  LOGWARNING("connection state dumped to /tmp/conn_dump.txt");
}

bool HttpServer::PostWorkTask(std::function<void()> task, bool enforce_backpressure) {
  if (!works_executor_) {
    LOGERROR("WORKS executor 不可用，任务投递失败");
    return false;
  }

  if (enforce_backpressure &&
      pending_work_count_.load(std::memory_order_acquire) >= max_work_queue_depth_) {
    return false;
  }

  pending_work_count_.fetch_add(1, std::memory_order_acq_rel);
  try {
    works_executor_->post([this, task = std::move(task)]() mutable {
      try {
        task();
      } catch (const std::exception& e) {
        LOGERROR(std::string("WORKS task exception: ") + e.what());
      } catch (...) {
        LOGERROR("WORKS task unknown exception");
      }

      const auto left = pending_work_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (left == 0) {
        std::lock_guard<std::mutex> lock(drain_mutex_);
        drain_cv_.notify_all();
      }
    });
    return true;
  } catch (const std::exception& e) {
    pending_work_count_.fetch_sub(1, std::memory_order_acq_rel);
    LOGERROR(std::string("WORKS executor post failed: ") + e.what());
    return false;
  } catch (...) {
    pending_work_count_.fetch_sub(1, std::memory_order_acq_rel);
    LOGERROR("WORKS executor post failed: unknown exception");
    return false;
  }
}

bool HttpServer::PostBlockingTask(std::function<void()> task) {
  if (!blocking_executor_) {
    LOGERROR("Blocking executor 不可用，任务投递失败");
    return false;
  }
  try {
    blocking_executor_->post([task = std::move(task)]() mutable {
      try {
        task();
      } catch (const std::exception& e) {
        LOGERROR(std::string("Blocking task exception: ") + e.what());
      } catch (...) {
        LOGERROR("Blocking task unknown exception");
      }
    });
    return true;
  } catch (const std::exception& e) {
    LOGERROR(std::string("Blocking executor post failed: ") + e.what());
    return false;
  } catch (...) {
    LOGERROR("Blocking executor post failed: unknown exception");
    return false;
  }
}

void HttpServer::Stop(){
  LOGINFO("Http服务器关闭");

  if (net_server_) {
    net_server_->Stop();
  }

  {
    std::unique_lock<std::mutex> lock(drain_mutex_);
    drain_cv_.wait(lock, [this] {
      return pending_work_count_.load(std::memory_order_acquire) == 0;
    });
  }

  std::vector<std::shared_ptr<ConnectionWorkContext>> tracked_ctxs;
  {
    std::lock_guard<std::mutex> lock(tracked_ctxs_mutex_);
    auto it = tracked_ctxs_.begin();
    while (it != tracked_ctxs_.end()) {
      if (auto ctx = it->lock()) {
        tracked_ctxs.push_back(std::move(ctx));
        ++it;
      } else {
        it = tracked_ctxs_.erase(it);
      }
    }
  }

  for (auto& ctx : tracked_ctxs) {
    std::vector<concurrencpp::result<void>> results;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      results.swap(ctx->pending_coro_results);
    }
    for (auto& result : results) {
      if (!result) continue;
      try {
        result.get();
      } catch (const std::exception& e) {
        LOGERROR(std::string("coro result on stop: ") + e.what());
      } catch (...) {
        LOGERROR("coro result on stop: unknown exception");
      }
    }
  }

  if (blocking_executor_) {
    blocking_executor_->shutdown();
    blocking_executor_.reset();
  }
  if (works_executor_) {
    works_executor_->shutdown();
    works_executor_.reset();
  }
  cc_runtime_.reset();

  // 旧 ThreadPool 仍保留在类中，停机时一起回收线程资源。
  threadpool_.stop();

  SqlConnPool::Instance()->ClosePool();
}
void HttpServer::HandleNewConnection(spIConnection iconn){
  auto conn = iconn;
  if (!conn) {
    LOGERROR("新连接为空");
    return;
  }
  LOGINFO("new connection(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")ok.");
  if (conn) {
    if (tls_ctx_) {
      conn->SetTlsContext(tls_ctx_);
    }
    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->conn = conn;
    ctx->facade = std::make_shared<HttpFacade>();
    ctx->max_concurrent_workers = max_concurrent_workers_per_conn_;
    if (router_) {
      ctx->facade->SetRouter(router_);
    }
    conn->SetContext(ctx);
    std::lock_guard<std::mutex> lock(tracked_ctxs_mutex_);
    tracked_ctxs_.push_back(ctx);
  }
}
void HttpServer::HandleClose(spIConnection iconn){
  auto conn = iconn;
  if (!conn) {
    LOGERROR("关闭连接为空");
    return;
  }
  if (conn) {
    auto ctx_ptr = conn->GetContext<std::shared_ptr<ConnectionWorkContext>>();
    if (ctx_ptr && *ctx_ptr) {
      auto work_ctx = *ctx_ptr;
      std::lock_guard<std::mutex> lock(work_ctx->mutex);
      work_ctx->draining = true;
      work_ctx->queued_chunks.clear();
      work_ctx->queued_bytes = 0;
      for (auto& [seq, result] : work_ctx->pending_results) {
        if (result.sendfile_fd >= 0) {
          ::close(result.sendfile_fd);
          result.sendfile_fd = -1;
        }
      }
      work_ctx->pending_results.clear();
    }
  }
  LOGINFO("connection close(fd=" + std::to_string(conn->fd()) + ",ip=" + conn->ip() + ",port=" + std::to_string(conn->port()) + ")");
}
void HttpServer::HandleError(spIConnection iconn){
  auto conn = iconn;
  if (!conn) {
    LOGERROR("错误连接为空");
    return;
  }
  HandleClose(conn);
  LOGERROR("connection error(fd=" + std::to_string(conn->fd()) + ",ip=" + conn->ip() + ",port=" + std::to_string(conn->port()) + ")");
}
void HttpServer::HandleMessage(spIConnection iconn){
  auto conn = iconn;
  if (!conn) {
    LOGERROR("连接为空，无法处理消息");
    return;
  }
  
  LOGINFO("处理了(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+")的数据.");
  
  BufferBlock& inputbuffer = conn->getInputBuffer();
  size_t readable_bytes = inputbuffer.readableBytes();
  if (readable_bytes == 0) {
    LOGINFO("缓冲区无数据，跳过处理");
    return;
  }

  std::shared_ptr<ConnectionWorkContext> ctx;
  if (auto* existing = conn->GetContext<std::shared_ptr<ConnectionWorkContext>>(); existing && *existing) {
    ctx = *existing;
  } else {
    ctx = std::make_shared<ConnectionWorkContext>();
    ctx->conn = conn;
    ctx->facade = std::make_shared<HttpFacade>();
    ctx->max_concurrent_workers = max_concurrent_workers_per_conn_;
    if (router_) {
      ctx->facade->SetRouter(router_);
    }
    conn->SetContext(ctx);
    std::lock_guard<std::mutex> lock(tracked_ctxs_mutex_);
    tracked_ctxs_.push_back(ctx);
  }

  if (coroutine_enabled_ && conn->IsProactorMode()) {
    const size_t pending_work = pending_work_count_.load(std::memory_order_acquire);
    // #region debug-point D:handle-message-coro-branch
    DebugReportHttp("D", "HttpServer.cpp:HandleMessage",
                    "coroutine branch decision",
                    "{\"fd\":" + std::to_string(conn->fd()) + ",\"pending_work\":" +
                        std::to_string(pending_work) + ",\"input_readable\":" +
                        std::to_string(inputbuffer.readableBytes()) + "}",
                    "fd-" + std::to_string(conn->fd()));
    // #endregion
    if (pending_work >= max_work_queue_depth_) {
      LOGERROR("协程工作队列过长，触发背压 fd=" + std::to_string(conn->fd()) +
               " queue_size=" + std::to_string(pending_work));
      SendServiceUnavailable(conn, "work queue overloaded");
      return;
    }

    bool should_start_coro = false;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      if (ctx->draining) {
        return;
      }
      if (!ctx->coroutine_running) {
        ctx->coroutine_running = true;
        should_start_coro = true;
      }
    }

    if (!should_start_coro) {
      return;
    }

    std::weak_ptr<IConnection> weak_conn = conn;
    if (!PostWorkTask([this, weak_conn, ctx]() mutable {
          auto result = HandleMessageCoro(std::move(weak_conn), ctx);
          std::lock_guard<std::mutex> lock(ctx->mutex);
          ctx->pending_coro_results.emplace_back(std::move(result));
        })) {
      {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->coroutine_running = false;
      }
      SendServiceUnavailable(conn, "work queue overloaded");
    }
    return;
  }

  std::string new_data = inputbuffer.bufferToString();
  inputbuffer.consumeBytes(readable_bytes);

  const size_t pending_work = pending_work_count_.load(std::memory_order_acquire);
  if (pending_work >= max_work_queue_depth_) {
    LOGERROR("工作队列过长，触发背压 fd=" + std::to_string(conn->fd()) +
             " queue_size=" + std::to_string(pending_work));
    SendServiceUnavailable(conn, "work queue overloaded");
    return;
  }

  bool should_start_worker = false;
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->queued_bytes + new_data.size() > max_conn_pending_bytes_) {
      LOGERROR("连接待处理数据过大，触发背压 fd=" + std::to_string(conn->fd()) +
               " queued_bytes=" + std::to_string(ctx->queued_bytes + new_data.size()));
      SendServiceUnavailable(conn, "connection pending data overloaded");
      ctx->queued_chunks.clear();
      ctx->pending_results.clear();
      ctx->queued_bytes = 0;
      ctx->last_applied_response_seq = 0;
      ctx->next_response_seq = 1;
      ctx->next_enqueue_seq = 1;
      ctx->facade->ClearPending();
      return;
    }
    PendingChunk chunk;
    chunk.data = std::move(new_data);
    chunk.enqueue_seq = ctx->next_enqueue_seq++;
    chunk.enqueue_tp = std::chrono::steady_clock::now();
    ctx->queued_bytes += chunk.data.size();
    ctx->queued_chunks.push_back(std::move(chunk));
    if (!ctx->worker_running && !ctx->draining) {
      ctx->worker_running = true;
      ctx->active_worker_count = 1;
      should_start_worker = true;
    }
  }

  if (!should_start_worker) {
    return;
  }

  std::weak_ptr<IConnection> weak_conn = conn;
  if (!PostWorkTask([this, weak_conn, ctx]() mutable {
        HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
      })) {
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      ctx->worker_running = false;
      ctx->active_worker_count = 0;
      ctx->draining = true;
      ctx->queued_chunks.clear();
      ctx->queued_bytes = 0;
      for (auto& [seq, result] : ctx->pending_results) {
        CloseSendFileFd(result);
      }
      ctx->pending_results.clear();
    }
    SendServiceUnavailable(conn, "work queue overloaded");
  }
}

void HttpServer::HandleMessageInWorker(std::weak_ptr<IConnection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx) {
  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    OnWorkerExit(ctx, nullptr);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->draining) {
      ctx->active_worker_count--;
      if (ctx->active_worker_count == 0) {
        ctx->worker_running = false;
      }
      return;
    }
  }

  PendingChunk chunk;
  bool should_chain = false;
  bool queue_empty = false;
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);

    if (ctx->queued_chunks.empty()) {
      // 修复：不能在持有 ctx->mutex 时调用 OnWorkerExit（其内部会再次加锁，
      // 同一线程对非递归 mutex 二次加锁 → 死锁，executor 线程被永久挂起）。
      queue_empty = true;
    } else {
      chunk = std::move(ctx->queued_chunks.front());
      ctx->queued_chunks.pop_front();
      ctx->queued_bytes -= chunk.data.size();

      if (!ctx->queued_chunks.empty() &&
          ctx->active_worker_count < ctx->max_concurrent_workers &&
          !ctx->draining) {
        ctx->active_worker_count++;
        should_chain = true;
      }
    }
  }

  if (queue_empty) {
    OnWorkerExit(ctx, conn);
    return;
  }

  if (should_chain) {
    if (!PostWorkTask([this, weak_conn, ctx]() mutable {
          HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
        }, false)) {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      if (ctx->active_worker_count > 0) {
        ctx->active_worker_count--;
      }
    }
  }

  ProcessSingleRequest(weak_conn, ctx, std::move(chunk));

  OnWorkerExit(ctx, conn);
}

concurrencpp::result<void> HttpServer::HandleMessageCoro(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx) {
  pending_work_count_.fetch_add(1, std::memory_order_acq_rel);
  struct WorkGuard {
    HttpServer* self;
    std::shared_ptr<ConnectionWorkContext> ctx;

    ~WorkGuard() {
      {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->coroutine_running = false;
      }
      const auto left = self->pending_work_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (left == 0) {
        std::lock_guard<std::mutex> lock(self->drain_mutex_);
        self->drain_cv_.notify_all();
      }
    }
  } guard{this, std::move(ctx)};

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    co_return;
  }

  auto* uring_conn = conn->AsUringConnection();
  if (!uring_conn) {
    co_return;
  }

  // #region debug-point B:coro-enter
  DebugReportHttp("B", "HttpServer.cpp:HandleMessageCoro",
                  "coro enter",
                  "{\"fd\":" + std::to_string(conn->fd()) + ",\"pending_work\":" +
                      std::to_string(pending_work_count_.load(std::memory_order_acquire)) + "}",
                  "fd-" + std::to_string(conn->fd()));
  // #endregion

  std::shared_ptr<RequestContext> req_ctx;
  try {
    while (!conn->IsDisconnected()) {
      bool has_facade_pending = false;
      {
        std::lock_guard<std::mutex> facade_lock(guard.ctx->facade_mutex);
        has_facade_pending = guard.ctx->facade && guard.ctx->facade->GetPendingSize() > 0;
      }

      if (uring_conn->GetBufferedInputSize() == 0 && !has_facade_pending) {
        // #region debug-point A:coro-await-recv
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro await recv",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"facade_pending\":" +
                            std::string(has_facade_pending ? "true" : "false") + "}",
                        "fd-" + std::to_string(conn->fd()));
        // #endregion
        const size_t readable = co_await uring_conn->AsyncRecvReady();
        // #region debug-point A:coro-resume-recv
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro resume recv",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"readable\":" +
                            std::to_string(readable) + "}",
                        "fd-" + std::to_string(conn->fd()));
        // #endregion
        if (readable == 0) {
          co_return;
        }
      }

      if (conn->IsDisconnected()) {
        co_return;
      }

      req_ctx = std::make_shared<RequestContext>();
      PendingChunk chunk;
      chunk.enqueue_tp = std::chrono::steady_clock::now();

      if (uring_conn->GetBufferedInputSize() > 0) {
        chunk.data = uring_conn->ConsumeBufferedInput();
      }

      while (true) {
        const size_t incoming_bytes = chunk.data.size();
        if (incoming_bytes > 0) {
          bool overloaded = false;
          {
            std::lock_guard<std::mutex> lock(guard.ctx->mutex);
            if (guard.ctx->queued_bytes + incoming_bytes > max_conn_pending_bytes_) {
              overloaded = true;
              guard.ctx->queued_chunks.clear();
              guard.ctx->queued_bytes = 0;
              for (auto& [seq, result] : guard.ctx->pending_results) {
                CloseSendFileFd(result);
              }
              guard.ctx->pending_results.clear();
              guard.ctx->last_applied_response_seq = 0;
              guard.ctx->next_response_seq = 1;
              guard.ctx->next_enqueue_seq = 1;
            } else {
              guard.ctx->queued_bytes += incoming_bytes;
            }
          }

          if (overloaded) {
            {
              std::lock_guard<std::mutex> facade_lock(guard.ctx->facade_mutex);
              if (guard.ctx->facade) {
                guard.ctx->facade->ClearPending();
              }
            }
            conn->PostIoTask([this, conn]() { SendServiceUnavailable(conn, "connection pending data overloaded"); });
            co_return;
          }
        }

        PhaseParseAndRoute(weak_conn, guard.ctx, chunk, req_ctx);

        if (incoming_bytes > 0) {
          std::lock_guard<std::mutex> lock(guard.ctx->mutex);
          guard.ctx->queued_bytes -= std::min(guard.ctx->queued_bytes, incoming_bytes);
        }

        if (!req_ctx->suspended) {
          break;
        }

        req_ctx->suspended = false;
        const size_t readable = co_await uring_conn->AsyncRecvReady();
        if (readable == 0) {
          co_return;
        }
        chunk.enqueue_tp = std::chrono::steady_clock::now();
        chunk.data = uring_conn->ConsumeBufferedInput();
      }

      if (req_ctx->result != HttpServerResult::SUCCESS || !req_ctx->message) {
        // 修复：err.IsOk() 为 true 表示没有真实错误信息，此时才填充兜底 500；
        // 原实现 (!IsOk()) 会把路由层产生的真实错误（如 404 ROUTE_NOT_FOUND）
        // 覆盖成笼统的 INTERNAL_ERROR，导致任意未匹配路径都返回 500。
        if (req_ctx->err.IsOk()) {
          req_ctx->err.code = HttpErrc::INTERNAL_ERROR;
          req_ctx->err.status = HttpStatusCode::INTERNAL_SERVER_ERROR;
          req_ctx->err.message = "Internal Server Error";
          req_ctx->err.ctx.stage = HttpErrorStage::UNKNOWN;
          req_ctx->err.ctx.detail = "missing error details";
        }

        auto error_resp = ResponseFactory::CreateHttpError(req_ctx->err, req_ctx->request_id, true);
        if (auto* req = dynamic_cast<HttpRequest*>(req_ctx->message.get())) {
          ApplyCorsHeaders(*error_resp, req);
        }
        ApplyCommonResponseHeaders(*error_resp, req_ctx->request_id);
        error_resp->SetHeader("Connection", "close");
        {
          std::lock_guard<std::mutex> facade_lock(guard.ctx->facade_mutex);
          if (guard.ctx->facade) {
            guard.ctx->facade->ClearPending();
          }
        }
        co_await uring_conn->AsyncWriteResponse(error_resp->Serialize(), true);
        co_return;
      }

      req_ctx->business_begin = std::chrono::steady_clock::now();
      PhaseIoOperation(weak_conn, guard.ctx, req_ctx);
      if (req_ctx->suspended) {
        LOGERROR("协程路径暂不支持 IO_OPERATION 挂起");
        conn->PostIoTask([conn]() { conn->setCloseOnSendComplete(true); });
        co_return;
      }

      req_ctx->serialize_begin = std::chrono::steady_clock::now();
      std::string response_data = req_ctx->response.Serialize();
      if (response_data.empty()) {
        req_ctx->response.SetStatusCode(HttpStatusCode::NOT_FOUND);
        req_ctx->response.SetHeader("Content-Type", "text/plain");
        req_ctx->response.SetHeader("Connection", "close");
        req_ctx->response.SetBody("Not Found");
        response_data = req_ctx->response.Serialize();
        req_ctx->keep_alive = false;
      }

      const bool close_after = !req_ctx->keep_alive;
      if (req_ctx->file_fd >= 0) {
        const int file_fd = req_ctx->file_fd;
        req_ctx->file_fd = -1;
        // #region debug-point A:coro-await-sendfile
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro await sendfile",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"file_fd\":" +
                            std::to_string(file_fd) + ",\"file_length\":" +
                            std::to_string(req_ctx->file_length) + ",\"close_after\":" +
                            std::string(close_after ? "true" : "false") + "}",
                        req_ctx->request_id.empty() ? "fd-" + std::to_string(conn->fd()) : req_ctx->request_id);
        // #endregion
        co_await uring_conn->AsyncSendFile(
            std::move(response_data),
            file_fd,
            req_ctx->file_offset,
            req_ctx->file_length,
            close_after);
        // #region debug-point A:coro-sendfile-done
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro sendfile done",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"close_after\":" +
                            std::string(close_after ? "true" : "false") + "}",
                        req_ctx->request_id.empty() ? "fd-" + std::to_string(conn->fd()) : req_ctx->request_id);
        // #endregion
      } else {
        // #region debug-point A:coro-await-write
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro await write",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"response_bytes\":" +
                            std::to_string(response_data.size()) + ",\"close_after\":" +
                            std::string(close_after ? "true" : "false") + "}",
                        req_ctx->request_id.empty() ? "fd-" + std::to_string(conn->fd()) : req_ctx->request_id);
        // #endregion
        co_await uring_conn->AsyncWriteResponse(std::move(response_data), close_after);
        // #region debug-point A:coro-write-done
        DebugReportHttp("A", "HttpServer.cpp:HandleMessageCoro",
                        "coro write done",
                        "{\"fd\":" + std::to_string(conn->fd()) + ",\"close_after\":" +
                            std::string(close_after ? "true" : "false") + "}",
                        req_ctx->request_id.empty() ? "fd-" + std::to_string(conn->fd()) : req_ctx->request_id);
        // #endregion
      }

      if (close_after) {
        co_return;
      }
    }
  } catch (const std::exception& e) {
    LOGERROR(std::string("HandleMessageCoro exception: ") + e.what());
    if (req_ctx && req_ctx->file_fd >= 0) {
      ::close(req_ctx->file_fd);
      req_ctx->file_fd = -1;
    }
    if (conn && !conn->IsDisconnected()) {
      conn->PostIoTask([conn]() {
        if (!conn->IsDisconnected()) {
          conn->setCloseOnSendComplete(true);
        }
      });
    }
  } catch (...) {
    LOGERROR("HandleMessageCoro exception: unknown");
    if (req_ctx && req_ctx->file_fd >= 0) {
      ::close(req_ctx->file_fd);
      req_ctx->file_fd = -1;
    }
    if (conn && !conn->IsDisconnected()) {
      conn->PostIoTask([conn]() {
        if (!conn->IsDisconnected()) {
          conn->setCloseOnSendComplete(true);
        }
      });
    }
  }

  co_return;
}

void HttpServer::PhaseParseAndRoute(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    PendingChunk& chunk,
    std::shared_ptr<RequestContext> req_ctx) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    return;
  }

  {
    std::lock_guard<std::mutex> facade_lock(ctx->facade_mutex);
    if (!chunk.data.empty()) {
      ctx->facade->AppendPending(std::move(chunk.data));
      chunk.data.clear();
    }

    req_ctx->parse_begin = std::chrono::steady_clock::now();
    req_ctx->result = ctx->facade->ProcessPending(req_ctx->message, req_ctx->response, req_ctx->err);
    auto parse_end = std::chrono::steady_clock::now();

    // 无论解析结果如何，都按已消费字节裁剪 pending 缓冲（H1 修复）：
    // 若只在 SUCCESS 时裁剪，NEED_MORE_DATA 时已消费的前缀会残留，
    // 下次分片到达后 parser 会从字节 0 重读这些字节，导致请求头/body 被拼坏。
    size_t consumed_bytes = ctx->facade->GetConsumedBytes();
    ctx->facade->ErasePending(consumed_bytes);

    if (req_ctx->result == HttpServerResult::NEED_MORE_DATA) {
      req_ctx->suspended = true;
      return;
    }
  }

  if (req_ctx->result != HttpServerResult::SUCCESS || !req_ctx->message) {
    return;
  }

  if (!req_ctx->message->IsRequest()) {
    LOGERROR("收到的不是HTTP请求消息");
    return;
  }

  HttpRequest* request = dynamic_cast<HttpRequest*>(req_ctx->message.get());
  if (!request) {
    LOGERROR("无法将消息转换为HttpRequest");
    return;
  }

  req_ctx->request_id =
      std::to_string(conn->fd()) + "-" +
      std::to_string(request_seq_.fetch_add(1, std::memory_order_relaxed));

  req_ctx->path = request->GetPath();
  req_ctx->method = request->GetMethodString();
  LOGINFO("请求方法: " + req_ctx->method + ", 路径: " + req_ctx->path);

  req_ctx->keep_alive = (request->GetVersion() == HttpVersion::HTTP_1_1);
  auto connection_header = request->GetHeader("Connection");
  if (connection_header.has_value()) {
    std::string conn_value = connection_header.value();
    LowerAsciiInPlace(conn_value);
    if (conn_value.find("close") != std::string::npos) {
      req_ctx->keep_alive = false;
    } else if (conn_value.find("keep-alive") != std::string::npos) {
      req_ctx->keep_alive = true;
    }
  }

  ProcessRequest(request, req_ctx->response);
  ApplyCorsHeaders(req_ctx->response, request);
  ApplyCommonResponseHeaders(req_ctx->response, req_ctx->request_id);

  if (req_ctx->keep_alive) {
    req_ctx->response.SetHeader("Connection", "keep-alive");
  } else {
    req_ctx->response.SetHeader("Connection", "close");
  }
}

void HttpServer::PhaseIoOperation(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    std::shared_ptr<RequestContext> req_ctx) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    return;
  }

  if (!req_ctx->response.HasSendFile()) {
    return;
  }

  int file_fd = ::open(req_ctx->response.GetSendFilePath().c_str(), O_RDONLY | O_CLOEXEC);
  if (file_fd < 0) {
    int e = errno;
    req_ctx->response.ClearSendFile();
    req_ctx->response.SetHeader("Content-Type", "text/plain");
    if (e == EACCES) {
      req_ctx->response.SetStatusCode(HttpStatusCode::FORBIDDEN);
      req_ctx->response.SetBody("Forbidden");
    } else {
      req_ctx->response.SetStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
      req_ctx->response.SetBody("Internal Server Error");
    }
    return;
  }

  req_ctx->file_fd = file_fd;
  req_ctx->file_offset = static_cast<off_t>(req_ctx->response.GetSendFileOffset());
  req_ctx->file_length = req_ctx->response.GetSendFileLength();
  req_ctx->response.SetBody("");

  // future io_uring extension point:
  // if (io_uring_submit(...) == -EAGAIN) {
  //   req_ctx->suspended = true;
  //   RegisterResumeCallback(weak_conn, ctx, chunk, req_ctx);
  //   return;
  // }
}

void HttpServer::PhaseSerializeAndSend(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    PendingChunk& chunk,
    std::shared_ptr<RequestContext> req_ctx) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    if (req_ctx->file_fd >= 0) {
      ::close(req_ctx->file_fd);
      req_ctx->file_fd = -1;
    }
    return;
  }

  auto worker_begin = std::chrono::steady_clock::now();

  WorkResult work_result;
  work_result.method = req_ctx->method;
  work_result.path = req_ctx->path;
  work_result.route_bucket = ClassifyRouteBucket(req_ctx->path);
  work_result.is_download = IsDownloadRoute(req_ctx->path);
  work_result.parse_route_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - req_ctx->parse_begin).count();
  work_result.request_id = req_ctx->request_id;
  work_result.close_after_send = !req_ctx->keep_alive;

  if (req_ctx->result == HttpServerResult::SUCCESS && req_ctx->message) {
    work_result.business_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - req_ctx->business_begin).count();

    req_ctx->serialize_begin = std::chrono::steady_clock::now();
    std::string response_data = req_ctx->response.Serialize();
    auto serialize_end = std::chrono::steady_clock::now();
    work_result.serialize_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        serialize_end - req_ctx->serialize_begin).count();

    if (response_data.empty()) {
      LOGERROR("响应数据为空，无法发送响应");
      req_ctx->response.SetStatusCode(HttpStatusCode::NOT_FOUND);
      req_ctx->response.SetHeader("Content-Type", "text/plain");
      req_ctx->response.SetHeader("Connection", "close");
      req_ctx->response.SetBody("Not Found");
      response_data = req_ctx->response.Serialize();
      work_result.close_after_send = true;
    }

    if (req_ctx->file_fd >= 0) {
      work_result.has_sendfile = true;
      work_result.sendfile_fd = req_ctx->file_fd;
      work_result.sendfile_offset = req_ctx->file_offset;
      work_result.sendfile_length = req_ctx->file_length;
      work_result.sendfile_bytes = req_ctx->file_length;
      req_ctx->file_fd = -1;
    }

    work_result.has_response = true;
    work_result.response_data = std::move(response_data);
  } else {
    work_result.is_error = true;
    work_result.route_bucket = "parse_error";
    // 修复：err.IsOk() 为 true 表示没有真实错误信息，此时才填充兜底 500；
    // 原实现 (!IsOk()) 会把路由层产生的真实错误（如 404 ROUTE_NOT_FOUND）
    // 覆盖成笼统的 INTERNAL_ERROR，导致任意未匹配路径都返回 500。
    if (req_ctx->err.IsOk()) {
      req_ctx->err.code = HttpErrc::INTERNAL_ERROR;
      req_ctx->err.status = HttpStatusCode::INTERNAL_SERVER_ERROR;
      req_ctx->err.message = "Internal Server Error";
      req_ctx->err.ctx.stage = HttpErrorStage::UNKNOWN;
      req_ctx->err.ctx.detail = "missing error details";
    }

    LOGERROR("HTTP请求处理失败 request_id=" + req_ctx->request_id +
             " fd=" + std::to_string(conn->fd()) +
             " ip=" + conn->ip() +
             " port=" + std::to_string(conn->port()) +
             " http_status=" + std::to_string(static_cast<int>(req_ctx->err.status)) +
             " code=" + ToString(req_ctx->err.code) +
             " stage=" + ToString(req_ctx->err.ctx.stage) +
             (req_ctx->err.ctx.detail.empty() ? "" : " detail=" + req_ctx->err.ctx.detail));

    if (req_ctx->err.IsServerError() && !req_ctx->err.stack.empty()) {
      std::string stack = req_ctx->err.stack;
      if (stack.size() > 2048) stack.resize(2048);
      for (char& c : stack) {
        if (c == '\n') c = ' ';
        if (c == '\r') c = ' ';
      }
      LOGERROR("HTTP错误堆栈 request_id=" + req_ctx->request_id + " stack=" + stack);
    }

    auto error_resp = ResponseFactory::CreateHttpError(req_ctx->err, req_ctx->request_id, true);
    if (auto* req = dynamic_cast<HttpRequest*>(req_ctx->message.get())) {
      ApplyCorsHeaders(*error_resp, req);
    }
    ApplyCommonResponseHeaders(*error_resp, req_ctx->request_id);
    error_resp->SetHeader("Connection", "close");

    work_result.has_response = true;
    work_result.close_after_send = true;
    req_ctx->serialize_begin = std::chrono::steady_clock::now();
    work_result.response_data = error_resp->Serialize();
    auto serialize_end = std::chrono::steady_clock::now();
    work_result.serialize_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        serialize_end - req_ctx->serialize_begin).count();
    work_result.business_ms = 0;

    {
      std::lock_guard<std::mutex> facade_lock(ctx->facade_mutex);
      ctx->facade->ClearPending();
    }
  }

  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    work_result.response_seq = ctx->next_response_seq++;
  }
  work_result.queue_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      worker_begin - chunk.enqueue_tp).count();
  work_result.worker_exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - worker_begin).count();
  PostResultToIoLoop(weak_conn, ctx, std::move(work_result));
}

void HttpServer::ProcessSingleRequest(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    PendingChunk chunk,
    std::shared_ptr<RequestContext> req_ctx) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    return;
  }

  if (!req_ctx) {
    req_ctx = std::make_shared<RequestContext>();
  }

  if (req_ctx->next_phase == RequestPhase::PARSE_AND_ROUTE) {
    PhaseParseAndRoute(weak_conn, ctx, chunk, req_ctx);
    if (req_ctx->suspended) return;
    req_ctx->next_phase = RequestPhase::IO_OPERATION;
  }

  if (req_ctx->next_phase == RequestPhase::IO_OPERATION) {
    req_ctx->business_begin = std::chrono::steady_clock::now();
    PhaseIoOperation(weak_conn, ctx, req_ctx);
    if (req_ctx->suspended) return;
    req_ctx->next_phase = RequestPhase::SERIALIZE_AND_SEND;
  }

  if (req_ctx->next_phase == RequestPhase::SERIALIZE_AND_SEND) {
    PhaseSerializeAndSend(weak_conn, ctx, chunk, req_ctx);
  }
}

void HttpServer::OnWorkerExit(
    std::shared_ptr<ConnectionWorkContext> ctx,
    spIConnection conn) {
  bool should_schedule = false;
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->active_worker_count > 0) {
      ctx->active_worker_count--;
    }

    if (ctx->active_worker_count == 0) {
      if (!ctx->queued_chunks.empty() && !ctx->draining) {
        ctx->active_worker_count = 1;
        should_schedule = true;
      } else {
        bool has_facade_pending = false;
        if (!ctx->draining && conn) {
          std::lock_guard<std::mutex> facade_lock(ctx->facade_mutex);
          has_facade_pending = (ctx->facade && ctx->facade->GetPendingSize() > 0);
        }

        if (has_facade_pending && !ctx->draining && conn) {
          PendingChunk chunk;
          chunk.enqueue_seq = ctx->next_enqueue_seq++;
          chunk.enqueue_tp = std::chrono::steady_clock::now();
          ctx->queued_chunks.push_back(std::move(chunk));

          ctx->active_worker_count = 1;
          should_schedule = true;
        } else {
          ctx->worker_running = false;
        }
      }
    }
  }

  if (should_schedule) {
    if (!PostWorkTask(
            [this, weak_conn = std::weak_ptr<IConnection>(conn), ctx]() mutable {
              HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
            },
            false)) {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      if (ctx->active_worker_count > 0) {
        ctx->active_worker_count--;
      }
      if (ctx->active_worker_count == 0) {
        ctx->worker_running = false;
      }
    }
  }
}

void HttpServer::PostResultToIoLoop(std::weak_ptr<IConnection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx, WorkResult result) {
  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    CloseSendFileFd(result);
    return;
  }

  result.io_enqueue_tp = std::chrono::steady_clock::now();
  LOGDEBUG("POST-RESULT fd=" + std::to_string(conn->fd()) +
           " seq=" + std::to_string(result.response_seq) +
           " bytes=" + std::to_string(result.response_data.size()));
  conn->PostIoTask([this, weak_conn, ctx, result = std::move(result)]() mutable {
    auto strong_conn = weak_conn.lock();
    LOGDEBUG("APPLY-ENTER fd=" + std::to_string(strong_conn ? strong_conn->fd() : -1) +
             " seq=" + std::to_string(result.response_seq) +
             " conn_alive=" + (strong_conn && !strong_conn->IsDisconnected() ? "1" : "0"));
    if (!strong_conn || strong_conn->IsDisconnected()) {
      CloseSendFileFd(result);
      return;
    }

    auto apply_result = [&strong_conn](WorkResult& r) {
      BufferBlock& outputbuffer = strong_conn->getOutputBuffer();
      if (r.has_response && !r.response_data.empty()) {
        outputbuffer.append(r.response_data.c_str(), r.response_data.size());
      }
      if (r.close_after_send) {
        strong_conn->setCloseOnSendComplete(true);
      }
      if (r.has_sendfile && r.sendfile_fd >= 0) {
        strong_conn->StartSendFile(r.sendfile_fd, r.sendfile_offset, r.sendfile_length, true);
        r.sendfile_fd = -1;
      }
      strong_conn->send();
    };

    std::vector<WorkResult> to_apply;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);

      if (result.response_seq <= ctx->last_applied_response_seq) {
        LOGDEBUG("APPLY-DROP-STALE fd=" + std::to_string(strong_conn->fd()) +
                 " seq=" + std::to_string(result.response_seq) +
                 " last_applied=" + std::to_string(ctx->last_applied_response_seq));
        CloseSendFileFd(result);
        return;
      }

      if (ctx->draining) {
        LOGDEBUG("APPLY-DROP-DRAINING fd=" + std::to_string(strong_conn->fd()) +
                 " seq=" + std::to_string(result.response_seq));
        CloseSendFileFd(result);
        return;
      }

      if (result.response_seq != ctx->last_applied_response_seq + 1) {
        ctx->pending_results.emplace(result.response_seq, std::move(result));
        return;
      }

      to_apply.push_back(std::move(result));
      ctx->last_applied_response_seq++;
      while (true) {
        auto it = ctx->pending_results.find(ctx->last_applied_response_seq + 1);
        if (it == ctx->pending_results.end()) {
          break;
        }
        to_apply.push_back(std::move(it->second));
        ctx->pending_results.erase(it);
        ctx->last_applied_response_seq++;
      }
    }

    const size_t kMaxApplyPerBatch = max_apply_per_batch_;
    size_t applied = 0;
    for (auto& r : to_apply) {
      if (applied >= kMaxApplyPerBatch) {
        strong_conn->PostIoTask([this, weak_conn, ctx, result = std::move(r)]() mutable {
          PostResultToIoLoop(weak_conn, ctx, std::move(result));
        });
        continue;
      }
      LOGDEBUG("APPLY-SEND fd=" + std::to_string(strong_conn->fd()) +
               " seq=" + std::to_string(r.response_seq));
      apply_result(r);
      applied++;
      const auto io_flush_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - r.io_enqueue_tp).count();
      const long pipeline_ms = (r.queue_wait_ms > 0 ? r.queue_wait_ms : 0) + r.worker_exec_ms + io_flush_ms;
      LOGINFO("worker链路 request_id=" + r.request_id +
              " seq=" + std::to_string(r.response_seq) +
              " route=" + r.route_bucket +
              " queue_wait_ms=" + std::to_string(r.queue_wait_ms) +
              " parse_route_ms=" + std::to_string(r.parse_route_ms) +
              " business_ms=" + std::to_string(r.business_ms) +
              " serialize_ms=" + std::to_string(r.serialize_ms) +
              " worker_exec_ms=" + std::to_string(r.worker_exec_ms) +
              " io_flush_ms=" + std::to_string(io_flush_ms) +
              " pipeline_ms=" + std::to_string(pipeline_ms));
      if (pipeline_ms >= slow_request_ms_threshold_) {
        LOGWARNING("慢请求 request_id=" + r.request_id +
                   " method=" + r.method +
                   " path=" + r.path +
                   " route=" + r.route_bucket +
                   " pipeline_ms=" + std::to_string(pipeline_ms));
      }
      RecordPhase3Metrics(r, io_flush_ms, pipeline_ms);
    }
  });
}

void HttpServer::CloseSendFileFd(WorkResult& result) {
  if (result.sendfile_fd >= 0) {
    ::close(result.sendfile_fd);
    result.sendfile_fd = -1;
  }
}

void HttpServer::SendServiceUnavailable(spIConnection conn, const std::string& reason) {
  if (!conn) return;
  HttpResponse response;
  response.SetStatusCode(HttpStatusCode::SERVICE_UNAVAILABLE);
  response.SetHeader("Content-Type", "application/json; charset=utf-8");
  response.SetHeader("Connection", "close");
  response.SetBody("{\"success\":false,\"message\":\"Service busy: " + reason + "\"}");

  auto& outputbuffer = conn->getOutputBuffer();
  std::string data = response.Serialize();
  outputbuffer.append(data.c_str(), data.size());
  conn->setCloseOnSendComplete(true);
  conn->send();
}

void HttpServer::RecordPhase3Metrics(const WorkResult& result, long io_flush_ms, long pipeline_ms) {
  const std::string bucket = result.route_bucket.empty() ? "other" : result.route_bucket;
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto& metric = route_metrics_[bucket];
    metric.requests++;
    if (result.is_error) metric.errors++;
    metric.total_pipeline_ms += static_cast<uint64_t>(pipeline_ms > 0 ? pipeline_ms : 0);
    metric.max_pipeline_ms = std::max<uint64_t>(metric.max_pipeline_ms, pipeline_ms > 0 ? static_cast<uint64_t>(pipeline_ms) : 0);
    metric.total_queue_wait_ms += static_cast<uint64_t>(result.queue_wait_ms > 0 ? result.queue_wait_ms : 0);
    metric.total_worker_exec_ms += static_cast<uint64_t>(result.worker_exec_ms > 0 ? result.worker_exec_ms : 0);
    metric.total_io_flush_ms += static_cast<uint64_t>(io_flush_ms > 0 ? io_flush_ms : 0);
    metric.total_parse_route_ms += static_cast<uint64_t>(result.parse_route_ms > 0 ? result.parse_route_ms : 0);
    metric.total_business_ms += static_cast<uint64_t>(result.business_ms > 0 ? result.business_ms : 0);
    metric.total_serialize_ms += static_cast<uint64_t>(result.serialize_ms > 0 ? result.serialize_ms : 0);
    if (result.has_sendfile) {
      metric.sendfile_requests++;
      metric.sendfile_bytes += result.sendfile_bytes;
    }
  }
  MaybeLogPhase3Snapshot();
}

void HttpServer::MaybeLogPhase3Snapshot() {
  const uint64_t observed = metrics_observed_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (observed % metrics_snapshot_every_ != 0) {
    return;
  }

  std::ostringstream oss;
  oss << "Phase3指标快照 total_observed=" << observed;
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    for (const auto& kv : route_metrics_) {
      const auto& bucket = kv.first;
      const auto& m = kv.second;
      if (m.requests == 0) continue;
      const uint64_t avg_pipeline = m.total_pipeline_ms / m.requests;
      const uint64_t avg_queue_wait = m.total_queue_wait_ms / m.requests;
      const uint64_t avg_worker_exec = m.total_worker_exec_ms / m.requests;
      const uint64_t avg_io_flush = m.total_io_flush_ms / m.requests;
      oss << " | route=" << bucket
          << ", req=" << m.requests
          << ", err=" << m.errors
          << ", avg_pipeline_ms=" << avg_pipeline
          << ", max_pipeline_ms=" << m.max_pipeline_ms
          << ", avg_queue_wait_ms=" << avg_queue_wait
          << ", avg_worker_exec_ms=" << avg_worker_exec
          << ", avg_io_flush_ms=" << avg_io_flush;
      if (m.sendfile_requests > 0) {
        oss << ", sendfile_req=" << m.sendfile_requests
            << ", sendfile_bytes=" << m.sendfile_bytes;
      }
    }
  }
  LOGINFO(oss.str());
}

/**
 * 路由处理器函数实现
 * 
 * 职责：作为Router和业务服务层之间的适配器
 * - 解析HTTP请求参数
 * - 调用对应的业务服务（AuthService、DownloadService等）
 * - 生成HTTP响应
 */
// 设置路由
void HttpServer::SetupRoutes(Router& router) {
  // 初始化页面处理器集合
  auto pageHandlers = std::make_shared<std::unordered_map<std::string, std::shared_ptr<IPageHandler>>>();
  (*pageHandlers)["/index.html"] = std::make_shared<IndexPageHandler>(static_path_);
  (*pageHandlers)["/welcome.html"] = std::make_shared<WelcomePageHandler>(static_path_);
  (*pageHandlers)["/login.html"] = std::make_shared<LoginPageHandler>(static_path_);
  (*pageHandlers)["/register.html"] = std::make_shared<RegisterPageHandler>(static_path_);
  (*pageHandlers)["/picture.html"] = std::make_shared<PicturePageHandler>(static_path_);
  (*pageHandlers)["/video.html"] = std::make_shared<VideoPageHandler>(static_path_);
  
  // 创建页面路由处理器（使用lambda捕获pageHandlers的shared_ptr）
  auto pageRouteHandler = [pageHandlers](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::GET) {
      return false;
    }
    
    std::string path = request->GetPath();
    if (path == "/") path = "/index.html"; // 根路径映射到 index.html
    if (path.empty()) path = "/index.html";
    auto it = pageHandlers->find(path);
    if (it != pageHandlers->end() && it->second) {
      it->second->Handle(request, response);
      return true;
    }
    
    return false;
  };
  
  // 注册业务API路由
  router.Post("/register", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::POST) {
      return false;
    }
    
    // 解析POST表单数据
    std::string body = request->GetBody();
    auto form_data = ParseFormData(body);
    
    // 提取用户名和密码
    std::string username = form_data.count("username") > 0 ? form_data.at("username") : "";
    std::string password = form_data.count("password") > 0 ? form_data.at("password") : "";
    
    // 调用AuthService处理注册
    bool success = AuthService::HandleRegister(username, password);
    
    // 生成JSON响应
    if (success) {
      SetJsonSuccessResponse(response, "注册成功");
    } else {
      SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "注册失败，用户名可能已存在");
    }
    
    return true;
  });
  
  router.Post("/login", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::POST) {
      return false;
    }
    
    // 解析POST表单数据
    std::string body = request->GetBody();
    auto form_data = ParseFormData(body);
    
    // 提取用户名和密码
    std::string username = form_data.count("username") > 0 ? form_data.at("username") : "";
    std::string password = form_data.count("password") > 0 ? form_data.at("password") : "";
    
    // 调用AuthService处理登录
    auto login_result = AuthService::HandleLogin(username, password);
    
    // 生成JSON响应
    if (login_result) {
      // 登录成功，获取token
      std::string access_token = login_result.value().access_token;
      std::string refresh_token = login_result.value().refresh_token;
      // 在响应中包含token和refresh_token
      std::string data = "{\"token\":\"" + access_token + "\",\"refresh_token\":\"" + refresh_token + "\"}";
      SetJsonSuccessResponseWithData(response, data, "登录成功");
    } else {
      SetJsonErrorResponse(response, HttpStatusCode::UNAUTHORIZED, "登录失败，用户名或密码错误");
    }
    
    return true;
  });

  router.Post("/refresh-token", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request || request->GetMethod() != HttpMethod::POST) {
      return false;
    }
    
    // 解析POST表单数据
    std::string body = request->GetBody();
    auto form_data = ParseFormData(body);
    
    // 提取refresh_token
    std::string refresh_token = form_data.count("refresh_token") > 0 ? form_data.at("refresh_token") : "";
    
    if (refresh_token.empty()) {
      SetJsonErrorResponse(response, HttpStatusCode::BAD_REQUEST, "refresh_token不能为空");
      return true;
    }
    
    // 验证refresh_token并生成新的access_token
    auto new_token = JwtUtil::RefreshToken(refresh_token);
    if (new_token) {
      // 刷新成功，返回新的token
      std::string data = "{\"token\":\"" + new_token.value() + "\"}";
      SetJsonSuccessResponseWithData(response, data, "Token刷新成功");
    } else {
      SetJsonErrorResponse(response, HttpStatusCode::UNAUTHORIZED, "refresh_token无效");
    }
    
    return true;
  });

  // ===== 文件 API 说明 =====
  // GET /api/files：只读枚举列表，已恢复启用。仅列出 images/video/uploads 目录中
  // 白名单扩展名的文件名（FileApiService::HandleListFiles），与公开的 /images/*、
  // /video/* 静态读取一致，不暴露额外攻击面（无写入能力）。
  // 上传 / 写类接口保持注销（H7：无鉴权，匿名可写）：前端无上传入口
  // （resumableUpload.ts 未被任何组件调用），且匿名可上传文件、覆盖静态资源。
  // H8（uploadId 路径遍历）、H9（覆盖已有文件 / SVG XSS）已在 UploadService.cpp
  // 代码层修复（IsSafeUploadId 白名单 + 禁止覆盖 + 去 svg）。
  // 后续若需启用写接口：挂 JWT 鉴权中间件（Router::AddMiddlewareForPath + AuthService::ValidateToken）
  // 后取消注释即可。
  //
  router.Get("/api/files", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return FileApiService::HandleListFiles(request, response, static_path_);
  });
  //
  // router.Get("/api/files/preview", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return FileApiService::HandlePreview(request, response, static_path_);
  // });
  //
  // router.Post("/api/uploads/init", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return UploadService::HandleInit(request, response, static_path_);
  // });
  //
  // router.Put("/api/uploads/:uploadId/parts/:partNo", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return UploadService::HandleUploadPart(request, response, params, static_path_);
  // });
  //
  // router.Post("/api/uploads/:uploadId/complete", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return UploadService::HandleComplete(request, response, params, static_path_);
  // });
  router.Get("/favicon.ico", [](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    response.SetStatusCode(HttpStatusCode::NO_CONTENT);
    response.SetHeader("Content-Type", "image/x-icon");
    return true;
  });
  router.Get("/favicon.svg", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Head("/favicon.svg", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Get("/assets/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Head("/assets/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });

  router.Options("/*", [](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    response.SetStatusCode(HttpStatusCode::NO_CONTENT);
    response.SetHeader("Content-Type", "text/plain; charset=utf-8");
    ApplyCorsHeaders(response, request);
    return true;
  });
  router.Get("/download/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    // 添加静态路径到路由参数中，供处理器使用
    RouteParams new_params = params;
    new_params.params_["static_path"] = static_path_;
    return DownloadService::HandleDownload(request, response, static_path_);
  });
  router.Head("/download/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    RouteParams new_params = params;
    new_params.params_["static_path"] = static_path_;
    return DownloadService::HandleDownload(request, response, static_path_);
  });
  
  // 注册静态文件路由
  router.Get("/images/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Head("/images/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  
  router.Get("/video/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Head("/video/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });

  // ===== /uploads/* 读取路由已注销（随上传功能一并禁用，见上方 H7/H8/H9 说明）=====
  // 该路由原用于匿名读取上传目录，注销后 /uploads/* 一律 404。
  // 若后续重新启用上传功能，应同时恢复这两条路由并挂鉴权。
  //
  // router.Get("/uploads/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return StaticFileService::HandleStaticFile(request, response, static_path_);
  // });
  // router.Head("/uploads/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
  //   auto* request = dynamic_cast<HttpRequest*>(&message);
  //   if (!request) return false;
  //   return StaticFileService::HandleStaticFile(request, response, static_path_);
  // });
  
  // 注册页面路由（使用lambda表达式）
  router.Get("/", pageRouteHandler);
  router.Get("/index.html", pageRouteHandler);
  router.Get("/welcome.html", pageRouteHandler);
  router.Get("/login.html", pageRouteHandler);
  router.Get("/register.html", pageRouteHandler);
  router.Get("/picture.html", pageRouteHandler);
  router.Get("/video.html", pageRouteHandler);
}

// 处理HTTP请求的辅助函数
void HttpServer::ProcessRequest(HttpRequest* request, HttpResponse& response) {
  // 设置响应版本
  response.SetVersion(request->GetVersion());
  
  // 注意：HttpServer不再自行实现路由功能，而是通过HttpFacade提供的接口使用路由
  // 路由匹配和处理已经在HttpFacade::Process()中执行
  
  // 这里可以添加一些额外的处理逻辑（如果需要）
  
  // 检查响应是否已经设置了状态码
  if (response.getStatusCodeInt() == 0) {
    // 如果没有设置状态码，说明路由处理失败，返回404错误
    response.SetStatusCode(HttpStatusCode::NOT_FOUND);
    response.SetHeader("Content-Type", "text/plain");
    response.SetBody("Not Found");
  }
}




void HttpServer::HandleSendComplete(spIConnection conn){

  LOGINFO("Message send complete.");

}
/*
void HttpServer::HandleTimeOut(EventLoop*loop){
  std::cout<<"EchoServer timeout."<<std::endl;

}   
*/
