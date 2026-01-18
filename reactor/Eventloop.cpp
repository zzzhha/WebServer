#include"Eventloop.h"
//时间戳
// int createtimerfd(int sec = 30){
//   int tfd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
//   struct itimerspec timeout;    //定时时间的数据结构
//   memset(&timeout,0,sizeof(itimerspec));
//   timeout.it_value.tv_sec= sec;
//   timeout.it_value.tv_nsec=0;
//   timeout.it_interval.tv_sec = 1;
//   timeout.it_interval.tv_nsec = 0;
//   timerfd_settime(tfd,0,&timeout,0);
//   return tfd;
// }

EventLoop::EventLoop(/*bool mainloop,int timetvl, int timeout*/)
:ep_(new Epoll),wakeupfd_(eventfd(0,EFD_NONBLOCK)),wakeupchannel_(new Channel(this,wakeupfd_)),/*
timerfd_(createtimerfd(timeout)),timerchannel_(new Channel(this,timerfd_)),mainloop_(mainloop),
timetvl_(timetvl),timeout_(timeout),*/stop_(false){

  wakeupchannel_->setreadcallback(std::bind(&EventLoop::handlewakeup,this));
  wakeupchannel_->enablereading();
  
  //时间戳
  // timerchannel_->setreadcallback(std::bind(&EventLoop::handletimer,this));
  // timerchannel_->enablereading();
}

EventLoop::~EventLoop(){
}

void EventLoop::run(){
  //printf("EventLoop::run() thread is %d\n",syscall(SYS_gettid));
  //获取事件循环所在线程，在这里获取线程是因为在TcpServer中我们先创建了主事件循环，在创建了io线程
  //因此在主事件循环的构造函数中获取的话获取的是main函数的进程，而并非是自己运行的线程中的线程号（主事件循环能正确获得但是其他不行）
  //在run中，不管是主还是从时间循环，run函数都是先加入了线程池中，再进行run函数，所以主从事件循环都能正确获得线程号
  threadid_ =syscall(SYS_gettid); 
  while(stop_==false){
    
    //loop中 取得由ep监听的fd中发生了事件的fd，并且封装为channel
    //设置了channel的revent(已发生事件)，并且拷贝了此fd对应的channel的信息
    //相当于把由channel封装后的fd，将其发生事件的fd提取出来，并根据其发生的事件设置其revent
    //再将这些fd以数组形式返回
    std::vector<Channel*> vcn=ep_->loop(10*1000);
    
    //如果channel为空，表示超时，回调TcpServer::sepolltimeout()
    if(vcn.empty()) {
      epolltimeoutcallback_(this);
    }
    else{
      for(auto &ch:vcn){
LOGDEBUG("有新的事件准备处理");
      ch->handleevent();
      }
    }
  }
}

void EventLoop::stop(){
  stop_=true;
  wakeup();
}

void EventLoop::updatechannel(Channel *ch){
  ep_->updatechannel(ch);
}
void EventLoop::removechannel(Channel *ch){
  ep_->removechannel(ch);
}
void EventLoop::setepolltimeoutcallback(std::function<void(EventLoop*)>fn){
  epolltimeoutcallback_=fn;
}
bool EventLoop::isinloopthread(){
  return threadid_==syscall(SYS_gettid);
}

void EventLoop::queueinloop(std::function<void()> fn){
  {
    std::lock_guard<std::mutex> lock(mutex_); //加锁
    taskqueue_.push(fn);    //任务入队
  }
  //唤醒事件
LOGDEBUG("有任务入队，唤醒事件");
  wakeup();
}

void EventLoop::wakeup(){
  uint64_t val=1;
  write(wakeupfd_,&val,sizeof(val));
}

void EventLoop::handlewakeup(){
  //printf("handlewakeup() thread is %d\n",syscall(SYS_gettid));
LOGDEBUG("处理因事件管道唤起的事件");
  uint64_t val;
  read(wakeupfd_,&val,sizeof(val)); //从eventfd读出数据，如果不读，则会一直触发eventfd的读事件
  std::function<void()> fn;
  std::lock_guard<std::mutex> lock(mutex_);
  while(taskqueue_.size()>0){
      fn=std::move(taskqueue_.front());
      taskqueue_.pop();
      fn();       //执行任务
  }
  
}

//时间戳

// void EventLoop::handletimer(){
//   uint64_t expirations;
//   read(timerfd_, &expirations, sizeof(expirations));
//   if (timerwheelcallback_) {
//         timerwheelcallback_();
//   }
// }
/*
//时间戳
void EventLoop::newconnection(spConnection conn){
  {
    std::lock_guard<std::mutex> lock(mmutex_);      
    conns_[conn->fd()]=conn;
  }
}

*/
//时间戳
// void EventLoop::settimercallback(std::function<void()> fn){
//   timerwheelcallback_=fn;
// }

