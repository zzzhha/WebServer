#include <cassert>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "reactor/ThreadPool.h"

using Clock = std::chrono::steady_clock;

bool g_all_passed = true;

static void check(bool cond, const std::string& msg)
{
  if (!cond) {
    std::cerr << "FAIL: " << msg << std::endl;
    g_all_passed = false;
  } else {
    std::cout << "  PASS: " << msg << std::endl;
  }
}

static void section(const std::string& name)
{
  std::cout << "\n=== " << name << " ===" << std::endl;
}

int test_chain_scheduling_basic()
{
  section("test_chain_scheduling_basic");
  ThreadPool pool(4, "TEST_CHAIN", 4096);

  struct ConnCtx {
    std::mutex m;
    std::deque<int> chunks;
    std::atomic<size_t> active_workers{0};
    size_t max_concurrent{3};
    std::vector<int> processed_order;
    std::mutex order_m;
  };
  auto ctx = std::make_shared<ConnCtx>();

  for (int i = 0; i < 10; i++) ctx->chunks.push_back(i);

  std::mutex launch_m;
  auto try_launch = [&pool, &launch_m](std::shared_ptr<ConnCtx> c,
                                        std::function<void(std::shared_ptr<ConnCtx>)> proc) {
    std::lock_guard<std::mutex> lk(launch_m);
    size_t cur = c->active_workers.load(std::memory_order_relaxed);
    bool need_launch = false;
    {
      std::lock_guard<std::mutex> lk2(c->m);
      if (!c->chunks.empty() && cur < c->max_concurrent) {
        need_launch = true;
        c->active_workers.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (need_launch) {
      pool.addtask([c, proc]() { proc(c); });
    }
  };

  std::function<void(std::shared_ptr<ConnCtx>)> process_one;
  process_one = [&process_one, &try_launch](std::shared_ptr<ConnCtx> c) {
    int val = -1;
    {
      std::lock_guard<std::mutex> lk(c->m);
      if (c->chunks.empty()) {
        c->active_workers.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      val = c->chunks.front();
      c->chunks.pop_front();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    {
      std::lock_guard<std::mutex> lk(c->order_m);
      c->processed_order.push_back(val);
    }

    try_launch(c, process_one);

    c->active_workers.fetch_sub(1, std::memory_order_relaxed);
  };

  ctx->active_workers.fetch_add(1, std::memory_order_relaxed);
  pool.addtask([ctx, &process_one]() { process_one(ctx); });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  size_t count = 0;
  {
    std::lock_guard<std::mutex> lk(ctx->order_m);
    count = ctx->processed_order.size();
  }
  check(count == 10,
        "all 10 chunks processed, got " + std::to_string(count));

  pool.stop();
  return 0;
}

int test_worker_count_bound()
{
  section("test_worker_count_bound");
  ThreadPool pool(4, "TEST_BOUND", 4096);

  struct ConnCtx {
    std::mutex m;
    std::deque<int> chunks;
    std::atomic<size_t> active_workers{0};
    std::atomic<size_t> peak_workers{0};
    size_t max_concurrent{2};
  };
  auto ctx = std::make_shared<ConnCtx>();

  for (int i = 0; i < 20; i++) ctx->chunks.push_back(i);

  std::mutex launch_m;
  auto try_launch = [&pool, &launch_m](std::shared_ptr<ConnCtx> c,
                                        std::function<void(std::shared_ptr<ConnCtx>)> proc) {
    std::lock_guard<std::mutex> lk(launch_m);
    size_t cur = c->active_workers.load(std::memory_order_relaxed);
    bool need_launch = false;
    {
      std::lock_guard<std::mutex> lk2(c->m);
      if (!c->chunks.empty() && cur < c->max_concurrent) {
        need_launch = true;
        c->active_workers.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (need_launch) {
      pool.addtask([c, proc]() { proc(c); });
    }
  };

  std::function<void(std::shared_ptr<ConnCtx>)> process_one;
  process_one = [&process_one, &try_launch](std::shared_ptr<ConnCtx> c) {
    size_t cur = c->active_workers.load(std::memory_order_relaxed);
    size_t prev = c->peak_workers.load(std::memory_order_relaxed);
    while (cur > prev &&
           !c->peak_workers.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}

    int val = -1;
    {
      std::lock_guard<std::mutex> lk(c->m);
      if (c->chunks.empty()) {
        c->active_workers.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      val = c->chunks.front();
      c->chunks.pop_front();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3));

    try_launch(c, process_one);

    c->active_workers.fetch_sub(1, std::memory_order_relaxed);
  };

  ctx->active_workers.fetch_add(1, std::memory_order_relaxed);
  pool.addtask([ctx, &process_one]() { process_one(ctx); });

  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  check(ctx->peak_workers.load() <= ctx->max_concurrent,
        "peak workers " + std::to_string(ctx->peak_workers.load()) +
        " <= max " + std::to_string(ctx->max_concurrent));
  bool empty_chunks = false;
  {
    std::lock_guard<std::mutex> lk(ctx->m);
    empty_chunks = ctx->chunks.empty();
  }
  check(empty_chunks, "all chunks consumed");

  pool.stop();
  return 0;
}

int test_response_ordering()
{
  section("test_response_ordering");
  ThreadPool pool(4, "TEST_ORDER", 4096);

  struct ConnCtx {
    std::mutex m;
    std::deque<int> chunks;
    std::atomic<size_t> active_workers{0};
    size_t max_concurrent{3};
    std::map<int, std::string> pending_results;
    int last_applied_seq{0};
    std::vector<int> applied_order;
    std::atomic<int> next_seq{1};
  };
  auto ctx = std::make_shared<ConnCtx>();

  for (int i = 0; i < 12; i++) ctx->chunks.push_back(i);

  auto apply_result = [ctx](int seq) {
    std::lock_guard<std::mutex> lk(ctx->m);
    if (seq == ctx->last_applied_seq + 1) {
      ctx->applied_order.push_back(seq);
      ctx->last_applied_seq = seq;
      while (true) {
        auto it = ctx->pending_results.find(ctx->last_applied_seq + 1);
        if (it == ctx->pending_results.end()) break;
        ctx->applied_order.push_back(it->first);
        ctx->last_applied_seq++;
      }
    } else {
      ctx->pending_results.emplace(seq, "");
    }
  };

  std::mutex launch_m;
  auto try_launch = [&pool, &launch_m](std::shared_ptr<ConnCtx> c,
                                        std::function<void(std::shared_ptr<ConnCtx>)> proc) {
    std::lock_guard<std::mutex> lk(launch_m);
    size_t cur = c->active_workers.load(std::memory_order_relaxed);
    bool need_launch = false;
    {
      std::lock_guard<std::mutex> lk2(c->m);
      if (!c->chunks.empty() && cur < c->max_concurrent) {
        need_launch = true;
        c->active_workers.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (need_launch) {
      pool.addtask([c, proc]() { proc(c); });
    }
  };

  std::function<void(std::shared_ptr<ConnCtx>)> process_one;
  process_one = [&process_one, &try_launch, &apply_result](std::shared_ptr<ConnCtx> c) {
    int val = -1;
    int seq = 0;
    {
      std::lock_guard<std::mutex> lk(c->m);
      if (c->chunks.empty()) {
        c->active_workers.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      val = c->chunks.front();
      c->chunks.pop_front();
      seq = c->next_seq.fetch_add(1, std::memory_order_relaxed);
    }

    int delay_ms = (val % 4 + 1) * 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    apply_result(seq);

    try_launch(c, process_one);

    c->active_workers.fetch_sub(1, std::memory_order_relaxed);
  };

  ctx->active_workers.fetch_add(1, std::memory_order_relaxed);
  pool.addtask([ctx, &process_one]() { process_one(ctx); });

  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  size_t applied_count = 0;
  {
    std::lock_guard<std::mutex> lk(ctx->m);
    applied_count = ctx->applied_order.size();
  }
  check(applied_count == 12,
        "12 results applied, got " + std::to_string(applied_count));

  bool in_order = true;
  {
    std::lock_guard<std::mutex> lk(ctx->m);
    for (size_t i = 0; i < ctx->applied_order.size(); i++) {
      if (ctx->applied_order[i] != static_cast<int>(i + 1)) {
        in_order = false;
        break;
      }
    }
  }
  check(in_order, "responses applied in strict order by response_seq");

  pool.stop();
  return 0;
}

int test_draining_no_new_worker()
{
  section("test_draining_no_new_worker");
  ThreadPool pool(4, "TEST_DRAIN", 4096);

  struct ConnCtx {
    std::mutex m;
    std::deque<int> chunks;
    std::atomic<size_t> active_workers{0};
    std::atomic<size_t> new_workers_after_drain{0};
    size_t max_concurrent{2};
    std::atomic<bool> draining{false};
  };
  auto ctx = std::make_shared<ConnCtx>();

  for (int i = 0; i < 10; i++) ctx->chunks.push_back(i);

  std::mutex launch_m;
  auto try_launch = [&pool, &launch_m](std::shared_ptr<ConnCtx> c,
                                        std::function<void(std::shared_ptr<ConnCtx>)> proc) {
    std::lock_guard<std::mutex> lk(launch_m);
    if (c->draining.load(std::memory_order_relaxed)) {
      return;
    }
    size_t cur = c->active_workers.load(std::memory_order_relaxed);
    bool need_launch = false;
    {
      std::lock_guard<std::mutex> lk2(c->m);
      if (!c->chunks.empty() && cur < c->max_concurrent) {
        need_launch = true;
        c->active_workers.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (need_launch) {
      pool.addtask([c, proc]() {
        if (c->draining.load(std::memory_order_relaxed)) {
          c->new_workers_after_drain.fetch_add(1, std::memory_order_relaxed);
          c->active_workers.fetch_sub(1, std::memory_order_relaxed);
          return;
        }
        proc(c);
      });
    }
  };

  std::function<void(std::shared_ptr<ConnCtx>)> process_one;
  process_one = [&process_one, &try_launch](std::shared_ptr<ConnCtx> c) {
    if (c->draining.load(std::memory_order_relaxed)) {
      c->active_workers.fetch_sub(1, std::memory_order_relaxed);
      return;
    }

    int val = -1;
    {
      std::lock_guard<std::mutex> lk(c->m);
      if (c->chunks.empty()) {
        c->active_workers.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      val = c->chunks.front();
      c->chunks.pop_front();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3));

    try_launch(c, process_one);

    c->active_workers.fetch_sub(1, std::memory_order_relaxed);
  };

  ctx->active_workers.fetch_add(1, std::memory_order_relaxed);
  pool.addtask([ctx, &process_one]() { process_one(ctx); });

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  ctx->draining.store(true, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  check(ctx->new_workers_after_drain.load() == 0,
        "no new workers started after drain: " +
        std::to_string(ctx->new_workers_after_drain.load()));

  pool.stop();
  return 0;
}

int test_fd_no_leak_on_disconnect()
{
  section("test_fd_no_leak_on_disconnect");

  std::atomic<int> closed_count{0};
  std::atomic<int> leaked_count{0};

  auto safe_close = [&closed_count, &leaked_count](int fd) {
    if (fd < 0) return;
    int rc = ::fcntl(fd, F_GETFD);
    if (rc == -1) {
      leaked_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ::close(fd);
    closed_count.fetch_add(1, std::memory_order_relaxed);
  };

  struct SendFileResult {
    int fd{-1};
    int seq{0};
  };

  ThreadPool pool(4, "TEST_FD", 4096);
  std::deque<SendFileResult> results;
  std::mutex res_m;

  for (int i = 0; i < 10; i++) {
    int fd = ::open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    SendFileResult r;
    r.fd = fd;
    r.seq = i + 1;
    std::lock_guard<std::mutex> lk(res_m);
    results.push_back(r);
  }

  for (int i = 0; i < 6; i++) {
    pool.addtask([&results, &res_m, &safe_close]() {
      SendFileResult r;
      bool has = false;
      {
        std::lock_guard<std::mutex> lk(res_m);
        if (!results.empty()) {
          r = results.front();
          results.pop_front();
          has = true;
        }
      }
      if (!has) return;
      if (r.seq % 3 == 0) {
        safe_close(r.fd);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  {
    std::lock_guard<std::mutex> lk(res_m);
    for (auto& r : results) {
      safe_close(r.fd);
    }
    results.clear();
  }

  pool.stop();

  check(closed_count.load() > 0, "some fds properly closed: " + std::to_string(closed_count.load()));
  check(leaked_count.load() == 0,
        "no leaked fds (double-close detected): " + std::to_string(leaked_count.load()));

  return 0;
}

int test_backpressure_with_parallel()
{
  section("test_backpressure_with_parallel");
  ThreadPool pool(4, "TEST_BP", 400);

  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<int> completed{0};

  for (int i = 0; i < 2000; i++) {
    Task t;
    t.fn = [&completed]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      completed.fetch_add(1, std::memory_order_relaxed);
    };
    if (pool.addTask(std::move(t))) {
      accepted.fetch_add(1, std::memory_order_relaxed);
    } else {
      rejected.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  check(accepted.load() > 0, "some tasks accepted: " + std::to_string(accepted.load()));
  check(rejected.load() > 0, "some tasks rejected via backpressure: " +
        std::to_string(rejected.load()));
  check(accepted.load() + rejected.load() == 2000,
        "all tasks counted: " + std::to_string(accepted.load() + rejected.load()));

  pool.stop();
  return 0;
}

int main()
{
  std::cout << "HttpServer Parallel Tests (Strategy B Phase 1)" << std::endl;
  std::cout << "================================================" << std::endl;

  test_chain_scheduling_basic();
  test_worker_count_bound();
  test_response_ordering();
  test_draining_no_new_worker();
  test_fd_no_leak_on_disconnect();
  test_backpressure_with_parallel();

  std::cout << "\n================================================" << std::endl;
  if (g_all_passed) {
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
  } else {
    std::cout << "SOME TESTS FAILED" << std::endl;
    return 1;
  }
}
