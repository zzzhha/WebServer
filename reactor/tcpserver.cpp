#include"tcpserver.h"
#include"Connection.h"

TcpServer::TcpServer(const std::string &ip,const uint16_t port,int threadnum,int timeoutS,bool OptLinger)
:threadnum_(threadnum),mainloop_(new EventLoop(/*true,30,timeoutMs/1000*/)),
acceptor_(mainloop_.get(),ip,port,OptLinger),threadpool_(threadnum_,"IO"),
ts_tcp_conn_timeout_s_(timeoutS),time_wheel_(1,60),log(LogFac::Instance()){
  //对log进行初始化
  log.Init(true);
  mainloop_->setepolltimeoutcallback(std::bind(&TcpServer::epolltimeout,this,std::placeholders::_1));

  acceptor_.setnewconnecioncb(std::bind(&TcpServer::newconnection,this,std::placeholders::_1));

  //创建从事件循环
LOGDEBUG("tcp:创建从事件循环"+std::to_string(threadnum_)+"个");
  for(int i=0;i<threadnum_;i++){
    subloops_.emplace_back(new EventLoop(/*false,30,timeoutMs/1000*/));   //创建从事件循环，存入subloops_容器中
    subloops_[i]->setepolltimeoutcallback(std::bind(&TcpServer::epolltimeout,this,std::placeholders::_1));
    //时间戳
    //subloops_[i]->settimercallback([this]{time_wheel_.tick();});   //设置清闲空闲tcp连接的回调函数
    
    threadpool_.addtask(std::bind(&EventLoop::run,subloops_[i].get()));
  }
  //定时器
  //ts_timer_.run();

}

TcpServer:: ~TcpServer(){
}

void TcpServer::start(){
  time_wheel_.start();
  mainloop_->run();
}
void TcpServer::stop(){
  //停止主事件循环
  mainloop_->stop();
  //停止从事件循环
  for(int i=0;i<threadnum_;i++){
    subloops_[i]->stop();
  }
  //停止io线程池
  threadpool_.stop();
}
void TcpServer::newconnection(std::unique_ptr<Socket>clientsock){
  spConnection conn(new Connection(subloops_[clientsock->fd()%threadnum_].get(),std::move(clientsock)));  
  conn->setclosecallback(std::bind(&TcpServer::closeconnection,this,std::placeholders::_1));
  conn->seterrorcallback(std::bind(&TcpServer::errorconnection,this,std::placeholders::_1));
  conn->setonmessagecallback(std::bind(&TcpServer::message,this,std::placeholders::_1/*暂且先注释了等后面需要用到工作线程在开出来,std::placeholders::_2*/));
  conn->setsendcompletecallback(std::bind(&TcpServer::sendcomplete,this,std::placeholders::_1));
  
  //定时器
  //conn->setupdatetimercallback(std::bind(&TcpServer::update_conn_timeout_time,this,std::placeholders::_1));
  ////conn->setclosetimercallback(std::bind(&TcpServer::closeconntimer,this,std::placeholders::_1));
  //add_new_tcp_conn(conn);//增加一个定时器，设定时间，超过时间后将关闭连接
  //时间轮
  conn->setupdatetimercallback(std::bind(&TcpServer::update_conn_timeout_time,this,std::placeholders::_1));
  add_new_conn_timernode(conn);
  {
    std::lock_guard<std::mutex> lock(mmutex_);
LOGDEBUG("tcp:存放入map容器");
    conns_[conn->fd()]=conn; //把conn存放到map容器中
  }
  //时间戳
  //subloops_[conn->fd()%threadnum_]->newconnection(conn);      //把conn存放到EventLoop的map容器中
LOGDEBUG("tcp初始化新客户端数据完毕,准备回调http服务器的HandleNewConnection");
  if(newconnectioncb_)newconnectioncb_(conn);
}

void TcpServer::closeconnection(spConnection conn){
  if(closeconnectioncb_)closeconnectioncb_(conn);
  //ts_timer_.cancel(conn->get_timer_id());
  time_wheel_.remove_connection(conn);
  //printf("client(eventfd=%d) disconnected.\n",conn->fd());
  {
    std::lock_guard<std::mutex> lock(mmutex_);
    conns_.erase(conn->fd());
  }

}

void TcpServer::errorconnection(spConnection conn){
  if(errorconnectioncb_) errorconnectioncb_(conn);
  //ts_timer_.cancel(conn->get_timer_id());
  time_wheel_.remove_connection(conn);
  {
    std::lock_guard<std::mutex> lock(mmutex_);
    conns_.erase(conn->fd());
  }

}

void TcpServer::message(spConnection conn/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock*/)
{
  if(onmessagecb_)onmessagecb_(conn/*暂且先注释了等后面需要用到工作线程在开出来,buf*/);
}

void TcpServer::sendcomplete(spConnection conn){
  //printf("send complete.\n");

  if(sendcompletecb_)sendcompletecb_(conn);
}
void TcpServer::epolltimeout(EventLoop*loop){
  if(timeoutcb_) timeoutcb_(loop);
}

void TcpServer::setnewconnection(std::function<void(spConnection)>fn){
  newconnectioncb_=fn;
}     
void TcpServer::setcloseconnection(std::function<void(spConnection)>fn){
  closeconnectioncb_=fn;
}         
void TcpServer::seterrorconnection(std::function<void(spConnection)>fn){
  errorconnectioncb_=fn;
}        
void TcpServer::setonmessage(std::function<void(spConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)>fn){
  onmessagecb_=fn;
}      
void TcpServer::setsendcomplete(std::function<void(spConnection)>fn){
  sendcompletecb_=fn;
}    

//时间戳
// void TcpServer::settimeout(std::function<void(EventLoop*)> fn){
//   timeoutcb_= fn;
// }     

//删除conns_中的Connection对象，在EventLoop::handletimer()中回调此函数
// void TcpServer::removeconn(int fd){
//   {
//     std::lock_guard<std::mutex> lock(mmutex_);
//     conns_.erase(fd); //从map中删除conn
//   }
// }


//定时器

// void TcpServer::add_new_tcp_conn(spConnection conn){
//   std::weak_ptr<Connection> weak_conn = conn;
  
//   auto timer_id = ts_timer_.run_after(ts_tcp_conn_timeout_ms_,false,
//     [weak_conn]()->void
//     {
//       if (auto shared_conn = weak_conn.lock()){
//         LOGINFO("tcp conn timeout.");
//         shared_conn->closecallback();
//       }else{
//         LOGERROR("Failed to lock weak_ptr in timer callback - connection already destroyed.");
//       }
      
//     }   
//   );
//   conn->set_timer_id(timer_id);
// }
// void TcpServer::update_conn_timeout_time(spConnection conn){
//   ts_timer_.cancel(conn->get_timer_id());
//   add_new_tcp_conn(conn);
// }

// void TcpServer::set_tcp_conn_timeout_ms(int ms){
//   ts_tcp_conn_timeout_ms_ =ms;
// }

// void TcpServer::closeconntimer(spConnection conn){
//   ts_timer_.cancel(conn->get_timer_id());
// }

void TcpServer::add_new_conn_timernode(spConnection conn) {
    // 添加到时间轮，设置超时时间
    time_wheel_.add_connection(conn, ts_tcp_conn_timeout_s_);
    
    // ... 其余代码
}
void TcpServer::update_conn_timeout_time(spConnection conn) {
    // 更新时间轮中的定时器
    time_wheel_.update_connection(conn);
}

