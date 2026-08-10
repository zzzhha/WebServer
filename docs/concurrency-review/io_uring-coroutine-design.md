# io_uring 协程化（Proactor + Coroutine）设计与实施计划

## 0. 实施状态跟踪（务必先读）

> 本节用于避免实施者把"已落地"的代码当成"待新增"重复开发。状态以 `cppBackend/` 当前主干代码为准。

| 阶段 | 状态 | 说明 |
|------|------|------|
| 阶段一：Awaitable 基础设施 + UringConnection 扩展 | **已落地** | `IConnection::AsUringConnection/IsProactorMode`、`UringConnection::AsyncRecvReady/AsyncWriteResponse/AsyncSendFile`、`ResumeEntry/recv_resume_/write_resume_`、`Register*/ClearAllResumeHandles`、`UringWorker::GetCoroutineExecutor/SetCoroutineExecutor` 均已实现；`HandleRecvComplete/HandleWriteComplete/HandleTlsPollCQE` 已包含协程分支；`DoClose` 已调用 `ClearAllResumeHandles`；§3.2.1.1 中缺陷 A/B/C 已修复，且 awaiter 返回类型已显式化，支持跨翻译单元 `co_await`。 |
| 阶段二：HttpServer 协程路径集成 | **已落地** | `HandleMessageCoro` 已实现；`HandleMessage` 已支持协程/回调分流；`ConnectionWorkContext::pending_coro_results` 与 `tracked_ctxs_` 已补齐；`WEBSERVER_COROUTINE=1` 已可运行期开启协程路径。 |
| 阶段三：停机安全与边界场景 | **已落地** | `HandleMessageCoro` 已接入 `pending_work_count_` RAII，`Stop()` 会等待在飞协程并收集 `pending_coro_results`；`DoClose/ClearAllResumeHandles` 已完成挂起协程清理；`HandleTlsPollCQE` 已补齐写完成恢复与 TLS 下 sendfile 文件体搬运，停机/断链/TLS 写边界已具备可运行实现。 |
| 阶段四：测试与验证 | **已落地（HTTP）** | 已新增 `test/io_uring_coroutine_phase4_tests.cpp`，覆盖正确性、HTTP 行为、sendfile、压力、异常、双模式回归、停机 7 条 HTTP 自动化测试；TLS/SSL 集成验证按当前范围暂不纳入本轮。 |

**给实施者的两条铁律**：
1. 不要再新增 §3.2.1、§3.2.4 接口隔离（IConnection/UringConnection）和 §6 表中标"新增"的阶段一项；它们已存在，重做只会造成重复定义/链接冲突。
2. 阶段一中的 `HandleWriteComplete` 提前 return、awaiter 注册竞态与 `resume_mutex_` data race 已完成修复；后续改动应继续保持 `recv_resume_/write_resume_` 为空时完全回落到原回调路径。

## 1. 背景与目标

在完成了双网络模型（Reactor/Proactor）的底层重构后，目前 `io_uring`（Proactor）模式采用了 Thread-per-Core 架构，有效消除了全局锁竞争并实现了静态文件的零拷贝发送（Splice）。当前 `HttpServer` 中的业务处理链路已经实现了 Phase2 三段式异步架构（`PhaseParseAndRoute` -> `PhaseIoOperation` -> `PhaseSerializeAndSend`），包含连接级背压控制、乱序回写排序、多 worker 并发处理等机制。

**本阶段目标**：
在保证 **Reactor 模式完全不受影响** 且 **双模式均能稳定运行** 的前提下，利用 C++20 协程与 `concurrencpp` 调度器，为 Proactor 模式提供可选的协程处理路径。协程路径复用现有 Phase2 的背压/排序/生命周期基础设施，但将回调链路替换为 `co_await` 形式，降低开发者心智负担。

**前置条件（已满足）**：
- C++20 已启用（`cppBackend/CMakeLists.txt`: `CMAKE_CXX_STANDARD 20`）
- `concurrencpp v.0.1.7`（注意：仓库真实 tag 是 `v.0.1.7`，v 后含点）已通过 `FetchContent` 集成
- `HttpServer` 已持有 `concurrencpp::runtime cc_runtime_`、`works_executor_`、`blocking_executor_`，并在构造时通过 `UringServer::SetCoroutineExecutor(works_executor_)` 注入到 `UringWorker`
- `HttpServer` 已具备 `coroutine_enabled_`（默认 `false`）与 `HandleMessageCoro` 实现，并通过 `WEBSERVER_COROUTINE=1` 支持运行期开启
- **阶段一基础设施已落地**（本文档初版未记录的事实）：`IConnection` 已含 `AsUringConnection()/IsProactorMode()` 虚函数；`UringConnection` 已含 `AsyncRecvReady/AsyncWriteResponse/AsyncSendFile`、`ResumeEntry/recv_resume_/write_resume_`、`Register*/ClearAllResumeHandles`；`UringWorker` 已含 `GetCoroutineExecutor/SetCoroutineExecutor`；`HandleRecvComplete/HandleWriteComplete/HandleTlsPollCQE` 已包含协程分支；`DoClose` 已调用 `ClearAllResumeHandles`。详见 §0 与 §3.2.1。

## 2. 当前架构概览（协程化改造基线）

### 2.1 Proactor 数据流（改造前）

```
IO线程(UringWorker)                       业务线程(works_executor_)
─────────────────────                     ──────────────────────────
IoLoop: wait_cqe
  ├─ RECV CQE
  │   └─ HandleRecvComplete
  │       ├─ append→inputbuffer_
  │       ├─ NotifyMessage → HandleMessage  ──→  PostWorkTask
  │       └─ SubmitRecv (auto-resubmit)           │
  │                                               ▼
  │                                    HandleMessageInWorker
  │                                      ├─ PhaseParseAndRoute
  │                                      │   (facade_mutex 保护)
  │                                      ├─ PhaseIoOperation
  │                                      │   (open file_fd)
  │                                      └─ PhaseSerializeAndSend
  │                                           │
  │                                           ▼
  │                                    PostResultToIoLoop
  │                                      conn->PostIoTask ──→
  │                                               │
  ├─ WRITEV CQE                           ▼       │
  │   └─ HandleWriteComplete         DrainTaskQueue
  │       ├─ consumeBytes                apply_result
  │       └─ NotifySendComplete          ├─ append→outputbuffer_
  │                                      ├─ StartSendFile (如有)
  └─ SPLICE CQE                          └─ conn->send()
      └─ HandleSplicePipe2Sock
```

