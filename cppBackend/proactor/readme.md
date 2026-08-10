# Proactor 模块文档（基于 io\_uring）

> **目标读者**：刚接触本项目的后台开发工程师。本文档假设你有基本的 Linux 网络编程和 C++17 基础，但对 io\_uring 和本模块的实现细节不熟悉。

***

## 目录

1. [模块定位](#1-模块定位)
2. [架构全景](#2-架构全景)
3. [三个核心类](#3-三个核心类)
4. [启动流程](#4-启动流程)
5. [数据收发全链路](#5-数据收发全链路)
6. [零拷贝文件传输（splice）](#6-零拷贝文件传输splice)
7. [TLS 支持与 POLL 回退](#7-tls-支持与-poll-回退)
8. [连接超时管理（时间轮）](#8-连接超时管理时间轮)
9. [Cross-Thread 任务投递](#9-cross-thread-任务投递)
10. [内核特性说明](#10-内核特性说明)
11. [关键配置项](#11-关键配置项)
12. [与 Reactor 的对比](#12-与-reactor-的对比)
13. [推荐阅读顺序](#13-推荐阅读顺序)

***

## 1. 模块定位

Proactor 是项目的第二个网络后端实现，基于 Linux 5.1+ 的 **io\_uring** 系统调用。上层 HttpServer 通过 `INetServer` / `IConnection` 纯虚接口与网络层解耦，所以 **Proactor 和 Reactor 可以无缝切换**。

```
切换方式：环境变量 WEBSERVER_NET_MODE=proactor
        （默认为 reactor）

创建入口：NetFactory::CreateServer() → 根据 mode 决定 new TcpServer 还是 new UringServer
```

** Proactor 与 Reactor 的核心区别**：

| <br />     | Reactor (epoll)                               | Proactor (io\_uring)                             |
| ---------- | --------------------------------------------- | ------------------------------------------------ |
| 事件通知       | epoll\_wait 告知 fd 可读/可写，**用户负责实际 read/write** | io\_uring CQE 告知 I/O **已完成**，内核替你做完了 read/write  |
| syscall 次数 | 至少 2 次/epoll 事件 + 1 次 read/write              | 提交 SQE 后内核一次性完成，1 个 CQE 对应全部完成                   |
| 零拷贝        | sendfile()，文件→socket                          | splice()，文件→pipe→socket，更灵活                      |
| TLS        | epoll POLLOUT + SSL\_read/write（用户态）          | kTLS 时走原生 read/write/splice；否则退回 POLL\_ADD + SSL |

**适用场景**：高吞吐静态文件服务、大并发短连接、希望减少 syscall 开销的场景。

***

## 2. 架构全景

```
┌──────────────────────────────────────────────────────────────────┐
│                         HttpServer                               │
│   (通过 INetServer/IConnection 接口操作，对网络模型无感)            │
└──────────────┬───────────────────────────────────────────────────┘
               │ 回调注册: SetCallbacks(...)
               ▼
┌──────────────────────────────────────────────────────────────────┐
│                      UringServer                                 │
│   - 持有 N 个 UringWorker (N = CPU 核数)                          │
│   - 维护全局 conns_ map (fd → UringConnection)                    │
│   - 转发各 worker 的回调到 HttpServer                              │
└──────┬───────────┬───────────┬───────────────────────────────────┘
       │           │           │
       ▼           ▼           ▼
┌──────────┐ ┌──────────┐ ┌──────────┐
│ Worker 0 │ │ Worker 1 │ │ Worker N │  每个Worker是一个独立的IO线程
│          │ │          │ │          │  拥有自己的 io_uring 实例
│ listenfd │ │ listenfd │ │ listenfd │  SO_REUSEPORT 多监听
│          │ │          │ │          │
│ conns_   │ │ conns_   │ │ conns_   │  每个Worker管理自己accept到的连接
│          │ │          │ │          │
│ eventfd  │ │ eventfd  │ │ eventfd  │  用于跨线程唤醒（由外部线程投递任务）
│          │ │          │ │          │
│ 时间轮   │ │ 时间轮   │ │ 时间轮   │  每1秒tick一次，超时关闭连接
└──────────┘ └──────────┘ └──────────┘
```

**核心设计原则**：

- **Thread-per-Core**：每个 CPU 核一个 UringWorker 线程，避免锁竞争和上下文切换
- **SO\_REUSEPORT**：每个 Worker 各自 bind+listen 同一个端口，内核负责将新连接分发到不同 Worker
- **IO 线程是连接的所有者**：一个连接的全部 I/O 操作（读、写、定时器）都发生在它所属的那个 Worker 线程上

***

## 3. 三个核心类

### 3.1 UringServer（文件：UringServer.h / .cpp）

**角色**：服务器入口，实现 `INetServer` 接口。对上暴露回调注册，对下管理所有 Worker。

**关键职责**：

- `start()`：创建 `num_workers_`（默认=CPU核数）个 UringWorker，各 Worker 独立 `InitListenSocket()` + `Start()`
- `Stop()`：停止所有 Worker
- 将 Worker 的 5 个回调（新连接/关闭/错误/消息/发送完成）转发给 HttpServer

**代码入口**：[UringServer.cpp](UringServer.cpp)

***

### 3.2 UringWorker（文件：UringWorker.h / .cpp）

**角色**：**模块核心**。每个 Worker 是一个独立的 IO 线程，拥有自己的 `io_uring` 实例。

**关键数据结构**：

| 成员                                                             | 用途                                                      |
| -------------------------------------------------------------- | ------------------------------------------------------- |
| `io_uring ring_`                                               | 内核态 io\_uring 实例                                        |
| `int listen_fd_`                                               | 该 Worker 自己的监听 socket（SO\_REUSEPORT）                    |
| `int eventfd_`                                                 | 跨线程唤醒：外部线程投递任务后 write eventfd，Worker 被 io\_uring CQE 唤醒 |
| `unordered_map<int, shared_ptr<UringConnection>> connections_` | 该 Worker 管理的所有连接                                        |
| `IoTimeWheel time_wheel_`                                      | 1秒粒度、60槽的时间轮，管理 TCP 空闲超时                                |
| `atomic<TaskNode*> mpsc_head_`                                 | 无锁 MPSC 队列头，接收外部线程投递的任务                                 |
| `AcceptSlot accept_slots_[32]`                                 | 预分配的 accept 槽位，支持最多 32 个并发 accept SQE                   |

**核心操作类型（OpType）**：

| OpType             | io\_uring 操作         | 说明                      |
| ------------------ | -------------------- | ----------------------- |
| `RECV`             | `IORING_OP_RECV`     | 异步读取明文数据                |
| `WRITEV`           | `IORING_OP_WRITEV`   | 异步 scatter-gather 写入    |
| `ACCEPT`           | `IORING_OP_ACCEPT`   | 异步接受新连接                 |
| `SPLICE_FILE2PIPE` | `IORING_OP_SPLICE`   | 零拷贝：文件 → pipe           |
| `SPLICE_PIPE2SOCK` | `IORING_OP_SPLICE`   | 零拷贝：pipe → socket       |
| `POLL_ADD`         | `IORING_OP_POLL_ADD` | 注册 fd 可读/可写事件（TLS 回退路径） |
| `TIMEOUT_TICK`     | `IORING_OP_TIMEOUT`  | 1 秒定时器，驱动时间轮 tick       |
| `EVENTFD_READ`     | `IORING_OP_READ`     | 读取 eventfd，用于唤醒 IO 线程   |

**代码入口**：[UringWorker.cpp](UringWorker.cpp) 中的 `IoLoop()` 和 `ProcessCQE()`

***

### 3.3 UringConnection（文件：UringConnection.h / .cpp）

**角色**：代表一个 TCP 连接，实现 `IConnection` 接口。

**关键成员**：

| 成员                                     | 用途                                    |
| -------------------------------------- | ------------------------------------- |
| `inputbuffer_` / `outputbuffer_`       | 读写缓冲区（复用 Reactor 的 BufferBlock）       |
| `sendfile_` (SendFileState)            | 文件传输状态：fd、offset、remaining、pipe 两端 fd |
| `recv_buf_[8192]`                      | 8KB 固定接收缓冲区                           |
| `tls_session_`                         | TLS 会话（握手、SSL\_read、SSL\_write）       |
| `recv_submitted_` / `write_submitted_` | 防重入标志：同一方向同时只能有一个 SQE 在飞行             |
| `timer_generation_`                    | 定时器代数，每次 IO 活动递增，用于检测过期定时器            |

**代码入口**：[UringConnection.cpp](UringConnection.cpp)

***

## 4. 启动流程

```
main()
  └─ NetFactory::CreateServer("proactor", options)
       └─ new UringServer(ip, port, cfg, opt_linger)
            └─ UringServer::start()
                 ├─ 计算 num_workers_ = CPU 核数
                 │
                 └─ for i in 0..num_workers_:
                      ├─ new UringWorker(i, cfg)
                      ├─ worker->SetCallbacks(...)   // 注册5个回调到 UringServer
                      ├─ worker->InitListenSocket(ip, port, opt_linger)
                      │    ├─ socket(SOCK_NONBLOCK)
                      │    ├─ setsockopt(SO_REUSEPORT)    ← 关键: 多worker共享端口
                      │    ├─ bind() + listen(SOMAXCONN)
                      │    └─ 返回 true
                      │
                      └─ worker->Start()
                           ├─ io_uring_queue_init_params(cfg.uring_entries, &ring_)
                           │    └─ 可选的 SQPOLL / COOP_TASKRUN / SINGLE_ISSUER flags
                           │
                           ├─ eventfd() + SubmitEventFdRead()  // 注册唤醒机制
                           ├─ for i in 0..32: SubmitAccept()   // 批量提交 accept SQE
                           ├─ SubmitTickTimeout()              // 启动 1 秒超时定时器
                           │
                           └─ 启动 IO 线程:
                                ├─ 可选: pthread_setaffinity_np() 绑定 CPU
                                └─ IoLoop()
                                     └─ 无限循环:
                                          io_uring_wait_cqe → ProcessCQE → DrainTaskQueue
```

**关键理解**：`start()` 是同步阻塞的——主线程在创建完所有 Worker 后进入 `sleep` 循环保持进程存活。所有真正的 I/O 工作都在各 Worker 线程中进行。

***

## 5. 数据收发全链路

### 5.1 接受新连接（Accept）

```
                内核                              Worker线程                          上层
                 │                                  │                                 │
  客户端SYN ──→  │                                  │                                 │
                 │  SO_REUSEPORT 分发到某 Worker      │                                 │
                 │                                  │                                 │
                 │  IORING_OP_ACCEPT CQE             │                                 │
                 │  ──────────────────────────────→  ProcessCQE(ACCEPT)               │
                 │                                  │  ├─ HandleAccept(fd, addr)      │
                 │                                  │  │   └─ new UringConnection    │
                 │                                  │  ├─ AddConnection(conn)         │
                 │                                  │  │   └─ connections_[fd] = conn │
                 │                                  │  │   └─ AddConnTimer(conn)     │
                 │                                  │  ├─ 回调 new_conn_cb_(conn)     │
                 │                                  │  │   └─ UringServer::           │
                 │                                  │  │      HandleWorkerNewConnection│
                 │                                  │  │   ─────────────────────────→ HttpServer::
                 │                                  │  │                              HandleNewConnection
                 │                                  │  └─ QueueTask: conn->SubmitRecv()│            │
                 │                                  │                                  │            │
                 │                                  │  SubmitAccept() // 补充 accept 槽位│           │
```

### 5.2 接收请求数据（Recv → HttpServer::HandleMessage）

```
  SubmitRecv()
    │
    ├─ 明文 HTTP:
    │   └─ SubmitRead(conn)
    │        └─ io_uring_prep_recv(sqe, fd, recv_buf_, 8192, 0)
    │             └─ io_uring_submit()
    │                  │
    │                  │ 数据到达 CQE
    │                  ▼
    │             ProcessCQE(RECV) → HandleRecvComplete(result)
    │                  ├─ inputbuffer_.append(recv_buf_, result)
    │                  ├─ NotifyMessage(conn) → message_cb_(conn)
    │                  │    └─ HttpServer::HandleMessage(conn)
    │                  │         └─ 从 inputbuffer 取数据 → 背压检查 → 投递给 ThreadPool worker
    │                  └─ if (!disconnected) SubmitRecv()  // 重新提交，形成读循环
    │
    └─ TLS (无kTLS RX):
        └─ SubmitTlsPoll(POLLIN|POLLOUT)
             └─ 详见第7节 TLS支持
```

**关键点**：每次 `HandleRecvComplete` 成功后，立即再次 `SubmitRecv()`，形成持续的"读循环"。`recv_submitted_` 标志确保同一时刻只有一个 RECV SQE 在飞行。

### 5.3 发送响应数据（Worker → IO 线程 → Write）

```
  HttpServer worker 线程处理完业务
    │
    └─ PostResultToIoLoop(...)
         └─ conn->PostIoTask([写入outputbuffer + send的lambda])
              │
              │ 这是跨线程调用！
              ▼
         UringWorker::QueueTask(lambda)
              ├─ new TaskNode → MPSC 链表入队 (无锁)
              └─ write(eventfd, 1) → 内核唤醒 Worker 线程
                   │
                   │ EventFD CQE 到达
                   ▼
              IoLoop → DrainTaskQueue()
                   └─ 在 IO 线程中执行 lambda:
                        ├─ outputbuffer_.append(response_data)
                        ├─ 可选: conn->StartSendFile(fd, offset, length)
                        └─ conn->send()
                             │
                             ├─ 普通响应 → SubmitWrite(conn)
                             │    └─ io_uring_prep_writev(sqe, fd, iovs, count, 0)
                             │         └─ CQE → HandleWriteComplete(result)
                             │              ├─ outputbuffer_.consumeBytes(result)
                             │              ├─ 如果 outputbuffer 还有数据 → 继续 send()
                             │              └─ 如果 close_on_send_complete_ → DoClose()
                             │
                             └─ 有文件 → SubmitSpliceChain()
                                   └─ 详见第6节
```

***

## 6. 零拷贝文件传输（splice）

这是 Proactor 相比 Reactor 最大的性能优势之一。使用 `io_uring` 的 `IORING_OP_SPLICE` 在内核中直接搬运数据，**完全绕过用户态内存**。

### 6.1 工作原理

```
  ┌──────────┐    splice()     ┌──────────┐    splice()     ┌──────────┐
  │  文件页缓存 │  ────────────→  │  pipe    │  ────────────→  │  socket  │
  │  (page    │   内核零拷贝     │ [1MB]    │   内核零拷贝     │  (TCP)   │
  │   cache)  │                │          │                │          │
  └──────────┘                └──────────┘                └──────────┘
```

每次 splice 传输最多 1MB（与 pipe 的 `F_SETPIPE_SZ` 容量一致）。

### 6.2 关键函数调用链

```
  conn->send()
    └─ sendfile_.active == true
         └─ SubmitSpliceChain()                         // UringConnection
              └─ owner_->SubmitSendFile(conn)            // → UringWorker
                   ├─ io_uring_prep_splice(sqe1, file_fd → pipe_wr)
                   │    sqe1->flags |= IOSQE_IO_LINK;    // 链接两个 SQE！
                   │
                   ├─ 第一个 SQE 的 user_data 不设置 (内核在 SQE1 成功后自动执行 SQE2)
                   │
                   ├─ io_uring_prep_splice(sqe2, pipe_rd → sock_fd)
                   │    sqe2->user_data = ud (OpType::SPLICE_PIPE2SOCK)
                   │
                   └─ io_uring_submit()
                        │
                        │ CQE 到达
                        ▼
                   ProcessCQE(SPLICE_PIPE2SOCK) → HandleSplicePipe2Sock(nsent)
                        ├─ nsent > 0: AdvanceSendFile(nsent)
                        │    └─ remaining > 0? → SubmitSpliceChain() 继续
                        ├─ nsent == EAGAIN: SubmitSpliceChain() 重试
                        └─ nsent == 0 或 remaining==0: CompleteSendFile() → NotifySendComplete
```

### 6.3 HandleSpliceFile2Pipe 为什么只处理完成？

```
  HandleSpliceFile2Pipe(nspliced):    // 只在 nspliced == 0 时有动作
  HandleSplicePipe2Sock(nsent):       // 承担全部进度跟踪和重试
```

原因：

- **File→Pipe** 是本地内核操作，pipe 容量(1MB) ≥ 单次 splice chunk，所以总能一次性完成
- **Pipe→Socket** 受 TCP 拥塞控制和接收窗口限制，可能短写（partial write），需要 `AdvanceSendFile` 跟踪剩余字节并通过 `SubmitSpliceChain` 发起下一轮 splice

### 6.4 Pipe 的生命周期

```
  StartSendFile(file_fd, offset, length)
    ├─ pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC)
    ├─ fcntl(F_SETPIPE_SZ, 1MB)     // 扩大 pipe 容量到 1MB
    └─ sendfile_.active = true
         │
         │ ... 传输过程中 pipe 被反复使用 ...
         │
  CompleteSendFile()
    └─ sendfile_.ClosePipe()        // close(pipe_fds[0]) + close(pipe_fds[1])
```

***

## 7. TLS 支持与 POLL 回退

### 7.1 决策树

```
              SetTlsContext() 设置 → tls_session_
                     │
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
  收到首包         发送方向          文件发送
     │               │               │
     ├ kTLS RX?     ├ kTLS TX?     ├ kTLS TX?
     │ YES→RECV     │ YES→WRITEV   │ YES→SPLICE
     │               │               │
     │ NO→POLL_ADD  │ NO→POLL_ADD  │ NO→目前不支持(会走普通SSL_write)
     │  (POLLIN)    │  (POLLOUT)   │
```

### 7.2 四种退回 POLL 的场景

| 场景                   | 触发条件                                            | 处理方式                                                              |
| -------------------- | ----------------------------------------------- | ----------------------------------------------------------------- |
| **TLS 握手**           | `tls_poll_state_ == HANDSHAKE`                  | `POLL_ADD(POLLIN\|POLLOUT)` → `DriveHandshake()` 推进 OpenSSL 握手状态机 |
| **TLS 读（无 kTLS RX）** | `IsTlsActive() && !KtlsRx()`                    | `POLL_ADD(POLLIN\|POLLOUT)` → `ReadPlain()` 用户态 SSL\_read 解密      |
| **TLS 写（无 kTLS TX）** | `IsTlsActive() && HandshakeDone() && !KtlsTx()` | `POLL_ADD(POLLIN\|POLLOUT)` → `WritePlain()` 用户态 SSL\_write 加密    |
| **TLS 重协商**          | 握手完成后 OpenSSL 触发 rekey                          | ReadPlain/WritePlain 内部返回 WANT\_READ 或 WANT\_WRITE，重新注册 POLL      |

### 7.3 HandleTlsPollCQE 的循环处理

```cpp
HandleTlsPollCQE(revents):
  did_work = true
  while (did_work):
    if (握手未完成):
      DriveHandshake() → OK → 继续循环 | WANT_READ/WRITE → 设定 next_mask → break
    if (revents & POLLIN):
      if (kTLS RX): ::read(fd, buf)  // 内核已解密
      else: ReadPlain(buf)           // 用户态 SSL_read
    if (revents & POLLOUT):
      if (!kTLS TX): WritePlain()    // 用户态 SSL_write 最多4次
    if (next_mask != 0): SubmitTlsPoll(next_mask)  // 重新注册
```

**关键设计**：每次 POLL 事件到达时用 `while(did_work)` 循环尽可能多地处理数据（例如连续 SSL\_read 直到 WANT\_READ），减少 POLL\_ADD 的提交次数。

***

## 8. 连接超时管理（时间轮）

### 8.1 为什么用时间轮

每个连接都需要独立超时（默认 360 秒空闲断开）。如果用 `IORING_OP_TIMEOUT` per-connection，会耗尽 io\_uring 的 SQE/CQE 资源；用全局 timerfd 则无法区分具体哪个连接超时。

**解决方案**：一个全局 1 秒粒度的 IoTimeWheel（60 槽），每秒 tick 一次，遍历当前槽位检查超时。

### 8.2 时间轮结构

```
  slots: [0]  ←→  [1]  ←→  [2]  ←→ ... ←→  [59]
          ↑
      current_slot
      (每秒前进一格)

  每个 slot 是一个 list<TimerNode>:
      TimerNode { fd, rotation, slot, generation }

  当 timeout_s = 360 秒时:
      rotation = 360 / 60 = 6    ← 需要转6圈
      slot = (current_slot + 360) % 60
```

### 8.3 工作流程

```
  SubmitTickTimeout()
    └─ io_uring_prep_timeout(sqe, &ts, 1, 0)  // 1秒后触发
         └─ CQE → HandleTickTimeout()
              ├─ time_wheel_.Tick(&expired)
              │    └─ 遍历 current_slot:
              │         rotation==0? → 过期 (检查 generation 防 ABA)
              │         rotation>0?  → rotation--
              │
              ├─ for each expired:
              │    if conn && generation 匹配:
              │      NotifyClose(conn) → DoClose()
              │
              └─ SubmitTickTimeout()  // 重新提交下一秒定时器
```

### 8.4 Generation 机制

每次 IO 活动（收包、发包）时 `AddOrRefresh(fd, ++timer_generation_, timeout_s)`，Tick 时比对 `generation`。如果连接在两次 Tick 之间被关闭后复用（fd 复用），旧定时器的 generation 不匹配会被安全跳过——防止 ABA 问题。

***

## 9. Cross-Thread 任务投递

### 9.1 问题

HttpServer 的业务处理在 ThreadPool 的 worker 线程中完成，但 `conn->send()`、`outputbuffer_` 的写入等操作必须在连接的 IO 线程中执行（线程安全要求）。如何从 worker 线程安全地把任务投递到正确的 IO 线程？

### 9.2 MPSC 队列 + eventfd

```
  Worker 线程 (非 IO 线程)                    IO 线程 (UringWorker)
  ════════════════════════                   ════════════════════════

  conn->PostIoTask(lambda)
    └─ owner_->QueueTask(lambda)
         ├─ TaskNode* node = new TaskNode{lambda}
         ├─ node->next = mpsc_head_.exchange(node)  // 无锁原子 push
         └─ write(eventfd, &val, 8)                  // 唤醒 IO 线程
                                                          │
                                             EventFD CQE 到达               │
                                                          │
                                             IoLoop → DrainTaskQueue()     │
                                              ├─ head = mpsc_head_.exchange(nullptr)  // 原子取出整条链表
                                              ├─ 翻转链表得到正确的 FIFO 顺序
                                              └─ for task in tasks: task()
```

**为什么是 MPSC（多生产者单消费者）**：多个 ThreadPool worker 线程是生产者，一个 IO 线程是消费者。

**为什么不用锁**：`atomic<TaskNode*>::exchange()` 是 lock-free 操作，避免 worker 线程和 IO 线程之间的锁竞争。

***

## 10. 内核特性说明

### 10.1 SO\_REUSEPORT

每个 Worker 独立 `socket()` + `bind()` + `listen()` 同一个端口，内核负责 TCP 连接的负载均衡分发。

**优势**：

- 消除单点 accept 瓶颈（Reactor 模式中 accept 发生在主 EventLoop 然后分发到 subloop）
- 新连接天然亲和到某个 Worker，减少跨线程连接迁移

**代码位置**：[UringWorker::InitListenSocket](UringWorker.cpp#L46-L48)

### 10.2 SQPOLL（内核轮询）

启用 `IORING_SETUP_SQPOLL` 后，内核启动一个专用线程轮询 Submission Queue，减少 `io_uring_submit()` 系统调用。

**配置**：`WEBSERVER_SQPOLL=1` 环境变量 / `NetServerOptions::sqpoll`

**注意**：SQPOLL 线程会占用一个 CPU 核，适合 IO 密集场景；CPU 密集场景慎用。

### 10.3 COOP\_TASKRUN

启用 `IORING_SETUP_COOP_TASKRUN`，内核在合适的时机协作运行 task，减少额外的内核唤醒开销。

### 10.4 SINGLE\_ISSUER

配合 CPU 亲和性（`pthread_setaffinity_np`）使用 `IORING_SETUP_SINGLE_ISSUER`，告诉内核只有一个线程会提交 SQE，内核可以省略一些同步开销。

***

## 11. 关键配置项

### 环境变量

| 变量                        | 值                          | 作用                  |
| ------------------------- | -------------------------- | ------------------- |
| `WEBSERVER_NET_MODE`      | `proactor` / `reactor`（默认） | 选择网络后端              |
| `WEBSERVER_SQPOLL`        | `1`                        | 启用内核 SQPOLL         |
| `WEBSERVER_COOP_TASKRUN`  | `1`                        | 启用 COOP\_TASKRUN    |
| `WEBSERVER_URING_ENTRIES` | 整数（默认256）                  | io\_uring SQ/CQ 环大小 |

### NetServerOptions（代码级配置）

```cpp
struct NetServerOptions {
    std::string ip;                    // 监听 IP
    uint16_t port;                     // 监听端口
    int io_threads;                    // IO 线程数（Proactor下 = Worker 数）
    int tcp_timeout_seconds{360};      // 连接空闲超时
    bool opt_linger{true};             // SO_LINGER

    int uring_entries{256};            // io_uring 条目数
    bool cpu_affinity{false};          // CPU 亲和性绑定
    bool sqpoll{false};               // 是否启用 SQPOLL
    int sqpoll_idle_ms{1000};         // SQPOLL 空闲超时
    bool coop_taskrun{false};         // COOP_TASKRUN
};
```

### 重要常量

| 常量             | 值    | 说明                 |
| -------------- | ---- | ------------------ |
| `kRecvBufSize` | 8192 | 每次 recv 的缓冲区大小     |
| `kMaxSplice`   | 1MB  | 单次 splice 最大传输量    |
| `kAcceptBatch` | 32   | 最大并发 accept SQE 数  |
| `kMaxIovs`     | 16   | writev 最大 iovec 数  |
| pipe 容量        | 1MB  | `F_SETPIPE_SZ` 设置值 |

***

## 12. 与 Reactor 的对比

| 维度             | Reactor (epoll + TcpServer)     | Proactor (io\_uring + UringServer)         |
| -------------- | ------------------------------- | ------------------------------------------ |
| **事件模型**       | 就绪通知：epoll 告知 fd 可读/可写          | 完成通知：io\_uring CQE 告知 I/O 已完成              |
| **Accept**     | 主线程 Acceptor → subloop 分发       | 每 Worker 独立 listen（SO\_REUSEPORT）          |
| **读操作**        | EPOLLIN → read() 用户态调用          | io\_uring\_prep\_recv → CQE 带数据量           |
| **写操作**        | writev() 用户态调用                  | io\_uring\_prep\_writev → CQE              |
| **零拷贝**        | sendfile()，需单独 syscall          | splice() file→pipe→socket，可链式 SQE          |
| **TLS**        | Poll + SSL\_read/write 用户态      | kTLS → 原生 read/write/splice；否则退回 POLL\_ADD |
| **唤醒机制**       | 跨线程 queueinloop + eventfd       | 跨线程 MPSC 队列 + eventfd（设计类似）                |
| **syscall 开销** | 每个事件至少 epoll\_wait + read/write | 批量 SQE 提交 + 单个 CQE                         |
| **代码复杂度**      | 较高（手动管理 Channel/事件分发）           | 中等（io\_uring 框架接管大部分调度）                    |
| **内核要求**       | Linux 2.6+                      | Linux 5.1+（推荐 5.12+）                       |

**选择建议**：

- 如果主要场景是高并发静态文件下载 → Proactor 的 splice 零拷贝优势明显
- 如果大量使用 TLS 且 kTLS 不可用 → Reactor 和 Proactor 的用户态 SSL 路径差异不大
- 如果内核版本较低 → 只能用 Reactor

***

## 13. 推荐阅读顺序

对于新人，建议按以下顺序阅读代码：

```
第一步：理解抽象接口         第二步：理解数据流
══════════════════════       ══════════════════════

INetServer.h                 UringConnection.cpp
  (5个回调函数的含义)           HandleRecvComplete()   ← 收包入口
                              send()                 ← 发包入口
IConnection.h
  (连接的操作接口)             UringWorker.cpp
  ├─ send()                    AddConnection()        ← 新连接注册
  ├─ PostIoTask()              SubmitRead()           ← 提交读 SQE
  ├─ StartSendFile()           SubmitWrite()          ← 提交写 SQE
  └─ setCloseOnSendComplete()  SubmitSendFile()       ← 提交 splice SQE
                                ProcessCQE()          ← CQE 分发中枢
                                DrainTaskQueue()      ← 跨线程任务消费

第三步：理解架构            第四步：深入细节
══════════════════          ══════════════════

UringServer.cpp              UringWorker.h
  start() / Stop()             IoTimeWheel           ← 超时管理
                                AcceptSlot[]          ← accept 槽位
UringWorker.cpp                Start() / IoLoop()    ← 启动流程 & 主循环
  Start()                      SubmitPollAdd()       ← TLS POLL 回退
  IoLoop()                   
                                HandleSplicePipe2Sock ← splice 进度跟踪
                                HandleTlsPollCQE      ← TLS 事件处理
```

**关键日志观察点**（启动后在日志中搜索）：

- `"UringWorker listen socket ready"` — Worker 监听就绪
- `"UringWorker accept worker="` — 新连接建立
- `"UringWorker SubmitSendFile"` — splice 零拷贝触发
- `"UringWorker IoLoop started"` / `"exited"` — Worker 生命周期
- `"TLS handshake done"` — TLS 握手完成

***

## 附录：io\_uring 基础概念速览

| 概念                      | 全称                                          | 说明                |
| ----------------------- | ------------------------------------------- | ----------------- |
| SQE                     | Submission Queue Entry                      | 提交给内核的 I/O 请求描述符  |
| CQE                     | Completion Queue Entry                      | 内核返回的 I/O 完成通知    |
| SQ                      | Submission Queue                            | SQE 的环形缓冲区（用户→内核） |
| CQ                      | Completion Queue                            | CQE 的环形缓冲区（内核→用户） |
| `io_uring_get_sqe()`    | 从 SQ 获取一个空闲 SQE                             | <br />            |
| `io_uring_prep_*()`     | 填充 SQE 的具体操作（recv/writev/splice/poll\_add…） | <br />            |
| `io_uring_submit()`     | 将 SQ 中的 SQE 批量提交给内核                         | <br />            |
| `io_uring_wait_cqe()`   | 阻塞等待至少一个 CQE 到达                             | <br />            |
| `io_uring_cq_advance()` | 标记 CQE 已消费，释放 CQ 槽位                         | <br />            |
| `IOSQE_IO_LINK`         | SQE flag：本 SQE 完成后自动执行下一个链接的 SQE            | <br />            |
| `IOSQE_FIXED_FILE`      | SQE flag：使用预注册的 fixed file 描述符              | <br />            |

> 更多细节参考 [liburing 官方文档](https://github.com/axboe/liburing) 和 `man 7 io_uring`。

