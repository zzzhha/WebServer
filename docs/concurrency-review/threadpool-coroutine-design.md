# WORKS 线程池替换与协程预留设计

## 一、背景

### 1.1 当前现状

当前项目的 `WORKS` 线程池由 `HttpServer` 持有，核心入口位于：

- `HandleMessage` 中将请求处理投递到工作线程
- `HandleMessageInWorker` 中进行分段处理与链式续投
- `OnWorkerExit` 中在队列未清空时继续补投 worker

现有实现使用自研 `ThreadPool`，代码见：

- [ThreadPool.h](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h)
- [ThreadPool.cpp](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.cpp)
- [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)
- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)

当前 `ThreadPool` 已经从早期全局单队列升级为 `per-worker deque + inject_queue + work-stealing`，在吞吐和锁竞争上明显优于旧版本，因此**本次改造的主要目标不是“紧急性能止血”**，而是：

1. 用成熟 executor 替换 `HttpServer` 的自研工作线程池实现，降低维护成本。
2. 保持当前三段式请求处理与 Reactor 回写模型不变。
3. 为后续 RPC 引入后的协程化处理预留执行器和调度基础。

### 1.2 当前阶段为什么协程收益有限

当前 `WORKS` 线程中的耗时任务，仍以以下同步处理为主：

- HTTP 解析、路由与响应构造
- 文件 `open` 等同步调用
- 同步 MySQL / 同步 RPC stub 也仍然会阻塞线程

因此在**本阶段只替换 `WORKS` 线程池**时，`concurrencpp` 带来的直接收益主要是：

- 更成熟的任务调度与关闭语义
- 更统一的 executor / coroutine 编程模型
- 为未来 `co_await rpc_call()` 提前铺好底座

而不是立刻获得显著的“线程不阻塞”收益。  
只有当后续真的接入**可挂起的异步 RPC / 异步 I/O awaitable** 时，协程模型的优势才会明显放大。

对同步 MySQL，本阶段采用**阶段性治理**而不是一次性协程化：

- **方案 A**：在 `HttpServer` 内允许引入一个仅服务阻塞子任务的临时 `blocking executor`，用于承接同步 MySQL、`open`、密码哈希等会长时间占住 worker 的步骤。
- **方案 B**：把 `SqlConnPool::GetConn()` 从无限期 `sem_wait()` 调整为**限时等待/快速失败**，避免 `WORKS` 线程被连接池耗尽永久挂住。

这两个方案的目标是：先把最明显的同步阻塞风险收敛住，同时不把本阶段扩大成“数据库全面异步化”项目。

### 1.3 本文档的范围收缩

本文档**只讨论 `HttpServer` 的 `WORKS` 线程池替换**，不再把以下内容并入本阶段：

- `TcpServer` 的 `IO` 线程池替换
- `INetServer` / `NetFactory` 接口扩展
- `ChunkedDownloadManager` 下载线程池替换
- “全进程单一 runtime，统一管理所有 executor”的大一统设计

以上内容后续可单独立项，不与本次 `WORKS` 替换耦合。

---

## 二、目标与非目标

## 2.1 设计目标

1. **只替换 `WORKS` 线程池**：`HttpServer` 不再依赖自研 `ThreadPool` 处理业务任务，改为使用 `concurrencpp` executor。
2. **保持行为不变**：连接级上下文、三段式处理、回 IO 线程发送、响应保序、背压策略保持现有语义。
3. **先替换 executor，再谈协程化**：第一阶段以普通 `post`/`submit` 为主，不强行把所有业务改成协程。
4. **为未来 RPC 预留协程入口**：后续若接入异步 RPC client，可在 `WORKS` executor 内逐步把回调链改为 `co_await` 风格。
5. **可回退**：若集成风险或性能不达预期，可切回旧 `ThreadPool`。

## 2.2 非目标

1. 不移除当前 `ThreadPool` 类本身。
2. 不修改 `TcpServer` 的 `subloop` 启动方式。
3. 不修改 `ChunkedDownloadManager` 的线程模型。
4. 不在本阶段引入 `io_uring awaiter`、异步文件 I/O awaiter 或协程化 MySQL。
5. 不承诺“仅替换线程池就一定获得显著吞吐提升”。

---

## 三、为什么选择 concurrencpp

## 3.1 选择理由

相对继续维护自研 `ThreadPool`，`concurrencpp` 的价值在于：

