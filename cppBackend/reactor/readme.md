Reactor 模块流程概览（按当前实现）

1) 组件关系
- TcpServer：组织主从 EventLoop、IO 线程池、Acceptor、Connection 生命周期与回调
- EventLoop/Epoll/Channel：epoll_wait 事件获取与回调分发
- Acceptor：监听 listenfd，accept 新连接并回调 TcpServer::newconnection
- Connection：管理 connfd 读写、输入/输出 BufferBlock、关闭/错误/写完成回调
- HttpServer：作为业务适配层，把 Connection 的字节流交给 HttpFacade 处理并回写响应

2) 启动阶段
- main 调用 HttpServer::start → TcpServer::start
- TcpServer::start 启动 TimeWheel，并运行 mainloop_->run()
- TcpServer 构造时创建 subloops_，并把每个 subloop 的 EventLoop::run 投递到 IO 线程池执行

3) 建立连接（listenfd → connfd）
- mainloop_ 上的 Acceptor::newconnection 在读事件触发时循环 accept4
  - accept4 返回 EAGAIN/EWOULDBLOCK：本轮 accept 结束
  - 其他错误：记录日志并返回，不构造非法 fd
- Acceptor 将 connfd 封装为 Socket 并回调到 TcpServer::newconnection
- TcpServer::newconnection
  - 选择一个 subloop（fd % threadnum_）
  - 创建 Connection（绑定到该 subloop）
  - 设置 close/error/message/sendcomplete 回调，并把 conn 放入 conns_
  - 关键：通过 subloop->queueinloop 调用 conn->connectEstablished()，确保 epoll 注册与 enablereading 在所属 IO 线程执行
  - 回调 HttpServer::HandleNewConnection（业务层可在此挂载连接级上下文）

4) 事件分发（Channel::handleevent）
- 优先处理 EPOLLERR/EPOLLHUP（直接走 error 回调并返回）
- EPOLLIN/EPOLLPRI：触发 readcallback
- EPOLLOUT：触发 writecallback
- EPOLLRDHUP：触发 closecallback

5) 读数据（Connection::onmessage）
- 采用 while 循环读到 EAGAIN/EWOULDBLOCK 为止（ET 模式）
- 读到 EAGAIN/EWOULDBLOCK：
  - 更新连接超时（TimeWheel）
  - 回调上层 onmessage（HttpServer::HandleMessage）
- read 返回 0：对端关闭，进入 closecallback
- read 返回其他错误（非 EINTR/EAGAIN）：进入 errorcallback，避免异常状态与忙等

6) 写数据（Connection::send / writecallback）
- HttpServer 将响应序列化后 append 到 outputbuffer_，调用 conn->send()
- conn->send 会在 IO 线程内直接 enablewriting；否则通过 queueinloop 投递给所属 IO 线程执行
- writecallback 使用 writev 批量发送，outputbuffer_ 发送完会 disablewriting
- 若 close_on_send_complete_ 为 true，则在发送完毕后关闭连接（用于错误响应或非 keep-alive 场景）

7) 本次提交相关稳定性修复点（reactor侧）
- Channel 分发不再使用 else-if，避免 EPOLLIN/EPOLLOUT 同时到来时写事件被吞
- Connection 的 epoll 注册/启用读事件放到 connectEstablished，并由 subloop 执行，规避跨线程竞态
- accept/read 错误处理补强：accept4 失败不构造连接；read 非 EAGAIN/EINTR 错误走 error 关闭
    
