#include"ThreadPool.h"

ThreadPool::ThreadPool(size_t threadnum,const std::string& threadtype):stop_(false),threadtype_(threadtype){
  for(int i=0;i<threadnum;i++){
    threads_.emplace_back([this]{
      //printf("create %s thread(%d).\n",threadtype_.c_str(),syscall(SYS_gettid));
      //std::cout << "create thread(" << std::this_thread::get_id() << ")." << std::endl;
      while(stop_==false){
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(this->mutex_);
        
          this->condition_.wait(lock,[this]{
            return !taskqueue_.empty() || stop_;
        });
        
        if(stop_ && taskqueue_.empty()) return; 

        task = move(taskqueue_.front());
        taskqueue_.pop();
        }
        tp_idl_tnum--;
        task();
        tp_idl_tnum++;
      }
    });
    tp_idl_tnum++;
  }
}

void ThreadPool::addtask(std::function<void()> task){
  {
    std::lock_guard<std::mutex> lock(mutex_);
    taskqueue_.push(task);
  }
  condition_.notify_one();
}

ThreadPool::~ThreadPool(){
  stop();
}

size_t ThreadPool::size(){
  return threads_.size();
}

void ThreadPool::stop(){
  if(stop_) return;
  stop_=true;
  condition_.notify_all();
  for(auto &thread:threads_){
    thread.join();
  }
}

int ThreadPool::idl_thread_cnt(){
  return tp_idl_tnum;
}