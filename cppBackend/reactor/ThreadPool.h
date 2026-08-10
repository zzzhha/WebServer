#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <deque>
#include <mutex>
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
  std::vector<std::thread> threads_;
  std::string threadtype_;
  std::atomic_bool stop_{false};
  std::atomic<size_t> pending_tasks_{0};
  size_t max_queue_size_;
  std::atomic<size_t> idle_count_{0};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> taskqueue_;

  bool addTaskImpl(Task t);
  void workerLoop();

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
