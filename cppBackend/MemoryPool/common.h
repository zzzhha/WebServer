#pragma once

#include<cstddef>
#include<atomic>
#include<array>

constexpr size_t ALIGNMENT = 8;
constexpr size_t MAXBYTE = 256 * 1024; //256KB - 小型内存上限
constexpr size_t LARGE_THRESHOLD = 4 * 1024 * 1024; //4MB - 大型内存阈值
constexpr size_t FREE_LIST_SIZE = MAXBYTE / ALIGNMENT;


class sizeclass{
public:
  static size_t roundUp(size_t bytes){////向上对齐函数(取得ALOGNM整ENT的倍数)
  //因为ALIGNMENT-1按位取反之后，比ALIGNMENT大于等于的二进制位都标志位1
  //再按位与& 就一定是8的倍数，传入0的话结果为0也是8的0倍
    return ((bytes +ALIGNMENT -1) & ~(ALIGNMENT - 1));
  }
  
  static size_t getIndex(size_t bytes){//获取大小对应的索引
    //确保bytes至少为ALIGNMENT
    bytes = std::max(bytes,ALIGNMENT); 
    //向上取整后-1 
    return ((bytes + ALIGNMENT - 1) / ALIGNMENT) -1;
  }
};

