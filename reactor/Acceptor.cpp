#include"Acceptor.h"
#include"Connection.h" 
#include"../logger/log_fac.h"
Acceptor::Acceptor(EventLoop* loop,const std::string &ip,uint16_t port,bool OptLinger)
:loop_(loop),servsock_(createnonblocking()),acceptchannel_(loop_,servsock_.fd()){
  
  InetAddress servaddr(ip,port);
  servsock_.setkeepalive(true);
  servsock_.setreuseaddr(true);
  //servsock_.settlinger(OptLinger);
  servsock_.setreuseport(true);
  servsock_.settcpnodelay(true);
  servsock_.bind(servaddr);
  servsock_.listen();

  //通过channel，将listenfd绑定channel绑定ep
  //设置ep监视fd的读事件
  acceptchannel_.setreadcallback(std::bind(&Acceptor::newconnection,this));
  acceptchannel_.enablereading();
}

Acceptor::~Acceptor(){

}

void Acceptor::newconnection(){
  InetAddress clientaddr;

  std::unique_ptr<Socket> clientsock(new Socket(servsock_.accept(clientaddr)));
  clientsock->setipport(clientaddr.ip(),clientaddr.port());
LOGDEBUG("Acceptor尝试连接新客户端");
  newconnectioncb_(std::move(clientsock));    //回调TcpServer::newconnection()
}  

void Acceptor::setnewconnecioncb(std::function<void(std::unique_ptr<Socket>)> fn){
  newconnectioncb_=fn;
}