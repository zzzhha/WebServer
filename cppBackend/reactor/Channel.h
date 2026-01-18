#pragma once
#include<sys/epoll.h>
#include<functional>
#include"InetAddress.h"
#include"Socket.h"
#include"Eventloop.h"
#include<memory>

#include"../logger/log_fac.h"
class EventLoop;

class Channel{
private:
  int fd_ =-1;          //Channel拥有的fd是一对一的关系
  EventLoop*loop_;
  bool inepoll_ =false; //Channel是否已添加到epoll树上，如果未添加，调用epoll_ctl()的时候使用EPOLL_CTL_ADD,否则使用EPOLL_CTL_MOD
  uint32_t events_ = 0;  //fd_需要检测的事件，listenfd和clientfd需要检测EPOLLIN,clientfd可能还需要监视EPOLLOUT
  uint32_t revents_ = 0; //fd_已发生的事件
  std::function<void()> readcallback_;  //fd_读事件的回调函数,如果是acceptchannel,将回调Acceptor::newconnection,如果是connectionchannel,则调用connection::onmessage
  std::function<void()> closecallback_; //关闭fd_的回调函数,将回调connection::closecallback()
  std::function<void()> errorcallback_; //fd_发生了错误的回调函数,将回调connection::errorcallback()
  std::function<void()> writecallback_; //想客户端写入数据,回调Connection::writecallback()

public:
  Channel(EventLoop*loop,int fd);//构造函数
  ~Channel();                   //析构函数
  int fd();                     //返回fd_成员
  void useet();                 //采用边缘触发
  void enablereading();         //让epoll_wait()监视fd_的读事件,注册读事件
  void disablereading();        //取消读事件
  void enablewriting();         //注册写事件
  void disablewriting();        //取消写事件
  void disableall();            //取消全部事件
  void remove();                //从事件循环中删除channel
  void setinepoll();            //把inepoll_成员的值设置为true 
  void setrevents(uint32_t ev); //设置revents_成员的值参数ev
  bool inpoll();                //返回inepoll_成员
  uint32_t events();            //返回events_成员
  uint32_t revents();           //返回revents_成员
  void setreadcallback(std::function<void()> fn);   //设置fd_读事件的回调函数
  void setclosecallback(std::function<void()> fn);  //设置fd_关闭的回调函数
  void seterrorcallback(std::function<void()> fn);  //设置fd_发生错误的回调函数
  void setwritecallback(std::function<void()> fn);  //设置写事件的回调函数
  
  void handleevent();           //事件处理函数，epoll_wait()返回后调用执行此函数

  
//  void onmessage();                             //处理对端发送过来的消息

};