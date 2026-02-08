#pragma once
#include<functional>
#include<atomic>
#include<sys/syscall.h>
#include<sys/sendfile.h>
#include"Socket.h"
#include"InetAddress.h"
#include"Channel.h"
#include"Eventloop.h"
#include"Buffer.h"
#include<memory>
//#include"Timestamp.h"

class Connection;
class EventLoop;
class Channel;
using spConnection = std::shared_ptr<Connection>;

class Connection:public std::enable_shared_from_this<Connection>{
private:
  EventLoop* loop_;   //一个connection对应一个从事件循环,在构造函数中传入,一个从事件循环会有多个Connection对象
  std::unique_ptr<Socket> clientsock_;   //与客户端通讯的Socket
  std::unique_ptr<Channel> clientchannel_; //connection对应的channel，在构造函数中创建
  std::function<void(spConnection)> closecallback_;  //关闭fd_的回调函数,将回调TcpServer::closeconnection()
  std::function<void(spConnection)> errorcallback_;  //关闭fd_的回调函数,将回调TcpServer::errorconnection()
  std::function<void(spConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)> onmessagecallback_;  //处理报文的回调函数，将回调TcpServer::message()
  std::function<void(spConnection)>sendcompletecallback_;   //发送完数据后的回调函数，将回调TcpServer::sendcomplete()
  std::function<void(spConnection)>closetimercallback_;
  std::atomic_bool disconnect_;    //客户端连接是否断开，如果断开设置为true
  std::atomic_bool close_on_send_complete_;  //发送完成后是否关闭连接
 
  ///时间戳
  ///Timestamp lasttime_;             //时间戳，创建Connection对象时为当前时间，没收到一个报文，就更新时间戳为当前时间

  BufferBlock inputbuffer_;       //接收缓冲区
  BufferBlock outputbuffer_;      //发送缓冲区

  //定时器
  int tc_fd;
  int tc_timer_id{ -1 };
  std::function<void(spConnection)>updatetimercallback_;  //Connection发送报文后更新定时器，将回调TcpServer::update_conn_timeout_time()

public:
  Connection(EventLoop*loop,std::unique_ptr<Socket>clientsock);
  ~Connection();

  int fd() const;             //返回fd_成员
  std::string ip()const;      //返回ip_成员
  uint16_t port() const;      //返回port_成员

  void onmessage();           //处理对端发送过来的消息
  void closecallback();       //tcp连接断开的回调函数,供channel回调
  void errorcallback();       //tcp连接错误的回调函数,同上
  void writecallback();       //处理写事件的回调函数，供channel回调
  //fd_连接断开的回调函数
  void setclosecallback(std::function<void(spConnection)> fn);
  //fd_连接错误的回调函数
  void seterrorcallback(std::function<void(spConnection)> fn);
  
  void setonmessagecallback(std::function<void(spConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)> fn);
  void setsendcompletecallback(std::function<void(spConnection)> fn);
  
  //不管在任何线程中,都是调用此函数发送数据
  void send();
  //发送数据，如果当前线程是IO线程，则直接调用此函数，如果是工作线程则把此函数传给IO线程
  void sendinloop(/*std::shared_ptr<std::string>data*/); 

  // 访问buffer的接口
  BufferBlock& getInputBuffer() { return inputbuffer_; }
  BufferBlock& getOutputBuffer() { return outputbuffer_; }
  
  // 关闭连接的接口
  void closeConnection() { closecallback(); }
  
  // 设置发送完成后是否关闭连接
  void setCloseOnSendComplete(bool close) { close_on_send_complete_ = close; }
  bool getCloseOnSendComplete() const { return close_on_send_complete_; }

  //时间戳
  ///bool timeout(time_t now,int val); //判断tcp连接是否超时
  
  //定时器
  void set_timer_id(int id) {tc_timer_id = id;}
  int get_timer_id() { return tc_timer_id;}
  void setupdatetimercallback(std::function<void(spConnection)> fn);
  void setclosetimercallback(std::function<void(spConnection)>fn);

};