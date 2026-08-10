#pragma once
#include "Eventloop.h"
#include"Socket.h"
#include"Channel.h"
#include"Acceptor.h"
#include"Connection.h"
#include"ThreadPool.h"
#include"../net/INetServer.h"
#include"../logger/log_fac.h"
#include"../timer/timer.h"
#include"../timer/TimeWheel.h"
#include"Buffer.h"
#include<map>
#include<memory>
#include<mutex>
#include<vector>


class TcpServer : public INetServer {
private:
  std::unique_ptr<EventLoop>mainloop_;      //主事件循环
  std::vector<std::unique_ptr<EventLoop>> subloops_;  //存放从事件循环的容器
  Acceptor acceptor_;  //一个TcpServer只有一个Acceptor对象(因为封装的是listenfd)
  int threadnum_;               //线程池大小,即从事件循环的个数
  ThreadPool threadpool_;       //线程池
  std::mutex mmutex_;           //保护conns_的互斥锁
  std::map<int,spConnection> conns_;//一个TcpServer可以有多个Connection对象(因为封装的是connectfd)
  ConnCallback newconnectioncb_;      //回调HttpServer::HandleNewConnection()
  ConnCallback closeconnectioncb_;    //回调HttpServer::HandleClose()
  ConnCallback errorconnectioncb_;    //回调HttpServer::HandleError()
  ConnCallback onmessagecb_;   //回调HttpServer::HandleMessage()
  ConnCallback sendcompletecb_;       //回调HttpServer::HandleSendComplete()
  std::function<void(EventLoop*)> timeoutcb_;            //回调HttpServer::HandleTimeOut()
  LogFac& log;

  //定时器
  //Timer ts_timer_;
  int ts_tcp_conn_timeout_s_ { 360 };

  //时间轮 
  TimeWheel time_wheel_;
public:
  TcpServer(const std::string &ip,const uint16_t port, int threadnum=3,int timeoutS=360,bool OptLinger=true);
  ~TcpServer();

  void start() override; //运行事件循环
  void Stop() override;  //停止事件循环
  void stop();           //兼容旧接口
  
  void newconnection(std::unique_ptr<Socket> clientsock); //处理新客户端的连接请求,在Acceptor类回调此函数
  void closeconnection(spIConnection conn); //关闭客户端的连接，在Connection类中回调此函数
  void errorconnection(spIConnection conn); //客户端的连接错误，在Connection类中回调此函数
  void message(spIConnection conn/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock*/); //处理客户端的请求报文，在Connection类回调此函数
  void sendcomplete(spIConnection conn);     //数据发送完成后，在Connection类中回调此函数
  void epolltimeout(EventLoop*loop);      //epoll_wait()超时，在EventLoop类中回调此函数

  void setnewconnectioncb(ConnCallback cb) override;
  void setcloseconnectioncb(ConnCallback cb) override;
  void seterrorconnectioncb(ConnCallback cb) override;
  void setonmessagecb(ConnCallback cb) override;
  void setsendcompletecb(ConnCallback cb) override;

  // 兼容旧接口命名
  void setnewconnection(ConnCallback cb);
  void setcloseconnection(ConnCallback cb);
  void seterrorconnection(ConnCallback cb);
  void setonmessage(ConnCallback cb);
  void setsendcomplete(ConnCallback cb);
  void setnewconnection(std::function<void(spConnection)> cb);
  void setcloseconnection(std::function<void(spConnection)> cb);
  void seterrorconnection(std::function<void(spConnection)> cb);
  void setonmessage(std::function<void(spConnection)> cb);
  void setsendcomplete(std::function<void(spConnection)> cb);
  
  //时间戳
  //void settimeout(std::function<void(EventLoop*)> );    
  //void removeconn(int fd);


  //定时器代码
  //void set_tcp_conn_timeout_ms(int ms);
  //void update_conn_timeout_time(spConnection conn);
  //void add_new_tcp_conn(spConnection conn);
  //void closeconntimer(spConnection conn);
  
  //时间轮
  void add_new_conn_timernode(spConnection Connection);
  void update_conn_timeout_time(spIConnection conn);
  void closeconntimer(spIConnection conn);

};
