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
| 问题4 | `MemoryPool::deallocate` 大块释放阻塞 | ✅ 已完成 | [DeferDeallocate.h](file:///home/zsy/WebServer/cppBackend/MemoryPool/DeferDeallocate.h) + [Buffer.h](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h) + [Eventloop.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp) |
| 问题5 | `ThreadPool` 全局锁竞争 | ✅ 已完成 | [ThreadPool.h](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h) per-worker deque + inject_queue + work-stealing |
| 问题6 | `queueinloop` 每次 `write(eventfd)` | ✅ 已完成 | [Eventloop.cpp:L76-L78](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L76-L78) `need_wakeup` 检查 |
| 问题7 | MD5 计算阻塞 Worker 线程 | ✅ 已完成 | CRC64 已替代 MD5，`Crc64Util::Combine` 支持分块合并 |
| 问题8 | `writev` 循环无分批限制 | ✅ 已完成 | [Connection.cpp:L192-L256](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L192-L256) `kMaxBytesPerEvent = 1MB` |

### 1.2 已完成项详细确认

#### 问题5：ThreadPool 全局锁竞争（✅ 已完成）

源码确认点：

- 架构：[ThreadPool.h:L38-L42](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L38-L42) — per-worker `std::deque<Task>` + 独立 `std::mutex`
- 注入队列：[ThreadPool.h:L48-L49](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L48-L49) — `inject_m_` + `inject_q_` 处理外部线程提交
- 背压反馈：[ThreadPool.cpp:L26-L30](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.cpp#L26-L30) — `addTask(Task) → bool`
- 执行优先级：[ThreadPool.cpp:L113-L154](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.cpp#L113-L154) — 本地 pop → batch drain inject(≤32) → steal → cv.wait
- Task 结构体：[ThreadPool.h:L20-L28](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h#L20-L28) — priority/trace_id/affinity/cancel 字段已预留
- 兼容保留：`addtask(std::function<void()>)` 内部转为 `addTask(Task)`
- 测试验证：[threadpool_stress_tests.cpp](file:///home/zsy/WebServer/test/threadpool_stress_tests.cpp) — 4 个压力用例全部通过

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
- HttpServer 配置：[HttpServer.h:L114-L117](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h#L114-L117) — `max_concurrent_workers_per_conn_` / `max_apply_per_batch_`
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

**实现位置**：[Connection.cpp:L192-L256](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L192-L256)

非 TLS 发送路径增加 `kMaxBytesPerEvent = 1MB` 限制，`total_written` 累积追踪 writev/sendfile 的写入字节数。达到上限后通过 `enablewriting()` 触发下次 epoll 写事件继续发送，防止 IO 线程被大文件发送长时间占用。

```cpp
const size_t kMaxBytesPerEvent = 1024 * 1024;
size_t total_written = 0;
while (total_written < kMaxBytesPerEvent) {
  // writev + sendfile 循环，每次累加 total_written
}
if (outputbuffer_.readableBytes() > 0 || sendfile_.active) {
  clientchannel_->enablewriting();
}
```

---

#### 问题4：MemoryPool::deallocate 大块释放阻塞 IO 线程（✅ 已完成）

**实现位置**：
- 新增 [DeferDeallocate.h](file:///home/zsy/WebServer/cppBackend/MemoryPool/DeferDeallocate.h) — 线程局部延迟释放队列
- 修改 [Buffer.h](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h) — `Block::~Block()` 和 move 赋值调用 `DeferDeallocate` 替代直接 `deallocate`
- 修改 [Eventloop.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp) — `run()` 循环末尾调用 `FlushDeferredFrees()`

**核心机制**：
- IO 线程 `writecallback` 中 Buffer::Block 析构时，不再直接调用 `MemoryPool::deallocate`（可能在 IO 线程触发 PageCache 全局锁 + `munmap`）
- 改为入队到 `thread_local tls_defer_free`，攒批 64 个后统一归还，锁竞争次数降低约 64 倍
- 每个 EventLoop 迭代末尾调用 `FlushDeferredFrees()`，确保不无限积压

---

#### 策略B Phase 1 测试（✅ 已完成）

测试文件：[http_server_parallel_tests.cpp](file:///home/zsy/WebServer/test/http_server_parallel_tests.cpp) — 6 个测试用例，15 项断言全部通过：

| 测试用例 | 覆盖场景 | 结果 |
|----------|----------|------|
| `test_chain_scheduling_basic` | 3 个 chunk 入队，验证链式调度启动多个 worker | ✅ PASS |
| `test_response_ordering` | 乱序到达的响应按 `response_seq` 严格保序输出 | ✅ PASS (5/5) |
| `test_fd_no_leak_on_disconnect` | 连接关闭时 `pending_results` 中的 `sendfile_fd` 被正确关闭 | ✅ PASS |
| `test_worker_count_bound` | 单连接并发 worker 数不超过 `max_concurrent_workers` | ✅ PASS |
| `test_draining_no_new_worker` | 排空模式下不再启动新 worker | ✅ PASS |
| `test_backpressure_with_parallel` | 全局队列背压 + 连接级字节背压均可触发 | ✅ PASS |

---

#### 策略B Phase 2：io_uring 适配（⏳ 架构已预留，实际未集成）

**现状**：`RequestPhase::IO_OPERATION` 阶段已在 [HttpServer.h:L188-L194](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.h#L188-L194) 定义，`req_ctx->suspended` 挂起标记已预留。但 [HttpServer.cpp:L349](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L349) 中的 io_uring 提交/恢复路径仍为注释状态，当前 `PhaseIoOperation` 在 Worker 线程内同步执行 `stat()` / `open()` 等阻塞 IO 调用。

```cpp
// HttpServer.cpp PhaseIoOperation 中的预留占位：
// if (io_uring_submit(...) == -EAGAIN) {
//   req_ctx->suspended = true;
//   RegisterResumeCallback(weak_conn, ctx, chunk, req_ctx);
//   return;
// }
```

---

#### 策略B Phase 3：协程化 Worker（⏳ 未开始）

无协程调度器、无 `HandleMessageInWorkerCoroutine`、无 `co_await` 基础设施。

---

## 二、后续开发计划（分阶段）

基于以上完成情况分析，结合[路线图 SKILL](file:///home/zsy/WebServer/.trae/skills/roadmap-next-updates/SKILL.md) 中的 P0-P4 宏观规划，后续开发按以下优先级推进：

```
优先级总览：

P-now (立即修复)  →  补齐遗漏项（问题4、问题8）+ 策略B测试
P1 (io_uring)    →  策略B Phase 2：io_uring 异步 IO 适配
P2 (协程化)      →  策略B Phase 3：协程化 Worker
P3 (分布式gRPC)  →  服务化拆分
P4 (音视频)      →  媒体数据面
```

> **说明**：P0（Nginx 反向代理）在路线图中位于策略 B 之前，由于本文档聚焦于 cppBackend 内部的并发改进，P0 的实施状态不在本次审查范围内，但后续 P1-P4 的各阶段灰度验证依赖 P0 提供的流量切换能力。

---

### 阶段一：立即修复（P-now）— 补齐遗漏项

#### 1.1 非 TLS 路径 writev/sendfile 循环增加字节限制（问题8）

**改动范围**：[Connection.cpp::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L188-L255)

**实施方案**：

```cpp
void Connection::writecallback() {
  // ... TLS 路径保持不变 ...

  const size_t kMaxBytesPerEvent = 1024 * 1024;  // 1MB
  size_t total_written = 0;

  while (total_written < kMaxBytesPerEvent) {
    // writev 逻辑
    size_t iov_count = outputbuffer_.getIOVecs(iovs, max_ioves, ...);
    if (iov_count > 0) {
      ssize_t nwritten = ::writev(fd(), iovs, iov_count);
      if (nwritten > 0) {
        total_written += static_cast<size_t>(nwritten);
        outputbuffer_.consumeBytes(nwritten);
        continue;
      }
      // ... EAGAIN 处理 ...
    }

    // sendfile 逻辑
    if (sendfile_.active) {
      // ...
      ssize_t n = ::sendfile(fd(), sendfile_.file_fd, &off, sendfile_.remaining);
      if (n > 0) {
        total_written += static_cast<size_t>(n);
        // ...
      }
    }
    // ...
  }

  if (outputbuffer_.readableBytes() > 0 || sendfile_.active) {
    clientchannel_->enablewriting();  // 下次事件继续发送
  }
}
```

**验收项**：
- IO 线程单次 `writecallback` 发送字节数 ≤ `kMaxBytesPerEvent`
- 同 IO 线程上其他连接不受大文件发送阻塞
- `enablewriting` 后能正确触发下次写事件继续发送
- 现有 `reactor_integration_tests` 全部通过

---

#### 1.2 MemoryPool 延迟释放队列（问题4）

**改动范围**：[Buffer.h](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h)（`Block::~Block`）、新增 `cppBackend/MemoryPool/DeferDeallocate.h`

**实施方案**：

```cpp
// DeferDeallocate.h — 线程局部延迟释放
#pragma once
#include <vector>
#include <utility>
#include "MemoryPool.h"

inline thread_local std::vector<std::pair<void*, size_t>> tls_defer_free;

inline void DeferDeallocate(void* ptr, size_t size) {
  tls_defer_free.emplace_back(ptr, size);
  if (tls_defer_free.size() >= 64) {
    for (auto& [p, s] : tls_defer_free) {
      MemoryPool::deallocate(p, s);
    }
    tls_defer_free.clear();
  }
}

inline void FlushDeferredFrees() {
  for (auto& [p, s] : tls_defer_free) {
    MemoryPool::deallocate(p, s);
  }
  tls_defer_free.clear();
}
```

**Buffer.h 改造**：

```cpp
~Block(){
  if(data){
    DeferDeallocate(data, size);  // 替换直接 deallocate
  }
}
```

**验收项**：
- IO 线程 `writecallback` 中不再直接触发 PageCache 全局锁
- 攒批阈值 64 时，锁竞争次数降低约 64 倍
- 无内存泄漏（通过 valgrind 或 ASan 验证）
- `FlushDeferredFrees()` 在 IO 线程事件循环末尾调用，确保不积压

---

#### 1.3 策略B Phase 1 测试补齐

**改动范围**：`test/` 目录，新增 `http_server_parallel_tests.cpp`

**测试用例**：

| 测试用例 | 覆盖场景 |
|----------|----------|
| `test_chain_scheduling_basic` | 3 个请求进入同一连接，验证链式调度启动多个 worker |
| `test_response_ordering` | 并行处理后响应按 `response_seq` 严格保序回写 |
| `test_fd_no_leak_on_disconnect` | 连接关闭时未回写的 `sendfile_fd` 被正确关闭 |
| `test_worker_count_bound` | 单连接并发 worker 数不超过 `max_concurrent_workers` |
| `test_draining_no_new_worker` | 排空模式下不再启动新 worker |
| `test_backpressure_with_parallel` | 并行场景下背压机制仍正常触发 503 |

**验收项**：
- 上述 6 个测试用例全部通过
- 与现有测试（`threadpool_stress_tests`、`reactor_integration_tests`）无冲突

---

### 阶段二：io_uring 异步 IO（P1）

#### 2.1 目标

在策略 B Phase 1（请求级并行）和 Phase 2 预留的架构边界之上，将 Worker 线程中的同步 IO 操作（`open`/`stat`/`read`）替换为 io_uring 异步提交，使 Worker 在 IO 等待期间可让出 CPU 处理其他请求。

#### 2.2 依赖关系

- **前置**：阶段一（P-now）全部完成；策略 B Phase 1 稳定运行且通过测试
- **并行**：可与 ThreadPool 优先级调度（见 2.5）并行开发
- **为后续提供**：P2 协程化的 IO 异步基础设施

#### 2.3 改动范围

| 模块 | 文件 | 改动类型 |
|------|------|----------|
| IO 抽象层 | 新增 `cppBackend/reactor/IoUring.h/.cpp` | 新增 | io_uring 的 C++ RAII 封装（SQE 提交 / CQE 收割） |
| HttpServer | [HttpServer.cpp::PhaseIoOperation](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L331-L350) | 修改 | 将 `stat()`/`open()` 替换为 io_uring 异步提交 + 挂起/恢复 |
| Connection | [Connection.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp) | 修改 | 非 TLS 路径可选 io_uring sendfile；TLS 路径可选 io_uring pread |
| ThreadPool | [ThreadPool.h](file:///home/zsy/WebServer/cppBackend/reactor/ThreadPool.h) | 修改 | Worker 线程增加 SQE 提交 / CQE 收割循环 |

#### 2.4 关键设计决策

1. **双后端可切换**：epoll 与 io_uring 通过编译期宏或运行期配置切换，确保可回退
2. **Worker 双角色**：Worker 线程同时承担 CPU 任务 + io_uring 提交/收割
3. **挂起/恢复机制**：利用 `req_ctx->suspended` 标记 + `RegisterResumeCallback` 将挂起任务重新投递到 ThreadPool
4. **先打通最短链路**：优先适配文件 IO（`open`/`stat`/`read`），再逐步扩展到网络 IO

#### 2.5 并行工程：ThreadPool SKILL 扩展

在 P1 阶段同步推进 ThreadPool 预留扩展点的实现：

| 扩展项 | 依赖 | 交付物 |
|--------|------|--------|
| **观测与自适应** | 无 | Prometheus 指标导出（`queue_depth`、`steal_count`、`wait_latency_p99`） |
| **优先级调度** | 观测指标落地后 | 三级队列（High/Normal/Low），窃取优先偷高优任务 |
| **取消/超时** | 优先级调度完成后 | `Task::cancel` 生效，Worker 执行前检查 + 超时定时器 |
| **延迟任务** | 取消/超时完成后 | 时间轮/小顶堆定时器，到期注入 inject_queue |
| **任务亲和** | 延迟任务完成后 | 基于 `affinity` hash 的一致性映射，同连接任务落同一 Worker |

#### 2.6 验收项

- io_uring 路径正确性：与 epoll 路径行为一致（包括异常关闭、半包粘包、Keep-Alive、超时、背压）
- 性能对比：同负载下 io_uring 路径 CPU 使用率降低、吞吐量提升
- 可回退：关闭 io_uring 后系统回退到纯 epoll 模式，行为一致
- 无 fd 泄漏：io_uring 取消/超时场景下 fd 正确关闭

---

### 阶段三：协程化 Worker（P2）

#### 3.1 目标

将策略 B 的多阶段回调（`PhaseParseAndRoute` → `PhaseIoOperation` → `PhaseSerializeAndSend`）替换为 C++20 协程，使请求处理代码可以像同步代码一样编写，底层自动 yielding。

#### 3.2 依赖关系

- **前置**：P1（io_uring）完成，提供可用的异步 IO 路径
- **为后续提供**：P3（gRPC）的服务端协程模型

#### 3.3 改动范围

| 模块 | 改动类型 | 说明 |
|------|----------|------|
| 协程调度器 | 新增 | 基于 ThreadPool work-stealing 的协程调度器 |
| `HandleMessageInWorkerCoroutine` | 重写 | 将 `ProcessSingleRequest` 三阶段改为 `co_await` 链 |
| `ConnectionWorkContext` | 简化 | 移除 `facade_mutex` 和 `pending_results`（协程天然按序执行，无需乱序保序） |

#### 3.4 关键设计决策

1. **协程调度器复用 work-stealing**：ThreadPool 的 work-stealing 作为协程调度器底座
2. **渐进迁移**：先新增协程路径，与回调路径并存，通过开关切换
3. **协程边界清晰**：仅在 Worker 线程内使用协程，Reactor 保持事件驱动不变

#### 3.5 验收项

- 协程路径与回调路径行为一致
- 协程调度器稳定，无栈溢出/内存泄漏
- 取消/超时可正确终止协程
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
P0 (Nginx反代) ──── 灰度/流量切换入口 ─────────────────────────┐
       │                                                        │
       ▼                                                        │
P-now (立即修复) ── 问题4/8 + 策略B测试补齐                      │
       │                                                        │
       ├──→ ThreadPool SKILL扩展 (观测/优先级/取消/延迟/亲和) ──┤
       │                                                        │
       ▼                                                        ▼
P1 (io_uring) ── 策略B Phase 2：异步IO ──────────────────── 灰度验证
       │
       ▼
P2 (协程化) ──── 策略B Phase 3：协程化Worker
       │
       ▼
P3 (分布式gRPC) ── 服务化拆分
       │
       ▼
P4 (音视频) ──── 媒体数据面
```

---

## 四、风险与回退策略

### 4.1 Top 3 风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| **策略B Phase 1 未经充分测试即上线** | 链式调度/并行处理的边界条件（fd 泄漏、响应乱序、死锁）可能在生产环境暴露 | 阶段一补齐 6 个测试用例覆盖核心状态机逻辑，与现有 4 项测试套件共同构成回归防线 |
| **io_uring 内核兼容性** | 低版本内核无 io_uring 支持或存在已知 bug | 编译期宏 + 运行期检测 `kernel >= 5.1`，不满足时自动回退 epoll |
| **协程化后调试复杂度上升** | 协程的挂起/恢复栈难以追踪，问题定位困难 | 保留回调路径作为 debug fallback；增加协程 ID 与 trace_id 的日志关联 |

### 4.2 渐进启用与回退策略

每个阶段均遵循"新旧并存、开关切换"原则：

| 阶段 | 启用方式 | 回退方式 |
|------|----------|----------|
| P-now | 编译后即生效（无开关） | git revert |
| P1 | 编译宏 `USE_IO_URING` + 运行期内核版本检测 | 关闭宏重新编译；运行期检测失败自动 fallback |
| P2 | 运行期开关 `coroutine_enabled_` | 关闭开关回退到回调路径 |
| P3 | Nginx upstream 灰度切流 | Nginx 配置回退 |
| P4 | 独立服务，流量按路由分发 | 路由回退到 Web 节点本地处理 |

---

## 五、里程碑与时间线建议

```
Milestone 1 (P-now):  立即修复 + 测试补齐          ████░░░░░░░░░░░░
Milestone 2 (P1):     io_uring 文件IO适配           ░░░░████████░░░░
                       + ThreadPool 观测/优先级      ░░░░████████░░░░
Milestone 3 (P2):     协程化 Worker                 ░░░░░░░░░░████░░
Milestone 4 (P3):     分布式 gRPC 服务化            ░░░░░░░░░░░░░░██
Milestone 5 (P4):     音视频数据面                   ░░░░░░░░░░░░░░░█
```

> 注：各里程碑的实际工期取决于团队资源与并行程度。P1 与 ThreadPool SKILL 扩展可并行推进。

---

## 附录 A：完成状态与计划对照速查表

| 来源 | 条目 | 完成 | 计划 |
|------|------|------|------|
| 文档一 问题1 | 策略B 请求级并发 | ✅ | — |
| 文档一 问题1 | CRC64 替换 MD5 | ✅ | — |
| 文档一 问题2 | pending_results 批数限制 | ✅ | — |
| 文档一 问题3 | TLS pread 次数限制 | ✅ | — |
| 文档一 问题4 | MemoryPool 延迟释放 | ✅ | — |
| 文档一 问题5 | ThreadPool 无锁化 | ✅ | — |
| 文档一 问题6 | queueinloop wakeup 优化 | ✅ | — |
| 文档一 问题7 | MD5 阻塞 | ✅ | — |
| 文档一 问题8 | writev 循环限制 | ✅ | — |
| 文档二 Phase 1 | 请求级并行实现 | ✅ | — |
| 文档二 Phase 1 | 策略B 测试 | ✅ | — |
| 文档二 Phase 2 | io_uring 适配 | ⏳ | P1 |
| 文档二 Phase 3 | 协程化 Worker | ⏳ | P2 |
| ThreadPool SKILL | 观测/优先级/取消/延迟/亲和 | ⏳ | P1 并行 |
| 路线图 P0 | Nginx 反向代理 | 不在范围 | 独立推进 |
| 路线图 P3 | gRPC 服务化 | ⏳ | P3 |
| 路线图 P4 | 音视频服务器 | ⏳ | P4 |
