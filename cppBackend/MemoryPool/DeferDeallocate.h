#pragma once
#include <vector>
#include <utility>
#include <cstddef>
#include "MemoryPool.h"

inline thread_local std::vector<std::pair<void*, size_t>> tls_defer_free;

inline void DeferDeallocate(void* ptr, size_t size) {
  if (!ptr) return;
  tls_defer_free.emplace_back(ptr, size);
  if (tls_defer_free.size() >= 64) {
    for (auto& [p, s] : tls_defer_free) {
      MemoryPool::deallocate(p, s);
    }
    tls_defer_free.clear();
  }
}

inline void FlushDeferredFrees() {
  if (tls_defer_free.empty()) return;
  for (auto& [p, s] : tls_defer_free) {
    MemoryPool::deallocate(p, s);
  }
  tls_defer_free.clear();
}
