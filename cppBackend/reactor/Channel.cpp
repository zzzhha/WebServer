#include"Channel.h"
Channel::Channel(EventLoop*loop,int fd):fd_(fd),loop_(loop){
}

Channel::~Channel(){
  //在析构函数中，不要销毁loop_,也不能关闭fd_;
  //这俩不属于Channel类，只是构造函数挪过来使用它们而已；
}

int Channel::fd(){
  return fd_;
}
                     
void Channel::useet(){
  events_ |= EPOLLET;
}                
void Channel::enablereading(){
  events_ |= EPOLLIN;
  loop_->updatechannel(this);
}
void Channel::disablereading(){
  events_ &= ~EPOLLIN;
  loop_->updatechannel(this);
}
void Channel::enablewriting(){
LOGDEBUG("注册写事件");
  events_ |= EPOLLOUT;
  loop_->updatechannel(this);
}
void Channel::disablewriting(){
  events_ &= ~EPOLLOUT;
  loop_->updatechannel(this);
}
void Channel::disableall(){
  events_ =0;
  loop_->updatechannel(this);
}
void Channel::remove(){
  disableall();   //先取消全部事件
  loop_->removechannel(this); //从epoll树上删除fd
}
void Channel::Channel::setinepoll(){
  inepoll_=true;
}
void Channel::setrevents(uint32_t ev){
  revents_ = ev;
} 
bool Channel::inpoll(){
  return inepoll_;
}              
uint32_t Channel::events(){
  return events_;
}
uint32_t Channel::revents(){
  return revents_;
}
void Channel::setreadcallback(std::function<void()> fn){
  readcallback_=fn;
}
void Channel::setclosecallback(std::function<void()> fn){
  closecallback_=fn;
}  //设置fd_关闭的回调函数
void Channel::seterrorcallback(std::function<void()> fn){
  errorcallback_=fn;
}
void Channel::setwritecallback(std::function<void()> fn){
  writecallback_=fn;
}


void Channel::handleevent(){
  if(revents_& EPOLLRDHUP){ //对方已关闭
    
    closecallback_();
  }
  else if(revents_ &(EPOLLIN/*普通数据*/ | EPOLLPRI/*带外数据（一般不使用）*/)){//接收缓冲区有数据可读
LOGDEBUG("发生读事件");
    readcallback_();
  }
  else if(revents_ & EPOLLOUT) {
LOGDEBUG("发生写事件");
    writecallback_();
  } //有数据可写
  else{ //其他事件，视为错误
    errorcallback_();
  } 
}



