#include"ThreadPool.h"

ThreadPool::ThreadPool(size_t threadnum, const std::string& threadtype, size_t max_queue_size)
  : threadtype_(threadtype), stop_(false), max_queue_size_(max_queue_size), idle_count_(0)
{
  for (size_t i = 0; i < threadnum; i++) {
    threads_.emplace_back([this] { workerLoop(); });
  }
}

ThreadPool::~ThreadPool() {
  stop();
}

bool ThreadPool::addTaskImpl(Task t) {
  size_t old = pending_tasks_.fetch_add(1, std::memory_order_acq_rel);
  if (old >= max_queue_size_) {
    pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    taskqueue_.push_back(std::move(t));
  }
  cv_.notify_one();
  return true;
}

bool ThreadPool::addTask(Task t) {
  return addTaskImpl(std::move(t));
}

void ThreadPool::addtask(std::function<void()> task) {
  Task t(std::move(task));
  addTaskImpl(std::move(t));
}

void ThreadPool::workerLoop() {
  while (!stop_.load(std::memory_order_acquire)) {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      idle_count_.fetch_add(1, std::memory_order_relaxed);
      cv_.wait(lk, [this] {
        return stop_.load(std::memory_order_acquire) || !taskqueue_.empty();
      });
      idle_count_.fetch_sub(1, std::memory_order_relaxed);
      if (stop_.load(std::memory_order_acquire) && taskqueue_.empty()) {
        return;
      }
      task = std::move(taskqueue_.front());
      taskqueue_.pop_front();
    }

    try {
      task.fn();
    } catch (const std::exception& e) {
      std::cerr << "Exception in " << threadtype_ << " thread: "
                << e.what() << std::endl;
    } catch (...) {
      std::cerr << "Unknown exception in " << threadtype_ << " thread"
                << std::endl;
    }
    pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

size_t ThreadPool::size() {
  return threads_.size();
}

void ThreadPool::stop() {
  if (stop_.exchange(true)) return;
  cv_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

int ThreadPool::idl_thread_cnt() {
  return static_cast<int>(idle_count_.load(std::memory_order_relaxed));
}

size_t ThreadPool::queue_size() {
  return pending_tasks_.load(std::memory_order_acquire);
}
