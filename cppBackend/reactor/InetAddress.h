#pragma once

#include<arpa/inet.h>
#include<netinet/in.h>
#include<string>

class InetAddress{
private:
  sockaddr_in addr_;
public:
  InetAddress();
  InetAddress(const std::string &ip,uint16_t port);//如果是监听fd,则用这个构造函数
  InetAddress(const sockaddr_in addr);//如果是客户端连接上的fd，则需要用此函数
  ~InetAddress();
  const char* ip() const; //返回字符串表示的地址(点分十进制)
  uint16_t port() const;  //返回整数表示的端口
  const sockaddr* addr() const; //返回addr_成员的地址，转化为了sockaddr这个结构体
  void setaddr(sockaddr_in clientaddr); //将参数赋值给addr_
};