### 2.2 关键约束（改造必须遵守）

| 约束 | 说明 |
|------|------|
| **auto-resubmit recv** | `HandleRecvComplete` 自动调用 `SubmitRecv()`，协程需选择保留或替换此模式 |
| **背压控制** | 全局队列深度 `max_work_queue_depth_`、单连接待处理字节 `max_conn_pending_bytes_` |
| **有序回写** | `response_seq` + `last_applied_response_seq` + `pending_results` 保证 HTTP 流水线响应按序发送 |
| **HttpFacade 非线程安全** | 当前通过 `facade_mutex` 保护，协程需延续此保证 |
| **TLS 路径** | Proactor TLS 走 `POLL_ADD + TlsSession`，非 `IORING_OP_RECV/SEND` |
| **splice 零拷贝** | 文件发送走 `IORING_OP_SPLICE` 链式提交，独立于常规 writev |
| **连接超时** | `IoTimeWheel` 在 IO 线程管理连接超时 |
| **DoClose 安全** | `DoClose()` 设置 `disconnected_` 并通知 owner，需安全清理挂起协程 |

## 3. 架构设计

### 3.1 整体协作流程

1. **UringWorker (IO 线程)**：继续负责维护 `io_uring` 实例，提交 SQE 和收割 CQE。协程模式下，IO 线程额外承担**唤醒挂起协程**的职责——CQE 完成后通过 `executor->post` 将协程句柄调度到业务线程池恢复。
2. **concurrencpp works_executor_ (业务线程)**：执行 HTTP 解析、路由、业务逻辑。协程的业务段在此执行。
3. **Awaitable 对象**：为 IO 操作封装 `co_await` 接口。`await_suspend` 中将协程句柄注册到 IO 线程上下文并提交 SQE，CQE 完成时恢复协程。

### 3.2 核心组件改造

#### 3.2.1 UringConnection 的 Awaitable 扩展（阶段一：已落地）

> 实施提示：本节描述的接口**已在主干代码中实现**（见 [UringConnection.h](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.h)、[UringConnection.cpp:508-585](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L508-L585)），本节用于核对实现是否符合设计、并暴露当前存在的三处致命缺陷。不要重复新增这些接口。

当前 `UringConnection::HandleRecvComplete` 会自动向 `inputbuffer_` 追加数据并重新提交 `SubmitRecv()`。协程化的关键决策是 **保留 auto-resubmit 机制，协程从 inputbuffer_ 消费数据**，而不是让协程直接控制 recv SQE 的提交。这样：
- 避免协程忘记 resubmit 导致连接"饿死"
- 保持与 TLS 路径的兼容（TLS 下 recv 由 POLL_ADD 驱动）
- 不破坏 UringWorker 的 CQE 处理流程

已实现的 awaitable 接口（签名与下方伪代码一致，详见头文件）：

```cpp
class UringConnection : public IConnection {
public:
    // ... 现有接口不变 ...

    // ===== 协程化接口（已实现） =====
    auto AsyncRecvReady();                                              // 等待 inputbuffer_ 有可读数据
    auto AsyncWriteResponse(std::string data, bool close_after = false);  // 异步写响应
    auto AsyncSendFile(std::string header_data, int file_fd,
                       off_t offset, size_t length, bool close_after = false); // 异步 sendfile
    void RegisterRecvResumeHandle(std::coroutine_handle<> h,
                                   std::shared_ptr<concurrencpp::thread_pool_executor> exec);
    void RegisterWriteResumeHandle(std::coroutine_handle<> h,
                                    std::shared_ptr<concurrencpp::thread_pool_executor> exec);
    void ClearAllResumeHandles();  // DoClose 时调用

private:
    struct ResumeEntry {
        std::coroutine_handle<> handle;
        std::shared_ptr<concurrencpp::thread_pool_executor> executor;
    };
    std::optional<ResumeEntry> recv_resume_;    // 等待 recv 的协程
    std::optional<ResumeEntry> write_resume_;   // 等待 write 完成的协程
};
```

`AsyncWriteResponse/AsyncSendFile` 的 `await_suspend` 当前实现为"先 RegisterResumeHandle 再 PostIoTask"，具体实现与缺陷见 §3.2.1.1。

**与现有 recv 流程的衔接**（已实现，见 [UringConnection.cpp:118-158](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L118-L158)）：

`HandleRecvComplete` 在追加数据到 `inputbuffer_` 后，检查 `recv_resume_` 是否有挂起协程。若有，则通过 `executor->post` 恢复到业务线程池，**不再调用 `NotifyMessage`**（协程路径绕过原有的回调链）：

```cpp
void UringConnection::HandleRecvComplete(int result) {
    recv_submitted_ = false;
    if (disconnected_.load()) return;
    // ... 错误处理不变 ...

    inputbuffer_.append(recv_buf_.get(), static_cast<size_t>(result));
    owner_->RefreshConnTimer(SharedFromThis());

    // 协程路径：直接恢复挂起协程，不走路由消息回调
    if (recv_resume_.has_value()) {
        auto entry = std::move(recv_resume_);
        recv_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    } else {
        // 回调路径：原有逻辑
        owner_->NotifyMessage(shared_from_this());
    }

    SubmitRecv();  // 保留 auto-resubmit
}
```

##### 3.2.1.1 已知缺陷与修复方向（必须在进入阶段二前修复）

当前实现（见 [UringConnection.cpp:159-188](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L159-L188) 与 [UringConnection.cpp:528-585](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L528-L585)）存在三个缺陷，会导致 `AsyncSendFile` 静默丢失文件体、partial write 下提前恢复协程、以及跨线程 data race：

**缺陷 A：`HandleWriteComplete` 协程分支提前 return，跳过尾部驱动**（已修复）

