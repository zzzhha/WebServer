#pragma once
#include"common.h"
#include<map>
#include<mutex>

class pageCache{
public:
  static const size_t PAGE_SIZE = 4096; //4K页大小

  static pageCache& getInstance(){
    static pageCache instance;
    return instance;
  }

  //分配指定页数的span
  void *allocateSpan(size_t numPages);

  //释放span
  void deallocateSpan(void* ptr,size_t numPages);

  //分配大型内存（>LARGE_THRESHOLD）
  void* allocateLarge(size_t size);

  //释放大型内存
  void deallocateLarge(void* ptr, size_t size);

private:
  pageCache() =default;

  //向系统申请内存
  void *systemAlloc(size_t numPages);

  struct Span{
    void *pageAddr;   //页起始地址
    size_t numPages;  //页数
    Span *next;       //链表指针
  };

  //按页数管理空闲的span,不同页数对应不同Span链表
  std::map<size_t,Span*> freeSpans_;

  //页号到span的映射，用于回收
  std::map<void*,Span*> spanMap_;

  //大型内存映射（ptr -> numPages），用于大型内存释放
  std::map<void*,size_t> largeSpanMap_;

  //中型内存的mutex
  std::mutex mutex_;

  //大型内存的独立mutex
  std::mutex largeMutex_;
};