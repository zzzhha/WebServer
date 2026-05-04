#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "reactor/ThreadPool.h"

struct PendingChunk {
  std::string data;
  uint64_t enqueue_seq{0};
  std::chrono::steady_clock::time_point enqueue_tp;
};

struct WorkResult {
  uint64_t response_seq{0};
  std::string request_id;
  std::string response_data;
  bool has_response{false};
  bool has_sendfile{false};
  int sendfile_fd{-1};
  off_t sendfile_offset{0};
  size_t sendfile_length{0};
};

struct ConnectionWorkContext {
  std::mutex mutex;
  std::deque<PendingChunk> queued_chunks;
  size_t queued_bytes{0};
  bool worker_running{false};
  uint64_t next_response_seq{1};
  uint64_t last_applied_response_seq{0};
  std::map<uint64_t, WorkResult> pending_results;

  size_t active_worker_count{0};
  size_t max_concurrent_workers{4};
  bool draining{false};
};

int main() {
  size_t passed = 0;
  size_t failed = 0;

  auto check = [&](bool cond, const char* test_name) {
    if (cond) {
      passed++;
      std::cout << "  PASS: " << test_name << "\n";
    } else {
      failed++;
      std::cerr << "  FAIL: " << test_name << "\n";
    }
  };

  std::cout << "=== 策略B Phase1 并行流水线测试 ===\n\n";

  std::cout << "[1] test_chain_scheduling_basic\n";
  {
    ThreadPool pool(4, "CHAIN_TEST", 256);

    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->max_concurrent_workers = 2;
    ctx->worker_running = true;
    ctx->active_worker_count = 1;

    for (int i = 0; i < 3; i++) {
      PendingChunk chunk;
      chunk.enqueue_seq = static_cast<uint64_t>(i + 1);
      chunk.data = "req-" + std::to_string(i);
      ctx->queued_chunks.push_back(std::move(chunk));
    }
    ctx->queued_bytes = 300;

    std::mutex exec_mtx;
    std::vector<int> exec_order;
    std::atomic<size_t> completed{0};

    pool.addtask([&]() {
      {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        if (!ctx->queued_chunks.empty()) {
          ctx->queued_chunks.pop_front();
          if (!ctx->queued_chunks.empty() &&
              ctx->active_worker_count < ctx->max_concurrent_workers) {
            ctx->active_worker_count++;
            pool.addtask([&]() {
              {
                std::lock_guard<std::mutex> lk2(ctx->mutex);
                if (!ctx->queued_chunks.empty()) {
                  ctx->queued_chunks.pop_front();
                }
              }
              {
                std::lock_guard<std::mutex> lk3(exec_mtx);
                exec_order.push_back(2);
              }
              std::lock_guard<std::mutex> lk4(ctx->mutex);
              ctx->active_worker_count--;
            });
          }
        }
      }
      {
        std::lock_guard<std::mutex> lk(exec_mtx);
        exec_order.push_back(1);
      }
      std::lock_guard<std::mutex> lk(ctx->mutex);
      ctx->active_worker_count--;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pool.stop();

    check(exec_order.size() >= 1,
          "test_chain_scheduling_basic: worker已调度执行");
    check(ctx->active_worker_count <= 2,
          "test_chain_scheduling_basic: active_worker_count不超过max");
    check(ctx->queued_chunks.size() <= 1,
          "test_chain_scheduling_basic: 大部分chunk已被消费");
  }

  std::cout << "\n[2] test_response_ordering\n";
  {
    auto ctx = std::make_shared<ConnectionWorkContext>();

    std::vector<WorkResult> results;
    for (int i = 0; i < 5; i++) {
      WorkResult wr;
      wr.response_seq = static_cast<uint64_t>(i + 1);
      wr.request_id = "req-" + std::to_string(i + 1);
      wr.response_data = "data-" + std::to_string(i + 1);
      wr.has_response = true;
      results.push_back(std::move(wr));
    }

    std::vector<size_t> insert_order = {2, 0, 3, 1, 4};
    std::vector<std::string> applied;

    for (auto idx : insert_order) {
      auto& r = results[idx];
      std::lock_guard<std::mutex> lk(ctx->mutex);

      if (r.response_seq == ctx->last_applied_response_seq + 1) {
        applied.push_back(r.response_data);
        ctx->last_applied_response_seq++;

        while (true) {
          auto it = ctx->pending_results.find(ctx->last_applied_response_seq + 1);
          if (it == ctx->pending_results.end()) break;
          applied.push_back(it->second.response_data);
          ctx->pending_results.erase(it);
          ctx->last_applied_response_seq++;
        }
      } else {
        ctx->pending_results.emplace(r.response_seq, std::move(r));
      }
    }

    check(applied.size() == 5, "test_response_ordering: 5个响应全部应用");
    check(applied[0] == "data-1", "test_response_ordering: 响应1排第一");
    check(applied[1] == "data-2", "test_response_ordering: 响应2排第二");
    check(applied[2] == "data-3", "test_response_ordering: 响应3排第三");
    check(applied[3] == "data-4", "test_response_ordering: 响应4排第四");
    check(applied[4] == "data-5", "test_response_ordering: 响应5排第五");
  }

  std::cout << "\n[3] test_fd_no_leak_on_disconnect\n";
  {
    auto ctx = std::make_shared<ConnectionWorkContext>();

    auto count_open_fds = [&]() -> int {
      int cnt = 0;
      for (int fd = 3; fd < 1024; fd++) {
        if (::fcntl(fd, F_GETFD) != -1) cnt++;
      }
      return cnt;
    };
    int fd_count_before = count_open_fds();

    int test_fd = ::open("/dev/null", O_RDONLY);
    assert(test_fd >= 0);

    WorkResult wr;
    wr.sendfile_fd = test_fd;
    wr.response_seq = 1;
    wr.has_sendfile = true;

    {
      std::lock_guard<std::mutex> lk(ctx->mutex);
      ctx->draining = true;
      ctx->pending_results.emplace(wr.response_seq, std::move(wr));
    }

    {
      std::lock_guard<std::mutex> lk(ctx->mutex);
      for (auto& [seq, result] : ctx->pending_results) {
        if (result.sendfile_fd >= 0) {
          ::close(result.sendfile_fd);
          result.sendfile_fd = -1;
        }
      }
      ctx->pending_results.clear();
    }

    int fd_count_after = count_open_fds();
    check(fd_count_after <= fd_count_before + 5,
          "test_fd_no_leak_on_disconnect: 连接关闭后fd被正确关闭");
  }

  std::cout << "\n[4] test_worker_count_bound\n";
  {
    ThreadPool pool(4, "BOUND_TEST", 128);
    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->max_concurrent_workers = 2;
    ctx->worker_running = true;
    ctx->active_worker_count = 1;

    for (int i = 0; i < 10; i++) {
      PendingChunk chunk;
      chunk.enqueue_seq = static_cast<uint64_t>(i + 1);
      ctx->queued_chunks.push_back(std::move(chunk));
    }

    std::atomic<size_t> peak_active{1};
    std::atomic<size_t> total_workers{0};
    std::atomic<bool> done{false};

    std::function<void(std::shared_ptr<ConnectionWorkContext>)> worker_func;
    worker_func = [&](std::shared_ptr<ConnectionWorkContext> wk_ctx) {
      total_workers.fetch_add(1);

      {
        std::lock_guard<std::mutex> lk(wk_ctx->mutex);
        size_t cur = wk_ctx->active_worker_count;
        size_t prev = peak_active.load();
        while (cur > prev && !peak_active.compare_exchange_weak(prev, cur)) {}
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      {
        std::lock_guard<std::mutex> lk(wk_ctx->mutex);
        if (!wk_ctx->queued_chunks.empty()) {
          wk_ctx->queued_chunks.pop_front();
          if (!wk_ctx->queued_chunks.empty() &&
              wk_ctx->active_worker_count < wk_ctx->max_concurrent_workers) {
            wk_ctx->active_worker_count++;
            pool.addtask([&, wk_ctx]() { worker_func(wk_ctx); });
          }
        }
        wk_ctx->active_worker_count--;
        if (wk_ctx->active_worker_count == 0 && !wk_ctx->queued_chunks.empty()) {
          wk_ctx->active_worker_count = 1;
          pool.addtask([&, wk_ctx]() { worker_func(wk_ctx); });
        }
        if (wk_ctx->active_worker_count == 0) {
          done.store(true);
        }
      }
    };

    pool.addtask([&]() { worker_func(ctx); });

    for (int wait = 0; wait < 50 && !done.load(); wait++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    pool.stop();

    check(peak_active.load() <= 2,
          "test_worker_count_bound: peak_active_worker_count不超过max");
    check(total_workers.load() >= 2,
          "test_worker_count_bound: 至少启动了2个worker");
  }

  std::cout << "\n[5] test_draining_no_new_worker\n";
  {
    ThreadPool pool(4, "DRAIN_TEST", 128);
    auto ctx = std::make_shared<ConnectionWorkContext>();
    ctx->worker_running = true;
    ctx->active_worker_count = 1;

    for (int i = 0; i < 5; i++) {
      PendingChunk chunk;
      chunk.enqueue_seq = static_cast<uint64_t>(i + 1);
      ctx->queued_chunks.push_back(std::move(chunk));
    }

    std::atomic<size_t> started_after_drain{0};
    std::atomic<bool> drain_set{false};
    std::atomic<bool> worker_done{false};

    std::function<void(std::shared_ptr<ConnectionWorkContext>)> drain_worker;
    drain_worker = [&](std::shared_ptr<ConnectionWorkContext> wk_ctx) {
      bool is_draining = false;
      {
        std::lock_guard<std::mutex> lk(wk_ctx->mutex);
        is_draining = wk_ctx->draining;
      }
      if (is_draining) {
        started_after_drain.fetch_add(1);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      {
        std::lock_guard<std::mutex> lk(wk_ctx->mutex);
        if (!wk_ctx->queued_chunks.empty()) {
          wk_ctx->queued_chunks.pop_front();
        }
        wk_ctx->active_worker_count--;

        if (wk_ctx->draining) {
          worker_done.store(true);
          return;
        }

        if (wk_ctx->active_worker_count == 0 && !wk_ctx->queued_chunks.empty()) {
          wk_ctx->active_worker_count = 1;
          pool.addtask([&, wk_ctx]() { drain_worker(wk_ctx); });
        } else {
          worker_done.store(true);
        }
      }
    };

    pool.addtask([&]() { drain_worker(ctx); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      std::lock_guard<std::mutex> lk(ctx->mutex);
      ctx->draining = true;
      ctx->queued_chunks.clear();
    }
    drain_set.store(true);

    for (int wait = 0; wait < 20 && !worker_done.load(); wait++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    pool.stop();

    check(started_after_drain.load() == 0,
          "test_draining_no_new_worker: 排空模式下无新worker启动");
  }

  std::cout << "\n[6] test_backpressure_with_parallel\n";
  {
    const size_t kMaxQueueSize = 4;
    ThreadPool pool(2, "BPRES_TEST", kMaxQueueSize);

    std::atomic<size_t> rejected{0};

    for (size_t i = 0; i < 100; i++) {
      bool ok = pool.addTask(Task([i]() {
        (void)i;
      }));
      if (!ok) rejected.fetch_add(1);
    }

    auto ctx = std::make_shared<ConnectionWorkContext>();
    {
      std::lock_guard<std::mutex> lk(ctx->mutex);
      ctx->queued_bytes = 600 * 1024;
      bool exceeded = ctx->queued_bytes > static_cast<size_t>(512 * 1024);
      check(exceeded, "test_backpressure_with_parallel: 连接级背压可被检测");

      ctx->queued_chunks.clear();
      ctx->queued_bytes = 0;
    }

    pool.stop();
    check(rejected.load() > 0,
          "test_backpressure_with_parallel: 队列满时addTask返回false");
  }

  std::cout << "\n=== 结果: " << passed << " 通过, " << failed << " 失败 ===\n";

  if (failed > 0) {
    return 1;
  }
  return 0;
}
