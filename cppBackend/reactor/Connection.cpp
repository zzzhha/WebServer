#include"Connection.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <algorithm>
#include <cctype>

#include "TlsContext.h"
#include "TlsSession.h"


Connection::Connection(EventLoop* loop,std::unique_ptr<Socket>clientsock)
:loop_(loop),clientsock_(std::move(clientsock)),disconnect_(false),close_on_send_complete_(false),clientchannel_(new Channel(loop_,clientsock_->fd())){
  //clientchannel_=new Channel(loop_,clientsock_->fd());   
  clientchannel_->setreadcallback (std::bind(&Connection::onmessage,this));
  clientchannel_->setclosecallback(std::bind(&Connection::closecallback,this));
  clientchannel_->seterrorcallback(std::bind(&Connection::errorcallback,this));
  clientchannel_->setwritecallback(std::bind(&Connection::writecallback,this));
}
Connection::~Connection(){
LOGDEBUG("Connection析构函数调用");
  ClearSendFile();
}

int Connection::fd() const{
  return clientsock_->fd();
}
std::string Connection::ip()const{
  return clientsock_->ip();
} 
uint16_t Connection::port() const{
  return clientsock_->port();
}
EventLoop* Connection::getLoop() const{
  return loop_;
}

void Connection::closecallback(){
LOGINFO("正常关闭Connection");
  disconnect_=true;
  clientchannel_->remove();
  // if(tc_fd!= -1){
  //   closetimercallback_(shared_from_this());
  // }
  closecallback_(shared_from_this());
}   
void Connection::errorcallback(){
LOGDEBUG("因错误关闭Connection");
  disconnect_=true;
  clientchannel_->remove();
  // if(tc_fd!= -1){
  //   closetimercallback_(shared_from_this());
  // }
  errorcallback_(shared_from_this());
}

//不管在任何线程中,都是调用此函数发送数据
void Connection::send(){
LOGDEBUG("调用Connection send函数");

  if(loop_->isinloopthread()){   //判断当前线程是否为事件循环线程（io线程）

LOGDEBUG("本线程是从事件线程，将处理发送事件");
    sendinloop();
  }else{
    //如果当前线程不是io线程，调用EventLoop::queueinloop(),把sendinloop()交给事件循环去执行
    //printf("send()不在事件循环的线程中\n");
LOGDEBUG("将发送数据传输给从事件循环去做");
    loop_->queueinloop(std::bind(&Connection::sendinloop,this));
  }

}

//发送数据，如果当前线程是IO线程，则直接调用此函数，如果是工作线程则把此函数传给IO线程
void Connection::sendinloop(){
LOGDEBUG("调用Connection sendinloop函数");

LOGDEBUG("唤起写事件");
  clientchannel_->enablewriting();
}

