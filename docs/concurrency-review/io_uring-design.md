# 双网络模型（Reactor/Proactor）架构与 io_uring 实现设计文档

> **设计准则**：在不侵入、不修改现有 `reactor` 模块核心逻辑的前提下，从零实现基于 `io_uring` 的 `proactor` 网络模块。通过上层接口抽象，使最上层业务服务（`HttpServer`）能够对下层网络底层实现完全无感，并通过配置文件实现两大网络后端的灵活切换与互不干扰。

## 目录

- [1. 架构愿景：接口隔离与双引擎并存](#1-架构愿景接口隔离与双引擎并存)
- [2. 网络层接口抽象 (NetCore)](#2-网络层接口抽象-netcore)
- [3. Reactor 层的无缝适配](#3-reactor-层的无缝适配)
- [4. Proactor (io_uring) 模块从零设计 — Thread-per-Core 架构](#4-proactor-io_uring-模块从零设计--thread-per-core-架构)
- [5. HttpServer 的无感切换实现](#5-httpserver-的无感切换实现)
- [6. 配置系统设计](#6-配置系统设计)
- [7. 开发与测试路线图](#7-开发与测试路线图)
- [8. Proactor 模式下的连接超时与定时器设计](#8-proactor-模式下的连接超时与定时器设计)

---

## 1. 架构愿景：接口隔离与双引擎并存

当前架构中，`HttpServer` 直接强依赖 `TcpServer` 和 `Connection` 这两个由 Reactor 模式提供的具体类。为了实现 `io_uring` 的安全落地，我们将引入一套**网络抽象接口层 (`INetServer`, `IConnection`)**。

```text
┌────────────────────────────────────────────────────────────┐
│                        HttpServer                          │
│ (业务逻辑、路由、HTTP报文解析、Phase2异步链路、背压控制)   │
└───────────────┬────────────────────────────┬───────────────┘
                │ 持有 std::unique_ptr<INetServer>
┌───────────────▼────────────────────────────▼───────────────┐
│                     网络抽象接口层                         │
│            (INetServer, IConnection, NetConfig)            │
└───────┬────────────────────────────────────────────┬───────┘
        │                                            │
        ▼ 运行时动态多态                             ▼ 运行时动态多态
┌───────────────────────┐                ┌───────────────────────────────┐
│    Reactor 网络库     │                │     Proactor 网络库           │
│ (cppBackend/reactor)  │                │ (cppBackend/proactor)         │
│ 现有实现，基于 epoll  │                │ 全新实现，基于 io_uring       │
│ TcpServer, Connection │                │ UringServer, UringConn        │
│ 主从Reactor多线程     │                │ Thread-per-Core 架构          │
└───────────────────────┘                └───────────────────────────────┘
```

这种设计的最大优势是：**现有的 Reactor 模块无需进行伤筋动骨的重构**。`io_uring` 网络层将作为一个平级的、独立的模块（`cppBackend/proactor`）从 0 开始实现，只在接口层面与上层契合。

## 2. 网络层接口抽象 (NetCore)

新增 `cppBackend/net/` 目录，用于放置网络接口抽象，作为 `HttpServer` 与底层网络库的通讯契约。

### 2.1 IConnection 接口

上层服务（如 `HttpServer` 和业务逻辑）只关心数据的读写、上下文的挂载、连接的基本属性，以及**将任务投递回该连接所属的 IO 线程**。引入 `PostIoTask` 接口是为了剥离对 Reactor `EventLoop::queueinloop` 的强依赖，使得业务响应完成后可以跨网络模型唤醒 IO 线程执行回写。

```cpp
#pragma once
#include <string>
#include <memory>
#include <any>
#include "Buffer.h"

class TlsContext;

class IConnection : public std::enable_shared_from_this<IConnection> {
public:
    virtual ~IConnection() = default;

    // 连接属性
    virtual int fd() const = 0;
    virtual std::string ip() const = 0;
    virtual uint16_t port() const = 0;
    virtual bool IsDisconnected() const = 0;

    // 读写缓冲交互
    virtual BufferBlock& getInputBuffer() = 0;
    virtual BufferBlock& getOutputBuffer() = 0;

    // 发送与关闭控制
    virtual void send() = 0;
    virtual void setCloseOnSendComplete(bool close) = 0;
    virtual void StartSendFile(int fd, off_t offset, size_t length, bool auto_close) = 0;
    virtual void ClearSendFile() = 0;

    // IO 线程任务投递 (用于 Worker 线程将响应发送任务投递回 IO 线程)
    virtual void PostIoTask(std::function<void()> task) = 0;

    // 上下文挂载 (支持 HttpFacade, ConnectionWorkContext 等连接级对象)
    template <typename T>
    void SetContext(const T& ctx) { context_ = ctx; }
    
    template <typename T>
    T* GetContext() { return std::any_cast<T>(&context_); }
    
    // TLS 设置
    virtual void SetTlsContext(std::shared_ptr<TlsContext> ctx) = 0;

protected:
    std::any context_;
};

using spIConnection = std::shared_ptr<IConnection>;
```

### 2.2 INetServer 接口

抽象 TCP 服务器的生命周期管理和事件回调注入。

```cpp
#pragma once
#include <functional>
#include <memory>
#include "IConnection.h"

class INetServer {
public:
    using ConnCallback = std::function<void(spIConnection)>;

    virtual ~INetServer() = default;

    // 生命周期
    virtual void start() = 0;
    virtual void Stop() = 0;

    // 事件回调注册
    virtual void setnewconnectioncb(ConnCallback cb) = 0;
    virtual void setcloseconnectioncb(ConnCallback cb) = 0;
    virtual void seterrorconnectioncb(ConnCallback cb) = 0;
    virtual void setonmessagecb(ConnCallback cb) = 0;
    virtual void setsendcompletecb(ConnCallback cb) = 0;
};
```

## 3. Reactor 层的无缝适配

对当前 `cppBackend/reactor` 目录的修改**仅限于增加接口继承**。原有的内部逻辑（`Epoll`, `EventLoop`, `Channel`、线程池等）一字不改，确保系统的绝对稳定。

```cpp
// 在 reactor/Connection.h 中
#include "../net/IConnection.h"
class Connection : public IConnection {
    // Connection 原本就实现了 fd(), ip(), getInputBuffer() 等方法，
    // 此处仅通过 public 继承即自动满足了 IConnection 的绝大多数要求。
    
    // 实现新增的 PostIoTask 接口
    void PostIoTask(std::function<void()> task) override {
        if (loop_) {
            loop_->queueinloop(std::move(task));
        }
    }
};

// 在 reactor/tcpserver.h 中
#include "../net/INetServer.h"
class TcpServer : public INetServer {
    // 回调签名由 spConnection 变更为 spIConnection。
    // 内部在触发 HttpServer 的回调时，将 shared_ptr<Connection> 隐式转换为 spIConnection。
};
```

## 4. Proactor (io_uring) 模块从零设计 — Thread-per-Core 架构

在全新的 `cppBackend/proactor/` 目录下从零建立网络框架。因为完全与现有的 Reactor 脱钩，所以彻底摒弃 `Channel`（事件分发）和 `Epoll`（就绪通知）概念，直接采用基于异步操作完成（Completion）的模型。

> **核心架构：Thread-per-Core**。每个 CPU 核心对应一个独立的 IO 线程（`UringWorker`），每个 `UringWorker` 持有私有的 `io_uring` 实例。新连接通过对 fd 取模的方式被**固定分配**到某个 `UringWorker`，此后该连接的全部 IO 操作（recv/send/splice）均由该 `UringWorker` 独占处理。Worker 线程池仍负责业务计算，计算完成后通过 `PostIoTask` 将响应投递回连接所属的 `UringWorker`。
>
> **与单 IO 线程方案的对比**：单线程方案的 `io_uring` 实例是全局瓶颈，单个 CQ 队列在高连接数下成为争用点，且无法利用多核并行收割。Thread-per-Core 让每个核心拥有独立的 SQ/CQ 队列与 uring 实例，天然消除了全局锁，在多核机器上吞吐量随核数线性扩展。这种架构与 Seastar、Glommio 等高性能 io_uring 框架的设计理念一致。

### 4.1 整体架构图

```text
                     ┌─────────────────────────────┐
                     │        UringServer           │
                     │  (实现 INetServer 接口)       │
                     │  - 监听 listenfd             │
                     │  - 管理 UringWorker 池       │
                     │  - 分发新连接到 Worker       │
                     └──────────┬──────────────────┘
                                │ Round-Robin / Hash 分发
            ┌───────────────────┼───────────────────┐
            ▼                   ▼                   ▼
   ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
   │  UringWorker 0  │ │  UringWorker 1  │ │  UringWorker N  │
   │ (CPU Core 0)    │ │ (CPU Core 1)    │ │ (CPU Core N)    │
   │                 │ │                 │ │                 │
   │ ┌─────────────┐ │ │ ┌─────────────┐ │ │ ┌─────────────┐ │
   │ │  io_uring   │ │ │ │  io_uring   │ │ │ │  io_uring   │ │
   │ │ (私有实例)  │ │ │ │ (私有实例)  │ │ │ │ (私有实例)  │ │
   │ └─────────────┘ │ │ └─────────────┘ │ │ └─────────────┘ │
   │                 │ │                 │ │                 │
   │ ┌─────────────┐ │ │ ┌─────────────┐ │ │ ┌─────────────┐ │
   │ │ task_queue  │ │ │ │ task_queue  │ │ │ │ task_queue  │ │
   │ │ (无锁MPSC)  │ │ │ │ (无锁MPSC)  │ │ │ │ (无锁MPSC)  │ │
   │ └─────────────┘ │ │ └─────────────┘ │ │ └─────────────┘ │
   │                 │ │                 │ │                 │
   │ ┌─────────────┐ │ │ ┌─────────────┐ │ │ ┌─────────────┐ │
   │ │  eventfd    │ │ │ │  eventfd    │ │ │ │  eventfd    │ │
   │ └─────────────┘ │ │ └─────────────┘ │ │ └─────────────┘ │
   │                 │ │                 │ │                 │
   │  独立IO线程     │ │  独立IO线程     │ │  独立IO线程     │
   └────────┬────────┘ └────────┬────────┘ └────────┬────────┘
            │                   │                   │
   ┌────────┴────────┐ ┌───────┴─────────┐ ┌───────┴─────────┐
   │ UringConnection  │ │ UringConnection │ │ UringConnection │
   │ UringConnection  │ │ UringConnection │ │ UringConnection │
   │ UringConnection  │ │ ...             │ │ ...             │
   │ ...              │ │                 │ │                 │
   └─────────────────┘ └─────────────────┘ └─────────────────┘
       (Core 0 的         (Core 1 的         (Core N 的
        连接集合)           连接集合)          连接集合)

        ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
                    Worker 线程池 (HttpServer)
                 处理完业务后通过 PostIoTask 回传
```

### 4.2 核心组件设计

#### 4.2.1 UringServer（实现 INetServer）

`UringServer` 是 Proactor 模块的顶层编排类，负责：

- **创建与管理 `UringWorker` 池**：根据配置的 IO 线程数（默认等于 CPU 核心数）创建对应数量的 `UringWorker`，每个 Worker 绑定一个 CPU 核心（通过 `pthread_setaffinity_np` 实现 CPU 亲和）。
- **监听 listenfd**：`UringServer` 自身持有 listenfd，采用两种 accept 策略之一（见 4.2.3）接受新连接。
- **连接分发**：新连接的 fd 通过 `fd % worker_count`（或更精细的 hash）分配到对应的 `UringWorker`，从此该连接的所有 IO 操作均由该 Worker 独占。
- **生命周期管理**：启动时创建所有 Worker 线程，停止时向所有 Worker 发送退出信号并等待线程汇合。

```cpp
class UringServer : public INetServer {
public:
    UringServer(const std::string& ip, uint16_t port,
                int worker_count = 0,  // 0 = 自动检测 CPU 核心数
                int uring_entries = 4096);
    ~UringServer() override;

    void start() override;
    void Stop() override;

    void setnewconnectioncb(ConnCallback cb) override;
    void setcloseconnectioncb(ConnCallback cb) override;
    void seterrorconnectioncb(ConnCallback cb) override;
    void setonmessagecb(ConnCallback cb) override;
    void setsendcompletecb(ConnCallback cb) override;

private:
    int listenfd_;
    std::vector<std::unique_ptr<UringWorker>> workers_;
    std::atomic<uint64_t> next_worker_idx_{0};

    void DispatchConnection(int connfd, const std::string& ip, uint16_t port);
    void AcceptLoop();
};
```

#### 4.2.2 UringWorker（Per-Core IO 线程 + io_uring 实例）

`UringWorker` 是 Thread-per-Core 架构的核心，是真正执行异步 IO 的单元。每个 `UringWorker` 持有：

- **私有的 `io_uring` 实例**：完全独立的 SQ/CQ 队列，无任何全局竞争。
- **独立 IO 线程**：循环执行 `io_uring_submit_and_wait()` 收割 CQE。IO 线程不进行任何业务计算，仅负责异步操作的提交与完成事件收割。
- **无锁 MPSC 任务队列**：供 Worker 线程池（HttpServer 的业务线程）向该 IO 线程回传发送任务。Multi-Producer（多个 Worker 线程并发入队）Single-Consumer（仅本 IO 线程出队）。
- **eventfd**：当任务入队时写入 eventfd，触发 `io_uring` 的 `IORING_OP_READ` 完成，唤醒可能阻塞在 `submit_and_wait` 中的 IO 线程。
- **连接注册表**：`std::unordered_map<int, std::shared_ptr<UringConnection>>`，维护分配到本 Worker 的所有活跃连接。

```cpp
class UringWorker {
public:
    explicit UringWorker(int worker_id, int uring_entries = 4096);
    ~UringWorker();

    void Start();
    void Stop();

    void AddConnection(std::shared_ptr<UringConnection> conn);
    void RemoveConnection(int fd);

    void QueueTask(std::function<void()> task);

    io_uring* uring() { return &ring_; }
    int worker_id() const { return worker_id_; }

private:
    int worker_id_;
    io_uring ring_;
    int eventfd_;

    std::thread io_thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<int, std::shared_ptr<UringConnection>> connections_;
    std::mutex conns_mutex_;

    struct MPSCNode {
        std::function<void()> task;
        MPSCNode* next{nullptr};
    };
    std::atomic<MPSCNode*> mpsc_head_{nullptr};

    void IoLoop();
    void ProcessCQE(io_uring_cqe* cqe);
    void DrainTaskQueue();
    void FlushSubmitted();
};
```

#### 4.2.3 Accept 策略

Thread-per-Core 架构下有两种 accept 实现策略：

**策略 A：主线程 Accept + 分发（推荐，Phase 2 采用）**

```text
main thread                    UringWorker 0   UringWorker 1   ...   UringWorker N
    │                              │               │                     │
    │  io_uring_wait_cqe()         │               │                     │
    │  ◄── IORING_OP_ACCEPT ──┐    │               │                     │
    │                          │    │               │                     │
    │  accept 返回 connfd      │    │               │                     │
    │  hash(connfd) → Worker N ─────┼───────────────┼────────────────────►│
    │                              │               │              AddConnection()
    │  重新提交 ACCEPT SQE     │    │               │              提交 RECV SQE
    │  ┌───────────────────────┘    │               │                     │
    │  ...                          │               │                     │
```

- `UringServer` 在主线程（或独立 accept 线程）中使用一个专用的小型 `io_uring` 实例，循环提交 `IORING_OP_ACCEPT`。
- 每次收到新连接后，通过 `fd % worker_count` 选择目标 Worker，将 connfd 投递给该 Worker。
- 优点：实现简单，连接分配均匀可控；listenfd 不需要在多个 uring 实例间共享。
- 缺点：accept 线程是单点，但在实践中 accept 本身不会成为瓶颈（内核 5.19+ 已支持 multi-accept）。

**策略 B：SO_REUSEPORT 多监听（Phase 4 可选优化）**

- 每个 `UringWorker` 各自创建独立的 listenfd（通过 `SO_REUSEPORT` 允许多个 socket 监听同一端口）。
- 内核负责将新连接均匀分配到各 Worker 的 listenfd 上。
- 完全消除 accept 单点，但需内核 3.9+，且调试/连接计数更复杂。
- **留作 Phase 4 性能压测阶段视需要引入**。

#### 4.2.4 UringConnection（实现 IConnection）

`UringConnection` 处理单连接的读写逻辑。连接从创建到销毁始终隶属于同一个 `UringWorker`，完全避免了跨线程访问缓冲区的竞态问题。

```cpp
class UringConnection : public IConnection {
public:
    UringConnection(int fd, const std::string& ip, uint16_t port,
                    UringWorker* owner);
    ~UringConnection() override;

    int fd() const override { return fd_; }
    std::string ip() const override { return ip_; }
    uint16_t port() const override { return port_; }
    bool IsDisconnected() const override { return disconnected_; }

    BufferBlock& getInputBuffer() override { return inputbuffer_; }
    BufferBlock& getOutputBuffer() override { return outputbuffer_; }

    void send() override;
    void setCloseOnSendComplete(bool close) override;
    void StartSendFile(int fd, off_t offset, size_t length, bool auto_close) override;
    void ClearSendFile() override;

    void PostIoTask(std::function<void()> task) override {
        owner_->QueueTask(std::move(task));
    }

    void SetTlsContext(std::shared_ptr<TlsContext> ctx) override;

    void SubmitRecv();
    void HandleRecvComplete(int result);
    void HandleWriteComplete(int result);
    void HandleSpliceComplete(int result);

private:
    int fd_;
    std::string ip_;
    uint16_t port_;
    bool disconnected_{false};
    UringWorker* owner_;

    BufferBlock inputbuffer_;
    BufferBlock outputbuffer_;

    bool close_on_send_complete_{false};

    struct SendFileState {
        int file_fd{-1};
        off_t offset{0};
        size_t remaining{0};
        bool auto_close{false};
        int pipe_fds[2]{-1, -1};
    } sendfile_;

    struct RecvBuffer {
        void* buf{nullptr};
        size_t size{0};
    } recv_buf_;

    std::shared_ptr<TlsContext> tls_ctx_;
};
```

### 4.3 事件驱动链路 (Proactor 纯异步)

#### 4.3.1 接收请求（持续 Recv）

```text
UringWorker (IO线程)                  Worker线程池
      │                                    │
      │ SubmitRecv()                       │
      │ → IORING_OP_RECV SQE              │
      │                                    │
      │ io_uring_submit_and_wait()         │
      │                                    │
      │ ◄── CQE (recv完成,数据已在Buffer)  │
      │                                    │
      │ HandleRecvComplete(result)         │
      │ → onmessagecb(conn) ──────────────►│ HandleMessage()
      │                                    │ → 入任务队列
      │                                    │ → Worker执行业务
      │ 立即提交新的 RECV SQE              │
      │ (保持持续读,无阻塞)                 │
      │                                    │
```

- `UringConnection` 建立时，由所属 `UringWorker` 直接提交 `IORING_OP_RECV`（附带预先注册好的内存 Buffer）。
- 当 CQE 唤醒 IO 线程时，表示数据**已经躺在** Buffer 中。IO 线程直接将收到的数据 + 连接打包，调用 `onmessagecb` 触发 HttpServer。
- HttpServer 的 `HandleMessage` 将数据推入任务队列，交由 Worker 线程池处理（此时 IO 线程已返回）。
- **紧接着，IO 线程立即向该连接提交一个新的 `IORING_OP_RECV`，保持持续读，整个过程无任何业务阻塞。**

#### 4.3.2 发送响应（Write）

```text
Worker线程池                          UringWorker (IO线程)
      │                                    │
      │ 完成业务计算                        │
      │ conn->PostIoTask(lambda)           │
      │ → MPSC 队列入队                    │
      │ → eventfd 触发写入 ────────────────►│
      │                                    │ ◄── eventfd READ CQE 唤醒
      │                                    │ DrainTaskQueue()
      │                                    │ → 执行 lambda: 填充outputbuffer_
      │                                    │ → send()
      │                                    │ → 提交 IORING_OP_WRITEV SQE
      │                                    │
      │                                    │ io_uring_submit_and_wait()
      │                                    │ ◄── CQE (write完成)
      │                                    │ HandleWriteComplete()
      │                                    │ → sendcompletecb / 清理
```

- Worker 线程完成计算后，通过 `UringConnection::PostIoTask` 将序列化好的数据回传给连接所属的 `UringWorker`。
- `PostIoTask` 内部将任务压入该 Worker 的无锁 MPSC 队列，并向 eventfd 写入 8 字节触发唤醒。
- IO 线程被 eventfd 的 CQE 唤醒后，`DrainTaskQueue()` 依次取出任务执行。此时处于该 Worker 的 IO 线程上下文中，可以安全地操作该 Worker 管理的连接缓冲区。
- 执行时填满 `outputbuffer_` 并调用 `send()`。`UringConnection` 从缓冲中提取 `iovec`，提交 `IORING_OP_WRITEV`。内核在后台搬运数据，完成后返回 CQE 触发清理。

#### 4.3.3 零拷贝发文件（Splice）

```text
UringWorker (IO线程)
      │
      │ StartSendFile(file_fd, offset, length)
      │ → pipe() 创建管道 pipe_fds_[0/1]
      │ → 提交 IORING_OP_SPLICE:
      │     file_fd → pipe_fds_[1] (length字节)
      │ → 完成后串联提交:
      │     pipe_fds_[0] → connfd (length字节)
      │ → 重复直到 remaining == 0
      │ → 若 auto_close: close(file_fd)
      │
      │ 注意: SPLICE 在同一 io_uring 实例内
      │ 可利用 IOSQE_IO_LINK 实现链式提交,
      │ 内核自动串联执行,无需中间唤醒
```

- 遇到大文件分发时，直接提交 `IORING_OP_SPLICE` 指令，让内核在内部完成从文件描述符到 Socket 的直传，取代原有的 `sendfile` 阻塞调用。
- 利用 `IOSQE_IO_LINK` 可以将"文件→管道"和"管道→Socket"两步 SPLICE 链式提交，内核自动串联执行，进一步减少 IO 线程的介入次数。

### 4.4 Thread-per-Core 的线程安全保证

Thread-per-Core 架构从设计层面消除了绝大多数线程安全问题：

| 对象 | 所属线程 | 跨线程访问方式 |
|------|----------|----------------|
| `UringConnection` 缓冲区 | 固定一个 IO 线程 | Worker 通过 `PostIoTask` 投递回 IO 线程执行 |
| `io_uring` 实例的 SQ/CQ | 固定一个 IO 线程 | 仅该 IO 线程提交 SQE 和收割 CQE |
| `UringWorker::connections_` | 固定一个 IO 线程 | 仅该 IO 线程读写（Main thread 仅在启动时 AddConnection） |
| MPSC 任务队列 | 多写单读 | 无锁 MPSC，Worker 线程并发入队，IO 线程单消费者出队 |
| `UringConnection::PostIoTask` | Worker 线程调用 | 内部走 MPSC 队列 + eventfd，无锁安全 |

**核心原则**：连接的数据（缓冲区、状态）永远只被其所属的 IO 线程访问。Worker 线程只做业务计算，计算完毕后将回写操作以任务形式投递回对应的 IO 线程。这比单 IO 线程模型更进一步——不仅 IO 和业务分离，**IO 本身也在多个核心上并行**，且各核心的连接数据完全隔离，无需任何锁同步。

### 4.5 CPU 亲和性与 NUMA 感知

```cpp
void UringWorker::Start() {
    io_thread_ = std::thread([this]() {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(worker_id_, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        IoLoop();
    });
}
```

- 每个 `UringWorker` 的 IO 线程通过 `pthread_setaffinity_np` 绑定到对应的 CPU 核心，避免线程在核心间迁移导致的缓存失效。
- 在 NUMA 架构的机器上，`io_uring` 的 SQ/CQ 队列内存和连接的 Buffer 内存默认由所在核心的 NUMA 节点分配，确保本地访问延迟最低。
- 后续可进一步在 `io_uring_params` 中使用 `IORING_SETUP_COOP_TASKRUN` 等标志减少内核侧的跨核唤醒。

### 4.6 TLS 降级处理

由于 OpenSSL 当前仍需依赖用户态同步读写，在 `UringConnection` 处理 TLS 请求时：
- 对 TLS Socket 提交 `IORING_OP_POLL_ADD`，该指令会让 io_uring 表现得像 epoll。
- 当 CQE 提示就绪后，回调中调用 `SSL_read`/`SSL_write`（与 Reactor 的处理表现完全一致）。
- 因为 TLS 连接同样被固定在某个 `UringWorker` 上，`SSL_read`/`SSL_write` 在对应的 IO 线程中执行，不存在跨线程并发访问 `SSL*` 的问题。

### 4.7 与 Reactor 主从模型的架构对照

| 维度 | Reactor (主从 epoll) | Proactor (Thread-per-Core io_uring) |
|------|---------------------|--------------------------------------|
| IO 多路复用 | epoll_wait | io_uring_submit_and_wait |
| 事件模型 | 就绪通知（Ready） | 完成通知（Completion） |
| 线程模型 | MainLoop(accept) + SubLoop 池 | Accept 线程 + Per-Core Worker 池 |
| 连接分配 | Round-Robin 到 SubLoop | fd % N 固定到 Worker |
| 跨线程回写 | EventLoop::queueinloop + eventfd | UringWorker::QueueTask + eventfd |
| 发文件 | sendfile 系统调用 | IORING_OP_SPLICE（链式） |
| TLS | EPOLLIN/EPOLLOUT + SSL_read/write | IORING_OP_POLL_ADD + SSL_read/write |
| CPU 亲和 | 无绑定 | 每个 Worker 绑定核心 |
| 全局锁 | 无（per-EventLoop） | 无（per-Worker uring 实例） |

## 5. HttpServer 的无感切换实现

`HttpServer` 从依赖具体实现转为依赖抽象接口。

**HttpServer.h 变更**：
```cpp
#pragma once
#include "../net/INetServer.h"
#include "../net/IConnection.h"

class HttpServer {
private:
    std::unique_ptr<INetServer> net_server_; // 替代原本的 TcpServer tcpserver_;
    // ... 其他如 ThreadPool、Router、Phase2 背压控制成员完全保持不变
public:
    // HttpServer 构造函数中将接收已创建好的 INetServer 实例
    HttpServer(std::unique_ptr<INetServer> net_server, /* 其他业务配置 */);
};
```

**HttpServer.cpp 变更**：
内部方法如 `HandleNewConnection`、`HandleMessage` 的参数统一修改为 `spIConnection`。
特别是对于 `HttpServer::PostResultToIoLoop`，原有实现强依赖于 `EventLoop::queueinloop`：
```cpp
EventLoop* io_loop = conn->getLoop(); // 破坏抽象
io_loop->queueinloop([...]() { /* 执行回写与 send() */ });
```
现在将直接改为调用 `IConnection::PostIoTask`：
```cpp
conn->PostIoTask([...]() { /* 执行回写与 send() */ });
```

在 Proactor 模式下，`PostIoTask` 的实际效果是：将回写任务投入该连接所属 `UringWorker` 的 MPSC 队列，并通过 eventfd 唤醒对应的 IO 线程执行。`HttpServer` 无需知道是 `EventLoop::queueinloop` 还是 `UringWorker::QueueTask`，对底层网络模型的切换实现 **100% 无感**。

**与 Phase2 异步链路的兼容**：Phase2 的"IO收包 + Worker处理 + IO回写"三段式架构在 Thread-per-Core 下天然成立。IO 收包（`UringWorker` IO线程）→ Worker 处理（`ThreadPool` 工作线程）→ IO 回写（通过 `PostIoTask` 投递回 `UringWorker` IO线程），全链路连接数据始终在固定的 IO 线程上下文中操作，乱序回写缓冲和背压控制逻辑无需任何修改。

## 6. 配置系统设计

实现一个基于配置文件（或环境变量）的网络后端工厂，使得 Reactor 与 Proactor 互不干扰，运行时随心切换。

```cpp
// cppBackend/net/NetFactory.h
struct NetServerOptions {
    std::string ip;
    uint16_t port{0};
    int io_threads{6};            // Reactor: SubLoop数 / Proactor: Worker数(0=CPU核数)
    int tcp_timeout_seconds{360};
    bool opt_linger{true};
    int uring_entries{4096};      // Proactor 专用: 每个 uring 实例的 CQ 队列深度
    bool cpu_affinity{true};      // Proactor 专用: 是否绑定 CPU 核心
};

class NetFactory {
public:
    static std::unique_ptr<INetServer> CreateServer(
        const std::string& mode, const NetServerOptions& options) {
        if (mode == "proactor") {
            if (CheckIoUringKernelSupport()) {
                return std::make_unique<UringServer>(
                    options.ip, options.port,
                    options.io_threads > 0 ? options.io_threads : GetCpuCoreCount(),
                    options.uring_entries);
            } else {
                LOGWARN("Kernel version too low for io_uring, fallback to Reactor.");
            }
        }
        return std::make_unique<TcpServer>(/*...*/);
    }
};
```

**配置文件示例** (`etc/webserver.conf`)：
```ini
[net]
mode = proactor          ; reactor | proactor
io_threads = 0           ; 0 = 自动检测CPU核数
uring_entries = 4096     ; 每个 uring 实例的队列深度
cpu_affinity = true      ; 是否绑定CPU核心(仅proactor生效)
```

在程序入口 `main.cpp` 中：
```cpp
int main() {
    std::string net_mode = ReadConfig("net.mode", "reactor");
    
    auto net_server = NetFactory::CreateServer(net_mode, options);
    
    HttpServer server(std::move(net_server), ...);
    server.start();
    return 0;
}
```

## 7. 开发与测试路线图

为确保主干分支的稳定性，整个 `io_uring` 落地将分为 4 个清晰的阶段：

- **Phase 1：接口抽象与防腐层建立**（已完成）
  - 定义 `INetServer` 和 `IConnection`。
  - 让 Reactor 网络库继承接口。
  - 改造 `HttpServer` 适配接口。
  - **验收**：跑通所有的现有 Google Test 和集成测试用例，确保没有任何功能回归（Regression）。

- **Phase 2：Proactor 网络层从 0 建设 (Thread-per-Core 明文 HTTP)**（已完成）
  - 在 `cppBackend/proactor` 创建 `UringServer`、`UringWorker` 和 `UringConnection`。
  - 实现 `UringWorker`：私有 `io_uring` 实例 + 独立 IO 线程 + eventfd 唤醒 + 无锁 MPSC 任务队列。
  - 采用主线程 Accept（专用 `io_uring` 实例 + `IORING_OP_ACCEPT`）+ Round-Robin 分发策略。
  - 实现纯异步的 `IORING_OP_RECV`、`IORING_OP_WRITEV` 以及基于 eventfd 的唤醒机制。
  - 实现 CPU 亲和绑定（`pthread_setaffinity_np`）。
  - 实现 `UringConnection` 上的 `sendfile`（Phase2 暂用 `sendfile()` 系统调用通过 QueueTask 投递，后续 Phase3 升级为 `IORING_OP_SPLICE` 零拷贝）。
  - `NetFactory` 已支持 `proactor` 模式创建，`NetServerOptions` 新增 `uring_entries` 和 `cpu_affinity` 字段。
  - **验收**：编译通过，所有现有测试通过。通过配置 `net.mode=proactor` 可切换至 Proactor 后端。

- **Phase 3：Proactor 高级特性与 TLS 兼容**
  - 为 `UringConnection` 添加 `IORING_OP_SPLICE` 零拷贝文件发送功能（含 `IOSQE_IO_LINK` 链式提交）。
  - 实现 `IORING_OP_POLL_ADD`，接管 TLS 流量的优雅降级处理。
  - **验收**：在 Proactor 模式下，HTTPS 访问和超大静态文件下载功能正常。

- **Phase 4：压测与深度优化**
  - 引入 `SO_REUSEPORT` 多监听策略（策略 B），消除 Accept 单点瓶颈。
  - 引入 `SQPOLL`（内核轮询）机制，进一步减少系统调用开销。
  - 引入 `IORING_SETUP_COOP_TASKRUN` 等内核优化标志。
  - 对比 Reactor 与 Proactor 模式在不同负载下的吞吐率（QPS）、尾延迟（P99 Latency）与 CPU 占用率，重点关注多核扩展性。

- **Phase 5：Proactor 连接超时与空闲清理（⏳ 未开始）**
  - 为 Proactor 模式接入 `TimeWheel` 连接超时管理。
  - **验收**：Proactor 模式下空闲连接在配置的超时时间内被主动关闭，不存在连接泄漏。

---

## 8. Proactor 模式下的连接超时与定时器设计

### 8.1 现状差距

当前 Proactor 模式**完全没有连接超时机制**：

| 维度 | Reactor 模式 | Proactor 模式 |
|------|-------------|---------------|
| 超时管理组件 | `TcpServer` 内部持有 `TimeWheel` | 无 |
| 新连接注册 | `TcpServer::add_new_conn_timernode()` | 缺失 |
| 活跃更新 | `TcpServer::update_conn_timeout_time()` | 缺失 |
| 超时回调 | `TimeWheel::tick()` → `conn->closecallback()` | 缺失 |
| 空闲连接清理 | 有（默认 360s） | 无——连接永不超时 |

Reactor 模式下 `TcpServer` 在 `newconnection` 时调用 `time_wheel_.add_connection(conn, ts_tcp_conn_timeout_s_)`，每次收到数据后通过 `Connection::update_conn_timeout_time()` 刷新超时。`TimeWheel` 独立线程每秒 tick 一次，发现超时连接后通过 `conn->getLoop()->queueinloop()` 投递关闭操作到 IO 线程执行。

Proactor 模式下 `UringServer` 没有集成任何定时器，这意味着：
- 恶意客户端可以通过建立连接但不发数据来占用连接资源
- 异常断连（网络中断、客户端崩溃）下服务端无法感知，连接永不释放
- 长时间运行的进程会出现 fd 泄漏和内存泄漏

### 8.2 设计目标

为 Proactor 模式补齐连接超时管理，要求：
1. 复用现有 `TimeWheel` 的“时间轮算法与槽结构”，但**解耦驱动方式**（时间轮不强绑定“内部线程 + Reactor Connection”）
2. Proactor 模式下不额外引入“全局 tick 线程 + 全局锁竞争”，优先做到 **连接生命周期、超时刷新、超时关闭全部发生在连接所属 UringWorker IO 线程**
3. 不把 Reactor 的概念（`EventLoop` / `queueinloop` / `closecallback()`）强塞进 `IConnection`，避免接口膨胀与跨后端的语义污染
4. Reactor 模式行为保持不变（仍可保留当前“TimeWheel 自带线程”用法，或通过轻量包装适配）

### 8.3 改造方案

#### 8.3.1 为什么文档旧方案不是最优

把 `TimeWheel` 直接“泛化为 `spIConnection` + tick 线程里 PostIoTask(dynamic_cast<UringConnection>::DoClose)”虽然能跑通，但有明显代价：

- **接口污染**：`TimeWheel` 的 tick/关闭逻辑天然依赖“怎么关闭连接”，而 `IConnection` 当前并没有统一的“强制关闭”接口；为了兼容要么给 `IConnection` 增加 close 语义（会影响两套网络后端的边界），要么在 tick 线程里做 `dynamic_pointer_cast`（把后端类型泄露进定时器组件）。
- **线程模型不匹配**：现有 `TimeWheel` 自带独立线程与互斥锁，会引入额外的 wakeup、锁竞争与 cache miss；而 Proactor 的核心优势恰恰是“所有连接状态收敛在固定 IO 线程”，独立 tick 线程会把这点打折。
- **所有权不清晰**：Proactor 的连接实际归属 `UringWorker`，把时间轮放在 `UringServer` 或全局线程中会迫使“跨 worker 更新/关闭”，后续扩展（按 worker 分片、按连接亲和）更难。

因此更合理的做法是：让 `TimeWheel` 回归成**纯算法容器**（只输出“哪些 fd 超时了”），驱动与关闭策略由网络后端来决定。

#### 8.3.2 推荐方案：TimeWheel 核心解耦 + Per-Worker 驱动（更贴合 Proactor）

将 `TimeWheel` 拆成“核心(无线程)”与“驱动(可选线程/可选 io_uring timeout)”两层：

```cpp
// 仅示意：TimeWheelCore 不包含线程，也不依赖 Connection/IConnection
struct ExpiredTimer {
    int fd;
    uint64_t generation;
};

class TimeWheelCore {
public:
    void Add(int fd, uint64_t generation, int timeout_s);
    void Refresh(int fd, uint64_t generation, int timeout_s);
    void Remove(int fd);
    void Tick(std::vector<ExpiredTimer>* expired_out);
};
```

`generation` 依然保留（非常重要）：用于防止“旧定时器误关新连接”（fd 复用、连接对象替换等）。但 generation 不必上升到 `IConnection`，可以由 Reactor/Proactor 的具体连接类各自维护。

在 Proactor 中，把时间轮放进 `UringWorker`（而不是 `UringServer`），并由 `io_uring` 自己产生“每秒一次”的 tick 事件：

```cpp
// UringWorker.h（示意）
class UringWorker {
private:
    TimeWheelCore time_wheel_;
    int tcp_timeout_s_{360};
    void SubmitTickTimeout();        // IORING_OP_TIMEOUT：每秒触发一次 CQE
    void OnTick();                   // Tick + 关闭超时连接（在 IO 线程内执行）
    void AddConnTimer(const std::shared_ptr<UringConnection>& c);
    void RefreshConnTimer(const std::shared_ptr<UringConnection>& c);
    void RemoveConnTimer(int fd);
};
```

驱动方式建议优先用 `IORING_OP_TIMEOUT`（而不是额外线程或 timerfd），优点是：

- tick 与 IO 完成事件同源（都走 CQE），无需额外 fd/线程/唤醒
- tick 回调天然发生在 `UringWorker::IoLoop()` 线程，关闭连接可直接调用 `UringConnection::DoClose()`，不需要跨线程 `PostIoTask`

Proactor 生命周期对齐点（都在 worker IO 线程）：

- `HandleAccept()` 创建 `UringConnection` 后：`AddConnTimer(conn)`
- 每次成功收包并判定“连接活跃”时：`RefreshConnTimer(conn)`
- 连接关闭/异常时：`RemoveConnTimer(fd)`（或在 `DoClose()`/`RemoveConnection()` 里统一触发）

```cpp
// UringWorker.cpp（示意）
void UringWorker::OnTick() {
    std::vector<ExpiredTimer> expired;
    time_wheel_.Tick(&expired);

    for (const auto& e : expired) {
        auto conn = GetConnection(e.fd); // worker 内部 conns_ map 查找
        if (!conn || conn->IsDisconnected()) continue;
        if (conn->GetTimerGeneration() != e.generation) continue;
        conn->DoClose();
    }
    SubmitTickTimeout(); // 重新提交下一次 tick
}
```

#### 8.3.3 Reactor 模式保持兼容

Reactor 现有实现是：`TimeWheel` 自带线程 tick，然后通过 `conn->getLoop()->queueinloop(...)` 关闭连接。若按上面的“TimeWheelCore”解耦：

- Reactor 可以继续保留“TimeWheelThread（线程驱动）”，其 `OnTick()` 里从 `TcpServer::conns_` 查找连接并 `queueinloop(closecallback)`，从而保持对现有行为 100% 兼容。
- Proactor 则不启用线程驱动，改为 `io_uring timeout` 驱动。

这样能做到：同一套时间轮算法，**两种网络后端采用各自最自然的驱动方式**。

#### 8.3.4 Generation 代际校验（两后端一致）

Reactor 模式下 `TimeWheel` 依赖 `Connection::GetTimerGeneration()` 和 `Connection::BumpTimerGeneration()` 做代际校验，防止关闭一个已经被复用 fd 的新连接。

Proactor 推荐同样在 `UringConnection` 上增加 `timer_generation_`，并在每次 `Add/Refresh` 时调用 `BumpTimerGeneration()`，tick 时比对 `GetTimerGeneration()`。generation 不必进入 `IConnection`：时间轮输出的是 `(fd, generation)`，由后端自行校验并关闭。

### 8.4 变更范围

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `timer/TimeWheel.h/.cpp` | 修改 | 解耦为“TimeWheelCore(无线程、无 Connection 依赖)”与“可选驱动层(线程/外部驱动)” |
| `proactor/UringWorker.h/.cpp` | 修改 | 每个 worker 持有 TimeWheelCore；用 `IORING_OP_TIMEOUT` 驱动 tick；在 accept/message/close 上 add/refresh/remove |
| `proactor/UringConnection.h` | 修改 | 增加 `timer_generation_` 与 `Bump/GetTimerGeneration()`（用于 fd 复用保护） |
| `reactor/TcpServer.*` | 修改 | 适配 TimeWheelCore 的输出：从 conns_ 定位连接并 `queueinloop(closecallback)`（行为不变） |

### 8.5 验收标准

- Proactor 模式下启动，每个 worker 都能周期性触发 tick（`IORING_OP_TIMEOUT`），无需额外 TimeWheel 线程
- 新连接建立后被注册到所属 worker 的时间轮；收到数据后能刷新超时
- 客户端建立连接后不发送任何数据，在配置的超时时间（如 30s）后被服务端主动关闭
- 超时关闭发生在 worker IO 线程内（无需跨线程 PostIoTask），连接被正常清理（fd 释放、连接从 worker conns_ 移除）
- Reactor 模式下的所有测试仍然通过（Reactor 的超时语义保持不变）
- 模拟 1000 个空闲连接，在超时后全部被回收，无 fd 泄漏