- 提供成熟的 executor 抽象，减少线程池调度细节维护成本。
- 原生支持 `post`、`submit`、`result<T>`、`resume_on` 等后续协程能力。
- 生命周期与关闭语义更明确，便于后续扩展到协程任务。
- 后续如果接入异步 RPC，可直接在现有 executor 上演进，而不用二次推翻线程池接口。

## 3.2 本阶段对 concurrencpp 的使用原则

本阶段采用“**最小接入**”原则：

- 仅在 `HttpServer` 内持有 `concurrencpp::runtime`
- 默认创建 `WORKS` 对应 executor
- 若启用同步 MySQL 的阶段性治理，可在 `HttpServer` 内额外创建一个**不向外扩散**的临时 `blocking executor`
- 当前仍以普通任务投递为主
- 协程接口只做预留，不要求第一阶段全部落地

这意味着我们使用 concurrencpp 的目的不是“全面协程化”，而是“先替换调度底座，再逐步引入协程”。

---

## 四、总体架构

本阶段改造后的结构如下：

```text
Reactor(IO 线程)
  EventLoop / TcpServer / Connection
        |
        | HandleMessage
        v
HttpServer
  |- pending_work_count_
  |- cc_runtime_
  |- works_executor_
  |- blocking_executor_(optional)
  `- PostWorkTask(...)
        |
        | works_executor_->post(...)
        v
WORKS 执行器
  |- HandleMessageInWorker(...)
  |- ProcessSingleRequest(...)
  `- 未来可演进为 HandleMessageCoro(...)
        |
        | 遇到同步 MySQL / open / 哈希等阻塞步骤时
        v
BlockingExecutor(可选)
  `- PostBlockingTask(...)
        |
        | PostResultToIoLoop(...)
        v
Reactor(IO 线程)
  outputbuffer.append(...)
  conn->send()
```

关键边界：

- `Reactor` 层保持不变，仍负责收包、连接管理和回写。
- `WORKS` executor 只承担业务处理任务。
- 若启用 `blocking executor`，它只承接同步阻塞子步骤，不承担 `Reactor` loop，也不向下载模块扩散。
- `Worker -> IO` 的回切仍通过现有 `PostResultToIoLoop` / `PostIoTask` 完成。
- 同一连接的响应保序逻辑不改，仍由 `pending_results + response_seq` 控制。

---

## 五、执行器分层建议

为避免把 `Reactor`、业务任务、下载任务三类完全不同的执行语义混到同一个线程池中，建议后续演进采用“三层执行器分治”：

1. **Reactor IO 专用线程组**
2. **WORKS 业务执行器**
3. **Download 独立共享执行器**

三层结构如下：

```text
┌──────────────────────────────────────────────┐
│ Reactor IO 专用线程组                        │
│ - mainloop / subloop                         │
│ - 专属线程长期运行 EventLoop::run()          │
│ - 不承担业务协程、不承担下载任务             │
└──────────────────────────────────────────────┘
                    |
                    v
┌──────────────────────────────────────────────┐
│ WORKS 业务执行器                             │
│ - HttpServer 业务任务                        │
│ - 当前以 post/submit 为主                    │
│ - 后续承接 RPC 协程 / result<T> / resume_on   │
└──────────────────────────────────────────────┘
                    |
                    v
