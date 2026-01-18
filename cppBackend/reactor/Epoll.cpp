#include"Epoll.h"




Epoll::Epoll(){
  epollfd_ = epoll_create(1); //创建epoll句柄（红黑树）
  if(epollfd_ ==-1){
    printf("epoll_create error\n");
    exit(-1);
  }
}


Epoll::~Epoll(){
  close(epollfd_);
}

void Epoll::updatechannel(Channel *ch){
  epoll_event ev;//声明事件结构体
  ev.data.ptr=ch; //指定Channel
  ev.events = ch->events();//指定事件
  if(ch->inpoll()){//如果Channel已经在树上了（一个Channel只对应一个epoll，但是一个epoll能对应多个Channel）
  if(epoll_ctl(epollfd_,EPOLL_CTL_MOD,ch->fd(),&ev)==-1){
      LOGERROR("epoll_ctl failed\n");
      //perror("epoll_ctl failed\n");
      exit(-1);
    }
  }else{
    if(epoll_ctl(epollfd_,EPOLL_CTL_ADD,ch->fd(),&ev)==-1){
      LOGERROR("epoll_ctl failed\n");
      //perror("epoll_ctl failed\n");
      exit(-1);
    }
    ch->setinepoll(); //把channel的inepoll设置为true
  }
}
void Epoll::removechannel(Channel *ch){
  if(ch->inpoll()){//如果Channel已经在树上了（一个Channel只对应一个epoll，但是一个epoll能对应多个Channel）
    //printf("removechannel()\n");
    if(epoll_ctl(epollfd_,EPOLL_CTL_DEL,ch->fd(),0)==-1){
      LOGERROR("epoll_ctl failed\n");
      //perror("epoll_ctl failed\n");
      exit(-1);
    }
  }
}

std::vector<Channel*>Epoll::loop(int timeout){
  std::vector<Channel*> channels; //存放channels,channels内存储了events是所需的发生的时间
  bzero(events_,sizeof(events_));
  int number =epoll_wait(epollfd_,events_,MaxEvents,timeout);
  if(number<0){
    LOGERROR("epoll_wait error\n");
    exit(-1);
  }
  if(number ==0){
    
    //printf("epoll_wait() timeout\n");
    return channels;
  }
  for(int i=0;i<number;i++){
    Channel *ch =(Channel*)events_[i].data.ptr; //取出已发生事件的channel
    ch->setrevents(events_[i].events);          //设置channel的revents_成员
    channels.push_back(ch);
  }

  return channels;

}



