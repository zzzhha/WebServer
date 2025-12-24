#include"centralCache.h"
#include"pageCache.h"
#include<cassert>
#include<thread>

//每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

void *centralCache::fetchRange(size_t index,size_t batchNum,size_t &RealIncrease){
  //如果下标大于了最大长度或者申请的内存个数为0，就返回空
  if(index >= FREE_LIST_SIZE || batchNum == 0){
    return nullptr;
  }

  //自旋锁保护
  //这里调用函数test_and_set，这个函数是atomic_flag（这个类类似于一个bool变量）自带的成员函数
  //他的作用是读取当前对象的值，然后将当前对象设置为true，返回原来的值
  //放在这里就是当lock上锁的时候（也就是对象为true的时候），他会一直死循环（while(true（返回值为true）)）
  //然后std::this_thread::yield()会使当前线程自主放弃cpu，是线程进入准备态
  //实现了一个不占用cpu的死循环
  //而当lock没有上锁的时候（也就是对象为false的时候),他会获得锁，然后上锁，结束循环（while(false（返回值为false）)）
  //执行下面的语句
  //std::memory_order_acquire这个参数标志着，让cpu和编译器在 acquire 操作之后的所有内存操作（读和写），绝不会被重排到 acquire 操作之前
  //保证了这条语句是最先执行的，然后再进行下面的语句
  while(locks_[index].test_and_set(std::memory_order_acquire)){
    std::this_thread::yield();  //添加线程让步，避免忙等待，避免过度消耗CPU
  }

  void *result =nullptr;
  try{
    //load会获取当前对象的值并返回
    //std::memory_order_relaxed决定了可以让cpu和编译器对他的执行顺序位置进行改变
    //只要保证最后的结果能返回就行
    //在这里结果就是将当前对象的值赋值给result
    result = centralFreeList_[index].load(std::memory_order_relaxed);

    //如果中心缓存为空，从也缓存中获取新的内存块
    if(!result){
      //size是申请的内存的8的整数倍
      size_t size = (index + 1) * ALIGNMENT;
      //这里传入的是8的整数倍，是想内存申请一块内存块
      //可能不同于最上层申请的内存，但是一定会大于最上层申请的内存
      result = fetchFromPageCache(size);

      if(!result){
        //如果还是申请失败，那么我们释放锁返回空指针
        //release在该操作之前的所有读写操作（包括非原子的），都不能被重排到该 clear 之后
        //上方的acquire和这里的release可以确保临界区内的操作不会“泄露”到临界区之外，保证了正确性
        locks_[index].clear(std::memory_order_release);
        return nullptr;
      }

      // 将从PageCache获取的内存块切分成小块，每一块大小为size
      char* start = static_cast<char*>(result);
      //totalBlocks统计了我们申请的内存能分为多少个size
      size_t totalBlocks = (SPAN_PAGES *pageCache::PAGE_SIZE) / size;
      //allocBlocks标志着我们能最少能分配多少块
      //如果申请的内存分成size大小的个数不满足申请的个数(batchNum),那么就只能分配totalBlocks个
      //如果totalBlocks大于了batchNum，表示我们申请的个数多于所需的个数，那么分配batchNum个就可以了
      size_t allocBlocks = std::min(batchNum,totalBlocks);
      RealIncrease=allocBlocks;
      //构建返回给ThreadCache的内存块链表
      if(allocBlocks > 1){
        //确保至少有两个块才构建链表
        //构建链表
        for(size_t i =1; i<allocBlocks;i++){
          //current指向了当前链表的首地址
          void *current = start + (i - 1) * size;
          //next指向了当前链表的下一个的地址
          void *next =start + i * size;
          //设置当前Block的下一个Block为next的地址
          *reinterpret_cast<void**>(current) = next;
        }
        //如果无法构建链表说明allocBlocks=1,就把当前内存块指向下一个内存块的指针设置为空
        *reinterpret_cast<void **>(start + (allocBlocks - 1) * size)= nullptr;
      }

      //构建保留在CentralCache的链表
      //操作方式同上(allocblocks>1)的操作
      if(totalBlocks > allocBlocks){
        void *remainStart = start + allocBlocks * size;
        for(size_t i = allocBlocks +1; i< totalBlocks ;i++){
          void *current = start + ( i - 1) * size;
          void *next = start + i * size;
          *reinterpret_cast<void**>(current) = next;
        }
        //将最后一个内存块指向下一个内存块的指针设置为空
        *reinterpret_cast<void**>(start + (totalBlocks - 1 ) * size) = nullptr;
        //将remainstart(多余内存块的首地址)设置为中心链表的头结点
        //release的作用同上面release
        centralFreeList_[index].store(remainStart, std::memory_order_release);
      }
    }
    else{   // 如果中心缓存有index对应大小的内存块
      //从现有链表中获取指定数量的块
      void *current = result;
      void *prev = nullptr;
      size_t count=0;
      //指针移动，将cuurent移动到需要分割的地址
      //
      while(current && count < batchNum){
        prev=current;
        current = *reinterpret_cast<void**>(current);
        count++;
      }
      //这步代码有两个意思，第一如果count<batchNum，说明存储的内存小于申请的内存，那么我们将实际增加的内存改为count
      //如果count = batchNum那么说明实际能分配的内存就是申请的内存，那么RealInrease还是增加为count
      RealIncrease=count;
      
      if(prev){ //如果当前centralFreeList_[index]存储的链表上的内存块大于batchNum时
        *reinterpret_cast<void **>(prev) = nullptr;
      }
      centralFreeList_[index].store(current , std::memory_order_release);
    }
  }catch(...){
    //如果上方的try出现错误，就跳转到这里，把锁释放了，然后抛出异常
    locks_[index].clear(std::memory_order_release);
    throw;
  }

  // 释放锁
  locks_[index].clear(std::memory_order_release);
  return result;
}

void centralCache::returnRange(void* start, size_t size, size_t index){
  if(!start || index >= FREE_LIST_SIZE) return;

  while(locks_[index].test_and_set(std::memory_order_acquire)){
    std::this_thread::yield();
  }

  try{
    void *end = start;
    size_t count = 1;
    while(*reinterpret_cast<void **>(end) != nullptr && count < size){
      end = *reinterpret_cast<void **>(end);
      count++;
    }

    //将归还的链表连接到中心缓存的链表头部
    void *current = centralFreeList_[index].load(std::memory_order_relaxed);
    *reinterpret_cast<void**>(end) = current;  // 将原链表头接到归还链表的尾部
    centralFreeList_[index].store(start, std::memory_order_release);  // 将归还的链表头设为新的链表头
  }catch(...){
    locks_[index].clear(std::memory_order_release);
    throw;
  }

  locks_[index].clear(std::memory_order_release);
}

void* centralCache::fetchFromPageCache(size_t size){   
  //计算实际需要的页数
  size_t numPages = (size + pageCache::PAGE_SIZE - 1) / pageCache::PAGE_SIZE;

  //根据大小决定分配策略
  if (size <= SPAN_PAGES * pageCache::PAGE_SIZE) {
    // 小于等于32KB的请求，使用固定8页
    return pageCache::getInstance().allocateSpan(SPAN_PAGES);
  } 
  else {
    // 大于32KB的请求，按实际需求分配
    return pageCache::getInstance().allocateSpan(numPages);
  }
}