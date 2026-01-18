#pragma once
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/tcp.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include"InetAddress.h"

int createnonblocking();

class Socket{
private:
  const int fd_;
  std::string ip_;  //如果是listenfd,则存放服务器监听的ip,乳沟是客户端连接的fd,
  uint16_t port_;
public:
  Socket(int fd); //
  ~Socket();  //关闭fd

  int fd() const; //返回fd_成员
  std::string ip()const;      //返回ip_成员
  uint16_t port() const;      //返回port_成员
  void setipport(const std::string&ip,uint16_t port); //设置ip和port
  void setreuseaddr(bool on); //设置SO_REUSERADDR on true打开
  void setreuseport(bool on);//设置SO_REUSERPORT on true打开
  void settcpnodelay(bool on);//设置TCP_NODELAY on true打开
  void settlinger(bool on);
  void setkeepalive(bool on);//设置SO_KEEPALIVE on true打开
  void bind(const InetAddress& servaddr); //服务端socket调用此函数
  void listen(int n=128);   //服务端socket调用此函数
  int accept(InetAddress& clientaddr);//服务端socket调用此函数
};