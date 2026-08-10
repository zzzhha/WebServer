# 第1章 TCP连接建立与连接管理

本章解释本项目 Reactor 网络层如何建立与管理 TCP 连接，并给出与 HTTP 解析链路直接相关的“收包/回调/超时/发送”真实行为。

## 1.1 监听与接受连接：Acceptor

Acceptor 负责创建 listenfd、绑定地址并把“读事件”注册到 epoll；当 listenfd 可读时循环 `accept()` 取出新连接。

源码位置：

- [Acceptor.h](file:///home/zsy/WebServer/cppBackend/reactor/Acceptor.h)
- [Acceptor.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Acceptor.cpp)

关键实现见 [Acceptor::newconnection](file:///home/zsy/WebServer/cppBackend/reactor/Acceptor.cpp#L24-L55)：

- 使用 `while(true)` 反复 `accept()`，直到 `EAGAIN/EWOULDBLOCK` 表示本轮已接收完。
- 成功后构造 `Socket(connfd)` 并通过回调交给 `TcpServer::newconnection(...)`。

## 1.2 新连接分配：TcpServer

TcpServer 将新连接分配到某个 subloop（IO 事件循环）并建立 Connection。

源码位置：

- [tcpserver.h](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.h)
- [tcpserver.cpp](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp)

关键路径见 [TcpServer::newconnection](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L45-L74)：

- 使用 `loop_index = fd % threadnum_` 选择 subloop。
- 创建 `Connection(subloops_[loop_index].get(), ...)` 并绑定 close/error/message/sendcomplete 回调。
- 通过 `queueinloop` 把 `conn->connectEstablished()` 投递到 subloop 所在线程中执行（避免跨线程直接操作 Channel）。
- 连接超时管理：把连接加入时间轮（见 1.5）。

## 1.3 事件循环：EventLoop / Epoll

EventLoop 的 `run()` 是每个 IO 线程的主循环：

- `epoll_wait()` 返回活跃 channel 列表；
- 逐个 `handleevent()` 分发到读/写/错误/关闭回调；
- 若超时返回空列表则触发 `epolltimeoutcallback_`。

源码位置：

- [Eventloop.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp)
- [Epoll.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Epoll.cpp)

入口函数见 [EventLoop::run](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp#L13-L58)。

## 1.4 连接收包与回调：Connection

Connection 封装 connectfd，对应一个 Channel，并持有收发缓冲：

- `BufferBlock inputbuffer_ / outputbuffer_`：见 [Connection.h](file:///home/zsy/WebServer/cppBackend/reactor/Connection.h#L38-L41)。

### 1.4.1 收包：onmessage

真实行为见 [Connection::onmessage](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L150-L188)：

- 循环 `read()`，读到的数据追加到 `inputbuffer_`；
- 遇到 `EAGAIN/EWOULDBLOCK` 表示本轮数据读尽：
  - 先调用 `updatetimercallback_` 更新时间轮（见 1.5）；
  - 再调用 `onmessagecallback_` 交给上层（即 HttpServer）处理 `inputbuffer_` 中的完整字节流。

### 1.4.2 写出：writecallback（writev + sendfile）

真实行为见 [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L80-L148)：

- 优先用 `writev()` 发送 `outputbuffer_` 中的缓冲数据；
- 若启用了 sendfile（`sendfile_.active`），则继续调用 `::sendfile()` 发送文件体；
- 当 `outputbuffer_` 为空且不再有 sendfile 任务时：
  - 关闭写事件；
  - 触发 `sendcompletecallback_`；
  - 若 `close_on_send_complete_` 为真，则在发送完毕后关闭连接。

## 1.5 TCP 空闲连接超时：TimeWheel

TcpServer 使用时间轮管理 TCP 空闲连接：

- 启动：`time_wheel_.start()`，见 [TcpServer::start](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L31-L34)。
- 新连接入轮：`add_new_conn_timernode(conn)`，见 [TcpServer::newconnection](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L60-L74) 与 [add_new_conn_timernode](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L175-L178)。
- 连接活跃更新：Connection 每轮读尽触发 `update_conn_timeout_time(conn)`，见 [Connection::onmessage](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L166-L176) 与 [TcpServer::update_conn_timeout_time](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L179-L182)。

