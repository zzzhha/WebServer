# Epoll 详解

## 概述

`Epoll` 类是对 Linux `epoll` 系统调用的 C++ 封装，是 Reactor 模式的核心 I/O 多路复用组件。它提供了高效的事件监控机制，可以同时监控大量文件描述符的状态变化。

## 文件信息

- **头文件**: [Epoll.h](file:///home/zsy/WebServer/cppBackend/reactor/Epoll.h)
- **实现文件**: [Epoll.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Epoll.cpp)
- **依赖**: `Channel`, `log_fac`
- **被依赖**: `EventLoop`

## 类定义

### 成员变量

```cpp
private:
  static const int MaxEvents = 512;  // 每次 epoll_wait 返回的最大事件数
  int epollfd_ = -1;                 // epoll 文件描述符
  epoll_event events_[MaxEvents];    // 事件缓冲区
```

### 构造函数

```cpp
Epoll::Epoll() {
  epollfd_ = epoll_create(1);  // 创建 epoll 句柄（红黑树）
  if (epollfd_ == -1) {
    printf("epoll_create error\n");
    exit(-1);
  }
}
```

### 析构函数

```cpp
Epoll::~Epoll() {
  close(epollfd_);  // 关闭 epoll 文件描述符
}
```

## 核心方法详解

### 1. updatechannel() - 添加/更新 Channel

```cpp
void Epoll::updatechannel(Channel *ch) {
  epoll_event ev;           // 声明事件结构体
  ev.data.ptr = ch;         // 指定 Channel
  ev.events = ch->events(); // 指定事件
  
  if (ch->inpoll()) {  // 如果 Channel 已经在树上了
    if (epoll_ctl(epollfd_, EPOLL_CTL_MOD, ch->fd(), &ev) == -1) {
      char buf[256];
      snprintf(buf, sizeof(buf), "epoll_ctl mod failed: %s", strerror(errno));
      LOGERROR(buf);
      return;
    }
  } else {  // Channel 不在树上，需要添加
    if (epoll_ctl(epollfd_, EPOLL_CTL_ADD, ch->fd(), &ev) == -1) {
      char buf[256];
      snprintf(buf, sizeof(buf), "epoll_ctl add failed: %s", strerror(errno));
      LOGERROR(buf);
      return;
    }
    ch->setinepoll();  // 把 channel 的 inepoll 设置为 true
  }
}
```

**功能**:

- 将 `Channel` 添加或更新到 epoll 实例中
- 根据 `Channel` 是否已在 epoll 中决定使用 `EPOLL_CTL_ADD` 或 `EPOLL_CTL_MOD`
- 设置 `Channel` 的 `inepoll` 状态

**参数**:

- `ch`: 需要添加或更新的 `Channel` 指针

**返回值**: 无
**调用位置**: `EventLoop::updatechannel()`

### 2. removechannel() - 删除 Channel

```cpp
void Epoll::removechannel(Channel *ch) {
  if (ch->inpoll()) {  // 如果 Channel 已经在树上了
    if (epoll_ctl(epollfd_, EPOLL_CTL_DEL, ch->fd(), 0) == -1) {
      char buf[256];
      snprintf(buf, sizeof(buf), "epoll_ctl del failed: %s", strerror(errno));
      LOGERROR(buf);
      return;
    }
  }
}
```

**功能**:

- 从 epoll 实例中删除 `Channel`
- 使用 `EPOLL_CTL_DEL` 删除文件描述符

**参数**:

- `ch`: 需要删除的 `Channel` 指针

**返回值**: 无
**调用位置**: `EventLoop::removechannel()`

### 3. loop() - 等待事件发生

```cpp
std::vector<Channel*> Epoll::loop(int timeout) {
  std::vector<Channel*> channels;  // 存放 channels
  bzero(events_, sizeof(events_));  // 清空事件缓冲区
  
  int number = epoll_wait(epollfd_, events_, MaxEvents, timeout);
  if (number < 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "epoll_wait error: %s", strerror(errno));
    LOGERROR(buf);
    return channels;
  }
  
  if (number == 0) {  // 超时
    return channels;
  }
  
  for (int i = 0; i < number; i++) {
    Channel *ch = (Channel*)events_[i].data.ptr;  // 取出已发生事件的 channel
    ch->setrevents(events_[i].events);           // 设置 channel 的 revents_ 成员
    channels.push_back(ch);
  }
  
  return channels;
}
```

**功能**:

- 调用 `epoll_wait` 等待事件发生
- 将发生的事件转换为 `Channel` 对象列表返回
- 设置每个 `Channel` 的实际发生事件（`revents_`）

**参数**:

- `timeout`: 超时时间（毫秒），默认 -1（无限等待）

**返回值**:

- `std::vector<Channel*>`: 发生事件的 `Channel` 列表

**调用位置**: `EventLoop::run()`

## 设计特点

### 1. 高效的 epoll 封装

- 直接使用系统调用，无额外抽象层
- 边缘触发模式（Edge Triggered）
- 固定大小的事件缓冲区（512 个事件）

### 2. Channel 状态管理

- 通过 `Channel::inpoll()` 跟踪 Channel 是否在 epoll 中
- 智能选择 `EPOLL_CTL_ADD` 或 `EPOLL_CTL_MOD`
- 避免重复添加或遗漏删除

### 3. 错误处理

- 所有 epoll 系统调用都有错误检查
- 使用 `LOGERROR` 记录错误信息
- 函数在错误时优雅返回，不抛出异常

### 4. 内存管理

- 栈上分配事件缓冲区，无需动态内存分配
- 避免频繁的内存分配和释放
- 固定大小缓冲区适合大多数场景

## 事件处理流程

### Channel 注册流程

1. `EventLoop::updatechannel(ch)` 被调用
2. 委托给 `Epoll::updatechannel(ch)`
3. 检查 `ch->inpoll()` 状态
4. 调用 `epoll_ctl` 添加或修改注册
5. 设置 `ch->setinepoll()` 状态

### 事件等待和处理流程

1. `EventLoop::run()` 调用 `Epoll::loop(timeout)`
2. `Epoll::loop()` 调用 `epoll_wait()` 阻塞等待事件
3. 事件发生时，`epoll_wait()` 返回事件数量
4. 遍历事件数组，获取对应的 `Channel` 指针
5. 设置 `Channel` 的 `revents_` 成员
6. 返回 `Channel` 列表给 `EventLoop`
7. `EventLoop` 调用每个 `Channel::handleevent()`

## 性能考虑

### 缓冲区大小

- `MaxEvents = 512`: 每次 epoll\_wait 最大返回事件数
- 平衡内存使用和性能
- 适合高并发场景

### 超时时间

- 默认 -1（无限等待）
- `EventLoop` 调用时通常指定 10 秒超时
- 超时用于定期检查和清理

### 边缘触发模式

- 使用边缘触发（Edge Triggered）模式
- 需要正确处理 EAGAIN/EWOULDBLOCK
- 提高性能，减少系统调用

## 错误处理策略

### 1. epoll\_create 失败

- 打印错误信息
- 调用 `exit(-1)` 退出程序
- 严重错误，无法继续运行

### 2. epoll\_ctl 失败

- 记录错误日志
- 函数返回，不抛出异常
- 上层调用者需要处理失败情况

### 3. epoll\_wait 失败

- 记录错误日志
- 返回空的事件列表
- `EventLoop` 继续运行

## 使用注意事项

### 1. 线程安全

- `Epoll` 对象不是线程安全的
- 所有操作必须在同一个线程中进行
- `EventLoop` 确保线程安全性

### 2. Channel 生命周期

- 确保 `Channel` 在 epoll 中注册期间保持有效
- 移除 `Channel` 前确保不再有事件发生
- 避免悬空指针

### 3. 文件描述符管理

- epoll 实例会持有注册的文件描述符
- 关闭文件描述符前需要从 epoll 中移除
- 避免文件描述符泄漏

## 关联组件

### 1. Channel

- `Epoll` 管理 `Channel` 的注册和事件监控
- `Channel` 提供文件描述符和事件信息
- `Epoll` 将事件通知给对应的 `Channel`

### 2. EventLoop

- `EventLoop` 拥有 `Epoll` 实例
- `EventLoop` 调用 `Epoll` 的方法
- `Epoll` 是 `EventLoop` 的底层实现

### 3. 系统调用

- `epoll_create`: 创建 epoll 实例
- `epoll_ctl`: 添加/修改/删除监控的文件描述符
- `epoll_wait`: 等待事件发生

