#pragma once
#include"Epoll.h"
#include<functional>
#include<unistd.h>
#include<sys/syscall.h>
#include<memory>
#include<sys/syscall.h>
#include<queue>
#include<mutex>
#include<sys/eventfd.h>
#include<sys/timerfd.h>
#include<map>
#include"Connection.h"
#include<atomic>
#include"../logger/log_fac.h"
class Channel;
class Epoll;
class Connection;
using spConnection = std::shared_ptr<Connection>;

class EventLoop{
private:
  std::unique_ptr<Epoll> ep_;    //每一个事件循环有一个epoll
  std::function<void(EventLoop*)>epolltimeoutcallback_;   //epoll_wait()超时的回调函数
  pid_t threadid_;              //事件循环所在线程id,事件循环有一个线程id，但是不是所有线程都有一个事件循环
                                //所有事件循环都会分配到io线程中,而不会分配到工作线程中,所以获得都是io线程 
  std::queue<std::function<void()>> taskqueue_; //事件循环被eventfd唤醒后执行的任务队列
  std::mutex mutex_;            //任务队列同步的互斥锁
  int wakeupfd_;                //用于唤醒事件循环线程的eventfd
  std::unique_ptr<Channel> wakeupchannel_;  //eventfd的channel

  //时间戳
  //int timerfd_;                 //定时器的fd
  //std::unique_ptr<Channel> timerchannel_;   //定时器fd的channel
  // bool mainloop_;               //true表示主事件循环,false表示从事件循环
  // std::mutex mmutex_;           //保护conns_的互斥锁
  // std::map<int,spConnection> conns_;    //存放运行在该事件循环上全部的Connection对象
  //std::function<void()> timerwheelcallback_;  //删除TcpServer中超时的Connection对象，将被设置为TimeWheel::tick()
  // int timetvl_;                 //闹钟时间间隔,单位 秒
  // int timeout_;                 //Connection对象超时时间,单位 秒
  
  
  std::atomic_bool stop_;

public:
  EventLoop(/*bool mainloop,int timetvl=30,int timeout=60*/);    //在构造函数创建Epoll对象ep_
  ~EventLoop();   //销毁ep_

  void run();     //运行事件循环
  void stop();    //停止事件循环
  void updatechannel(Channel *ch);
  void removechannel(Channel *ch);
  void setepolltimeoutcallback(std::function<void(EventLoop*)>fn);

  bool isinloopthread();  //判断当前线程是否为事件循环线程

  void queueinloop(std::function<void()> fn);   //把任务添加到队列中
  void wakeup();      //唤醒线程
  void handlewakeup();    //事件循环线程被eventfd唤醒后执行的函数


  //时间戳
  //void handletimer();   //闹钟响时执行的函数
  // void newconnection(spConnection conn);  //把Connection对象保存到conns_中
  
  //定时器
  void settimercallback(std::function<void()> fn);//设置定时器的回调函数
};