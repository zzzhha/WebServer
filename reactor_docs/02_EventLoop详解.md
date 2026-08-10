# EventLoop 详解

## 概述

`EventLoop` 是 Reactor 模式的核心组件，负责驱动事件循环、管理 epoll 实例、处理任务队列和跨线程通信。每个 `EventLoop` 对象运行在一个独立的线程中，构成了 Reactor 模式中的事件处理单元。

## 文件信息

- **头文件**: [Eventloop.h](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.h)
- **实现文件**: [Eventloop.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Eventloop.cpp)
- **依赖**: `Epoll`, `Channel`, `Connection`, `log_fac`
- **被依赖**: `TcpServer`, `HttpServer`, `Connection`, `Acceptor`

## 类定义

### 成员变量

```cpp
private:
  std::unique_ptr<Epoll> ep_;                    // 每个事件循环有一个 epoll 实例
  std::function<void(EventLoop*)> epolltimeoutcallback_;  // epoll_wait 超时回调
  pid_t threadid_;                               // 事件循环所在线程 ID
  std::queue<std::function<void()>> taskqueue_;  // 任务队列
  std::mutex mutex_;                             // 任务队列同步的互斥锁
  int wakeupfd_;                                 // 用于唤醒事件循环线程的 eventfd
  std::unique_ptr<Channel> wakeupchannel_;       // eventfd 的 channel
  std::atomic_bool stop_;                        // 停止标志
```

### 构造函数

```cpp
EventLoop::EventLoop()
  : ep_(new Epoll),
    wakeupfd_(eventfd(0, EFD_NONBLOCK)),
    wakeupchannel_(new Channel(this, wakeupfd_)),
    stop_(false) {
  // 设置 wakeupchannel 的读回调
  wakeupchannel_->setreadcallback(std::bind(&EventLoop::handlewakeup, this));
  wakeupchannel_->enablereading();
}
```

## 核心方法详解

### 1. run() - 事件循环主函数

```cpp
void EventLoop::run() {
  threadid_ = syscall(SYS_gettid);  // 获取当前线程 ID
  while (stop_ == false) {
    // 调用 epoll_wait 获取发生事件的文件描述符
    std::vector<Channel*> vcn = ep_->loop(10 * 1000);
    
    // 如果 channel 为空，表示超时，回调超时处理函数
    if (vcn.empty()) {
      epolltimeoutcallback_(this);
    } else {
      // 处理所有发生事件的 channel
      for (auto &ch : vcn) {
        LOGDEBUG("有新的事件准备处理");
        ch->handleevent();
      }
    }
  }
}
```

**功能**: 
- 运行事件循环，持续调用 `epoll_wait` 等待事件
- 处理 epoll 超时情况
- 分发事件到对应的 `Channel` 对象

**参数**: 无
**返回值**: 无
**调用位置**: `TcpServer::start()` 中被调用
**超时时间**: 10 秒

### 2. queueinloop() - 任务队列投递

```cpp
void EventLoop::queueinloop(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);  // 加锁
    taskqueue_.push(fn);                        // 任务入队
  }
  LOGDEBUG("有任务入队，唤醒事件");
  wakeup();  // 唤醒事件循环线程
}
```

**功能**: 
- 将任务函数添加到任务队列中
- 线程安全地操作任务队列
- 唤醒事件循环线程处理新任务

**参数**: 
- `fn`: 需要执行的任务函数

**返回值**: 无
**典型使用场景**: 
- 跨线程投递任务到事件循环线程
- `Connection::send()` 在非 IO 线程中调用时使用

### 3. wakeup() 和 handlewakeup() - 线程唤醒机制

```cpp
void EventLoop::wakeup() {
  uint64_t val = 1;
  write(wakeupfd_, &val, sizeof(val));  // 向 eventfd 写入数据
}

void EventLoop::handlewakeup() {
  LOGDEBUG("处理因事件管道唤起的事件");
  uint64_t val;
  read(wakeupfd_, &val, sizeof(val));  // 读取 eventfd 数据，避免重复触发
  
  std::queue<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks = std::move(taskqueue_);  // 交换任务队列
  }
  
  while (!tasks.empty()) {
    auto fn = std::move(tasks.front());
    tasks.pop();
    fn();  // 执行任务
  }
}
```

**功能**: 
- `wakeup()`: 通过 eventfd 唤醒阻塞在 `epoll_wait` 的事件循环线程
- `handlewakeup()`: 处理唤醒事件，执行任务队列中的所有任务

