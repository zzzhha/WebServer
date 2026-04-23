#include"Channel.h"
Channel::Channel(EventLoop*loop,int fd):fd_(fd),loop_(loop){
}

Channel::~Channel(){
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
  disableall();
  loop_->removechannel(this);
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
}
void Channel::seterrorcallback(std::function<void()> fn){
  errorcallback_=fn;
}
void Channel::setwritecallback(std::function<void()> fn){
  writecallback_=fn;
}

void Channel::tie(const std::shared_ptr<void>& obj){
  tie_ = obj;
  tied_ = true;
}


void Channel::handleevent(){
  std::shared_ptr<void> guard;
  if(tied_){
    guard = tie_.lock();
    if(!guard){
      return;
    }
  }

  if (revents_ & (EPOLLERR | EPOLLHUP)) {
    if(errorcallback_) errorcallback_();
    return;
  }

  if (revents_ & (EPOLLIN | EPOLLPRI)) {
    LOGDEBUG("发生读事件");
    if(readcallback_) readcallback_();
  }

  if (revents_ & EPOLLOUT) {
    LOGDEBUG("发生写事件");
    if(writecallback_) writecallback_();
  }

  if (revents_ & EPOLLRDHUP) {
    if(closecallback_) closecallback_();
  }
}


