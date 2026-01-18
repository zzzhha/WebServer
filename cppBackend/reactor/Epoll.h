#pragma once
#include<stdio.h>
#include<stdlib.h>
#include<errno.h>
#include<strings.h>
#include<string.h>
#include<sys/epoll.h>
#include<vector>
#include<unistd.h>
#include"Channel.h"
#include"../logger/log_fac.h"
class Channel;

class Epoll{
private:
  static const int MaxEvents = 100;
  int epollfd_=-1;
  epoll_event events_[MaxEvents];

public:
  Epoll();  //创建fd
  ~Epoll(); //析构fd
  
  void updatechannel(Channel *ch);    //把Channel添加/更新到红黑树上，Channel中有fd和需要监听的事件
  void removechannel(Channel *ch);    //把Channel删除
  std::vector<Channel *>loop(int timeout = -1);//运行epoll_wait(),等待事件的发生，已发生的时间荣vector容器返回
};