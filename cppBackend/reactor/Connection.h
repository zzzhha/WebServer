#pragma once
#include<functional>
#include<atomic>
#include<cstdint>
#include<any>
#include<string>
#include<sys/syscall.h>
#include<sys/sendfile.h>
#include<unistd.h>
#include"Socket.h"
#include"InetAddress.h"
#include"Channel.h"
#include"Eventloop.h"
#include"Buffer.h"
#include"../net/IConnection.h"
#include<memory>
#include<utility>
//#include"Timestamp.h"

class Connection;
class EventLoop;
class Channel;
class TlsContext;
class TlsSession;
using spConnection = std::shared_ptr<Connection>;

class Connection: public IConnection {
private:
  EventLoop* loop_;   //一个connection对应一个从事件循环,在构造函数中传入,一个从事件循环会有多个Connection对象
  std::unique_ptr<Socket> clientsock_;   //与客户端通讯的Socket
  std::unique_ptr<Channel> clientchannel_; //connection对应的channel，在构造函数中创建
  std::function<void(spIConnection)> closecallback_;  //关闭fd_的回调函数,将回调TcpServer::closeconnection()
  std::function<void(spIConnection)> errorcallback_;  //关闭fd_的回调函数,将回调TcpServer::errorconnection()
  std::function<void(spIConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)> onmessagecallback_;  //处理报文的回调函数，将回调TcpServer::message()
  std::function<void(spIConnection)>sendcompletecallback_;   //发送完数据后的回调函数，将回调TcpServer::sendcomplete()
  std::function<void(spIConnection)>closetimercallback_;
  std::atomic_bool disconnect_;    //客户端连接是否断开，如果断开设置为true
  std::atomic_bool close_on_send_complete_;  //发送完成后是否关闭连接
 
  ///时间戳
  ///Timestamp lasttime_;             //时间戳，创建Connection对象时为当前时间，没收到一个报文，就更新时间戳为当前时间

  BufferBlock inputbuffer_;       //接收缓冲区
  BufferBlock outputbuffer_;      //发送缓冲区

  struct SendFileState{
    int file_fd{-1};
    off_t offset{0};
    size_t remaining{0};
    bool close_fd{true};
    bool active{false};
  };

  SendFileState sendfile_;

  std::shared_ptr<TlsContext> tls_ctx_;
  std::unique_ptr<TlsSession> tls_;
  bool tls_decided_{false};
  bool tls_plaintext_{false};
  std::string tls_out_pending_;

  //定时器
  int tc_fd;
  int tc_timer_id{ -1 };
  std::function<void(spIConnection)>updatetimercallback_;  //Connection发送报文后更新定时器，将回调TcpServer::update_conn_timeout_time()
  std::atomic<uint64_t> timer_generation_{0};

public:
  Connection(EventLoop*loop,std::unique_ptr<Socket>clientsock);
  ~Connection();

  int fd() const override;             //返回fd_成员
  std::string ip()const override;      //返回ip_成员
  uint16_t port() const override;      //返回port_成员
  EventLoop* getLoop() const; //返回loop_成员

  void onmessage();           //处理对端发送过来的消息
  void closecallback();       //tcp连接断开的回调函数,供channel回调
  void errorcallback();       //tcp连接错误的回调函数,同上
  void writecallback();       //处理写事件的回调函数，供channel回调
  //fd_连接断开的回调函数
  void setclosecallback(std::function<void(spIConnection)> fn);
  //fd_连接错误的回调函数
  void seterrorcallback(std::function<void(spIConnection)> fn);
  
  void setonmessagecallback(std::function<void(spIConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)> fn);
  void setsendcompletecallback(std::function<void(spIConnection)> fn);
  
  void connectEstablished();

  //不管在任何线程中,都是调用此函数发送数据
  void send() override;
  //发送数据，如果当前线程是IO线程，则直接调用此函数，如果是工作线程则把此函数传给IO线程
  void sendinloop(/*std::shared_ptr<std::string>data*/); 

  // 访问buffer的接口
  BufferBlock& getInputBuffer() override { return inputbuffer_; }
  BufferBlock& getOutputBuffer() override { return outputbuffer_; }
  
  // 关闭连接的接口
  void closeConnection() { closecallback(); }
  
  // 设置发送完成后是否关闭连接
  void setCloseOnSendComplete(bool close) override { close_on_send_complete_ = close; }
  bool getCloseOnSendComplete() const { return close_on_send_complete_; }

  void StartSendFile(int file_fd, off_t offset, size_t count, bool close_fd = true) override;
  void ClearSendFile() override;
  bool HasSendFile() const { return sendfile_.active; }

  void SetTlsContext(std::shared_ptr<TlsContext> ctx) override { tls_ctx_ = std::move(ctx); }
  void PostIoTask(std::function<void()> task) override;

  //时间戳
  ///bool timeout(time_t now,int val); //判断tcp连接是否超时
  
  //定时器
  void set_timer_id(int id) {tc_timer_id = id;}
  int get_timer_id() { return tc_timer_id;}
  void setupdatetimercallback(std::function<void(spIConnection)> fn);
  void setclosetimercallback(std::function<void(spIConnection)>fn);
  uint64_t BumpTimerGeneration() { return ++timer_generation_; }
  uint64_t GetTimerGeneration() const { return timer_generation_.load(); }
  bool IsDisconnected() const override { return disconnect_.load(); }

};
