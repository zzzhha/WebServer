#pragma once
#include <vector>
#include <utility>
#include <cstddef>
#include "MemoryPool.h"

namespace {

// 线程本地延迟释放缓冲：超过阈值时刷入 MemoryPool
struct DeferredFreeBuffer {
  std::vector<std::pair<void*, size_t>> items;

  ~DeferredFreeBuffer() {
    // 低危修复：线程退出时自动刷掉残留的延迟释放块。
    // 此前仅靠事件循环在固定位置调用 FlushDeferredFrees()，
    // 直接退出的线程（如 proactor worker）最多残留 63 块内存未归还 → 少量泄漏。
    for (auto& [p, s] : items) {
      MemoryPool::deallocate(p, s);
    }
    items.clear();
  }
};

inline thread_local DeferredFreeBuffer tls_defer_free;

}  // namespace

inline void DeferDeallocate(void* ptr, size_t size)
{
  tls_defer_free.items.emplace_back(ptr, size);
  if (tls_defer_free.items.size() >= 64)
  {
    for (auto& [p, s] : tls_defer_free.items)
    {
      MemoryPool::deallocate(p, s);
    }
    tls_defer_free.items.clear();
  }
}

inline void FlushDeferredFrees()
{
  for (auto& [p, s] : tls_defer_free.items)
  {
    MemoryPool::deallocate(p, s);
  }
  tls_defer_free.items.clear();
}
