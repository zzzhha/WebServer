# 并发审查后续开发计划

## 概述

本文档基于以下两份审查/设计文档的交叉对照生成：

- [顺序传输并发性能审查报告](file:///home/zsy/WebServer/docs/concurrency-review/sequential-transfer-concurrency-review.md)（下称"文档一"）
- [策略B：请求级并发架构重构设计文档](file:///home/zsy/WebServer/docs/concurrency-review/strategy-B-implementation-design.md)（下称"文档二"）

文档一识别了 8 个并发性能问题并给出了改进方案，文档二定义了策略 B（请求级并行）的完整实施方案与后续演进路径（Phase 1→2→3）。本文档通过逐项比对源码现状，确认各项改进的完成情况，识别剩余遗漏项，并结合[后续更新路线图](file:///home/zsy/WebServer/.trae/skills/roadmap-next-updates/SKILL.md)中的 P0-P4 宏观规划，输出一份可落地的分阶段开发计划。

---

## 一、文档一中各问题的完成情况

### 1.1 完成情况总览

| 问题编号 | 问题描述 | 状态 | 源码验证位置 |
|----------|----------|------|-------------|
| 问题1 (策略A) | Worker 内 Yield 机制 | ❌ 被覆盖 | 策略 B 已落地，策略 A 不再独立实施 |
| 问题1 (策略B) | 请求级并发——同连接不同请求并行处理 | ✅ 已完成 | [HttpServer.cpp:L175-L234](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L175-L234) |
| 问题1 (CRC64) | CRC64 替换 MD5，消除 CPU 密集阻塞 | ✅ 已完成 | [Crc64Util.cpp](file:///home/zsy/WebServer/cppBackend/download/src/Crc64Util.cpp) |
| 问题2 | `pending_results` map 刷出无批数限制 | ✅ 已完成 | [HttpServer.cpp:L593](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L593) `kMaxApplyPerBatch` |
| 问题3 | TLS 下 `pread` 阻塞 IO 线程 | ✅ 已完成 | [Connection.cpp:L108-L112](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L108-L112) `kMaxPreadsPerEvent = 4` |
| 问题4 | `MemoryPool::deallocate` 大块释放阻塞 | ✅ 已完成 | [DeferDeallocate.h](file:///home/zsy/WebServer/cppBackend/MemoryPool/DeferDeallocate.h) 延迟释放队列，攒批 64 后统一归还 |
| 问题5 | `ThreadPool` 全局锁竞争 | ✅ 已完成 | [ThreadPool.h](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h) per-worker deque + inject_queue + work-stealing |
| 问题6 | `queueinloop` 每次 `write(eventfd)` | ✅ 已完成 | [Eventloop.cpp:L76-L78](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L76-L78) `need_wakeup` 检查 |
| 问题7 | MD5 计算阻塞 Worker 线程 | ✅ 已完成 | CRC64 已替代 MD5，`Crc64Util::Combine` 支持分块合并 |
| 问题8 | `writev` 循环无分批限制 | ✅ 已完成 | [Connection.cpp:L194](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L194) `kMaxBytesPerEvent = 1MB` |
| 独立改造 | WORKS 执行器替换（concurrencpp 接入） | ✅ 已完成 | [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) `cc_runtime_`/`works_executor_`/`blocking_executor_`；[HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp) `PostWorkTask`/`PostBlockingTask` |
| 独立改造 | MySQL 连接池限时等待 + 阻塞任务隔离 | ✅ 已完成 | [sqlconnpool.cpp](file:///home/zsy/WebServer/cppBackend/mysql/sqlconnpool.cpp) `sem_timedwait`；[HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp) `PostBlockingTask` |
| 独立改造 | 协程预留点 | ✅ 已完成 | [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) `coroutine_enabled_` + `HandleMessageCoro` 注释预留 |

### 1.2 已完成项详细确认

#### 问题5：ThreadPool 全局锁竞争（✅ 已完成 / HttpServer 已切换至 concurrencpp）

源码确认点：

- 架构：[ThreadPool.h:L38-L42](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L38-L42) — per-worker `std::deque<Task>` + 独立 `std::mutex`
- 注入队列：[ThreadPool.h:L48-L49](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L48-L49) — `inject_m_` + `inject_q_` 处理外部线程提交
- 背压反馈：[ThreadPool.cpp:L26-L30](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.cpp#L26-L30) — `addTask(Task) → bool`
- 执行优先级：[ThreadPool.cpp:L113-L154](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.cpp#L113-L154) — 本地 pop → batch drain inject(≤32) → steal → cv.wait
- Task 结构体：[ThreadPool.h:L20-L28](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L20-L28) — priority/trace_id/affinity/cancel 字段已预留
- 兼容保留：`addtask(std::function<void()>)` 内部转为 `addTask(Task)`
- 测试验证：[threadpool_stress_tests.cpp](file:///home/zsy/WebServer/test/threadpool_stress_tests.cpp) — 4 个压力用例全部通过

> **注意**：HttpServer 的 WORKS 业务执行器已从自研 `ThreadPool` 切换至 `concurrencpp::thread_pool_executor`（详见 [threadpool-coroutine-design.md](file:///home/zsy/WebServer/docs/concurrency-review/threadpool-coroutine-design.md)）。`HttpServer::threadpool_` 仅保留用于 `Stop()` 中的 `threadpool_.stop()` 兼容回收，不再承载业务任务投递。业务任务统一走 `PostWorkTask()` → `works_executor_->post()`，阻塞子任务走 `PostBlockingTask()` → `blocking_executor_->post()`。

#### 问题6：queueinloop 优化（✅ 已完成）

源码确认点：[Eventloop.cpp:L76-L78](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L76-L78)

```cpp
bool need_wakeup = false;
{
  std::lock_guard<std::mutex> lock(mutex_);
  need_wakeup = taskqueue_.empty();  // 仅在队列从空变为非空时唤醒
  taskqueue_.push(std::move(fn));
}
if (need_wakeup) { wakeup(); }
```

#### 问题3：TLS 下 pread 限制（✅ 已完成）

源码确认点：[Connection.cpp:L107-L112](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L107-L112)

```cpp
const size_t kMaxPreadsPerEvent = 4;
size_t pread_count = 0;
// ...
if (pread_count >= kMaxPreadsPerEvent) {
  clientchannel_->enablewriting();
  return;
}
```

#### 策略B Phase 1：请求级并行（✅ 已完成）

源码确认点：

- `ConnectionWorkContext` 扩展：[HttpServer.h:L55-L59](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h#L55-L59) — `active_worker_count` / `max_concurrent_workers` / `draining` / `facade_mutex`
- HttpServer 配置：[HttpServer.h:L114-L117](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h#L114-L117) — `max_concurrent_workers_per_conn_` / `max_apply_per_batch_` / `parallel_pipelining_enabled_`
- 链式调度 `HandleMessageInWorker`：[HttpServer.cpp:L175-L234](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L175-L234)
- `ProcessSingleRequest` 多阶段执行器：[HttpServer.cpp:L484-L510](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L484-L510)
- `OnWorkerExit`：[HttpServer.cpp:L512-L527](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L512-L527)
- `PostResultToIoLoop` fd 泄漏防护 + 批处理限制：[HttpServer.cpp:L530-L629](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L530-L629)
- `HandleClose`/`HandleError` 排空处理：[HttpServer.cpp:L91-L112](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L91-L112)
- `CloseSendFileFd`：[HttpServer.cpp:L631-L636](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L631-L636)

#### CRC64 替换 MD5（✅ 已完成）

源码确认点：

- CRC64 工具类：[Crc64Util.h](file:///home/zsy/WebServer/cppBackend/download/include/Crc64Util.h) — `Compute` / `ComputeFileCrc64` / `Combine`（支持分块 CRC64 合并，零读盘校验）
- 下载模块集成：[ChunkedDownloadManager.cpp](file:///home/zsy/WebServer/cppBackend/download/src/ChunkedDownloadManager.cpp) — `VerifyCrc64()` 方法，通过 `Crc64Util::Combine` 组合各分块 CRC64 值

### 1.3 未完成项详细说明

#### 问题8：writev 循环无分批限制（✅ 已完成）

**源码确认点**：[Connection.cpp:L193-L255](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L193-L255)

```cpp
const size_t kMaxBytesPerEvent = 1024 * 1024;  // 1MB
size_t total_written = 0;

while(total_written < kMaxBytesPerEvent){
  // writev(...) / sendfile(...) 累计 total_written
}
if(outputbuffer_.readableBytes() > 0 || sendfile_.active){
  clientchannel_->enablewriting();  // 下次事件继续发送
}
```

---

#### 问题4：MemoryPool::deallocate 大块释放阻塞 IO 线程（✅ 已完成）

**源码确认点**：

- 延迟释放队列：[DeferDeallocate.h](file:///home/zsy/WebServer/cppBackend/MemoryPool/DeferDeallocate.h) — `DeferDeallocate()` 攒批 64，`FlushDeferredFrees()` 统一归还
- Buffer 改造：[Buffer.h:L22](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h#L22) — `Block::~Block()` 调用 `DeferDeallocate(data, size)` 替代直接 `MemoryPool::deallocate`
- IO 线程集成：[Eventloop.cpp:L55](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L55) — `run()` 中每轮事件循环末尾调用 `FlushDeferredFrees()`

---

#### 策略B Phase 1 测试（✅ 已完成）

新增测试文件 [http_server_parallel_tests.cpp](file:///home/zsy/WebServer/test/http_server_parallel_tests.cpp)，覆盖以下 6 个用例，全部通过：

| 测试用例 | 覆盖场景 | 结果 |
|----------|----------|------|
| `test_chain_scheduling_basic` | 10 个 chunk 入队，验证链式调度正确分发到多个 worker | ✅ PASS |
| `test_response_ordering` | 并行处理后响应按 `response_seq` 严格保序回写 | ✅ PASS |
| `test_worker_count_bound` | 验证 `active_worker_count <= max_concurrent_workers` | ✅ PASS |
| `test_draining_no_new_worker` | 排空模式下不再启动新 worker | ✅ PASS |
| `test_fd_no_leak_on_disconnect` | 验证 sendfile_fd 正确关闭，无泄漏 | ✅ PASS |
| `test_backpressure_with_parallel` | 背压机制在并行场景下正常触发拒绝 | ✅ PASS |

---

#### 策略B Phase 2：io_uring 适配（✅ 已完成）

**现状**：基于 Thread-per-Core 架构的全新 `proactor` 模块已在 `cppBackend/proactor/` 目录下完成开发，替代了早期计划中侵入 Reactor 模块的方案。通过在 `cppBackend/net/` 引入 `INetServer` 和 `IConnection` 抽象接口，`HttpServer` 实现了对底层网络模型的无感切换。`io_uring` 高级特性（Splice、SO_REUSEPORT、SQPOLL）及基于 `IORING_OP_TIMEOUT` 的连接超时机制均已落地（详见 [io_uring-design.md](file:///home/zsy/WebServer/docs/concurrency-review/io_uring-design.md)）。

---

#### 策略B Phase 3：协程化 Worker（⏳ 未开始）

无协程调度器、无 `HandleMessageInWorkerCoroutine`、无 `co_await` 基础设施。

---

## 二、后续开发计划（分阶段）

基于以上完成情况分析，结合[路线图 SKILL](file:///home/zsy/WebServer/.trae/skills/roadmap-next-updates/SKILL.md) 中的 P0-P4 宏观规划，后续开发按以下优先级推进：

```
优先级总览：

P-now (立即修复)  →  已全部完成 ✅
P1 (io_uring)    →  已全部完成 ✅
P2 (协程化)      →  策略B Phase 3：协程化 Worker
P3 (分布式gRPC)  →  服务化拆分
P4 (音视频)      →  媒体数据面
```

> **说明**：P0（Nginx 反向代理）在路线图中位于策略 B 之前，由于本文档聚焦于 cppBackend 内部的并发改进，P0 的实施状态不在本次审查范围内，但后续 P1-P4 的各阶段灰度验证依赖 P0 提供的流量切换能力。

---

### 阶段一：立即修复（P-now）— ✅ 已完成

#### 1.1 非 TLS 路径 writev/sendfile 循环增加字节限制（问题8） ✅

**改动点**：[Connection.cpp:L193-L255](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L193-L255)

- 增加 `kMaxBytesPerEvent = 1024 * 1024`（1MB）
- `while(total_written < kMaxBytesPerEvent)` 替代 `while(true)`
- 未发完时调用 `clientchannel_->enablewriting()` 触发下次 epoll 事件继续

---

#### 1.2 MemoryPool 延迟释放队列（问题4） ✅

**改动文件**：

- [DeferDeallocate.h](file:///home/zsy/WebServer/cppBackend/MemoryPool/DeferDeallocate.h) — 新增，`DeferDeallocate()` 攒批 64 后统一归还，`FlushDeferredFrees()` 强制刷出
- [Buffer.h:L22](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h#L22) — `Block::~Block()` 和 move-assignment 中调用 `DeferDeallocate(data, size)` 替代直接 `MemoryPool::deallocate`
- [Eventloop.cpp:L55](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L55) — `run()` 中每轮事件循环末尾调用 `FlushDeferredFrees()`

---

#### 1.3 策略B Phase 1 测试补齐 ✅

**改动文件**：[http_server_parallel_tests.cpp](file:///home/zsy/WebServer/test/http_server_parallel_tests.cpp)，已验证通过

| 测试用例 | 覆盖场景 |
|----------|----------|
| `test_chain_scheduling_basic` | 3 个请求进入同一连接，验证链式调度启动多个 worker |
| `test_response_ordering` | 并行处理后响应按 `response_seq` 严格保序回写 |
| `test_fd_no_leak_on_disconnect` | 连接关闭时未回写的 `sendfile_fd` 被正确关闭 |
| `test_worker_count_bound` | 单连接并发 worker 数不超过 `max_concurrent_workers` |
| `test_draining_no_new_worker` | 排空模式下不再启动新 worker |
| `test_backpressure_with_parallel` | 并行场景下背压机制仍正常触发 503 |

---
### 阶段二：网络库 Reactor/Proactor 双模式（P1）— io_uring 底层重构（✅ 已完成）

#### 2.1 架构愿景与接口隔离

在不侵入、不修改现有 `reactor` 模块核心逻辑的前提下，从零实现了基于 `io_uring` 的 `proactor` 网络模块。引入了网络抽象接口层 `INetServer` 和 `IConnection`。`HttpServer` 通过这些抽象接口与底层网络库通讯，实现了对底层网络模型的 100% 无感切换。

#### 2.2 Thread-per-Core 架构落地

`cppBackend/proactor` 目录下已实现完整的 Thread-per-Core 架构：
- **UringServer**：负责管理 `UringWorker` 池，监听端口，并将新连接分发到各个 Worker。
- **UringWorker**：每个 CPU 核心绑定一个 IO 线程，持有私有的 `io_uring` 实例。负责处理被分配到该核心的所有连接的 IO 操作，彻底消除了全局锁竞争。
- **无锁任务投递**：`HttpServer` 工作线程完成业务逻辑后，通过 `UringConnection::PostIoTask` 将响应投递回对应 `UringWorker` 的 MPSC 队列，利用 `eventfd` 唤醒 IO 线程执行回写。

#### 2.3 高级特性与性能优化支持

- **零拷贝发送**：利用 `IORING_OP_SPLICE` 实现了静态文件的零拷贝直传，并支持通过 `IOSQE_IO_LINK` 进行链式提交。
- **多监听与内核轮询**：支持 `SO_REUSEPORT` 多监听策略消除 accept 瓶颈，并支持 `IORING_SETUP_SQPOLL` 开启内核线程轮询，大幅减少系统调用开销。
- **TLS 降级兼容**：针对 TLS 连接，采用 `IORING_OP_POLL_ADD` 获取就绪事件，然后在用户态调用 OpenSSL 接口读写，确保与现有安全模块完美兼容。
- **原生连接超时机制**：解耦了原有的 `TimeWheel` 核心，并在 `UringWorker` 内通过 `IORING_OP_TIMEOUT` 原生驱动时间轮 tick，实现了无额外线程的空闲连接清理。

#### 2.4 配置系统集成

已在 `NetFactory` 中集成模式切换能力，支持通过配置灵活切换 `reactor` 与 `proactor` 模式，同时支持配置 io_uring 的各项参数（如队列深度、是否绑定 CPU 核心等）。

#### 2.5 并行工程：ThreadPool SKILL 扩展（⏳ 进行中）

ThreadPool 的核心重构已完成（见问题5），但以下高级特性仍可继续迭代扩展：

| 扩展项 | 依赖 | 交付物 |
|--------|------|--------|
| **观测与自适应** | 无 | Prometheus 指标导出（`queue_depth`、`steal_count`、`wait_latency_p99`） |
| **优先级调度** | 观测指标落地后 | 三级队列（High/Normal/Low），窃取优先偷高优任务 |
| **取消/超时** | 优先级调度完成后 | `Task::cancel` 生效，Worker 执行前检查 + 超时定时器 |
| **延迟任务** | 取消/超时完成后 | 时间轮/小顶堆定时器，到期注入 inject_queue |
| **任务亲和** | 延迟任务完成后 | 基于 `affinity` hash 的一致性映射，同连接任务落同一 Worker |

---

### 阶段三：协程化 Worker（P2）

#### 3.1 目标

将策略 B 的多阶段回调（`PhaseParseAndRoute` → `PhaseIoOperation` → `PhaseSerializeAndSend`）替换为 C++20 协程，使请求处理代码可以像同步代码一样编写，底层自动 yielding。

#### 3.2 依赖关系

- **前置**：P1（io_uring）完成，提供可用的异步 IO 路径；WORKS 执行器已切换至 `concurrencpp`（协程预留点已落地）
- **为后续提供**：P3（gRPC）的服务端协程模型

#### 3.3 当前底座状态

P2 的前置基础设施已就绪：

| 前置项 | 状态 | 位置 |
|--------|------|------|
| C++20 编译标准 | ✅ | `CMakeLists.txt` `CMAKE_CXX_STANDARD 20` |
| `concurrencpp` 执行器 | ✅ | [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) `cc_runtime_` / `works_executor_` |
| 协程运行期开关 | ✅ | [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) `coroutine_enabled_{false}` |
| `HandleMessageCoro` 预备接口 | ✅ | [HttpServer.h](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h) 注释预留 |
| 阻塞任务隔离机制 | ✅ | [HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp) `PostBlockingTask()` + `blocking_executor_` |
| MySQL 连接池不永久卡死 | ✅ | [sqlconnpool.cpp](file:///home/zsy/WebServer/cppBackend/mysql/sqlconnpool.cpp) `sem_timedwait` 3 秒超时 |

#### 3.4 改动范围

| 模块 | 改动类型 | 说明 |
|------|----------|------|
| `HandleMessageInWorkerCoroutine` | 新增 | 基于 `concurrencpp::result<void>` + `co_await` 的协程版请求处理，与回调路径通过 `coroutine_enabled_` 开关并存 |
| `ProcessSingleRequest` 协程化 | 重写 | 三阶段改为 `co_await concurrencpp::resume_on(*works_executor_)` 链 |
| `ConnectionWorkContext` | 简化 | 移除 `facade_mutex` 和 `pending_results`（协程天然按序执行，无需乱序保序） |
| 异步 IO awaitable | 新增 | 基于 P1 io_uring CQE 的 `co_await` 等待点（取代 `suspended` 标记轮询） |

#### 3.5 关键设计决策

1. **协程调度器直接使用 concurrencpp**：`concurrencpp::thread_pool_executor` 原生支持 `co_await resume_on()` 和 `result<T>`，无需自建协程调度器
2. **渐进迁移**：先新增 `HandleMessageInWorkerCoroutine` 协程路径，与现有 `HandleMessageInWorker` 回调路径通过 `coroutine_enabled_` 开关并存
3. **协程边界清晰**：仅在 Worker 线程内使用协程，Reactor 保持事件驱动不变；`WORKS executor` 承接协程，`blocking_executor_` 承接同步阻塞子任务
4. **收益前提**：真正收益要等到异步 RPC / io_uring awaitable 落地，而非仅把现有同步代码改写为协程语法

#### 3.6 验收项

- 协程路径与回调路径行为一致（通过 `coroutine_enabled_` 开关切回旧路径对比）
- `concurrencpp::result<void>` 协程调度稳定，无栈溢出/内存泄漏
- `co_await resume_on()` 正确在 `works_executor_` 和 IO 线程间切换
- 取消/超时可正确终止协程（`concurrencpp::cancellation_token`）
- 与 Reactor 的边界清晰（Reactor 负责连接与事件分发，协程负责异步任务编排）

---

### 阶段四：分布式 gRPC 服务化（P3）

> 详细设计待 P2 完成后制定，此处列出方向性目标。

- **目的**：将认证、下载、元数据、转码/媒体控制等能力拆到独立 gRPC 服务
- **依赖**：P2（协程/并发模型稳定），P0（Nginx 灰度入口）
- **关键交付物**：服务发现/重试/超时/熔断/限流策略；链路追踪与日志关联；与现有 HTTP 路由的职责划分

---

### 阶段五：音视频服务器（P4）

> 详细设计待 P3 完成后制定，此处列出方向性目标。

- **目的**：将高 CPU/高 IO 的音视频处理从 Web 请求面剥离，形成"控制面（Web）+ 数据面（媒体）"架构
- **依赖**：P3（RPC/服务治理），P0（Nginx 入口与路由清晰）
- **关键交付物**：媒体处理吞吐/延迟/资源隔离/失败恢复；媒体节点可横向扩展

---

## 三、依赖关系图

```
P0 (Nginx反代) ──── 灰度/流量切换入口 ─────────────────────────────┐
       │                                                            │
       ▼                                                            │
P-now (立即修复) ── 问题4/8 + 策略B测试补齐                          │
       │                                                            │
       ├──→ ThreadPool SKILL扩展 (观测/优先级/取消/延迟/亲和) ──────┤
       │     (与 P1 并行，改动区域正交：调度层 vs 网络层)              │
       │                                                            │
       ▼                                                            ▼
P1 (网络库Proactor) ── INotifier抽象 + Epoll/IoUring双后端 ─── 灰度验证
       │        IoUringNotifier 资源下沉至 EventLoop/Connection 层
       │
       ▼
P2 (协程化) ──── 策略B Phase 3：协程化Worker
       │           co_await 直接挂载 io_uring CQE
       │
       ▼
P3 (分布式gRPC) ── 服务化拆分
       │
       ▼
P4 (音视频) ──── 媒体数据面
```

---

## 四、风险与回退策略

### 4.1 Top 4 风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| **io_uring 内核兼容性** | 低版本内核（< 5.1）无 io_uring；部分早期 5.x 内核存在已知 bug（如 `IORING_OP_SEND` 在 5.3 前不可用）；`IORING_OP_READ_FIXED` 需 5.7+；SQPOLL unprivileged 需 5.11+ | `IoUring` 构造时 `uname` 检测内核版本 → 不满足自动降级为 `EpollNotifier`；`IORING_OP_SPLICE` 作为 `IORING_OP_SEND` 的 fallback；S3 阶段根据内核能力逐项启用高级特性（固定文件/缓冲区注册/SQPOLL），跳过不可用特性并记录 WARN 日志；SQPOLL 需 `CAP_SYS_NICE` 或内核 >= 5.11，启动时检测并自动降级 |
| **双路径行为不一致** | Proactor 的 CQE 收割批次与 Reactor 的 EPOLLOUT 触发时机存在微妙的时序差异，可能影响 writecallback/sendcompletecallback 的调用时序 | 核心缓解：Connection 内部**零 `if (proactor)` 分支**——Proactor 的差异完全由 `IoUringChannelContext` 在 Channel 层吸收，CQE 完成通知被转换为标准的 `EventType::READABLE/WRITABLE` 后走同一条分发路径；2.9 节异常处理矩阵全覆盖；`notifier_interface_tests.cpp` 参数化测试覆盖两种 Notifier 的所有异常场景；引入 "behavioral differential fuzzing"——同输入序列注入两种模式，自动比对 HTTP 响应和关闭时序 |
| **IoUringChannelContext buffer 生命周期** | Proactor 模式下提交给 io_uring 的读/写缓冲区必须在 CQE 收割前保持有效，提前释放会导致数据损坏或 use-after-free | `IoUringChannelContext` 通过 `read_buf_`（unique_ptr 持有）和 `writev_held_strings_` 显式持有 buffer 直到 CQE 收割；Connection 关闭时先 `submitCancel` 取消所有 pending_ops，等待 CQE `ECANCELED` 后再释放；`io_uring_channel_tests.cpp` 专门验证 buffer 生命周期和 pending_ops 清理 |
| **协程化后调试复杂度上升** | 协程的挂起/恢复栈难以追踪，问题定位困难 | 保留回调路径作为 debug fallback；增加协程 ID 与 trace_id 的日志关联 |
| **链式调度潜在活锁** | 极端负载下单连接可能反复拉起/释放 worker | 通过 `max_concurrent_workers_per_conn_` 限制并发，`worker_count_bound` 测试已覆盖 |

### 4.2 渐进启用与回退策略

每个阶段均遵循"新旧并存、开关切换"原则：

| 阶段 | 启用方式 | 回退方式 |
|------|----------|----------|
| P-now | 编译后即生效（无开关） | git revert |
| P1 | 配置文件 `net.mode=reactor`（默认值），不设置环境变量即可回退；运行期内核检测不满足自动降级；S1/S2/S3 三步递进，每步可独立回退 | S1: 修改 Channel EventLoop 即可回退到 Epoll 类；S2: 设置 `WEBSERVER_NET_MODE=reactor` 或删除配置文件；S3: 关闭高级特性配置项（register_files=false 等） |
| P2 | 运行期开关 `coroutine_enabled_` | 关闭开关回退到回调路径 |
| P3 | Nginx upstream 灰度切流 | Nginx 配置回退 |
| P4 | 独立服务，流量按路由分发 | 路由回退到 Web 节点本地处理 |

---

## 五、里程碑与时间线建议

```
Milestone 1 (P-now):  立即修复 + 测试补齐 ✅              ████░░░░░░░░░░░░ (已完成)
Milestone 1.5 (WORKS): concurrencpp 执行器替换 ✅        ████░░░░░░░░░░░░ (已完成)
                       ├── C++20 + concurrencpp 引入    ████░░░░░░░░░░░░
                       ├── PostWorkTask 统一投递         ████░░░░░░░░░░░░
                       ├── Stop/Drain + MySQL 保护       ████░░░░░░░░░░░░
                       └── 协程预留点落地                ████░░░░░░░░░░░░
Milestone 2 (P1):     网络库Proactor模式 ✅               ████████████████ (已完成)
                       ├── INetServer/IConnection 抽象   ████████████████
                       ├── UringServer/Worker 实现       ████████████████
                       ├── Splice/SQPOLL/超时支持        ████████████████
                       │   适配 + 配置文件机制             ████████████████
                       └── 接口兼容/性能基准/异常测试      ████████████████
                       + ThreadPool 观测/优先级 (并行)     ░░░░████████░░░░
Milestone 3 (P2):     协程化 Worker                      ░░░░░░░░░░████░░
Milestone 4 (P3):     分布式 gRPC 服务化                  ░░░░░░░░░░░░░░██
Milestone 5 (P4):     音视频数据面                         ░░░░░░░░░░░░░░░█
```

> 注：P1 子任务按顺序推进（接口抽象 → EpollNotifier 迁移 → IoUringNotifier 实现 → 适配 → 测试），ThreadPool SKILL 扩展可与 P1 全程并行。

---

## 附录 A：完成状态与计划对照速查表

| 来源 | 条目 | 完成 | 计划 |
|------|------|------|------|
| 文档一 问题1 | 策略B 请求级并发 | ✅ | — |
| 文档一 问题1 | CRC64 替换 MD5 | ✅ | — |
| 文档一 问题2 | pending_results 批数限制 | ✅ | — |
| 文档一 问题3 | TLS pread 次数限制 | ✅ | — |
| 文档一 问题4 | MemoryPool 延迟释放 | ✅ | — |
| 文档一 问题5 | ThreadPool 无锁化（HttpServer 已切换 concurrencpp） | ✅ | — |
| 文档一 问题6 | queueinloop wakeup 优化 | ✅ | — |
| 文档一 问题7 | MD5 阻塞 | ✅ | — |
| 文档一 问题8 | writev 循环限制 | ✅ | — |
| 文档二 Phase 1 | 请求级并行实现 | ✅ | — |
| 文档二 Phase 1 | 策略B 测试 | ✅ | — |
| 灰度开关 | parallel_pipelining_enabled_ | ❌ 已删除 | 不再需要，链式调度为唯一路径 |
| 文档二 Phase 2 | io_uring 适配（重构为网络库 Proactor 模式） | ✅ | — |
| 文档二 Phase 3 | 协程化 Worker（底座已就绪：concurrencpp + 协程预留点） | ⏳ | P2 |
| ThreadPool SKILL | 观测/优先级/取消/延迟/亲和 | ⏳ | P1 并行 |
| WORKS 替换 | C++20 + concurrencpp 依赖引入 | ✅ | — |
| WORKS 替换 | HttpServer 接入 cc_runtime_ / works_executor_ / blocking_executor_ | ✅ | — |
| WORKS 替换 | 统一 PostWorkTask / PostBlockingTask 投递入口 | ✅ | — |
| WORKS 替换 | 背压 pending_work_count_ 替换 queue_size | ✅ | — |
| WORKS 替换 | Stop / Drain 语义（drain_mutex_ + drain_cv_） | ✅ | — |
| WORKS 替换 | 协程预留点（coroutine_enabled_ + HandleMessageCoro） | ✅ | — |
| WORKS 替换 | MySQL 保护（sem_timedwait + blocking executor） | ✅ | — |
| P1 新增 | INetServer / IConnection 抽象接口 | ✅ | — |
| P1 新增 | UringServer / UringWorker (Thread-per-Core) | ✅ | — |
| P1 新增 | 零拷贝 (Splice) 与内核轮询 (SQPOLL) 支持 | ✅ | — |
| P1 新增 | TLS 降级处理 (POLL_ADD) 与超时清理 (TIMEOUT) | ✅ | — |
| P1 新增 | 配置文件 `etc/webserver.conf` 模式切换 | ✅ | — |
| 路线图 P0 | Nginx 反向代理 | 不在范围 | 独立推进 |
| 路线图 P3 | gRPC 服务化 | ⏳ | P3 |
| 路线图 P4 | 音视频服务器 | ⏳ | P4 |
