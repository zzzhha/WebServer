#pragma once
#include <vector>
#include <list>
#include <functional>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "../logger/log_fac.h"
#include "../reactor/Connection.h"

class TimeWheel{
public:
  TimeWheel(int slot_interval=1,int slots=60 );
  ~TimeWheel();

  // 启动时间轮线程
  void start();
  // 停止时间轮线程
  void stop();
 
  void add_connection(spConnection conn,int timeout);
  void update_connection(spConnection conn);
  void remove_connection(spConnection conn);


private:
  struct TimerNode{
    int fd;
    int rotation;
    int slot;
    std::weak_ptr<Connection> conn_;
  };
  void tick();
  void run(); 
  int slot_interval_;
  int slots_;
  int current_slot_;

  std::vector<std::list<TimerNode>>wheel_;
  std::unordered_map<int,std::list<TimerNode>::iterator> timer_map_;
  std::mutex mutex_;
  void add_connection_unsafe(spConnection conn, int timeout);
  void remove_connection_unsafe(spConnection conn);
  // 线程控制相关成员
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::condition_variable cv_;
  std::mutex cv_mutex_;
};