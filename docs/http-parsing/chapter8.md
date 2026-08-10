# 第8章 并发模型与内存管理要点

本章聚焦“请求解析链路在并发与内存层面如何落地”。结论全部来自源码实现：哪些逻辑运行在 IO 线程、哪些对象是连接级、Buffer/发送链路如何减少拷贝。

## 8.1 Reactor 并发模型：主从 EventLoop + IO 线程池

TcpServer 架构要点：

- 主事件循环 `mainloop_` 负责监听与接受连接（Acceptor 绑定 listenfd）。
- 从事件循环 `subloops_` 运行在 IO 线程池中，负责已建立连接的读写事件。

实现依据：

- subloops 的创建与 IO 线程启动：见 [TcpServer 构造函数](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L4-L26)。
- 新连接分配到 subloop：`loop_index = fd % threadnum_`，并把 `connectEstablished()` 投递到对应 subloop 执行，见 [TcpServer::newconnection](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L45-L74)。

## 8.2 HttpServer 的工作线程池现状

HttpServer 构造时创建了 `threadpool_(workthreadnum,"WORKS")`（见 [HttpServer 构造函数](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L29-L47)），但当前 `HandleMessage` 的处理逻辑并未把 HTTP 解析/路由派发到该工作线程池中（回调签名处仍保留了注释占位）。因此：

- 解析、校验、路由与大部分业务 handler 当前是在触发读事件的 IO 线程中执行。

## 8.3 连接级对象与线程安全边界

HttpFacade 内部持有可变状态（如 `parser_`、解析等待计时、SSL 握手状态），因此其生命周期与线程安全边界被设计为“连接级对象”：

- 创建并挂载到连接上下文： [HttpServer::HandleNewConnection](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L64-L73)。
- 随连接持续复用：同一连接上多次 `HandleMessage` 会复用该 facade（见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L108-L117)）。

这也是 `GetConsumeBytes()` 能正确推进“同一连接的粘包/拆包”解析进度的前提（见第 2 章）。

## 8.4 BufferBlock：分块内存 + MemoryPool

Connection 的收发缓冲使用 `BufferBlock`：

- 成员：`BufferBlock inputbuffer_ / outputbuffer_`，见 [Connection.h](file:///home/zsy/WebServer/cppBackend/reactor/Connection.h#L38-L41)。
- 追加与消费：见 [BufferBlock::append](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp#L46-L80) 与 [BufferBlock::consumeBytes](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp#L130-L149)。

BufferBlock 的内存块由 MemoryPool 分配/释放：

- 分配：`data = MemoryPool::allocate(cap);`
- 释放：`MemoryPool::deallocate(data, size);`
- 见 [Buffer.h](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h#L10-L28)。

## 8.5 发送链路：writev + sendfile

本项目发送链路分为两部分：

- 普通响应：序列化后的响应头/小 body 写入 `outputbuffer_`，由 `writev()` 批量发送，见 [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L86-L108)。
- 大文件响应：`HttpResponse::SetSendFile` 标记文件发送，HttpServer 先发响应头，再由 Connection 在写回调中用 `sendfile()` 推送文件体，见：
  - [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L189-L211)
  - [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L110-L148)

## 8.6 解析阶段的拷贝点与成本认知

当前解析接口以 `std::string` 作为输入载体：

- HttpServer 侧通过 `inputbuffer.bufferToString()` 生成连续字符串传给 HttpFacade（见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L101-L106)）。
- 这一步会产生一次“分块缓冲 → 连续字符串”的拷贝；其优势是简化解析器实现与消费推进，代价是对超大请求体/高 QPS 场景存在额外拷贝开销。

该行为属于当前实现的既定取舍，文档中不对其做“伪零拷贝”描述。

