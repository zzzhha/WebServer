#include"ThreadPool.h"
#include<chrono>
#include<cstdlib>

thread_local int ThreadPool::tls_worker_id_ = -1;

ThreadPool::ThreadPool(size_t threadnum, const std::string& threadtype, size_t max_queue_size)
  : stop_(false), threadtype_(threadtype), max_queue_size_(max_queue_size)
{
  workers_.reserve(threadnum);
  for (size_t i = 0; i < threadnum; i++) {
    workers_.emplace_back(std::make_unique<Worker>());
  }
  for (size_t i = 0; i < threadnum; i++) {
    threads_.emplace_back([this, i] {
      workerLoop(static_cast<int>(i));
    });
  }
}

ThreadPool::~ThreadPool() {
  stop();
}

bool ThreadPool::addTask(Task t) {
  size_t old = pending_tasks_.fetch_add(1, std::memory_order_acq_rel);
  if (old >= max_queue_size_) {
    pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }

  int wid = tls_worker_id_;
  if (wid >= 0 && static_cast<size_t>(wid) < workers_.size()) {
    auto& w = *workers_[static_cast<size_t>(wid)];
    {
      std::lock_guard<std::mutex> lk(w.m);
      w.dq.push_back(std::move(t));
    }
  } else {
    {
      std::lock_guard<std::mutex> lk(inject_m_);
      inject_q_.push_back(std::move(t));
    }
  }

  cv_.notify_one();
  return true;
}

void ThreadPool::addtask(std::function<void()> task) {
  Task t(std::move(task));
  addTask(std::move(t));
}

bool ThreadPool::tryPopLocal(int wid, Task& out) {
  auto& w = *workers_[static_cast<size_t>(wid)];
  std::lock_guard<std::mutex> lk(w.m);
  if (w.dq.empty()) return false;
  out = std::move(w.dq.front());
  w.dq.pop_front();
  return true;
}

size_t ThreadPool::drainInjectToLocal(int wid, size_t max_n) {
  if (max_n == 0) return 0;

  Task tmp;
  size_t count = 0;

  {
    std::lock_guard<std::mutex> lk(inject_m_);
    while (count < max_n && !inject_q_.empty()) {
      if (count == 0) {
        auto& w = *workers_[static_cast<size_t>(wid)];
        std::lock_guard<std::mutex> lk2(w.m);
        size_t batch = std::min(max_n, inject_q_.size());
        for (size_t i = 0; i < batch; i++) {
          w.dq.push_back(std::move(inject_q_.front()));
          inject_q_.pop_front();
        }
        count = batch;
      }
      break;
    }
  }

  return count;
}

bool ThreadPool::trySteal(int self_wid, Task& out) {
  size_t n = workers_.size();
  if (n <= 1) return false;

  static thread_local unsigned int rng_seed = static_cast<unsigned int>(
      reinterpret_cast<uintptr_t>(&out) ^ static_cast<uintptr_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));

  size_t start = static_cast<size_t>(rand_r(&rng_seed)) % n;
  for (size_t i = 1; i < n; i++) {
    size_t victim = (start + i) % n;
    if (victim == static_cast<size_t>(self_wid)) continue;

    auto& w = *workers_[victim];
    std::lock_guard<std::mutex> lk(w.m);
    if (!w.dq.empty()) {
      out = std::move(w.dq.back());
      w.dq.pop_back();
      return true;
    }
  }
  return false;
}

void ThreadPool::workerLoop(int wid) {
  tls_worker_id_ = wid;

  while (!stop_.load(std::memory_order_acquire)) {
    Task task;
    if (tryPopLocal(wid, task)) {
      try {
        task.fn();
      } catch (const std::exception& e) {
        std::cerr << "Exception in " << threadtype_ << " thread[" << wid
                  << "]: " << e.what() << std::endl;
      } catch (...) {
        std::cerr << "Unknown exception in " << threadtype_
                  << " thread[" << wid << "]" << std::endl;
      }
      pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
      continue;
    }

    if (drainInjectToLocal(wid, 32) > 0) {
      continue;
    }

    if (trySteal(wid, task)) {
      try {
        task.fn();
      } catch (const std::exception& e) {
        std::cerr << "Exception in " << threadtype_ << " thread[" << wid
                  << "] (stolen): " << e.what() << std::endl;
      } catch (...) {
        std::cerr << "Unknown exception in " << threadtype_
                  << " thread[" << wid << "] (stolen)" << std::endl;
      }
      pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
      continue;
    }

    std::unique_lock<std::mutex> lk(cv_m_);
    cv_.wait(lk, [this] {
      return stop_.load(std::memory_order_acquire) ||
             pending_tasks_.load(std::memory_order_acquire) > 0;
    });
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
  size_t idle = 0;
  size_t n = workers_.size();
  for (size_t i = 0; i < n; i++) {
    auto& w = *workers_[i];
    std::lock_guard<std::mutex> lk(w.m);
    if (w.dq.empty()) idle++;
  }
  {
    std::lock_guard<std::mutex> lk(inject_m_);
    if (!inject_q_.empty()) idle = 0;
  }
  return static_cast<int>(idle);
}

size_t ThreadPool::queue_size() {
  return pending_tasks_.load(std::memory_order_acquire);
}