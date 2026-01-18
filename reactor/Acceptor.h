#pragma once
#include<functional>
#include"Socket.h"
#include"InetAddress.h"
#include"Channel.h"
#include"Eventloop.h"
#include<memory>

class Acceptor{
private:
  EventLoop* loop_;   //一个accept对应一个事件循环,在构造函数中传入
  Socket servsock_;   //服务端用于监听的socket，在构造函数中创建
  Channel acceptchannel_; //acceptor对应的channel，在构造函数中创建
  std::function<void(std::unique_ptr<Socket>)> newconnectioncb_;
public:
  Acceptor(EventLoop* loop,const std::string &ip,const uint16_t port,bool OptLinger);
  ~Acceptor();

  void newconnection();
  void setnewconnecioncb(std::function<void(std::unique_ptr<Socket>)>);
};