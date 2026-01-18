#pragma once
#include"common.h"
#include<mutex>

class centralCache{
public:
  static centralCache& getInstance(){//单例模式的实现
    static centralCache instance;
    return instance;
  }

  void *fetchRange(size_t index,size_t batchNum,size_t& RealIncrease); 
  void returnRange(void *start,size_t size,size_t index);

private:
  // 相互是还所有原子指针为nullptr
  centralCache(){ //构造函数
  //store函数将第一个参数赋值给对象(原语)，相当于这里将指针值为空
  //第二个参数relaxed表明编译器和寄存器可以随意改变这里的顺序，只要最后结果将指针置为空就可以了
    for(auto &ptr:centralFreeList_){
      ptr.store(nullptr,std::memory_order_relaxed);
    }
    //初始化所有锁，将所有锁释放，使得资源都可以申请锁
    for(auto &lock:locks_){
      lock.clear();
    }
  }

  void *fetchFromPageCache(size_t size);//从PageCache中获取内存

  // 中心缓存的自由链表
  std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

  // 用于同步的自旋锁
  //没有用锁实现，用的是原子变量去实现自旋锁
  std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
};