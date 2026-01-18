#include"Socket.h"


int createnonblocking(){
  int listenfd = socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,IPPROTO_TCP);
  if(listenfd<0){
    printf("%s:%s:%d listen socket create error:%d\n",__FILE__,__FUNCTION__,__LINE__,errno);
    exit(-1);
  }
  return listenfd;
}

Socket::Socket(int fd):fd_(fd){}

int Socket::fd() const{
  return fd_;
}
std::string Socket::ip()const{
  return ip_;
} 
uint16_t Socket::port() const{
  return port_;
}
void Socket::setreuseaddr(bool on){
  int opt=on ? 1 :0;
  setsockopt(fd_,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
}

void Socket::setreuseport(bool on){
  int opt=on ? 1 :0;
  setsockopt(fd_,SOL_SOCKET,SO_REUSEPORT,&opt,sizeof(opt));
}

void Socket::settlinger(bool on){
  int opt =on?1:0;
  struct linger optLinger = { 0 };
    if(opt) {
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }
  setsockopt(fd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
}
void Socket::setkeepalive(bool on){
  int opt=on ? 1 :0;
  setsockopt(fd_,SOL_SOCKET,SO_KEEPALIVE,&opt,sizeof(opt));
}

void Socket::settcpnodelay(bool on){
  int opt=on ? 1 :0;
  setsockopt(fd_,SOL_SOCKET,TCP_NODELAY,&opt,sizeof(opt));
}

void Socket::setipport(const std::string&ip,uint16_t port){
  ip_=ip;
  port_=port;
}

void Socket::bind(const InetAddress& servaddr) {
  if(::bind(fd_,servaddr.addr(),sizeof(sockaddr))<0){
    perror("bind error");
    close(fd_);
    exit(-1);
  }
  setipport(servaddr.ip(),servaddr.port());
}

void Socket::listen(int n){
  if(::listen(fd_,n)!=0){
    perror("listen error");
    close(fd_);
    exit(-1);
  }
}

int Socket::accept(InetAddress& clientaddr){
  sockaddr_in peeraddr;
  socklen_t len=sizeof(peeraddr);

  int connfd = accept4(fd_,(struct sockaddr*)&peeraddr,&len,SOCK_NONBLOCK);
  clientaddr.setaddr(peeraddr);

  return connfd;
}

Socket::~Socket(){
  close(fd_);
}