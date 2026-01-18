/*
#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<arpa/inet.h>
#include<sys/fcntl.h>
#include<sys/epoll.h>
#include<netinet/tcp.h>   //TCP_NODELAY 需要包含此头文件
#include"InetAddress.h"
#include"Socket.h"
#include"Epoll.h"
#include"Channel.h"
#include"Eventloop.h"
*/
#include"HttpServer.h"
#include<signal.h>

HttpServer *httpserver;
void Stop(int sig){
  //调用EchoServer::stop函数停止服务
  httpserver->Stop();
  delete httpserver;
  exit(0);
}
int main(int argc ,const char*argv[]){
  if(argc<3){
    printf("usage: ip port\n");
    return -1;
  }

  signal(SIGTERM,Stop);
  signal(SIGINT,Stop);

  httpserver=new HttpServer(argv[1],atoi(argv[2]),360);
  httpserver->start();
  
  return 0;
}