┌──────────────────────────────────────────────┐
│ Download 独立共享执行器                      │
│ - 分块下载 / 文件写入 / 合并 / 校验           │
│ - 独立并发控制与取消语义                     │
│ - 不与 Reactor IO / WORKS 混用               │
└──────────────────────────────────────────────┘
```

### 5.1 为什么要分层

三类任务的运行特征完全不同：

- `Reactor IO`：长生命周期、强线程归属、事件循环驱动，典型任务是 `EventLoop::run()`。
- `WORKS`：短到中等生命周期的业务任务，后续可能演进为可挂起的协程任务。
- `Download`：吞吐型阻塞 I/O 任务，包含外部 HTTP、磁盘写入、合并与校验，不适合与请求业务竞争同一执行器。

若继续把它们放在同一种线程池语义里，会出现以下问题：

- `Reactor` 的 loop 线程被误当成普通任务线程管理。
- `WORKS` executor 被下载任务长期占住，影响请求处理延迟。
- 下载的取消、暂停、恢复语义与业务请求的 shutdown 语义耦合。

因此，后续线程池演进应优先做“职责拆分”，而不是“统一替换”。

### 5.2 Reactor IO 专用线程组建议

对于 `Reactor` 模型，建议把当前 `TcpServer` 的 IO 线程池重构为**专用 loop thread group**，而不是切到 `concurrencpp`。

原因：

- `EventLoop::run()` 是长驻循环，不是普通短任务。
- `Channel` / `Connection` / `queueinloop()` 强依赖线程归属，不适合可迁移恢复点。
- `Reactor` 本身是 callback-first 模型，底层 loop 层协程化收益很小，复杂度很高。

因此建议：

- 可以移除当前 `IO` 线程池中的 work-stealing。
- 可以把 `TcpServer` 中承载 `subloop` 的线程池改名/重构为 `LoopThreadGroup`、`ReactorLoopPool` 等更明确的类型。
- 但不建议让 `EventLoop::run()` 运行在 `concurrencpp` 的通用 executor 上。

换句话说，`Reactor` 这一层更适合“**去线程池化，改为专用 loop thread group**”，而不是“协程化线程池”。

### 5.3 WORKS 业务执行器建议

`WORKS` 层是最适合接入 `concurrencpp` 的部分：

- 当前已经有清晰的 `IO收包 -> Worker处理 -> IO回写` 三段式边界。
- `HttpServer::HandleMessageInWorker`、`ProcessSingleRequest`、`OnWorkerExit` 都属于业务处理语义。
- 后续引入 RPC 后，`WORKS` 能自然承接 `co_await rpc_call()` 等协程任务。

因此建议：

- 当前阶段：`WORKS` 使用 `concurrencpp` executor，但仍以普通 `post/submit` 为主。
- 下一阶段：在真正存在异步等待点时，再引入 `result<T>`、`resume_on`、业务协程入口。
- 不要为了协程而把 `Reactor` 的 `Connection` / `Channel` / `EventLoop` 向下改成协程风格。

### 5.4 Download 执行器建议

下载模块不建议复用 `Reactor` 的 IO 线程，也不建议直接复用 `WORKS` 业务执行器。

原因：

- 下载任务当前仍是**同步阻塞 I/O 模型**。
- 分块下载会执行外部 HTTP 请求、磁盘写入、分块合并、CRC 校验。
- 这些任务执行时间长、吞吐导向明显，与请求业务的低延迟目标不同。

因此建议下载模块采用：

- **独立的共享 DownloadExecutor**
- **每个下载任务只持有并发额度，不持有物理线程池**

也就是说，后续不应继续沿用“每个 `ChunkedDownloadManager` 自己创建一个线程池”的模式，而应改为：

```text
DownloadTaskManager
  |- 管理任务生命周期
  |- 管理任务级并发额度
  `- 提交 chunk 任务到全局 DownloadExecutor
```

### 5.5 Download 执行器应具备的能力

下载执行器建议重点补以下能力，而不是优先协程化：

- **全局并发上限**：限制所有下载任务的总 chunk 并发数。
- **单任务并发上限**：保留 `max_concurrency`，但其含义是任务配额，不是物理线程数。
- **每用户配额**：防止单个用户占满下载资源。
- **有界队列**：避免无限排队导致内存膨胀。
- **取消语义**：取消任务时仅终止该任务的待执行/可中止 chunk，不影响整个下载执行器。
- **合并低优先级**：`MergeParts` 与校验建议视为低优先级或单独阶段，避免与 chunk 下载争用。

### 5.6 下载模块未来是否协程化

下载模块后续是否协程化，建议分两步看：

1. **当前阶段**：先从“每任务一个线程池”改为“全局共享下载执行器”，先解决线程膨胀和调度语义问题。
2. **后续阶段**：当 HTTP client、文件写入、超时控制都有成熟 awaitable 后，再评估是否基于 `Proactor` 或独立 async executor 做下载协程化。

这意味着：

- 当前不建议为了统一模型，强行把下载任务塞进 `WORKS` executor。
- 当前也不建议把下载模块与 `Reactor` 的 IO loop 合并。
- 下载模块适合先完成**共享执行器 + 配额调度 + 取消语义**治理，再考虑协程化。

### 5.7 对当前方案的落地影响

按本文档当前范围，本阶段仍只替换 `WORKS` 执行器。  
但从中长期演进来看，推荐的结构是：

- `Reactor`：专用 `LoopThreadGroup`，不接入 `concurrencpp`
- `WORKS`：`concurrencpp` executor，后续接 RPC 协程
- `Download`：独立共享 `DownloadExecutor`

