#include"TimeWheel.h"

TimeWheel::TimeWheel(int slot_interval,int slots):slot_interval_(slot_interval),slots_(slots),current_slot_(0){
  wheel_.resize(slots);
}

TimeWheel::~TimeWheel() {
  stop();
  //  std::lock_guard<std::mutex> lock(mutex_);
  //   for (auto& slot : wheel_) {
  //       slot.clear();
  //   }
  //   timer_map_.clear();
}

void TimeWheel::start() {
  if (running_) return;
  running_ = true;
  thread_ = std::thread(&TimeWheel::run, this);
  LOGINFO("Time wheel thread started");
}

void TimeWheel::stop() {
  if (!running_) return;
  
  running_ = false;
  cv_.notify_all();  // 唤醒可能正在等待的线程
  
  if (thread_.joinable()) {
    thread_.join();
  }
  
  // 清理所有定时器
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& slot : wheel_) {
    slot.clear();
  }
  timer_map_.clear();
  
  LOGINFO("Time wheel thread stopped");
}

void TimeWheel::run() {
  while (running_) {
    // 等待指定的时间间隔
    std::unique_lock<std::mutex> lock(cv_mutex_);
    cv_.wait_for(lock, std::chrono::seconds(slot_interval_), [this] { 
      return !running_; 
    });
    
    if (!running_) break;
    
    // 执行tick操作
    tick();
  }
}

void TimeWheel::tick(){
  std::vector<std::weak_ptr<Connection>> weak_conns;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& current_list = wheel_[current_slot_];
    auto it = current_list.begin();
    
    while(it!= current_list.end()){
      if(it->rotation >0){
        it->rotation--;
        ++it;
      }else{
        if(!it->conn_.expired()){
          weak_conns.push_back(it->conn_);
        }
        timer_map_.erase(it->fd);
        it = current_list.erase(it);
      }
    }
    current_slot_ = (current_slot_ + 1) % slots_;
  }
  for(auto &weak_conn:weak_conns){
    if(auto conn=weak_conn.lock()){
LOGDEBUG("超时，将关闭操作提交到从事件循环");
      conn->getLoop()->queueinloop([conn](){
        conn->closecallback();
      });
    }
  }
}
void TimeWheel::add_connection(spConnection conn,int timeout){
  std::lock_guard<std::mutex> lock(mutex_);
  add_connection_unsafe(conn, timeout);
}

void TimeWheel::update_connection(spConnection conn){
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = timer_map_.find(conn->fd());
  if (it != timer_map_.end()) {
    // 直接操作内部数据结构，而不是调用 remove_connection
    int slot = it->second->slot; 
    wheel_[slot].erase(it->second); 
    timer_map_.erase(it);

    // 直接添加新定时器，而不是调用 add_connection
    add_connection_unsafe(conn, 360);
  }
}

void TimeWheel::remove_connection(spConnection conn){
  std::lock_guard<std::mutex> lock(mutex_);
  remove_connection_unsafe(conn);
}
void TimeWheel::add_connection_unsafe(spConnection conn, int timeout) {

  int ticks = timeout / slot_interval_;
  int rotation = ticks / slots_;
  int slot = (current_slot_ + ticks % slots_) % slots_;

  TimerNode node;
  node.fd=conn->fd();
  node.rotation = rotation;
  node.slot = slot;
  node.conn_ = conn;

  wheel_[slot].push_front(node);
  timer_map_[conn->fd()] = wheel_[slot].begin();
  
  LOGDEBUG("Added timer for fd: " + std::to_string(conn->fd()));
}

void TimeWheel::remove_connection_unsafe(spConnection conn) {
  auto it = timer_map_.find(conn->fd());
  if (it != timer_map_.end()) {
    int slot = it->second->slot; 
    wheel_[slot].erase(it->second);
    timer_map_.erase(it);
    
    LOGDEBUG("Removed timer for fd: " + std::to_string(conn->fd()));
  }
}