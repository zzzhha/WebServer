#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <queue>
#include <sys/syscall.h>
#include <mutex>
#include <unistd.h>
#include <thread>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
class ThreadPool{
private:
  std::vector<std::thread> threads_;                //线程池中的线程
  std::queue<std::function<void()>> taskqueue_;     //任务队列
  std::mutex mutex_;                                //任务队列同步互斥锁
  std::condition_variable condition_;               //任务队列同步的条件变量
  std::atomic_bool stop_;                           //析构函数中，将stop_设置为true,退出全部线程
  std::string threadtype_;                          //线程种类："IO","WORKS"
  std::atomic<int> tp_idl_tnum{ 0 };                //空闲线程数量
  size_t max_queue_size_;                           //任务队列最大容量
public:
  ThreadPool(size_t threadnum, const std::string& threadtype, size_t max_queue_size = 10000);
  
  void addtask(std::function<void()> task);
  int idl_thread_cnt();
  size_t size();
  size_t queue_size();
  //停止线程
  void stop();
  ~ThreadPool();
};