这三个方向彼此解耦，可以分阶段落地，避免一次性重构过大。

---

## 六、详细设计

## 6.1 持有方式

`HttpServer` 新增以下成员：

```cpp
#include "concurrencpp/concurrencpp.h"

class HttpServer {
private:
    std::shared_ptr<concurrencpp::runtime> cc_runtime_;
    std::shared_ptr<concurrencpp::thread_pool_executor> works_executor_;

    std::atomic<size_t> pending_work_count_{0};
    std::mutex drain_mutex_;
    std::condition_variable drain_cv_;
};
```

说明：

- `cc_runtime_` 只服务于 `HttpServer` 的 `WORKS` 执行器，不向 `TcpServer`、下载模块扩散。
- `works_executor_` 只替换当前 `threadpool_` 的业务线程池职责。
- `pending_work_count_` 从线程池内部计数迁移到 `HttpServer` 自身维护。

> 注：具体创建 executor 的 API 以最终接入的 concurrencpp 版本为准；若自定义线程数/名称的工厂接口与预期不完全一致，可先使用该版本提供的默认 `thread_pool_executor` 完成第一阶段落地，再补 executor 定制。

## 6.2 背压归属调整

当前背压逻辑依赖 `threadpool_.queue_size()`：

```cpp
if (threadpool_.queue_size() > max_work_queue_depth_) {
    SendServiceUnavailable(conn, "work queue overloaded");
    return;
}
```

替换后，背压由 `HttpServer` 自己维护：

```cpp
if (pending_work_count_.load(std::memory_order_acquire) >= max_work_queue_depth_) {
    SendServiceUnavailable(conn, "work queue overloaded");
    return;
}
```

核心原则：

- executor 只负责调度，不负责业务容量控制。
- 任何一次投递，只要会占用 `WORKS` 执行器处理能力，就必须计入 `pending_work_count_`。
- 任务正常完成、异常退出、shutdown 拒绝时，都必须正确回收计数。

## 6.3 统一投递辅助函数

为避免在多个 `post()` 点重复写 `fetch_add/fetch_sub`，建议增加统一辅助函数：

```cpp
template <class Fn>
bool HttpServer::PostWorkTask(Fn&& fn) {
    if (pending_work_count_.load(std::memory_order_acquire) >= max_work_queue_depth_) {
        return false;
    }

    pending_work_count_.fetch_add(1, std::memory_order_acq_rel);
    try {
        works_executor_->post([this, task = std::forward<Fn>(fn)]() mutable {
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
    } catch (...) {
        pending_work_count_.fetch_sub(1, std::memory_order_acq_rel);
        throw;
    }
}
```

这样可以统一处理：

- 背压检查
- 计数增减
- executor 投递失败回滚
- 任务异常日志
- drain 通知

## 6.4 `HttpServer` 中的替换点

本阶段只替换下面几类路径：

1. `HandleMessage` 中首个 worker 投递
2. `HandleMessageInWorker` 中 `should_chain` 的续投
3. `OnWorkerExit` 中因 `queued_chunks` 或 `facade pending` 触发的续投

对应当前代码位置：

- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L161-L205)
- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L247-L251)
- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L533-L569)

迁移后，所有这些 `addtask(...)` 都统一改为 `PostWorkTask(...)`。

### 6.4.1 首次投递

```cpp
std::weak_ptr<IConnection> weak_conn = conn;
if (!PostWorkTask([this, weak_conn, ctx]() mutable {
        HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
    })) {
    SendServiceUnavailable(conn, "work queue overloaded");
    return;
}
```

### 6.4.2 链式续投

```cpp
if (should_chain) {
    PostWorkTask([this, weak_conn, ctx]() mutable {
        HandleMessageInWorker(std::move(weak_conn), std::move(ctx));
    });
}
```

### 6.4.3 `OnWorkerExit` 补投

`OnWorkerExit` 中所有原先使用 `threadpool_.addtask(...)` 的地方，统一替换为 `PostWorkTask(...)`，避免遗漏计数。

## 6.5 Stop / Drain 语义

当前 `Stop()` 实现是：

```cpp
SqlConnPool::Instance()->ClosePool();
threadpool_.stop();
net_server_->Stop();
```

替换后建议调整顺序：

1. `net_server_->Stop()`：先停止接收新连接和新的读事件投递。
2. 等待 `pending_work_count_ == 0`，完成已进入 `WORKS` 的任务。
3. `works_executor_->shutdown()` 或释放 `cc_runtime_`，结束工作执行器。
4. `SqlConnPool::Instance()->ClosePool()`：最后关闭数据库资源。

