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
#include<algorithm>
#include<cerrno>
#include<cstring>
#include<fcntl.h>
#include<unistd.h>
#include<sstream>
#include<atomic>
#include<chrono>

HttpServer::HttpServer(const std::string &ip,uint16_t port,int timeoutS,bool OptLinger,
                       int sqlPort,const char*sqlUser,const char*sqlPwd,const char*dbName,
                       int subthreadnum,int workthreadnum,int connpoolnum,const std::string&static_path)
      :tcpserver_(ip,port,subthreadnum,timeoutS,OptLinger),
       threadpool_(static_cast<size_t>(std::max(1, workthreadnum)), "WORKS"),
       static_path_(static_path)
{
  // 以下代码不是必须的，业务关心什么事件，就指定相应的回调函数。
  tcpserver_.setnewconnection(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
  tcpserver_.setcloseconnection(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
  tcpserver_.seterrorconnection(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
  tcpserver_.setonmessage(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1/*, std::placeholders::_2*/));
  tcpserver_.setsendcomplete(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
  //tcpserver_.settimeout(std::bind(&HttpServer::HandleTimeOut, this, std::placeholders::_1));
  SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connpoolnum);
  
  router_ = std::make_shared<Router>();
  SetupRoutes(*router_);
  tls_ctx_ = TlsContext::CreateFromEnv();
  if (workthreadnum <= 0) {
    LOGWARNING("workthreadnum<=0，已自动调整为1，避免任务无人消费");
  }
}
HttpServer::~HttpServer(){

}
void HttpServer::start(){
  LOGINFO("Http服务器启动");
  tcpserver_.start();
}

void HttpServer::Stop(){
  LOGINFO("Http服务器关闭");
  SqlConnPool::Instance()->ClosePool();
  //停止工作线程
  threadpool_.stop();
  //停止IO线程
  tcpserver_.stop();
}
void HttpServer::HandleNewConnection(spConnection conn){
  LOGINFO("new connection(fd="+std::to_string(conn->fd())+",ip="+conn->ip()+",port="+std::to_string(conn->port())+ ")ok.");
  if (conn) {
    if (tls_ctx_) {
      conn->SetTlsContext(tls_ctx_);
    }
    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->facade = std::make_shared<HttpFacade>();
    ctx->max_concurrent_workers = max_concurrent_workers_per_conn_;
    if (router_) {
      ctx->facade->SetRouter(router_);
    }
    conn->SetContext(std::move(ctx));
  }
}
void HttpServer::HandleClose(spConnection conn){
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
void HttpServer::HandleError(spConnection conn){
  HandleClose(conn);
  LOGERROR("connection error(fd=" + std::to_string(conn->fd()) + ",ip=" + conn->ip() + ",port=" + std::to_string(conn->port()) + ")");
}
void HttpServer::HandleMessage(spConnection conn){
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
    ctx->facade = std::make_shared<HttpFacade>();
    ctx->max_concurrent_workers = max_concurrent_workers_per_conn_;
    if (router_) {
      ctx->facade->SetRouter(router_);
    }
    conn->SetContext(ctx);
  }

  std::string new_data = inputbuffer.bufferToString();
  inputbuffer.consumeBytes(readable_bytes);

  if (threadpool_.queue_size() > max_work_queue_depth_) {
    LOGERROR("工作队列过长，触发背压 fd=" + std::to_string(conn->fd()) +
             " queue_size=" + std::to_string(threadpool_.queue_size()));
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

  std::weak_ptr<Connection> weak_conn = conn;
  threadpool_.addtask([this, weak_conn, ctx]() mutable {
    HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
  });
}

void HttpServer::HandleMessageInWorker(std::weak_ptr<Connection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx) {
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
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);

    if (ctx->queued_chunks.empty()) {
      OnWorkerExit(ctx, conn);
      return;
    }

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

  if (should_chain) {
    threadpool_.addtask([this, weak_conn, ctx]() mutable {
      HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
    });
  }

  ProcessSingleRequest(weak_conn, ctx, std::move(chunk));

  OnWorkerExit(ctx, conn);
}

void HttpServer::PhaseParseAndRoute(
    std::weak_ptr<Connection> weak_conn,
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

    if (req_ctx->result == HttpServerResult::SUCCESS && req_ctx->message) {
      size_t consumed_bytes = ctx->facade->GetConsumedBytes();
      ctx->facade->ErasePending(consumed_bytes);
    }

    if (req_ctx->result == HttpServerResult::NEED_MORE_DATA) {
      LOGINFO("HTTP请求数据不完整，等待更多数据");
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

  auto connection_header = request->GetHeader("Connection");
  if (connection_header.has_value()) {
    std::string conn_value = connection_header.value();
    LowerAsciiInPlace(conn_value);
    req_ctx->keep_alive = (conn_value == "keep-alive");
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
    std::weak_ptr<Connection> weak_conn,
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
  req_ctx->file_length = static_cast<size_t>(req_ctx->response.GetSendFileLength());
  req_ctx->response.SetBody("");

  // future io_uring extension point:
  // if (io_uring_submit(...) == -EAGAIN) {
  //   req_ctx->suspended = true;
  //   RegisterResumeCallback(weak_conn, ctx, chunk, req_ctx);
  //   return;
  // }
}

void HttpServer::PhaseSerializeAndSend(
    std::weak_ptr<Connection> weak_conn,
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
    if (!req_ctx->err.IsOk()) {
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
    std::weak_ptr<Connection> weak_conn,
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
    std::shared_ptr<Connection> conn) {

  std::lock_guard<std::mutex> lock(ctx->mutex);
  ctx->active_worker_count--;

  if (ctx->active_worker_count == 0) {
    if (!ctx->queued_chunks.empty() && !ctx->draining) {
      ctx->active_worker_count = 1;
      threadpool_.addtask(
          [this, weak_conn = std::weak_ptr<Connection>(conn), ctx]() mutable {
            HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
          });
    } else {
      ctx->worker_running = false;
    }
  }
}

void HttpServer::PostResultToIoLoop(std::weak_ptr<Connection> weak_conn, std::shared_ptr<ConnectionWorkContext> ctx, WorkResult result) {
  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    CloseSendFileFd(result);
    return;
  }

  EventLoop* io_loop = conn->getLoop();
  result.io_enqueue_tp = std::chrono::steady_clock::now();
  io_loop->queueinloop([this, io_loop, weak_conn, ctx, result = std::move(result)]() mutable {
    auto strong_conn = weak_conn.lock();
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
        CloseSendFileFd(result);
        return;
      }

      if (ctx->draining) {
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
        io_loop->queueinloop(
            [this, weak_conn, ctx, result = std::move(r)]() mutable {
              PostResultToIoLoop(weak_conn, ctx, std::move(result));
            });
        continue;
      }
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

void HttpServer::SendServiceUnavailable(spConnection conn, const std::string& reason) {
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

  router.Get("/api/files", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return FileApiService::HandleListFiles(request, response, static_path_);
  });

  router.Get("/api/files/preview", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return FileApiService::HandlePreview(request, response, static_path_);
  });

  router.Post("/api/uploads/init", [this](IHttpMessage& message, HttpResponse& response, const RouteParams&) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return UploadService::HandleInit(request, response, static_path_);
  });

  router.Put("/api/uploads/:uploadId/parts/:partNo", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return UploadService::HandleUploadPart(request, response, params, static_path_);
  });

  router.Post("/api/uploads/:uploadId/complete", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return UploadService::HandleComplete(request, response, params, static_path_);
  });
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

  router.Get("/uploads/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  router.Head("/uploads/*", [this](IHttpMessage& message, HttpResponse& response, const RouteParams& params) {
    auto* request = dynamic_cast<HttpRequest*>(&message);
    if (!request) return false;
    return StaticFileService::HandleStaticFile(request, response, static_path_);
  });
  
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




void HttpServer::HandleSendComplete(spConnection conn){

  LOGINFO("Message send complete.");

}
/*
void HttpServer::HandleTimeOut(EventLoop*loop){
  std::cout<<"EchoServer timeout."<<std::endl;

}   
*/
