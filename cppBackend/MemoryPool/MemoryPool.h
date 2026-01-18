#pragma once
#include"threadcache.h"

//最上层类，直接调用ThreadCache的单例模式进行初始化，使用内存池时，直接使用此类
class MemoryPool{
public:
  static void* allocate(size_t size){
    return threadcache::getInstance()->allocate(size);
  }
  static void deallocate(void *ptr,size_t size){
    threadcache::getInstance()->deallocate(ptr,size);
  }
};