推荐伪代码：

```cpp
void HttpServer::Stop() {
    net_server_->Stop();

    {
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drain_cv_.wait(lock, [this] {
            return pending_work_count_.load(std::memory_order_acquire) == 0;
        });
    }

    if (works_executor_) {
        works_executor_->shutdown();
    }
    works_executor_.reset();
    cc_runtime_.reset();

    SqlConnPool::Instance()->ClosePool();
}
```

说明：

- 本阶段优先选择 **drain 后关闭**，避免业务处理中途被销毁。
- 若后续需要“快速停机”，可增加运行时配置，允许跳过 drain。
- 若启用了 `blocking executor`，其关闭时机与 `works_executor_` 保持一致，也应放在 drain 之后统一 shutdown。

---

## 六、协程接入策略

## 6.1 第一阶段不强推协程化

本阶段的落地重点是“`WORKS` 线程池替换”，不是“把所有请求处理改成协程”。因此：

- `HandleMessageInWorker` / `ProcessSingleRequest` 保持现有同步回调式逻辑。
- 不要求立即新增 `HandleMessageCoro` 并切生产流量。
- 不要求把同步文件 I/O / 同步数据库 / 同步 RPC 立即改成 awaitable。

这样做的原因是：

1. 当前业务主路径仍有大量同步代码，强行改协程收益有限。
2. 先替换 executor 可以显著降低一次性改动面。
3. 后续 RPC 落地时再把最合适的链路协程化，能把收益集中在真正有等待点的场景。

## 6.2 为未来 RPC 预留的协程模型

当后续接入真正的异步 RPC 客户端后，`WORKS` 线程可逐步演进为：

```cpp
concurrencpp::result<void> HttpServer::HandleMessageCoro(
    std::weak_ptr<IConnection> weak_conn,
    std::shared_ptr<ConnectionWorkContext> ctx) {
    co_await concurrencpp::resume_on(*works_executor_);

    auto req_ctx = co_await ParseRequestAsync(weak_conn, ctx);
    auto rpc_result = co_await RpcCallAsync(req_ctx);
    co_await SerializeAndSendAsync(weak_conn, ctx, rpc_result);
}
```

此时协程收益才真正体现为：

- `co_await rpc_call()` 等待期间不占住 worker
- 多阶段业务逻辑从状态机改为顺序代码
- 超时、取消、重试点可以在 await 边界统一处理

## 6.3 一个重要前提

如果后续 RPC 客户端仍然是**同步阻塞 stub**，即使外层写成协程函数，也不会自动获得“释放线程”的收益。  
因此后续阶段要明确区分：

- **同步 RPC 包装成协程函数**：可读性提升有限，性能收益几乎没有
- **真正异步 RPC awaitable**：协程收益显著

## 6.4 同步 MySQL 的暂行方案（A + B）

当前项目对 MySQL 的主要阻塞点有两个：

1. `SqlConnPool::GetConn()` 里的 `sem_wait()` 可能在连接池耗尽时长期阻塞。
2. `mysql_query` / `mysql_store_result` 一类同步调用会占住当前执行线程直到数据库返回。

因此本文档对 MySQL 的阶段性处理明确采用 **A + B**：

### A：阻塞数据库调用走临时 `blocking executor`

建议把同步 MySQL、同步 `open`、密码哈希这类阻塞步骤，从 `WORKS` executor 中拆出来，投递到仅服务阻塞任务的临时执行器。

```text
IO线程
  -> WORKS executor（解析、保序、组装上下文）
  -> blocking executor（同步 MySQL / open / 哈希）
  -> IO线程回写
```

这样做的收益不是“数据库变异步”了，而是：

- `WORKS` executor 不会被数据库查询长期占满。
- 阻塞型任务与普通业务任务的线程预算可以分开配置。
- 后续即使改成真正 async MySQL，也可以在这个边界上平滑替换。

### B：连接池获取改为限时等待或快速失败

即使 A 尚未全部接入，B 也建议单独尽快落地。

当前不建议继续使用无限期 `sem_wait()`；更合适的做法是：

- 改为 `sem_timedwait()` 或等价的限时等待；
- 超时后返回 `nullptr` / 明确错误码；
- 上层按“数据库忙”返回 503 或业务侧可识别的失败响应。

这样至少可以避免：