回调路径下 `HandleWriteComplete` 在 `consumeBytes` 之后会继续判断：
```cpp
if (outputbuffer_.readableBytes() > 0) SubmitWrite();        // 续写未发完数据
else if (sendfile_.active) SubmitSpliceChain();              // 启动文件体 splice 链
else if (close_on_send_complete_) DoClose();
```
而协程分支为：
```cpp
if (write_resume_.has_value()) {
    auto entry = std::move(write_resume_);
    write_resume_.reset();
    entry->executor->post([h = entry->handle]() { h.resume(); });
    return;   // ← 提前返回，后面三段逻辑全部被跳过
}
```
后果：
- partial write（EAGAIN 后重发）下协程被恢复且 `await_resume` 返回 `success_=true`，但 `outputbuffer_` 仍有未发数据；
- `AsyncSendFile` 在 `PostIoTask` 里 `append(header)` + `StartSendFile` + `send()`，header 写完后 `sendfile_.active` 为 true 应触发 `SubmitSpliceChain`，但协程分支早 return → **文件体永远不会被发送**。

修复方向（二选一，推荐方案 1）：
1. **协程分支不早 return**：在恢复协程前，先按回调路径的尾部逻辑驱动 `SubmitWrite/SubmitSpliceChain/DoClose`；只有当 `outputbuffer_` 为空且 `sendfile_` 不再活跃且非 close 时才恢复协程（即"写真正完成"语义）。`AsyncSendFile` 的等待点必须覆盖整条 `writev + splice` 链，由 `HandleSplicePipe2Sock` 在 `sendfile_.remaining == 0` 后调用 `CompleteSendFile` 时再 `resume(write_resume_)`。
2. 区分两种 resume 槽：`write_header_resume_`（仅 header 写完恢复，用于纯 `AsyncWriteResponse`）与 `sendfile_done_resume_`（整条 splice 链完成恢复，用于 `AsyncSendFile`）。`HandleWriteComplete` 与 `HandleSplicePipe2Sock` 各自只消费对应的槽。

**缺陷 B：awaiter "先 Register 再 PostIoTask" 的注册竞态**（已修复）

`AsyncWriteResponse/AsyncSendFile` 的 `await_suspend` 顺序为：
```cpp
conn->RegisterWriteResumeHandle(h, executor);   // 业务线程写 write_resume_
conn->PostIoTask([...]{ ...; conn->send(); });  // 投递到 IO 线程
```
问题场景：上一次 `send()` 触发的 `HandleWriteComplete` 可能在新协程 `RegisterWriteResumeHandle` 之前已经执行过一次（由于 `outputbuffer_` 中残留数据被新一轮 `SubmitWrite` 触发），并因 `write_resume_` 为空走 `NotifySendComplete`；此后新协程才设置 `write_resume_`，新一轮 `send()` 完成时如果 `outputbuffer_` 已空且无 sendfile，`HandleWriteComplete` 不会再被触发，协程永久挂死。

修复方向：
1. `await_suspend` 改为"先 PostIoTask 提交 send，再用 atomic CAS 注册 handle；IO 线程在 `HandleWriteComplete` 中先 check ready，若 ready 已满足则立即 resume，否则注册到槽"的二阶段握手；
2. 或在 `await_suspend` 提交前再次检查 `outputbuffer_.readableBytes() == 0 && !sendfile_.active`，配合方案 1 中的"真正完成才恢复"语义，避免虚假挂起。

**缺陷 C：`recv_resume_/write_resume_` 跨线程无锁访问**（已修复）

- `RegisterRecvResumeHandle/RegisterWriteResumeHandle` 由**业务线程**在 `await_suspend` 调用；
- `HandleRecvComplete/HandleWriteComplete/HandleTlsPollCQE/ClearAllResumeHandles` 由 **IO 线程**调用；
- 当前 `std::optional<ResumeEntry>` 无任何同步原语保护，存在 data race（UB）。

修复方向：为 `recv_resume_/write_resume_` 增加 `std::mutex resume_mutex_`（或使用 `std::atomic<std::shared_ptr<ResumeEntry>>`）；`DoClose` 的 `ClearAllResumeHandles` 必须在持锁状态下取走句柄再投递 resume，避免与 `HandleXxxComplete` 并发 consume。锁粒度小（只保护 optional 的赋值/move），不会显著影响 IO 线程吞吐。

#### 3.2.2 TLS 路径的协程化适配

TLS 路径不走 `IORING_OP_RECV`，而是走 `IORING_OP_POLL_ADD + SSL_read/SSL_write`。协程化不改变 TLS 的驱动方式——`HandleTlsPollCQE` 仍在 IO 线程内执行 SSL 操作并追加明文到 `inputbuffer_`。协程只需等待 `inputbuffer_` 有数据即可，TLS 细节对协程透明。

> 实施提示：当前 `HandleTlsPollCQE` 在 **ktls_rx** 与 **SSL_read 循环**两个分支都检查 `recv_resume_`（见 [UringConnection.cpp:333-367](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L333-L367)），不是只在"追加明文后"一处。SSL_read 循环中恢复协程后会 `continue` 继续读取，需注意不要在同一次 CQE 中重复 resume 同一协程。

```cpp
// HandleTlsPollCQE 追加明文后，同样检查 recv_resume_
void UringConnection::HandleTlsPollCQE(short revents) {
    // ... 原有 TLS 处理逻辑（ktls_rx 分支 + SSL_read 循环分支）...

    // 追加明文数据到 inputbuffer_ 后
    if (recv_resume_.has_value() && inputbuffer_.readableBytes() > 0) {
        auto entry = std::move(recv_resume_);
        recv_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    }
}
```

#### 3.2.3 HttpServer 的协程入口

协程入口 `HandleMessageCoro` **复用** `ConnectionWorkContext` 的背压与排序基础设施，但将 Phase 回调替换为 `co_await` 链。