void Connection::connectEstablished(){
  clientchannel_->useet();
  clientchannel_->enablereading();
}
void Connection::writecallback(){
  
  if(disconnect_){
    return;
  }

  if (tls_ && !tls_->HandshakeDone()) {
    TlsIoResult hr = tls_->DriveHandshake();
    if (hr == TlsIoResult::WANT_WRITE) {
      return;
    }
    if (hr == TlsIoResult::WANT_READ) {
      clientchannel_->disablewriting();
      return;
    }
    if (hr != TlsIoResult::OK) {
      errorcallback();
      return;
    }
  }

  if (tls_ && tls_->HandshakeDone() && !tls_->KtlsTx()) {
    while (true) {
      if (!tls_out_pending_.empty()) {
        size_t nwritten = 0;
        TlsIoResult wr = tls_->WritePlain(tls_out_pending_.data(), tls_out_pending_.size(), nwritten);
        if (wr == TlsIoResult::OK) {
          if (nwritten > 0) {
            tls_out_pending_.erase(0, nwritten);
            continue;
          }
        }
        if (wr == TlsIoResult::WANT_WRITE) {
          return;
        }
        if (wr == TlsIoResult::WANT_READ) {
          return;
        }
        if (wr == TlsIoResult::CLOSED) {
          closecallback();
          return;
        }
        errorcallback();
        return;
      }

      if (outputbuffer_.readableBytes() > 0) {
        size_t take = std::min<size_t>(16384, outputbuffer_.readableBytes());
        tls_out_pending_.assign(take, '\0');
        outputbuffer_.peekFromBlock(&tls_out_pending_[0], take);
        outputbuffer_.consumeBytes(take);
        continue;
      }

      if (sendfile_.active) {
        if (sendfile_.remaining == 0) {
          ClearSendFile();
          continue;
        }
        size_t to_read = std::min<size_t>(16384, sendfile_.remaining);
        tls_out_pending_.assign(to_read, '\0');
        ssize_t n = ::pread(sendfile_.file_fd, &tls_out_pending_[0], to_read, sendfile_.offset);
        if (n > 0) {
          tls_out_pending_.resize(static_cast<size_t>(n));
          sendfile_.offset += static_cast<off_t>(n);
          sendfile_.remaining -= static_cast<size_t>(n);
          continue;
        }
        if (n == 0) {
          sendfile_.remaining = 0;
          ClearSendFile();
          continue;
        }
        if (errno == EINTR) {
          tls_out_pending_.clear();
          continue;
        }
        LOGERROR("pread failed, fd: " + std::to_string(fd()) + " error: " + strerror(errno));
        errorcallback();
        return;
      }

      clientchannel_->disablewriting();
      if (sendcompletecallback_ && !disconnect_) {
        sendcompletecallback_(shared_from_this());
      }
      if (close_on_send_complete_ && !disconnect_) {
        closecallback();
      }
      return;
    }
  }
  
  //新版本
LOGDEBUG("准备发送数据");
  const size_t max_ioves =16;
  struct iovec iovs[max_ioves];

  while(true){
    size_t iov_count = 0;
    iov_count = outputbuffer_.getIOVecs(iovs,max_ioves,outputbuffer_.read_pos_);
    if(iov_count > 0){
      ssize_t nwritten = ::writev(fd(),iovs,iov_count);
      if(nwritten > 0){
        outputbuffer_.consumeBytes(nwritten);
        continue;
      }else if(nwritten == -1){
        if(errno ==EAGAIN || errno == EWOULDBLOCK){
          return;
        }else{
          LOGERROR("writev failed, fd: "+std::to_string(fd())+" error: "+strerror(errno));
          errorcallback();
          return;
        }
      }
    }

    if(sendfile_.active){
      if(sendfile_.remaining == 0){
        ClearSendFile();
      }else{
        off_t off = sendfile_.offset;
        ssize_t n = ::sendfile(fd(), sendfile_.file_fd, &off, sendfile_.remaining);
        if(n > 0){
          sendfile_.offset = off;
          sendfile_.remaining -= static_cast<size_t>(n);
          continue;
        }else if(n == 0){
          sendfile_.remaining = 0;
          ClearSendFile();
        }else{
          if(errno ==EAGAIN || errno == EWOULDBLOCK){
            return;
          }else{
            LOGERROR("sendfile failed, fd: "+std::to_string(fd())+" error: "+strerror(errno));
            errorcallback();
            return;
          }
        }
      }
    }

    if(outputbuffer_.readableBytes() == 0 && !sendfile_.active){
      clientchannel_->disablewriting();
LOGDEBUG("发送数据完毕");
      if(sendcompletecallback_ && !disconnect_){
        sendcompletecallback_(shared_from_this());
      }
      // 如果设置了发送完成后关闭连接，则关闭连接
      if(close_on_send_complete_ && !disconnect_){
        closecallback();
      }
      return;
    }
  }
}