- 单个 `WORKS` 线程被 MySQL 连接池耗尽永久挂住；
- drain / shutdown 时因为数据库借连接卡死而长时间无法退出。

### 说明

- 方案 A + B 只是**阻塞治理**，不是 MySQL 已经协程化或非阻塞化。
- 后续若接入真正的 async MySQL awaitable，可再把 A 从“换一个线程执行”升级为“等待期间不占线程执行”。
- 本阶段不要求把所有路由一次性迁到 A；可优先接入登录、注册等明确访问 MySQL 的路径。

## 6.5 `facade_mutex` 串行化的含义

这里的“串行”，不是只针对“上次没传输完的 pending 字符串”本身，而是针对**整个连接级 `HttpFacade` 的可变状态**，包括但不限于：

- parser 当前所处的解析阶段；
- 尚未消费完的 pending 缓冲；
- `GetConsumedBytes()` / `ErasePending()` 对消费边界的推进；
- `ProcessPending()` 内部依赖的请求边界和状态机。

因此 `facade_mutex` 的作用是：**保证同一连接上，对同一个 `HttpFacade` 的访问始终是单线程顺序执行的**。

如果没有这把锁，而同一连接有多个 worker 同时进入：

- worker A 追加了新字节，worker B 也同时追加；
- worker A 读到的 consumed bytes 可能已经被 worker B 改写；
- `ErasePending()` 可能基于过期的消费边界删除数据；
- 最终可能出现请求边界错乱、重复消费、漏消费，甚至响应顺序与请求语义不一致。

所以从**正确性**角度看，串行化不是问题，反而是当前设计下必须保留的保护措施。

### 对当前 pending 字符串是否有问题

如果当前 pending 只保留“上次未处理完/未消费完”的那部分字符串，那么**在串行访问前提下这是安全的**。  
原因是同一时刻只有一个 worker 能操作 `AppendPending -> ProcessPending -> GetConsumedBytes -> ErasePending` 这一整段逻辑，因此不会出现并发修改 pending 字符串本身的问题。

真正需要注意的是**性能语义**：

- 单连接即使允许 `max_concurrent_workers > 1`，涉及 `HttpFacade` 的解析/消费阶段仍然是串行的；
- 因此多 worker 带来的收益主要体现在不同连接之间，或者未来把阻塞业务步骤拆出 `facade` 临界区之后；
- 它并不意味着“同一连接的 HTTP 解析阶段已经完全并行化”。

---

## 七、与现有模块的边界

## 7.1 本阶段不改 Reactor IO 线程池

`TcpServer` 当前使用自研 `ThreadPool` 拉起各个 `subloop`：

- [tcpserver.h](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.h)
- [tcpserver.cpp](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp)

这一层的任务是长生命周期 `EventLoop::run()`，与 `WORKS` 的“短任务/业务任务投递”不同，不纳入本次替换。

## 7.2 本阶段不改下载线程池

`ChunkedDownloadManager` 目前独立持有自己的 `ThreadPool`：

