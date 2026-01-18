#pragma once
#include"common.h"
#include <cstdlib>

class threadcache{
public:
  static threadcache* getInstance(){//获取单例模式
    //thraed_local标志每一个线程都会初始化一个这样的实例
    //也就相当于每个线程都会有一个自己的ThreadCache实例
    //这样做避免了锁竞争，提高了并发性能，简化代码（不需要复杂的同步机制）,本地化分配减少了冲突
    static thread_local threadcache instance;
    return &instance;
  }

  void* allocate(size_t size);  //内存分配
  void deallocate(void* ptr,size_t size);//回收分配的内存

private:
  threadcache() =default; //默认构造函数
  //从中心缓存中获取内存
  void *fetchFromCentralCache(size_t index);
  //归还内存到中心缓存
  void returnToCentralCache(void* start,size_t size);
  //计算批量获取内存块的数量
  size_t getBatchNum(size_t size);
  //判断是否需要归还内存给中心缓存
  bool shouldReturnToCentralCache(size_t index);

  //每个线程的自由链表数组 FREE_LIST_SIZE= 256*1024/8
  //内存块链表的头指针数组
  //[0]存储8字节块,[1]存储16字节块,以此类推
  //每个字节快中又有一个指针指向下一个块，形成一个链表
  //很像hash的链表法
  std::array<void*, FREE_LIST_SIZE> freeList_;  //
  //链表长度计数器数组，实时跟踪每个链表的长度，分配了内存--，释放内存++
  std::array<size_t,FREE_LIST_SIZE> freeListSize_;
};