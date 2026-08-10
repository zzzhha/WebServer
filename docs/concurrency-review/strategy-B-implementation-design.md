# 请求级并发架构重构设计文档

## 概述

本文档为"策略 B：请求级并发"的完整实施方案。目标是将当前 HTTP 同连接内的串行请求处理模型，改造为**多 Worker 并行处理 + IO 线程严格保序回写**的并发模型。

### 架构变化

```
改造前（现状）：
  连接A 请求1(大文件) → 请求2(API) → 请求3(API)
         [Worker-1 串行处理全部] → IO线程按序回写

| **Worker 耗尽** | 单连接并发度需受控，防止全局 Worker 线程池被单一连接耗尽 |
| **背压兼容** | 现有全局队列背压 + 单连接字节背压机制需保留 |

---

## 路线图定位

本策略在[后续更新路线图](/home/zsy/WebServer/.trae/skills/roadmap-next-updates.md)中的定位如下：

```
P0 (Nginx 反向代理) ───→ 提供灰度/压测入口
       │
       ▼
P0.5 (策略B: 请求级并发)  ←── 本文档
       │
       ├── 为 P1 (io_uring) 提供"Worker 可挂起"的边界抽象
       ├── 为 P2 (协程化) 提供"单请求处理函数 ProcessSingleRequest"的接口契约
       └── 与 P0 (Nginx) 协同：Nginx 负责连接管理与流量分发，
           策略 B 负责单连接内的请求级并行
       │
       ▼
P1 (io_uring 异步 IO)
       │
       ▼
P2 (协程化 Worker)
```

**为什么策略 B 在 P0 之后、P1 之前**：
- Nginx 反向代理（P0）先行落地后，可以**灰度切换流量**到启用了并行处理的节点，降低回滚风险
- 策略 B 先行落地后，`HandleMessageInWorker` 的 `while(true)` 串行循环被拆解为**可独立调度的工作单元**，这是后续 io_uring 异步化和协程化的**前置条件**
- 策略 B 不需要新的 I/O 模型或异步框架，仅在现有 epoll + 线程池基础设施上做改造，**风险最低、收益最快**

---

## Phase 1：请求级并行（最小可行并行）

### 目标

将 `HandleMessageInWorker` 从单 worker 串行消费改为**多 worker 链式调度**，实现同连接请求级并行处理。

### 1.1 `ConnectionWorkContext` 扩展

**文件**：[HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)

```cpp
struct ConnectionWorkContext {
  // === 现有字段（保持不变） ===
  std::shared_ptr<HttpFacade> facade;
  std::mutex mutex;
  std::deque<PendingChunk> queued_chunks;
  size_t queued_bytes{0};
  bool worker_running{false};
  uint64_t next_enqueue_seq{1};
  uint64_t next_response_seq{1};
  uint64_t last_applied_response_seq{0};
  std::map<uint64_t, WorkResult> pending_results;

  // === 新增：并行控制 ===
  size_t active_worker_count{0};            // 当前正在执行的 worker 数
  size_t max_concurrent_workers{4};          // 单连接最大并发 worker 数
  bool draining{false};                      // 排空模式：不再启动新 worker