void Connection::onmessage(){
  if(disconnect_){
    return;
  }

  if (tls_ctx_ && !tls_decided_) {
    unsigned char probe[8];
    ssize_t n = ::recv(fd(), probe, sizeof(probe), MSG_PEEK);
    if (n == 0) {
      closecallback();
      return;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      errorcallback();
      return;
    }

    auto looks_like_tls = [](const unsigned char* p, size_t len) -> bool {
      if (len < 3) return false;
      unsigned char ct = p[0];
      if (ct != 0x14 && ct != 0x15 && ct != 0x16 && ct != 0x17) return false;
      if (p[1] != 0x03) return false;
      return true;
    };
    auto looks_like_http = [](const unsigned char* p, size_t len) -> bool {
      if (len < 3) return false;
      std::string s(reinterpret_cast<const char*>(p), std::min<size_t>(len, 8));
      for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return s.rfind("GET ", 0) == 0 || s.rfind("POST", 0) == 0 || s.rfind("PUT ", 0) == 0 ||
             s.rfind("HEAD", 0) == 0 || s.rfind("HTTP", 0) == 0 || s.rfind("OPTI", 0) == 0 ||
             s.rfind("DELE", 0) == 0 || s.rfind("PATC", 0) == 0;
    };

    if (looks_like_tls(probe, static_cast<size_t>(n))) {
      tls_ = std::make_unique<TlsSession>(tls_ctx_, fd());
      tls_decided_ = true;
      tls_plaintext_ = false;
    } else if (looks_like_http(probe, static_cast<size_t>(n))) {
      tls_decided_ = true;
      tls_plaintext_ = true;
      if (tls_ctx_ && tls_ctx_->Strict()) {
        closecallback();
        return;
      }
    } else {
      return;
    }
  }

  if (tls_) {
    while (true) {
      if (!tls_->HandshakeDone()) {
        TlsIoResult hr = tls_->DriveHandshake();
        if (hr == TlsIoResult::OK) {
          continue;
        }
        if (hr == TlsIoResult::WANT_WRITE) {
          clientchannel_->enablewriting();
          return;
        }
        if (hr == TlsIoResult::WANT_READ) {
          return;
        }
        errorcallback();
        return;
      }

      char buf[16384];
      size_t nread = 0;
      TlsIoResult rr = tls_->ReadPlain(buf, sizeof(buf), nread);
      if (rr == TlsIoResult::OK) {
        if (nread > 0) {
          inputbuffer_.append(buf, nread);
          continue;
        }
      }
      if (rr == TlsIoResult::WANT_READ) {
        break;
      }
      if (rr == TlsIoResult::WANT_WRITE) {
        clientchannel_->enablewriting();
        break;
      }
      if (rr == TlsIoResult::CLOSED) {
        closecallback();
        return;
      }
      errorcallback();
      return;
    }

    if (disconnect_) return;
    if (updatetimercallback_) {
      updatetimercallback_(shared_from_this());
    }
    if (onmessagecallback_) {
      onmessagecallback_(shared_from_this());
    }
    return;
  }

  char buffer[1024];
  while (true){
    bzero(&buffer, sizeof(buffer));
    ssize_t nread = read(fd(), buffer, sizeof(buffer));
    if(nread>0){
     inputbuffer_.append(buffer,nread);
    }else if(nread==-1 && errno == EINTR){
      continue;
    }else if(nread== -1 && ((errno==EAGAIN)|| (errno == EWOULDBLOCK))){//全部数据读完
      if(disconnect_){
        return;
      }
      //定时器
      if(updatetimercallback_) {
        updatetimercallback_(shared_from_this());
      }
      //时间戳
      //lasttime_=Timestamp::now();
      
      // 数据读取完毕，交给上层处理
      if(onmessagecallback_) {
        onmessagecallback_(shared_from_this());
      }
      break;
    }else if(nread==0){
LOGDEBUG("对方断开调用关闭");
      closecallback();  //回调TcpServer::closecallback()
      break;
    }else{
      LOGERROR("read failed, fd: "+std::to_string(fd())+" error: "+strerror(errno));
      errorcallback();
      break;
    }
  } 
}  
void Connection::setclosecallback(std::function<void(spConnection)> fn){
  closecallback_=fn;
}
  //fd_连接错误的回调函数
void Connection::seterrorcallback(std::function<void(spConnection)> fn){
  errorcallback_=fn;
}

void Connection::setonmessagecallback(std::function<void(spConnection/*暂且先注释了等后面需要用到工作线程在开出来,BufferBlock&*/)> fn){
  onmessagecallback_=fn;
}
void Connection::setsendcompletecallback(std::function<void(spConnection)> fn){
  sendcompletecallback_=fn;
}

//时间戳
// bool Connection::timeout(time_t now,int val){
//   return now-lasttime_.toint()>val;
// }
//定时器

void Connection::setupdatetimercallback(std::function<void(spConnection)> fn){
  updatetimercallback_=fn;
}

void Connection::setclosetimercallback(std::function<void(spConnection)>fn){
  closetimercallback_=fn;
}

void Connection::StartSendFile(int file_fd, off_t offset, size_t count, bool close_fd){
  ClearSendFile();
  sendfile_.file_fd = file_fd;
  sendfile_.offset = offset;
  sendfile_.remaining = count;
  sendfile_.close_fd = close_fd;
  sendfile_.active = (file_fd >= 0);
  clientchannel_->enablewriting();
}

void Connection::ClearSendFile(){
  if(sendfile_.file_fd >= 0 && sendfile_.close_fd){
    ::close(sendfile_.file_fd);
  }
  sendfile_ = SendFileState{};
}
