#include "pageCache.h"
#include <sys/mman.h>
#include <cstring>

void *pageCache::allocateSpan(size_t numPages){
  std::lock_guard<std::mutex> lock(mutex_);

  //查找合适的空闲span
  auto it = freeSpans_.lower_bound(numPages);
  if(it != freeSpans_.end()){
    std::unique_ptr<Span> spanPtr = std::move(it->second);
    Span *span = spanPtr.get();

    //将取出的span从原有的空闲链表freeSpans_[it->first]中移除
    if(span->next){
      freeSpans_[it->first] = std::move(span->next);
    }else{
      freeSpans_.erase(it);
    }

    //如果span大于需要的numPages则进行分割
    if(span->numPages>numPages){
      std::unique_ptr<Span> newSpan = std::make_unique<Span>();
      newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
      newSpan->numPages = span->numPages - numPages;
      newSpan->next = nullptr;
    
      //将超出部分放回空闲Span*列表头部
      auto &list = freeSpans_[newSpan->numPages];
      newSpan->next = std::move(list);
      list = std::move(newSpan);

      span->numPages = numPages;
    }

    spanMap_[span->pageAddr] = std::move(spanPtr);
    return span->pageAddr;
  }

  //没有合适的span，向系统申请
  void *memory = systemAlloc(numPages);
  if(!memory) return nullptr;

  // 创建新的span
  std::unique_ptr<Span> span = std::make_unique<Span>();
  span->pageAddr = memory;
  span->numPages =numPages;
  span->next = nullptr;

  // 记录span信息用于回收
  spanMap_[memory] = std::move(span);
  return memory;
}


void pageCache::deallocateSpan(void* ptr, size_t numPages){
  std::lock_guard<std::mutex> lock(mutex_);

  //查找对应的span，没找到代表不是PageCache分配的内存，直接返回
  auto it = spanMap_.find(ptr);
  if (it == spanMap_.end()) return;

  Span* span = it->second.get();

  //尝试合并相邻的span
  void *nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
  auto nextIt = spanMap_.find(nextAddr);

  if(nextIt != spanMap_.end()){
    Span *nextSpan = nextIt->second.get();

    //检查nextSpan是否在空闲链表中
    bool found = false;
    auto &nextList = freeSpans_[nextSpan->numPages];

    if(nextList.get() == nextSpan){
      nextList = std::move(nextSpan->next);
      found = true;
    }else if(nextList)// 只有在链表非空时才遍历
    {
      Span* prev = nextList.get();
      while(prev->next){
        if(prev->next.get() == nextSpan){
          // 将nextSpan从空闲链表中移除
          prev->next = std::move(nextSpan->next);
          found = true;
          break;
        }
        prev = prev->next.get();
      }
    }
    //只有在找到nextSpan的情况下才进行合并
    if (found){
      // 合并span
      span->numPages += nextSpan->numPages;
      spanMap_.erase(nextAddr);
      // nextSpan会被unique_ptr自动释放
    }
  }

  //将合并后的span通过头插法插入空闲列表
  auto& list = freeSpans_[span->numPages];
  span->next = std::move(list);
  list = std::move(it->second);
}

void* pageCache::systemAlloc(size_t numPages){
  size_t size = numPages * PAGE_SIZE;

  //使用mmap分配内存（因为PageCache是大块内存，mmap会比malloc会好点(大块内存malloc在底层就是用的mmap)）
  void* ptr =mmap(nullptr,size, PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1, 0);
  if (ptr == MAP_FAILED) return nullptr;

  // 清零内存
  memset(ptr, 0, size);
  return ptr;
}

void* pageCache::allocateLarge(size_t size){
  std::lock_guard<std::mutex> lock(largeMutex_);

  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) return nullptr;

  memset(ptr, 0, size);

  // 记录大型内存信息用于释放
  largeSpanMap_[ptr] = size;
  return ptr;
}

void pageCache::deallocateLarge(void* ptr, size_t size){
  std::lock_guard<std::mutex> lock(largeMutex_);

  auto it = largeSpanMap_.find(ptr);
  if (it != largeSpanMap_.end()) {
    size_t recordedSize = it->second;
    largeSpanMap_.erase(it);
    munmap(ptr, recordedSize);
  } else {
    munmap(ptr, size);
  }
}