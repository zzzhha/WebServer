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
#include<filesystem>
#include<vector>

namespace {
std::string ResolveStaticPath() {
  namespace fs = std::filesystem;
  const std::vector<fs::path> candidates = {
    fs::path("./html"),
    fs::path("../html"),
    fs::path("../../html")
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    fs::path index_file = candidate / "index.html";
    if (fs::exists(index_file, ec) && fs::is_regular_file(index_file, ec)) {
      return fs::weakly_canonical(candidate, ec).string();
    }
  }
  return "./html";
}
}

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

  httpserver=new HttpServer(argv[1],atoi(argv[2]),360,true,3306,"webuser","12589777","webserver",6,4,12,ResolveStaticPath());
  httpserver->start();
  
  return 0;
}
