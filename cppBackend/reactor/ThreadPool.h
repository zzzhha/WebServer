#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <deque>
#include <sys/syscall.h>
#include <mutex>
#include <unistd.h>
#include <thread>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <cstdint>
#include <memory>

enum class TaskPriority : uint8_t { Low, Normal, High };

struct Task {
  std::function<void()> fn;
  TaskPriority priority{TaskPriority::Normal};
  uint64_t enqueue_ns{0};
  uint64_t trace_id{0};
  uint32_t affinity{0};
  std::shared_ptr<std::atomic_bool> cancel;

  Task() = default;
  explicit Task(std::function<void()> f)
    : fn(std::move(f)), enqueue_ns(0) {}
};

class ThreadPool {
private:
  struct Worker {
    std::mutex m;
    std::deque<Task> dq;
  };

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;
  std::atomic_bool stop_{false};
  std::string threadtype_;
  std::atomic<size_t> pending_tasks_{0};
  size_t max_queue_size_;

  std::mutex inject_m_;
  std::deque<Task> inject_q_;

  std::mutex cv_m_;
  std::condition_variable cv_;

  static thread_local int tls_worker_id_;

  bool tryPopLocal(int wid, Task& out);
  bool trySteal(int self_wid, Task& out);
  size_t drainInjectToLocal(int wid, size_t max_n);
  void workerLoop(int wid);

public:
  ThreadPool(size_t threadnum, const std::string& threadtype, size_t max_queue_size = 10000);

  bool addTask(Task t);
  void addtask(std::function<void()> task);
  int idl_thread_cnt();
  size_t size();
  size_t queue_size();
  void stop();
  ~ThreadPool();
};