**工作原理**: 
1. 使用 `eventfd` 创建非阻塞文件描述符
2. `wakeupchannel_` 监听该 eventfd 的读事件
3. 当需要唤醒线程时，向 eventfd 写入数据
4. `epoll_wait` 检测到 eventfd 可读，触发 `handlewakeup()`
5. 在 `handlewakeup()` 中执行所有排队任务

### 4. updatechannel() 和 removechannel() - Channel 管理

```cpp
void EventLoop::updatechannel(Channel *ch) {
  ep_->updatechannel(ch);  // 委托给 Epoll 处理
}

void EventLoop::removechannel(Channel *ch) {
  ep_->removechannel(ch);  // 委托给 Epoll 处理
}
```

**功能**: 
- 更新 Channel 在 epoll 中的注册状态
- 从 epoll 中移除 Channel

**参数**: 
- `ch`: 需要更新或移除的 Channel 指针

**返回值**: 无
**调用位置**: `Channel::update()`, `Channel::remove()`

### 5. isinloopthread() - 线程检查

```cpp
bool EventLoop::isinloopthread() {
  return threadid_ == syscall(SYS_gettid);
}
```

**功能**: 检查当前线程是否为事件循环所在线程
**返回值**: 
- `true`: 当前线程是事件循环线程
- `false`: 当前线程不是事件循环线程

**用途**: 
- 确保 Channel 操作在正确的线程中执行
- 避免跨线程竞争条件

### 6. stop() - 停止事件循环

```cpp
void EventLoop::stop() {
  stop_ = true;  // 设置停止标志
  wakeup();      // 唤醒线程以退出循环
}
```

**功能**: 优雅地停止事件循环
**调用位置**: `TcpServer::~TcpServer()`, `HttpServer::Stop()`

## 事件处理流程

### 正常事件处理流程
1. `EventLoop::run()` 调用 `Epoll::loop()` 进行 `epoll_wait`
2. `Epoll::loop()` 返回发生事件的 `Channel` 列表
3. 遍历列表，调用每个 `Channel::handleevent()`
4. `Channel::handleevent()` 根据事件类型调用相应的回调函数

### 任务投递流程
1. 外部线程调用 `EventLoop::queueinloop()`
2. 任务被加入队列，互斥锁保护
3. 调用 `EventLoop::wakeup()` 写入 eventfd
4. `epoll_wait` 检测到 eventfd 可读，唤醒线程
5. `EventLoop::handlewakeup()` 读取 eventfd，执行所有排队任务

## 线程模型

### 线程关系
- 每个 `EventLoop` 对象运行在独立的线程中
- 主 `EventLoop` 运行在主线程或专用线程中
- 从 `EventLoop` 运行在 IO 线程池的线程中

### 线程标识
- `threadid_`: 在 `run()` 函数中通过 `syscall(SYS_gettid)` 获取
- 保证 Channel 相关操作在正确的线程中执行

## 设计特点

### 1. 线程安全的任务队列
- 使用互斥锁保护任务队列
- 支持跨线程安全投递任务
- 批量执行任务减少锁竞争

### 2. 高效的线程唤醒
- 使用 `eventfd` 替代管道或 socket pair
- 零拷贝唤醒机制
- 避免虚假唤醒

### 3. 统一的超时处理
- 支持 epoll_wait 超时回调
- 可用于连接超时检查等场景
- 超时时间可配置（当前固定为 10 秒）

### 4. 清晰的线程边界
- 明确的线程归属检查
- 避免跨线程操作 Channel
- 通过任务队列实现跨线程通信

## 使用示例

```cpp
// 创建 EventLoop
EventLoop loop;

// 启动事件循环（在独立线程中）
std::thread t([&loop]() {
  loop.run();
});

// 跨线程投递任务
loop.queueinloop([]() {
  // 这个任务会在事件循环线程中执行
  std::cout << "Task executed in event loop thread" << std::endl;
});

// 停止事件循环
loop.stop();
t.join();
```

## 注意事项

1. **线程安全**: Channel 的更新和删除必须在事件循环线程中进行
2. **资源清理**: 确保在销毁前停止事件循环
3. **任务类型**: 避免在任务中执行阻塞操作，会影响事件响应
4. **唤醒机制**: 每次唤醒后必须读取 eventfd，否则会重复触发

## 关联组件

### 直接依赖
- **Epoll**: 实际的 epoll 操作委托给 Epoll 类
- **Channel**: 管理具体文件描述符的事件
- **Connection**: 使用 EventLoop 进行任务投递

### 使用 EventLoop 的组件
- **TcpServer**: 管理多个 EventLoop（主从模式）
- **HttpServer**: 通过 TcpServer 间接使用 EventLoop
- **Acceptor**: 监听 socket 注册到主 EventLoop
- **Connection**: 每个连接绑定到一个从 EventLoop