> 实施提示：以下伪代码已对齐主干代码的真实 API：
> - `conn->AsUringConnection()`（**不是** `GetUringConnection()`）；
> - `HttpFacade::AppendPending / ProcessPending / GetConsumedBytes / ErasePending / ClearPending`（见 [HttpFacade.cpp:21-62](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp)）；
> - `RequestContext` 字段：`message/response/err/result/keep_alive/file_fd/file_offset/file_length/next_phase/suspended`（见 [HttpServer.h:202-222](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h#L202-L222)）；
> - `HttpResponse::HasSendFile/GetSendFilePath/GetSendFileOffset/GetSendFileLength/Serialize`；
> - `pending_work_count_` 必须在协程进入/退出时显式增减，否则 §3.3.2 的 `Stop()` 等待会立即穿透；
> - `IConnection` 没有公开 `DoClose`，关闭需通过 `uring_conn->DoClose()` 或投递 `PostIoTask`。
> - 错误响应统一用 `ResponseBuilder`/`ResponseFactory` 构造后 `Serialize`，**没有** `BuildErrorResponse` 这个函数。

```cpp
concurrencpp::result<void> HttpServer::HandleMessageCoro(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx) {

    // 1) 背压统计：进入协程即增加全局在飞计数，保证 Stop().drain_cv_.wait() 真正等待
    pending_work_count_.fetch_add(1, std::memory_order_acq_rel);
    struct WorkGuard {
        HttpServer* self;
        ~WorkGuard() {
            self->pending_work_count_.fetch_sub(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lk(self->drain_mutex_);
            self->drain_cv_.notify_all();
        }
    } guard{this};

    auto conn = weak_conn.lock();
    if (!conn || conn->IsDisconnected()) co_return;

    auto* uring_conn = conn->AsUringConnection();   // 安全向下转换，非 dynamic_cast
    if (!uring_conn) co_return;

    auto req_ctx = std::make_shared<RequestContext>();

    try {
        // ── 阶段1：循环等待数据 & 解析路由（对应 PhaseParseAndRoute） ──
        while (true) {
            size_t readable = co_await uring_conn->AsyncRecvReady();
            if (readable == 0) {                    // IsDisconnected() == true
                uring_conn->DoClose();
                co_return;
            }

            // 单连接待处理字节背压（与 HandleMessage 中相同阈值）
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (ctx->queued_bytes + readable > max_conn_pending_bytes_) {
                    ctx->facade->ClearPending();
                    ctx->queued_bytes = 0;
                    ctx->queued_chunks.clear();
                    SendServiceUnavailable(conn, "connection pending data overloaded");
                    co_return;
                }
                ctx->queued_bytes += readable;
            }

            // 把 inputbuffer_ 当前可读数据搬进 facade（与 PhaseParseAndRoute 同源）
            BufferBlock& in = conn->getInputBuffer();
            std::string chunk = in.bufferToString();
            in.consumeBytes(chunk.size());

            {
                std::lock_guard<std::mutex> facade_lock(ctx->facade_mutex);
                if (!chunk.empty()) ctx->facade->AppendPending(std::move(chunk));
                req_ctx->result = ctx->facade->ProcessPending(
                    req_ctx->message, req_ctx->response, req_ctx->err);
                if (req_ctx->result == HttpServerResult::SUCCESS && req_ctx->message) {
                    size_t consumed = ctx->facade->GetConsumedBytes();
                    ctx->facade->ErasePending(consumed);
                }
            }
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->queued_bytes -= std::min(ctx->queued_bytes, chunk.size());
            }

            if (req_ctx->result == HttpServerResult::NEED_MORE_DATA) {
                continue;                          // 等下一轮 recv
            }
            break;                                 // SUCCESS / 失败都跳出
        }

        if (req_ctx->result != HttpServerResult::SUCCESS || !req_ctx->message) {
            // 错误响应：用 ResponseBuilder 构造后再 Serialize
            HttpResponse err_resp = BuildErrorHttpResponse(req_ctx);   // 复用 ResponseBuilder
            co_await uring_conn->AsyncWriteResponse(err_resp.Serialize(), true);
            co_return;
        }

        // keep_alive / file_fd 解析（与 PhaseParseAndRoute 同源）
        auto* request = dynamic_cast<HttpRequest*>(req_ctx->message.get());
        req_ctx->keep_alive = (request->GetVersion() == HttpVersion::HTTP_1_1);
        // ... Connection 头、路由回调填充 req_ctx->response ...

        // ── 阶段2：IO 操作（对应 PhaseIoOperation） ──
        if (req_ctx->response.HasSendFile()) {
            int file_fd = ::open(req_ctx->response.GetSendFilePath().c_str(),
                                 O_RDONLY | O_CLOEXEC);
            if (file_fd < 0) {
                BuildNotFoundResponse(req_ctx->response);   // 转为 404，落到普通响应路径
            } else {
                req_ctx->file_fd = file_fd;
                req_ctx->file_offset = static_cast<off_t>(req_ctx->response.GetSendFileOffset());
                req_ctx->file_length = req_ctx->response.GetSendFileLength();
            }
        }

        // ── 阶段3：序列化与异步发送（对应 PhaseSerializeAndSend + PostResultToIoLoop） ──
        std::string response_data = req_ctx->response.Serialize();
        bool close_after = !req_ctx->keep_alive;

        if (req_ctx->file_fd >= 0) {
            // sendfile 路径：响应头 writev + 文件体 splice 零拷贝
            // 注意：依赖 §3.2.1.1 缺陷 A 修复后 AsyncSendFile 才会真正等待 splice 链完成
            co_await uring_conn->AsyncSendFile(
                std::move(response_data), req_ctx->file_fd,
                req_ctx->file_offset, req_ctx->file_length, close_after);
            req_ctx->file_fd = -1;   // fd 所有权转移给 sendfile 链
        } else {
            co_await uring_conn->AsyncWriteResponse(
                std::move(response_data), close_after);
        }

    } catch (const std::exception& e) {
        LOGERROR(std::string("HandleMessageCoro exception: ") + e.what());
        if (req_ctx && req_ctx->file_fd >= 0) ::close(req_ctx->file_fd);
        if (!conn->IsDisconnected()) {
            conn->setCloseOnSendComplete(true);
        }
    }
}
```

关键约束：
- **`pending_work_count_` 必须管理**：`WorkGuard` RAII 在协程任意出口（含异常）减计数，保证 `Stop()` 不会穿透；
- **`result` 引用必须被持有**：调用方 `HandleMessage` 必须把 `HandleMessageCoro(...)` 返回的 `concurrencpp::result<void>` 保存到 `ctx`（如 `ctx->pending_coro_results` 队列），否则协程内未捕获异常会触发 `concurrencpp::result_broken_promise`，且停机时无法 wait；
- **`facade_mutex` 不可省**：与现有回调路径一致，保留以兼容未来并发协程复用同一 `ctx->facade`；
- **有序回写**：第一阶段串行协程下不需要 `response_seq`；如需切换为并发协程，必须改为 `PostResultToIoLoop(weak_conn, ctx, WorkResult{...})` 走原有 `last_applied_response_seq` 排序，**不要**在协程内直接 `AsyncWriteResponse`。

#### 3.2.4 HttpServer 中协程/回调路径的分流

**不用 `dynamic_cast`**，已是 `IConnection` 的轻量级虚函数（**已落地**，无需再新增）：

```cpp
// IConnection.h（已存在，不要重复声明）
class IConnection : public std::enable_shared_from_this<IConnection> {
public:
    virtual class UringConnection* AsUringConnection() { return nullptr; }
    virtual bool IsProactorMode() const { return false; }
};

// UringConnection.h（已 override）
UringConnection* AsUringConnection() override { return this; }
bool IsProactorMode() const override { return true; }
```

`HttpServer::HandleMessage` 分流逻辑（**已落地**）：

```cpp
void HttpServer::HandleMessage(spIConnection conn) {
    if (coroutine_enabled_ && conn->IsProactorMode()) {
        auto ctx = GetOrCreateWorkContext(conn);
        if (ctx->coroutine_running) return;  // 单连接单协程主循环

        ctx->coroutine_running = true;
        auto weak_conn = std::weak_ptr<IConnection>(conn);
        PostWorkTask([this, weak_conn, ctx]() mutable {
            auto result = HandleMessageCoro(std::move(weak_conn), ctx);
            ctx->pending_coro_results.emplace_back(std::move(result));
        });
        return;
    }
    // 回调路径：原有 HandleMessage 逻辑不变
    // ...
}
```

要点：
- `result` 已保存到 `ctx->pending_coro_results`；停机时 `HttpServer::Stop()` 会在 `drain_cv_.wait()` 之后遍历所有 ctx 并 `get()` 收集异常，再 shutdown executor；
- 入口必须显式投到 `works_executor_`，**不要**让 `HandleMessage` 在 IO 线程上直接 `co_await`，以免 `UringWorker` 的 `IoLoop` 被协程挂起阻塞 CQE 收割；
- `coroutine_enabled_` 已支持通过 `WEBSERVER_COROUTINE=1` 显式打开；默认仍保持关闭，确保 Reactor/回调路径零回归。

### 3.3 连接生命周期与协程安全

#### 3.3.1 协程挂死防护

当连接异常断开（`DoClose`），必须确保挂起的协程被安全恢复并退出，而非永久挂起。

> 实施提示：当前 `DoClose` 已调用 `ClearAllResumeHandles`，但**没有锁保护** `recv_resume_/write_resume_`，与 §3.2.1.1 缺陷 C 同源，必须配合修复。

```cpp
void UringConnection::DoClose() {
    if (disconnected_.exchange(true)) return;

    // 1. 清理所有挂起的协程恢复句柄
    ClearAllResumeHandles();

    // 2. 原有关闭逻辑
    const int closing_fd = fd_;
    // ... shutdown + close + NotifyClose ...
}

void UringConnection::ClearAllResumeHandles() {
    // ⚠ 必须在 resume_mutex_ 持锁状态下访问，否则与业务线程的
    //    RegisterRecvResumeHandle/RegisterWriteResumeHandle 及 IO 线程的
    //    HandleXxxComplete 形成数据竞争（见 §3.2.1.1 缺陷 C）
    std::lock_guard<std::mutex> lk(resume_mutex_);

    // 恢复 recv 等待协程（传回 0 字节让它自行退出）
    if (recv_resume_.has_value()) {
        auto entry = std::move(recv_resume_);
        recv_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    }
    // 恢复 write 等待协程
    if (write_resume_.has_value()) {
        auto entry = std::move(write_resume_);
        write_resume_.reset();
        entry->executor->post([h = entry->handle]() { h.resume(); });
    }
}
```

`resume_mutex_` 必须新增为 `UringConnection` 成员，且 `RegisterRecvResumeHandle/RegisterWriteResumeHandle` 与所有 `HandleRecvComplete/HandleWriteComplete/HandleTlsPollCQE/HandleSplicePipe2Sock` 中读 `recv_resume_/write_resume_` 的位置都要持同一锁。

#### 3.3.2 服务停机时的协程清理

`HttpServer::Stop()` 需在关闭 `works_executor_` 前等待所有在飞协程完成。

> 实施提示：当前 `Stop()` 已经有 `pending_work_count_ == 0` 的等待逻辑（见 [HttpServer.cpp:173-180](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L173-L180)），但**回调路径的 `pending_work_count_` 增减点在 `PostWorkTask` 内部**（[HttpServer.cpp:102-136](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L102-L136)）。协程路径如果走 §3.2.3 的 `WorkGuard` RAII，`pending_work_count_` 才会被正确增减；否则 `Stop()` 的 `drain_cv_.wait()` 会立即穿透，导致在飞协程被 `works_executor_->shutdown()` 强制中止，触发 `concurrencpp::runtime_shutdown_exception` 落到未捕获 handler。

```cpp
void HttpServer::Stop() {
    // 1. 停止接受新连接（UringServer::Stop 会让 worker 退出 IoLoop）
    if (net_server_) net_server_->Stop();

    // 2. 等待在飞协程/回调任务完成
    //    - 回调路径：PostWorkTask 已 fetch_add/fetch_sub
    //    - 协程路径：HandleMessageCoro 的 WorkGuard 负责增减
    //    两者共用同一个 pending_work_count_，等待逻辑天然兼容
    {
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drain_cv_.wait(lock, [this] {
            return pending_work_count_.load(std::memory_order_acquire) == 0;
        });
    }

    // 3. 收集所有 ctx 中残留的 concurrencpp::result<void>，避免 broken_promise
    //    （需新增 ConnectionWorkContext::pending_coro_results 字段，见 §3.2.4）
    for (auto& weak_ctx : tracked_ctxs_) {
        if (auto ctx = weak_ctx.lock()) {
            for (auto& r : ctx->pending_coro_results) {
                if (r.get_status() == concurrencpp::result_status::idle) continue;
                try { r.get(); } catch (const std::exception& e) {
                    LOGERROR(std::string("coro result on stop: ") + e.what());
                }
            }
            ctx->pending_coro_results.clear();
        }
    }

    // 4. 关闭 executor（后续 resume 会抛 concurrencpp::runtime_shutdown_exception）
    if (blocking_executor_) blocking_executor_->shutdown();
    if (works_executor_) works_executor_->shutdown();
    cc_runtime_.reset();

    // ... 后续清理（旧 ThreadPool、SqlConnPool 等） ...
}
```

### 3.4 有序回写保证

当前 Phase2 通过 `response_seq` + `last_applied_response_seq` + `pending_results`（见 [HttpServer.cpp:727-825](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L727-L825)）保证 HTTP 流水线中响应按序发送。协程路径需延续此保证。

**方案**：每个连接的协程处理串行执行（同一连接同时只有一个协程在跑），通过 `ConnectionWorkContext::worker_running` 控制入口。串行模式下天然有序，不需要 `response_seq`。

**性能权衡（必须在 PR 描述中说明）**：现有回调路径在同一连接上允许 `max_concurrent_workers_per_conn_{4}` 个 worker 并发处理 pipeline 中的不同请求——一个 worker 在 `PostResultToIoLoop` 把响应投回 IO 线程后，另一个 worker 可以立刻开始解析下一个请求，由 `response_seq + last_applied_response_seq + pending_results` 保证乱序回写。串行协程模式下，协程必须 `co_await AsyncWriteResponse/AsyncSendFile` 等待整条写链完成才能继续 `AsyncRecvReady` 消费下一个请求，会**牺牲 HTTP/1.1 pipelining 的延迟隐藏能力**，对应吞吐会下降。对单连接顺序请求（无 pipeline）无影响。

推荐策略：
- 第一阶段默认走串行协程，但 `coroutine_enabled_` 仅在显式开启时生效；
- 若未来需要恢复并发能力，应直接让协程在 `PhaseSerializeAndSend` 阶段构造 `WorkResult` 并调用 `PostResultToIoLoop(weak_conn, ctx, std::move(result))`，沿用现有 `last_applied_response_seq` 排序机制。

### 3.5 HttpFacade 线程安全

`HttpFacade` 内部持有 `parser_` 等可变状态，非线程安全。当前回调路径通过 `ConnectionWorkContext::facade_mutex` 保护（见 [HttpServer.cpp:411-451](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L411-L451) 与 [HttpServer.cpp:625-636](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L625-L636)）。

协程路径建议**直接复用 `facade_mutex`**，无论串行还是并发：
- 串行协程模式下 mutex 无竞争，开销可忽略；
- 一旦后续切到并发协程，`facade_mutex` 已就位，无需改造；
- 移除 mutex 反而要承担"未来切并发时再次引入 race"的风险，得不偿失。

`ctx->facade` 与连接同生命周期，由 `HandleNewConnection` 创建并 `SetContext`（[HttpServer.cpp:213-229](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L213-L229)），协程取 `ctx` 时直接复用，**不要**在协程内新建 `HttpFacade`。

### 3.6 兼容性与隔离保障

- **接口隔离**：`IConnection` 的 `AsUringConnection()` 和 `IsProactorMode()` 两个虚函数**已落地**（默认实现返回 `nullptr` / `false`，`UringConnection` override 返回 `this` / `true`），不使用 `dynamic_cast`。Reactor 模式下的 `Connection` 无需任何改动。
- **配置开关**：`HttpServer` 已有 `coroutine_enabled_`（默认 `false`）。只有当 `mode=proactor` 且 `coroutine_enabled=true` 时启用协程路径，否则走原有回调链路；当前已支持通过 `WEBSERVER_COROUTINE=1` 在构造时显式开启。
- **零侵入回调路径**：协程相关代码仅在 `recv_resume_.has_value()` 为 true 时走入新分支，否则完全保持原有行为。该不变量在 §3.2.1.1 修复后仍须保持——任何对 `HandleRecvComplete/HandleWriteComplete/HandleTlsPollCQE/HandleSplicePipe2Sock` 的修改都不得在 `recv_resume_/write_resume_` 为空时改变回调路径语义。

## 4. 实施步骤

### 阶段一：Awaitable 基础设施 + UringConnection 扩展（**已落地**）

1. **IConnection 接口扩展**：新增 `AsUringConnection()` 和 `IsProactorMode()` 虚函数。 ✅ 已落地（[IConnection.h](file:///home/zsy/WebServer/cppBackend/net/IConnection.h)、[UringConnection.h](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.h)）。
2. **UringConnection Awaitable**： ✅ 已落地（[UringConnection.h](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.h)、[UringConnection.cpp:508-585](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L508-L585)）。
   - ✅ `ResumeEntry` 成员和 `RegisterRecvResumeHandle` / `RegisterWriteResumeHandle` / `ClearAllResumeHandles`。
   - ✅ `AsyncRecvReady` / `AsyncWriteResponse` / `AsyncSendFile` Awaitable。
   - ✅ §3.2.1.1 缺陷 A/B/C 已修复。
3. **UringWorker CQE 改造**： ✅ 已落地。
   - ✅ `HandleRecvComplete` 检查 `recv_resume_`（[UringConnection.cpp:147-153](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L147-L153)）。
   - ✅ `HandleWriteComplete` 已按"真正写完"语义恢复 `write_resume_`，不会再因提前 return 跳过尾部驱动。
   - ✅ `HandleTlsPollCQE` 在 ktls_rx 与 SSL_read 循环两处都检查 `recv_resume_`（[UringConnection.cpp:333-367](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L333-L367)）。
   - ✅ `DoClose` 调用 `ClearAllResumeHandles()`（[UringConnection.cpp:465](file:///home/zsy/WebServer/cppBackend/proactor/UringConnection.cpp#L465)）。
4. **UringWorker executor 传递**： ✅ 已落地 `GetCoroutineExecutor()` / `SetCoroutineExecutor()`（[UringWorker.h:91-92](file:///home/zsy/WebServer/cppBackend/proactor/UringWorker.h#L91-L92)、[UringWorker.cpp:735-742](file:///home/zsy/WebServer/cppBackend/proactor/UringWorker.cpp#L735-L742)），`HttpServer` 构造时已注入（[HttpServer.cpp:82](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L82)）。

**阶段一补丁清单（已完成）**：
- 已修复 §3.2.1.1 缺陷 A：`HandleWriteComplete` 协程分支不再提前 return，`AsyncSendFile` 会等待完整 `writev + splice` 链；
- 已修复 §3.2.1.1 缺陷 B：`AsyncWriteResponse/AsyncSendFile` 的 `await_suspend` 采用"先提交 send 再注册 handle"并辅以完成态二次检查，避免注册竞态挂死；
- 已修复 §3.2.1.1 缺陷 C：`resume_mutex_` 已覆盖 `Register*/Clear*` 与所有 `Handle*` 中访问 `recv_resume_/write_resume_` 的位置。

### 阶段二：HttpServer 协程路径集成（**已落地**）

1. **`HandleMessageCoro` 已实现**：复用 `PhaseParseAndRoute/PhaseIoOperation`，用 `co_await AsyncRecvReady/AsyncWriteResponse/AsyncSendFile` 串起 Proactor 协程主循环。
2. **`HandleMessage` 已完成分流**：当 `coroutine_enabled_ && conn->IsProactorMode()` 时，协程入口通过 `PostWorkTask` 投递到 `works_executor_`，避免 IO 线程直接挂起。
3. **上下文字段已补齐**：`ConnectionWorkContext` 已新增 `coroutine_running` 与 `pending_coro_results`，保证单连接只启动一个协程主循环并在停机时持有结果对象。
4. **`tracked_ctxs_` 已接入**：新连接创建上下文时会登记 `tracked_ctxs_`，供 `Stop()` 统一遍历收集协程结果。
5. **背压已接入协程路径**：`HandleMessage` 入口继续检查 `max_work_queue_depth_`；协程内部复用 `ctx->queued_bytes/max_conn_pending_bytes_`；`pending_work_count_` 由 `WorkGuard` RAII 维护。
6. **运行期开关已落地**：`WEBSERVER_COROUTINE=1` 会在 `HttpServer` 构造时打开协程路径；默认仍为关闭。
7. **剩余非阻塞优化**：`PhaseIoOperation` 中同步 `::open` 仍沿用现有实现，若后续要进一步隔离阻塞文件打开，可把该步骤迁移到 `blocking_executor_`，但不影响阶段二完成态。

### 阶段三：停机安全与边界场景（**已落地**）

1. **`DoClose` 协程清理**：§3.3.1 已落地，`ClearAllResumeHandles` 在 `resume_mutex_` 保护下回收挂起的 recv/write awaiter，断链时不会遗留永久挂起协程。
2. **`Stop` 等待**：§3.3.2 已落地，`pending_work_count_`、`pending_coro_results` 与 `tracked_ctxs_` 已形成完整停机收敛链路，`Stop()` 会等待在飞协程结束并逐个 `get()` 收集异常。
3. **超时交互**：`IoTimeWheel` 触发超时时调用 `DoClose`，`DoClose` 内清理协程句柄，协程通过 `IsDisconnected()` 检测后退出；`IoTimeWheel` 与 `ClearAllResumeHandles` 同属 IO 线程路径，且共享 `resume_mutex_` 保护。
4. **SQE 耗尽**：`UringWorker::SubmitWrite` 等在 SQE 不足时已有 `PostIoTask` 重试逻辑，与协程恢复不冲突；协程恢复走 `executor->post`，不依赖额外 SQE。
5. **TLS 写边界**：`HandleTlsPollCQE` 已补齐 write awaiter 恢复逻辑；对非 KTLS 发送文件场景，文件体会分块搬运到 TLS 输出缓冲区，避免“header 发完后协程或回调链挂死”。
6. **协程 cancel 语义**：`concurrencpp` 不支持取消已运行协程，停机时仍以自然退出为主；所有 awaiter 在 `IsDisconnected()` 时会快速返回，避免协程卡死在 `co_await` 上。

### 阶段四：测试与验证（**已落地，当前范围为 HTTP**）

1. **正确性验证**：已由 `io_uring_coroutine_phase4_tests.cpp` 覆盖，比较 Proactor 回调路径与协程路径在静态资源请求下的状态码、Header、响应体一致性。
2. **HTTP 行为验证**：已覆盖 GET / HEAD / 404 / Range 等 HTTP 行为，作为当前仓库未纳入 SSL 集成前的第 2 条替代验证项。
3. **sendfile 路径验证**：已覆盖下载路由下的整包文件校验与大文件 Range 校验，验证 sendfile 路径在当前 HTTP 场景下可正确返回文件内容。
4. **压力测试**：已覆盖回调路径与协程路径的轻量并发压力回归，并输出成功数与耗时，作为后续更大规模压测的基线。
5. **异常场景测试**：已覆盖半包断链、下载中途断开、非法 Range 后服务恢复等 HTTP 异常路径。
6. **双模式回归**：已覆盖 `reactor` / `proactor` / 未知模式回退 `reactor` 三种启动形态，验证关闭协程开关或切换后端后功能一致。
7. **停机测试**：已覆盖 HTTP 请求在飞时触发 `Stop()` 的进程退出验证，确认停机路径不会长期悬挂。

## 5. 风险与对策

| 风险 | 严重度 | 对策 |
|------|--------|------|
| **HandleWriteComplete 协程分支提前 return（缺陷 A）** | **致命** | `AsyncSendFile` 文件体静默丢失。修复：协程分支不早 return，按回调路径驱动 `SubmitWrite/SubmitSpliceChain`，仅"真正写完"才 resume；`AsyncSendFile` 等待点覆盖整条 splice 链，由 `HandleSplicePipe2Sock` 在 `remaining==0` 后 resume。见 §3.2.1.1 缺陷 A。 |
| **awaiter 注册竞态挂死（缺陷 B）** | 高 | `await_suspend` "先 Register 再 PostIoTask"可能永久挂起。修复：改为"先 PostIoTask 提交 send 再 CAS 注册 handle"二阶段握手，或提交前二次检查 ready。见 §3.2.1.1 缺陷 B。 |
| **`recv_resume_/write_resume_` 跨线程无锁（缺陷 C）** | 高 | Register 在业务线程、Consume/Clear 在 IO 线程，无锁 data race（UB）。修复：新增 `resume_mutex_` 覆盖所有访问点。见 §3.2.1.1 缺陷 C、§3.3.1。 |
| **concurrencpp::result 未持有导致 broken_promise** | 高 | `HandleMessageCoro` 返回的 result 必须保存到 `ctx->pending_coro_results`；停机时遍历 `get()` 收集异常。见 §3.2.4、§3.3.2。 |
| **Stop 等待穿透** | 高 | 协程路径必须在 `HandleMessageCoro` 入口 `pending_work_count_.fetch_add`，出口 RAII `fetch_sub + notify`，否则 `drain_cv_.wait()` 立即返回。见 §3.2.3 WorkGuard、§3.3.2。 |
| **串行协程致 pipelining 吞吐退化** | 中 | 串行协程牺牲 `max_concurrent_workers_per_conn_{4}` 的并发能力，HTTP/1.1 pipeline 吞吐下降。对策：默认关闭协程路径，仅在显式开启时生效；未来切并发协程改走 `PostResultToIoLoop` 排序。见 §3.4。 |
| **协程挂死/内存泄漏** | 高 | `DoClose` 中 `ClearAllResumeHandles()`（持 `resume_mutex_`）强制恢复；`pending_work_count_` 跟踪在飞协程；停机时 `drain_cv_.wait` + `pending_coro_results.get()` |
| **线程切换开销** | 中 | 单连接串行协程可避免不必要的 `resume_on`；小请求可在 IO 线程直接处理（`await_ready` 返回 true 跳过挂起）；后续可评估 IO 线程本地 executor |
| **协程与 auto-resubmit 冲突** | 中 | 保留 auto-resubmit，协程仅消费 `inputbuffer_`；若协程未及时消费，数据堆积在 `inputbuffer_` 触发背压 |
| **HttpFacade 状态串扰** | 高 | 协程路径复用 `facade_mutex` 保护（与回调路径一致），不做省略 |
| **有序回写破坏** | 高 | 第一阶段串行协程保证顺序；并发协程必须改走 `PostResultToIoLoop` 的 `last_applied_response_seq` 排序机制 |
| **TLS 路径与协程不兼容** | 低 | TLS 仍在 IO 线程驱动，追加明文到 `inputbuffer_` 后触发 `recv_resume_`，对协程透明；注意 SSL_read 循环中恢复协程后 `continue` 不要重复 resume |
| **splice 完成回调丢失** | 中 | 缺陷 A 修复后，`HandleSplicePipe2Sock` 在 `remaining==0` 时 `CompleteSendFile` 并 resume `write_resume_`；与 `HandleWriteComplete` 配合 |
| **concurrencpp 版本兼容** | 低 | 当前 `v.0.1.7`（注意 tag 写法）已验证可用；`thread_pool_executor` 为稳定 API；`resume_on` 未实际使用（当前走 `executor->post`） |

## 6. 与现有代码的映射关系

| 设计概念 | 代码位置 | 状态 |
|----------|----------|------|
| `AsyncRecvReady` | `UringConnection.h` / `UringConnection.cpp` | ✅ 已实现 |
| `AsyncWriteResponse` | `UringConnection.h` / `UringConnection.cpp` | ✅ 已实现 |
| `AsyncSendFile` | `UringConnection.h` / `UringConnection.cpp` | ✅ 已实现 |
| `recv_resume_` / `write_resume_` | `UringConnection.h` 私有成员 | ✅ 已实现 |
| `RegisterRecvResumeHandle` / `RegisterWriteResumeHandle` | `UringConnection.cpp` | ✅ 已实现 |
| `ClearAllResumeHandles` | `UringConnection.cpp` | ✅ 已实现 |
| `HandleRecvComplete` 协程分支 | `UringConnection.cpp:147-153` | ✅ 已实现 |
| `HandleWriteComplete` 协程分支 | `UringConnection.cpp` | ✅ 已修复，按"真正写完"语义恢复协程 |
| `HandleTlsPollCQE` 协程分支 | `UringConnection.cpp` | ✅ 已实现（ktls_rx / SSL_read 收包、TLS 写完成恢复、非 KTLS sendfile 分块搬运） |
| `HandleSplicePipe2Sock` 协程分支 | `UringConnection.cpp` | ✅ 已实现，sendfile 完成时恢复协程 |
| `DoClose` 协程清理 | `UringConnection.cpp:465` | ✅ 已调用 `ClearAllResumeHandles` |
| `AsUringConnection` / `IsProactorMode` | `IConnection.h` / `UringConnection.h` | ✅ 已实现 |
| `coroutine_enabled_` | `HttpServer.h` / `HttpServer.cpp` | ✅ 已有，支持 `WEBSERVER_COROUTINE=1` 开启 |
| `works_executor_` / `cc_runtime_` / `blocking_executor_` | `HttpServer.h:103-105` | ✅ 已有 |
| `GetCoroutineExecutor` / `SetCoroutineExecutor` | `UringWorker.h:91-92` / `UringWorker.cpp:735-742` | ✅ 已实现 |
| `HandleMessageCoro` | `HttpServer.h` / `HttpServer.cpp` | ✅ 已实现 |
| `HandleMessage` 分流 | `HttpServer.cpp` | ✅ 已实现 |
| `ConnectionWorkContext::pending_coro_results` | `HttpServer.h` ConnectionWorkContext | ✅ 已新增 |
| `HttpServer::tracked_ctxs_` | `HttpServer.h` | ✅ 已新增 |
| `UringConnection::resume_mutex_` | `UringConnection.h` | ✅ 已新增并用于跨线程同步 |