- [ChunkedDownloadManager.h](file:///home/zsy/WebServer/cppBackend/download/include/ChunkedDownloadManager.h)
- [ChunkedDownloadManager.cpp](file:///home/zsy/WebServer/cppBackend/download/src/ChunkedDownloadManager.cpp)

下载任务有独立的任务生命周期与取消语义，本次不与 `HttpServer` 的 `WORKS` executor 合并。

## 7.3 本阶段不改 `INetServer`

`INetServer` 和 `NetFactory` 仍保持当前接口：

- [INetServer.h](file:///home/zsy/WebServer/cppBackend/net/INetServer.h)
- [NetFactory.h](file:///home/zsy/WebServer/cppBackend/net/NetFactory.h)
- [NetFactory.cpp](file:///home/zsy/WebServer/cppBackend/net/NetFactory.cpp)

本阶段不新增 `SetIoExecutor` 等接口。

---

## 八、实施步骤

### 当前 todo

- [x] Step 1：升级编译标准并引入依赖
- [x] Step 2：在 `HttpServer` 中接入 runtime 与 `WORKS` executor
- [x] Step 3：封装统一 `PostWorkTask`
- [x] Step 4：替换背压判断
- [x] Step 5：改造 Stop / Drain
- [x] Step 6：保留协程预留点
- [x] Step 7：补同步 MySQL 的保护措施

## Step 1：升级编译标准并引入依赖

将根 `CMakeLists.txt` 与 `cppBackend/CMakeLists.txt` 升级到 C++20：

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

通过 `FetchContent` 或其他团队接受的方式引入 `concurrencpp`：

```cmake
include(FetchContent)
FetchContent_Declare(
    concurrencpp
    GIT_REPOSITORY https://github.com/David-Haim/concurrencpp.git
    GIT_TAG        v.0.1.7
)
FetchContent_MakeAvailable(concurrencpp)
target_link_libraries(${PROJECT_NAME} PRIVATE concurrencpp::concurrencpp)
```

> 注：仓库可解析的实际 tag 为 `v.0.1.7`。

## Step 2：在 `HttpServer` 中接入 runtime 与 `WORKS` executor

修改：

- [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)
- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)

完成：

1. 新增 `cc_runtime_`
2. 新增 `works_executor_`
3. 若启用方案 A，新增仅服务阻塞子任务的 `blocking_executor_`
4. 新增 `pending_work_count_` 和 drain 同步原语
5. 构造函数中初始化 `WORKS` executor（以及可选的 `blocking executor`）

## Step 3：封装统一 `PostWorkTask`

把所有 `threadpool_.addtask(...)` 替换为 `PostWorkTask(...)`，并确保：

1. 首投路径计数准确
2. `should_chain` 续投计数准确
3. `OnWorkerExit` 补投计数准确
4. executor 投递失败时计数能回滚

## Step 4：替换背压判断

将：

```cpp
threadpool_.queue_size()
```

替换为：

```cpp
pending_work_count_.load(...)
```

同时验证 503 背压仍符合当前语义。

## Step 5：改造 Stop / Drain

修改 `Stop()`，实现：

1. 先停网络入口
2. 再 drain 已进入 `WORKS` 的任务
3. 最后关闭 executor / runtime 与数据库资源

**落地细节**：在 `PostWorkTask` 的任务完成回调中，`pending_work_count_` 降为 0 时通过 `drain_mutex_` + `drain_cv_.notify_all()` 发出 drain 信号，避免 Missed Wakeup。`Stop()` 先 `net_server_->Stop()`、再 `drain_cv_.wait`、最后 `works_executor_->shutdown()` / `blocking_executor_->shutdown()` 逐级释放。

## Step 6：保留协程预留点

本阶段只预留以下内容，不强制启用：

- `coroutine_enabled_` 运行期开关（已落地：[HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) 成员 `bool coroutine_enabled_{false}`）
- `HandleMessageCoro(...)` 预备接口（以注释形式预留声明）
- 未来 `RpcCallAsync(...)` / `resume_on(...)` 的接入位置

## Step 7：补同步 MySQL 的保护措施

1. `SqlConnPool::GetConn()` 改为 `sem_timedwait()` 限时等待（3 秒超时），超时后返回 `nullptr`。落地于 [sqlconnpool.cpp](file:///home/zsy/WebServer/cppBackend/mysql/sqlconnpool.cpp)。
2. 对登录、注册等明确访问 MySQL 的链路，优先评估是否接入临时 `blocking executor`。已提供 `PostBlockingTask()` 统一投递入口，由 [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) 中的 `blocking_executor_` 承接。
3. 验证数据库繁忙时不会把 `WORKS` 线程长期卡死。

---

## 九、代码改动清单

本阶段应修改的文件：

- [CMakeLists.txt](file:///home/zsy/WebServer/CMakeLists.txt)
- [cppBackend/CMakeLists.txt](file:///home/zsy/WebServer/cppBackend/CMakeLists.txt)
- [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h)
- [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)
- [sqlconnpool.h](file:///home/zsy/WebServer/cppBackend/mysql/sqlconnpool.h)
- [sqlconnpool.cpp](file:///home/zsy/WebServer/cppBackend/mysql/sqlconnpool.cpp)

本阶段**不应修改**的文件：

- [tcpserver.h](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.h)
- [tcpserver.cpp](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp)
- [INetServer.h](file:///home/zsy/WebServer/cppBackend/net/INetServer.h)
- [NetFactory.cpp](file:///home/zsy/WebServer/cppBackend/net/NetFactory.cpp)
- [ChunkedDownloadManager.h](file:///home/zsy/WebServer/cppBackend/download/include/ChunkedDownloadManager.h)
- [ChunkedDownloadManager.cpp](file:///home/zsy/WebServer/cppBackend/download/src/ChunkedDownloadManager.cpp)

---

## 十、验收项

| 验收项 | 标准 |
| --- | --- |
| 编译 | C++20 编译通过，`concurrencpp` 成功链接 |
| 回归 | 现有后端测试通过，至少覆盖 `HttpServer` 主路径 |
| 行为一致 | 请求处理、响应保序、错误处理、回 IO 线程发送行为不变 |
| 背压 | `pending_work_count_` 超阈值时返回 503，且无计数泄漏 |
| 续投准确 | `should_chain` / `OnWorkerExit` 中的续投全部经过统一投递入口 |
| 关闭语义 | `Stop()` 后不再接收新任务，已提交任务可按预期 drain 完成 |
| 回退能力 | 可以通过编译开关或小范围代码回退到旧 `ThreadPool` |

---

## 十一、风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| C++20 升级后的项目级兼容性验证不充分 | 可能影响全项目编译或 CI 一致性 | 当前环境 `gcc 13.3` 已满足要求，重点转为做全量编译、测试与 CI/toolchain 一致性校验 |
| concurrencpp 版本 API 与预期不完全一致 | 文档中的创建方式需调整 | 第一阶段优先保证 executor 可用，细节按实际版本修正 |
| 背压计数遗漏 | 导致 503 失真或 drain 卡住 | 统一封装 `PostWorkTask`，禁止散落 `post()` |
| shutdown 语义处理不当 | 停机时丢任务或悬挂 | 默认采用 drain 后 shutdown |
| 同步 MySQL / `open` 长时间阻塞 `WORKS` | 业务线程被占满，尾延迟抖动明显 | 本阶段对 MySQL 明确采用 A+B：阻塞子任务临时隔离 + 连接池限时等待 |
| `facade_mutex` 使单连接解析阶段串行 | 单连接多 worker 的收益受限 | 明确其目标是保证 `HttpFacade` 状态正确性；若后续要提升单连接并行度，应先拆分 facade 内部状态边界 |
| 当前收益不及预期 | 替换后性能不一定立刻提升 | 明确本阶段收益以“维护性 + 未来演进”为主 |
| 协程预期过高 | 团队误以为替换线程池即可获得非阻塞收益 | 文档明确：真正收益要等异步 RPC / awaitable 落地 |

---

## 十二、回退策略

建议保留简单切换方式：

```cpp
#ifdef USE_CONCURRENCPP_WORKS
    // 使用 works_executor_ + PostWorkTask
#else
    // 使用旧 ThreadPool threadpool_
#endif
```

回退目标仅限 `HttpServer` 的 `WORKS` 执行器层，不影响 `TcpServer` 和下载模块。

---

## 十三、后续路线

本阶段完成后，后续演进建议如下：

### Phase A：稳定 `WORKS` executor 替换

- 先跑通普通任务投递
- 验证背压、续投、停机与错误处理
- 对同步 MySQL 先落 A+B，避免 `WORKS` 被数据库阻塞拖垮
- 确认性能不回退到不可接受水平

### Phase B：接入异步 RPC

- 在 `WORKS` executor 内引入 `RpcCallAsync`
- 将最合适的业务链路改为 `co_await rpc_call()`
- 保留 `Worker -> IO` 的回切模型不变

### Phase C：局部协程化请求处理

- 对真正存在等待点的业务路径引入 `HandleMessageCoro`
- 用顺序协程代码替代部分状态机式逻辑
- 引入超时、取消和结构化并发能力

### Phase D：再评估其他线程池是否替换

- `TcpServer` IO 线程池是否需要单独替换
- 下载模块是否需要独立 executor / 协程下载
- 是否需要更统一的 runtime 管理策略

---

## 十四、结论

本方案的落地边界应明确为：

- **当前要做的事**：用 `concurrencpp` 替换 `HttpServer` 的 `WORKS` 线程池。
- **当前对 MySQL 的暂行做法**：采用 A+B，即阻塞子任务临时隔离 + 连接池限时等待，而不是直接承诺 async MySQL。
- **当前不做的事**：不联动改 `IO` 线程池、不改下载线程池、不做全局 runtime 收口。
- **当前的直接收益**：降低线程池维护成本，统一未来协程接入底座。
- **未来的主要收益来源**：异步 RPC / 真正可挂起的 awaitable 落地后，协程在 `WORKS` 路径上的收益才会显著提升。

因此，本次改造应被定义为：  
**“WORKS 执行器替换 + 协程/RPC 预留”**，而不是“一次性线程池全面协程化”。
