#include"threadcache.h"
#include"centralCache.h"

  //分配内存
void* threadcache::allocate(size_t size){
  //处理0
  if(size ==0){
    size = ALIGNMENT; //  至少分配一个对齐大小
  }
  if(size > MAXBYTE) {
    return malloc(size);
  }

  size_t index =sizeclass::getIndex(size);

  //检查线程本地自由链表
  //如果freeList_[index]不为空，表示该链表有可用的内存块
  if(freeList_[index]){
    void *ptr =freeList_[index];
    //将freeList_[index]指向 内存块的下一块内存地址
    //这样做的原因是我们的内存块前8个字节是一个指针用于存储下一个块的地址
    freeList_[index] = *reinterpret_cast<void **>(ptr); 
    //更新自由链表大小
    freeListSize_[index]--;
    return ptr;
  }
  return fetchFromCentralCache(index);
}
//回收分配的内存
void threadcache::deallocate(void* ptr,size_t size){
  if(size > MAXBYTE){
    free(ptr);
    return;
  }

  size_t index =sizeclass::getIndex(size);

  //插入到线程本地自由链表
  *reinterpret_cast<void**>(ptr) = freeList_[index];
  freeList_[index] = ptr;

  //更新自由链表大小
  freeListSize_[index]++;

  if(shouldReturnToCentralCache(index)){
    returnToCentralCache(freeList_[index],size);
  }
}

//从中心缓存中获取内存
void* threadcache::fetchFromCentralCache(size_t index){
  size_t size = (index + 1) * ALIGNMENT;
  // 根据对象内存大小计算批量获取的数量
  size_t batchNum = getBatchNum(size);
  size_t RealIncrease=0;
  // 从中心缓存批量获取内存
  void * start = centralCache::getInstance().fetchRange(index,batchNum,RealIncrease);
  
  if(!start) return nullptr;

  freeListSize_[index]+=(RealIncrease - 1); //增大对应自由链表大小

  // 取一个返回，其余放入线程本地自由链表
  void *result =start;
  if(batchNum>1){
    freeList_[index]=*reinterpret_cast<void **>(start);
  }

  return result;
}

//归还内存到中心缓存
void threadcache::returnToCentralCache(void* start,size_t size){
  //根据大小计算对应索引
  size_t index = sizeclass::getIndex(size);

  //获取对齐之后的实际块的大小((向上查找)8的倍数取整)
  size_t alignedSize = sizeclass::roundUp(size);

  //计算要归还的内存块的数量
  size_t batchNum = freeListSize_[index];
  if(batchNum <= 1) return;

  //保留一部分在ThreadCache中（比如保留1/4）
  size_t keepNum= std::max(batchNum / 4 , size_t(1));
  size_t returnNum = batchNum - keepNum;

  //将内存块串成链表
  char *current =static_cast<char*>(start);
  //使用对齐后的大小计算分割点
  char *splitNode =current;
  for(size_t i = 0;i<keepNum -1;i++){
    splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
    if(splitNode ==nullptr){
      //如果链表提前结束，更新实际返回的数量
      returnNum = batchNum -(i+1);
      break;
    }
  }

  if(splitNode != nullptr){
    // 将要返回的部分和要保留的部分断开
    void* nextNode = *reinterpret_cast<void**>(splitNode);
    *reinterpret_cast<void**>(splitNode) = nullptr; // 断开连接

    // 更新ThreadCache的空闲链表
    freeList_[index] = start;

    // 更新自由链表大小
    freeListSize_[index] = keepNum;

    // 将剩余部分返回给CentralCache
    if (returnNum > 0 && nextNode != nullptr)
    {
        centralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
    }
  }
}
//计算批量获取内存块的数量
size_t threadcache::getBatchNum(size_t size){
  // 基准：每次批量获取不超过4KB内存
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 4KB

    // 根据对象大小设置合理的基准批量数
    size_t baseNum;
    if (size <= 32) baseNum = 64;    // 64 * 32 = 2KB
    else if (size <= 64) baseNum = 32;  // 32 * 64 = 2KB
    else if (size <= 128) baseNum = 16; // 16 * 128 = 2KB
    else if (size <= 256) baseNum = 8;  // 8 * 256 = 2KB
    else if (size <= 512) baseNum = 4;  // 4 * 512 = 2KB
    else if (size <= 1024) baseNum = 2; // 2 * 1024 = 2KB
    else baseNum = 1;                   // 大于1024的对象每次只从中心缓存取1个

    // 计算最大批量数
    size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);

    // 取最小值，但确保至少返回1
    return std::max((size_t)1, std::min(maxNum, baseNum));
}
//判断是否需要归还内存给中心缓存
bool threadcache::shouldReturnToCentralCache(size_t index){
  //设定阈值，当自由链表的大小超过一定数量时
  size_t shouldreturn = 64; //在这里设定如果超过64个内存块 就需要归还内存了
  return (freeListSize_[index]>shouldreturn);
}