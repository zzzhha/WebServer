#pragma once
#include<string>
#include<iostream>
#include<cstring>
#include<vector>
//#include"../memory_pool1/ConcurrentPool.h"
#include"../memory_pool2/MemoryPool.h"
#include<sys/uio.h>


class BufferBlock{
  struct Block{
    void *data;
    size_t size;
    size_t capacity;

    Block(size_t cap):capacity(cap),size(0){
      data =MemoryPool::allocate(cap);
      //data = ConcurrentAlloc(cap);
      //data = new char[cap];
    }

    ~Block(){
      if(data){
        //delete []data;
        //ConcurrentFree(data);
        MemoryPool::deallocate(data,size);
      }
    }

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
  
    Block(Block&& other) noexcept 
      : data(other.data), size(other.size), capacity(other.capacity) {
      other.data = nullptr;
      other.size = 0;
      other.capacity = 0;
    }
    
    Block& operator=(Block&& other) noexcept {
      if (this != &other) {
        if(data){
         // delete []data;
          //ConcurrentFree(data);
          MemoryPool::deallocate(data,size);
        }
        data = other.data;
        size = other.size;
        capacity = other.capacity;
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
      }
      return *this;
    }
  };
public:
  std::vector<Block> blocks_;
  size_t total_size_;
  size_t read_pos_;
  size_t write_pos_;
  size_t check_pos_;
  BufferBlock();
  ~BufferBlock();
  
  BufferBlock(const BufferBlock&)=delete;
  BufferBlock& operator=(const BufferBlock&)=delete;
  
  BufferBlock(BufferBlock&& rhs)noexcept;
  BufferBlock& operator=(BufferBlock&& rhs)noexcept;
  void append(const char* data,size_t size);    //将数据追加到buf_中
  void append(std::string&str);
  size_t readableBytes() const;                                //返回整体大小
  void clear();                                 //清空buf_
  void erase(int len);
 
  //bool pickmessage(std::string &ss);            //从buf_中拆分出一个报文，保存在ss,如果没有报文，则返回false
  const char *peek()const;
  void peekFromBlock(char* dest,size_t n)const;//从blocks_的start_pos位置获取n个字节到dest中
  void consumeBytes(size_t size);
  void readBytes(char* dest, size_t n);
  size_t getIOVecs(struct iovec* iovs, size_t max_count, size_t start_pos = 0) const;
  std::string bufferToString();
};