  // === 新增：HttpFacade 访问序列化 ===
  std::mutex facade_mutex;                   // 保护 facade 的独占访问
};
```

### 1.2 HttpServer 新增配置

**文件**：[HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)

```cpp
class HttpServer {
private:
  // === 新增：并行控制配置 ===
  size_t max_concurrent_workers_per_conn_{4};  // 单连接最大并发 Worker 数
  size_t max_apply_per_batch_{16};             // IO 线程单次最大刷出数
  bool parallel_pipelining_enabled_{false};    // 总开关（默认关闭，逐步灰度）
};
```

### 1.3 修改 `HandleMessage`：入队逻辑兼容链式调度

**文件**：[HttpServer.cpp::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L92-L163)

**改动要点**：

```cpp
void HttpServer::HandleMessage(spConnection conn) {
  // ... 前置检查（缓冲区空、背压等）保持不变 ...

  bool should_start_worker = false;
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);

    // 背压检查（保持不变）
    if (ctx->queued_bytes + new_data.size() > max_conn_pending_bytes_) {
      // ... 背压处理 ...
      return;
    }

    PendingChunk chunk;
    chunk.data = std::move(new_data);
    chunk.enqueue_seq = ctx->next_enqueue_seq++;
    chunk.enqueue_tp = std::chrono::steady_clock::now();
    ctx->queued_bytes += chunk.data.size();
    ctx->queued_chunks.push_back(std::move(chunk));

    // === 改造点：仅在 worker 完全停止时启动新 worker ===
    // 如果已有 worker 运行，由 worker 内部链式调度消费新 chunk
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
```

### 1.4 重写 `HandleMessageInWorker`：链式调度

**文件**：[HttpServer.cpp::HandleMessageInWorker](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L167-L378)

**核心设计**：
- 每个 worker 实例消费**一个 chunk** 后即返回
- 如果队列中还有剩余 chunk，**链式启动**新 worker 消费
- 使用 `active_worker_count` 控制并发度上限
- 使用 `facade_mutex` 序列化 HttpFacade 访问

```cpp
void HttpServer::HandleMessageInWorker(
    std::weak_ptr<Connection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx) {

  // --- Step 1: 检查连接状态 ---
  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    OnWorkerExit(ctx, nullptr);
    return;
  }

  // --- Step 2: 排空检查 ---
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

  // --- Step 3: 取出一个 chunk ---
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

    // 如果队列中还有剩余 chunk 且未达并发上限，链式启动新 worker
    if (!ctx->queued_chunks.empty() &&
        ctx->active_worker_count < ctx->max_concurrent_workers &&
        !ctx->draining) {
      ctx->active_worker_count++;
      should_chain = true;
    }
  }

  // --- Step 4: 链式启动下一个 worker（必须在锁外执行 addtask） ---
  if (should_chain) {
    threadpool_.addtask([this, weak_conn, ctx]() mutable {
      HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
    });
  }

  // --- Step 5: 处理当前请求 ---
  ProcessSingleRequest(weak_conn, ctx, std::move(chunk));

  // --- Step 6: 当前 worker 退出 ---
  OnWorkerExit(ctx, conn);
}
```

### 1.5 抽取 `ProcessSingleRequest`

**文件**：[HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)

将原 `HandleMessageInWorker` 中处理单个请求的逻辑抽取为独立函数：

```cpp
void HttpServer::ProcessSingleRequest(
    std::weak_ptr<Connection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    PendingChunk chunk) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    return;
  }

  // --- Step 1: 序列化访问 HttpFacade ---
  HttpResponse response;
  std::unique_ptr<IHttpMessage> message;
  HttpError err;
  HttpServerResult result;

  {
    std::lock_guard<std::mutex> facade_lock(ctx->facade_mutex);
    if (!chunk.data.empty()) {
      ctx->facade->AppendPending(std::move(chunk.data));
    }

    auto parse_begin = std::chrono::steady_clock::now();
    result = ctx->facade->ProcessPending(message, response, err);
    auto parse_end = std::chrono::steady_clock::now();

    if (result == HttpServerResult::SUCCESS && message) {
      size_t consumed_bytes = ctx->facade->GetConsumedBytes();
      ctx->facade->ErasePending(consumed_bytes);
    }
  }

  // --- Step 2: NEED_MORE_DATA 处理 ---
  if (result == HttpServerResult::NEED_MORE_DATA) {
    LOGINFO("HTTP请求数据不完整，等待更多数据");
    return;
  }

  // --- Step 3: 构建 WorkResult（与原 HandleMessageInWorker 相同） ---
  WorkResult work_result;
  work_result.parse_route_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      parse_end - parse_begin).count();

  if (result == HttpServerResult::SUCCESS && message) {
    // ... 与原相同的路由、业务处理、序列化逻辑 ...
    // 见现存 HttpServer.cpp:L210-L377
  } else {
    // ... 错误处理 ...
  }

  // 获取 response_seq
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    work_result.response_seq = ctx->next_response_seq++;
  }

  work_result.worker_exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - chunk.enqueue_tp).count();

  PostResultToIoLoop(weak_conn, ctx, std::move(work_result));
}
```

### 1.6 `OnWorkerExit` 实现

```cpp
void HttpServer::OnWorkerExit(
    std::shared_ptr<ConnectionWorkContext> ctx,
    std::shared_ptr<Connection> conn) {

  std::lock_guard<std::mutex> lock(ctx->mutex);
  ctx->active_worker_count--;

  if (ctx->active_worker_count == 0) {
    // 检查是否有新数据到达（在 worker 执行期间 IO 线程可能入了新数据）
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
```

### 1.7 `PostResultToIoLoop` 增强：fd 泄漏防护

**文件**：[HttpServer.cpp::PostResultToIoLoop](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L383)

```cpp
void HttpServer::PostResultToIoLoop(
    std::weak_ptr<Connection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    WorkResult result) {

  auto conn = weak_conn.lock();
  if (!conn || conn->IsDisconnected()) {
    CloseSendFileFd(result);
    return;
  }

  EventLoop* io_loop = conn->getLoop();
  result.io_enqueue_tp = std::chrono::steady_clock::now();
  io_loop->queueinloop([this, weak_conn, ctx, result = std::move(result)]() mutable {
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
        strong_conn->StartSendFile(r.sendfile_fd, r.sendfile_offset,
                                   r.sendfile_length, true);
        r.sendfile_fd = -1;
      }
      strong_conn->send();
    };

    std::vector<WorkResult> to_apply;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);

      // 过期结果
      if (result.response_seq <= ctx->last_applied_response_seq) {
        CloseSendFileFd(result);
        return;
      }

      // 排空模式
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
        if (it == ctx->pending_results.end()) break;
        to_apply.push_back(std::move(it->second));
        ctx->pending_results.erase(it);
        ctx->last_applied_response_seq++;
      }
    }

    // 批处理限制：防止 IO 线程被长时间占用
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
    }
  });
}

// === 新增：安全关闭 fd 辅助函数 ===
void HttpServer::CloseSendFileFd(WorkResult& result) {
  if (result.sendfile_fd >= 0) {
    ::close(result.sendfile_fd);
    result.sendfile_fd = -1;
  }
}
```

### 1.8 连接关闭与排空

```cpp
void HttpServer::HandleClose(spConnection conn) {
  auto ctx = conn->GetContext<std::shared_ptr<ConnectionWorkContext>>();
  if (ctx && *ctx) {
    auto work_ctx = *ctx;
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
  LOGINFO("connection close(fd=" + std::to_string(conn->fd()) + ")");
}

void HttpServer::HandleError(spConnection conn) {
  // 与 HandleClose 相同逻辑，可抽取公共函数
  HandleClose(conn);
}
```

---

## Phase 2：io_uring 适配与 Worker 可挂起边界

### 背景

P1（io_uring）要求在 Worker 中能够**挂起当前请求的处理**，等待异步 IO 完成后再恢复。但 Phase 1 的 `ProcessSingleRequest` 是同步阻塞的——`open()`、`stat()`、`read()` 都会阻塞 Worker 线程。

### 改造目标

在不改动 Phase 1 整体架构的前提下，为 `ProcessSingleRequest` 添加**可挂起/恢复**的扩展点，使其后续可被 io_uring 或协程利用。

### 2.1 拆分 ProcessSingleRequest 为多阶段回调

```cpp
// 将 ProcessSingleRequest 拆解为：
//
// Phase 2-A: ParseAndRoute   (纯 CPU，不可挂起)
//   → 解析 HTTP、路由分发、构造响应上下文
//
// Phase 2-B: IoOperation     (可挂起，适合 io_uring)
//   → open()/stat()/read() 文件
//   → 数据库查询
//   → 上传文件落盘
//
// Phase 2-C: SerializeAndSend (纯 CPU，不可挂起)
//   → 序列化响应、PostResultToIoLoop

enum class RequestPhase {
  PARSE_AND_ROUTE,
  IO_OPERATION,
  SERIALIZE_AND_SEND
};

struct RequestContext {
  std::unique_ptr<IHttpMessage> message;
  HttpResponse response;
  HttpError err;
  HttpServerResult result;
  std::string request_id;
  std::string method;
  std::string path;
  bool keep_alive{false};

  // IO 阶段上下文
  int file_fd{-1};
  off_t file_offset{0};
  size_t file_length{0};

  // 时间统计
  std::chrono::steady_clock::time_point parse_begin;
  std::chrono::steady_clock::time_point business_begin;
  std::chrono::steady_clock::time_point serialize_begin;

  // 继续令牌：标记 Phase 2 在何处恢复
  RequestPhase next_phase{RequestPhase::PARSE_AND_ROUTE};
  bool suspended{false};
};

// ProcessSingleRequest 改造为多阶段执行器
void HttpServer::ProcessSingleRequest(
    std::weak_ptr<Connection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx,
    PendingChunk chunk,
    std::shared_ptr<RequestContext> req_ctx) {

  if (!req_ctx) {
    req_ctx = std::make_shared<RequestContext>();
  }

  if (req_ctx->next_phase == RequestPhase::PARSE_AND_ROUTE) {
    PhaseParseAndRoute(weak_conn, ctx, chunk, req_ctx);
    if (req_ctx->suspended) return;  // NEED_MORE_DATA
    req_ctx->next_phase = RequestPhase::IO_OPERATION;
  }

  if (req_ctx->next_phase == RequestPhase::IO_OPERATION) {
    PhaseIoOperation(weak_conn, ctx, req_ctx);
    // 未来 io_uring 版本在此处挂起：
    // if (io_uring_submit(...) == -EAGAIN) {
    //   req_ctx->suspended = true;
    //   RegisterResumeCallback(weak_conn, ctx, chunk, req_ctx);
    //   return;
    // }
    req_ctx->next_phase = RequestPhase::SERIALIZE_AND_SEND;
  }

  if (req_ctx->next_phase == RequestPhase::SERIALIZE_AND_SEND) {
    PhaseSerializeAndSend(weak_conn, ctx, chunk, req_ctx);
  }
}
```

### 2.2 与 io_uring 的对接点

| 当前实现 | io_uring 改造后 |
|----------|----------------|
| `::open(path, O_RDONLY)` | `io_uring_prep_openat(sqe, dfd, path, flags, mode)` |
| `::stat(path, &st)` | `io_uring_prep_statx(sqe, dfd, path, flags, mask, &statxbuf)` |
| `::read(fd, buf, count)` | `io_uring_prep_read(sqe, fd, buf, count, offset)` |
| 阻塞等待 SQL 结果 | `io_uring_prep_recv(sqe, mysql_fd, ...)` |

**关键设计**：Phase 1 的 `ProcessSingleRequest` 签名中已经**接受 `PendingChunk` 并按需消费**，这意味着 io_uring 版本只需要在 `PhaseIoOperation` 中将同步调用替换为异步提交 + 挂起，不需要改动上层调度逻辑。

### 2.3 Worker 线程与 io_uring 的亲和性

在 P1 阶段，Worker 线程将同时承担两类工作：

```
Worker 线程:
  1. 同步路径（Phase 1）：执行 ProcessSingleRequest 的纯 CPU 阶段
  2. 异步路径（Phase 2）：提交 io_uring SQE → 挂起 → 被唤醒后继续

Worker 线程生命周期:
  [ProcessSingleRequest] → [提交 SQE] → [挂起/让出] → [被唤醒] → [收 CQE] → [继续]
```

这意味着 Worker 线程在 P1 阶段需要：
- 运行一个小的 **SQE 提交循环**（io_uring_submit 批处理）
- 运行一个 **CQE 收割循环**（io_uring_cqe_seen 批量处理）
- 在 `OnWorkerExit` 中确保未完成的 SQE 被清理

---

## Phase 3：协程化 Worker（与 P2 衔接）

### 背景

P2（协程化）的目标是用协程替代显式的回调/状态机，使 `ProcessSingleRequest` 可以像同步代码一样编写，底层自动 yielding。

### 3.1 协程调度器接口

```cpp
// 协程化后的 Worker 入口
// 在 P2 阶段，HandleMessageInWorker 将改为：

Task<void> HttpServer::HandleMessageInWorkerCoroutine(
    std::weak_ptr<Connection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx) {

  while (true) {
    // co_await 从队列中取出一个 chunk
    auto chunk = co_await DequeueChunk(ctx);
    if (!chunk) co_return;

    // co_await 解析 HTTP（可能跨多次 await 等待完整请求）
    auto request = co_await ParseHttpRequest(ctx, std::move(*chunk));

    // 路由 + 业务处理（纯 CPU，不阻塞）
    auto response = ProcessRequest(request);

    // co_await 文件打开（io_uring 异步）
    co_await OpenFileIfNeeded(response);

    // 序列化 + 回写
    co_await PostResultToIoLoop(weak_conn, ctx, std::move(result));
  }
}
```

### 3.2 Phase 1 到 Phase 3 的演进路径

```
Phase 1 (当前):
  HandleMessageInWorker:
    while(true) { chunk = queue.pop(); ProcessSingleRequest(); }

Phase 2 (io_uring 适配):
  ProcessSingleRequest:
    PhaseA (CPU) → PhaseB (io_uring submit) → PhaseC (CPU)
    PhaseB 通过 callback 恢复: io_loop→queueinloop([=](){ PhaseC(); })

Phase 3 (协程化):
  HandleMessageInWorkerCoroutine:
    while(true) {
      chunk = co_await queue.pop_async();
      co_await ParseAndRoute(chunk);
      co_await IoOperation();
      co_await SerializeAndSend();
    }
  编译器自动处理挂起/恢复，不再需要显式 callback
```

### 3.3 协程对 ConnectionWorkContext 的影响

协程化后，`ConnectionWorkContext` 中的部分字段将不再需要：

| 字段 | Phase 1 需要 | Phase 3 协程化后 |
|------|-------------|-----------------|
| `active_worker_count` | 控制并发度 | 仍需要，控制协程并发数 |
| `max_concurrent_workers` | 控制并发度 | 仍需要，但语义变为"最大协程数" |
| `facade_mutex` | 序列化 HttpFacade | 不再需要（协程单线程执行） |
| `draining` | 排空控制 | 仍需要 |
| `worker_running` | 标记 worker 状态 | 不再需要（协程生命周期由调度器管理） |
| `pending_results` | 乱序保序 | 不再需要（协程天然按序） |

---

## 灰度与回滚策略

### 灰度步骤

1. **Step 1**：Nginx 反代（P0）落地后，配置一组后端节点启用 `parallel_pipelining_enabled_ = true`
2. **Step 2**：在灰度节点上设置 `max_concurrent_workers_per_conn_ = 2`，观察 24h
3. **Step 3**：确认 fd 无泄漏、p99 延迟稳定后，提升到 4
4. **Step 4**：全量发布

### 一键回滚

```cpp
// HttpServer.h
bool parallel_pipelining_enabled_{false};

// HttpServer.cpp HandleMessageInWorker 入口
if (!parallel_pipelining_enabled_) {
  LegacyHandleMessageInWorker(weak_conn, ctx);  // 原 while(true) 实现
  return;
}
```

保留原 `HandleMessageInWorker` 实现为 `LegacyHandleMessageInWorker`，通过开关一键切换。

---

## 测试方案

### 单元测试

| 测试用例 | 覆盖场景 | 验证方法 |
|----------|----------|----------|
| `test_parallel_pipelining` | 同一连接 3 个请求并行处理，响应按序回写 | 检查 outputbuffer 中响应顺序 == 请求顺序 |
| `test_fd_no_leak_on_disconnect` | 连接关闭时未回写的 sendfile_fd 被正确关闭 | 使用 `lsof` 或 fd 计数检查 |
| `test_worker_count_bound` | 单连接并发 worker 数不超过 `max_concurrent_workers` | 插入调试日志验证 |
| `test_draining_no_new_worker` | 排空模式下不再启动新 worker | 验证 `active_worker_count` 不再增长 |
| `test_facade_mutex_no_deadlock` | 多 worker 竞争 facade_mutex 无死锁 | 压力测试 + 超时检测 |
| `test_backpressure_with_parallel` | 并行场景下背压机制仍正常触发 | 构造大负载验证 503 响应 |

### 集成测试

```
测试场景 1：混合负载
  - 10 个连接同时发送请求
  - 每个连接包含：1 个大文件下载 + 5 个 API 请求
  - 预期：API 请求不受大文件下载阻塞，p99 < 50ms

测试场景 2：并发上限验证
  - 单连接发送 20 个请求
  - 验证 active_worker_count <= max_concurrent_workers

测试场景 3：连接关闭安全
  - 大文件下载中关闭连接
  - 验证无 fd 泄漏
  - 验证服务不崩溃
```

---

## 附录 A：关键数据竞争分析

### A.1 `ctx->mutex` 竞争分析

| 访问点 | 线程 | 保护方式 |
|--------|------|----------|
| `queued_chunks` push | IO 线程 (HandleMessage) | `ctx->mutex` |
| `queued_chunks` pop | Worker 线程 (HandleMessageInWorker) | `ctx->mutex` |
| `pending_results` emplace | IO 线程 (PostResultToIoLoop) | `ctx->mutex` |
| `pending_results` find/erase | IO 线程 (PostResultToIoLoop) | `ctx->mutex` |
| `active_worker_count++` | Worker 线程 | `ctx->mutex` |
| `active_worker_count--` | Worker 线程 | `ctx->mutex` |
| `draining` 读写 | IO/Worker 线程 | `ctx->mutex` |
| `next_response_seq++` | Worker 线程 | `ctx->mutex` |

**结论**：所有对 `ConnectionWorkContext` 字段的访问都在 `ctx->mutex` 保护下，无数据竞争。

### A.2 `facade_mutex` 竞争分析

| 访问点 | 线程 | 保护方式 |
|--------|------|----------|
| `AppendPending` | Worker 线程 | `facade_mutex` |
| `ProcessPending` | Worker 线程 | `facade_mutex` |
| `ErasePending` | Worker 线程 | `facade_mutex` |

**结论**：`HttpFacade` 的访问通过独立的 `facade_mutex` 序列化，与 `ctx->mutex` 解耦，避免死锁。

### A.3 fd 生命周期分析

```
Worker 线程:
  open() → fd=N
  设置 work_result.sendfile_fd = N
  PostResultToIoLoop(...)  // fd 所有权移交给 WorkResult

IO 线程 (PostResultToIoLoop):
  if 连接已断开 → close(fd=N)
  if 过期结果 → close(fd=N)
  if 正常应用 → Connection::StartSendFile(fd=N)  // 所有权移交给 Connection
  if 排空模式 → close(fd=N)
  if 批处理限制导致重新入队 → WorkResult 仍持有 fd，下次再处理

Connection 析构/发送完成:
  if sendfile_fd >= 0 → close(sendfile_fd)
```

**结论**：fd 的 close 责任链清晰，无泄漏路径。

---

## 附录 B：代码改动清单

| 文件 | 改动类型 | 行数估算 | 说明 |
|------|----------|----------|------|
| `HttpServer.h` | 修改 | +15 行 | `ConnectionWorkContext` 新增字段 + HttpServer 新增配置 |
| `HttpServer.cpp` HandleMessage | 修改 | +15 行 | 入队逻辑兼容链式调度 |
| `HttpServer.cpp` HandleMessageInWorker | **重写** | - | 去除 `while(true)`，改为单请求处理 + 链式调度 |
| `HttpServer.cpp` ProcessSingleRequest | **新增** | +120 行 | 从原 while 循环体抽取 |
| `HttpServer.cpp` OnWorkerExit | **新增** | +30 行 | Worker 退出清理逻辑 |
| `HttpServer.cpp` PostResultToIoLoop | 修改 | +30 行 | fd 泄漏防护 + 批处理限制 |
| `HttpServer.cpp` HandleClose/HandleError | 修改 | +30 行 | 排空处理 |
| `HttpServer.cpp` CloseSendFileFd | **新增** | +10 行 | 安全关闭 fd 辅助函数 |
| `main.cpp` | 可选修改 | +5 行 | 配置传递 |
| 合计 | | **~260 行新增/修改** | |

---

## 附录 C：与路线图中其他阶段的衔接要点

### 与 P0（Nginx 反向代理）的配合

- Nginx 负责：TLS 终止、连接管理、静态资源缓存、流量灰度
- 策略 B 负责：单连接内的 HTTP Pipelining 并行处理
- 通过 Nginx 的 `upstream` 配置，可以将请求均匀分发到多个后端实例
- 后端实例启用策略 B 后，单个 Nginx 连接对应的后端连接可以利用并行性加速

### 与 P1（io_uring）的配合

- Phase 2 定义的 `IoOperation` 阶段是 io_uring 的直接替换点
- Worker 线程需要扩展为"Worker + io_uring 提交/收割"双角色
- `ProcessSingleRequest` 的多阶段签名为后续异步化提供了清晰的边界

### 与 P2（协程）的配合

- Phase 1 中 `ProcessSingleRequest` 的抽取是协程化的前置条件
- 协程化后 `HandleMessageInWorkerCoroutine` 可以直接复用：
  - `ConnectionWorkContext`（移除 `facade_mutex` 和 `pending_results`）
  - `PostResultToIoLoop`（保序回写逻辑不变）
  - `CloseSendFileFd`（fd 生命周期管理不变）