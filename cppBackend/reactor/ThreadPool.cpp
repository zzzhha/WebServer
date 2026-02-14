#include"ThreadPool.h"

ThreadPool::ThreadPool(size_t threadnum, const std::string& threadtype, size_t max_queue_size):stop_(false),threadtype_(threadtype),max_queue_size_(max_queue_size){
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
        tp_idl_tnum--;
        }
        try {
          task();
        } catch (const std::exception& e) {
          // 打印异常信息，避免线程终止
          std::cerr << "Exception in thread: " << e.what() << std::endl;
        } catch (...) {
          // 捕获所有其他异常
          std::cerr << "Unknown exception in thread" << std::endl;
        }
        {
          std::lock_guard<std::mutex> lock(this->mutex_);
          tp_idl_tnum++;
        }
      }
    });
    tp_idl_tnum++;
  }
}

void ThreadPool::addtask(std::function<void()> task){
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskqueue_.size() >= max_queue_size_) {
      // 任务队列已满，丢弃任务或处理错误
      std::cerr << "Task queue is full, task discarded" << std::endl;
      return;
    }
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

size_t ThreadPool::queue_size(){
  std::lock_guard<std::mutex> lock(mutex_);
  return taskqueue_.size();
}

// 模板方法实现，支持返回值的任务
template<typename F, typename... Args>
auto ThreadPool::add_task(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
  using return_type = typename std::result_of<F(Args...)>::type;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...)
  );

  std::future<return_type> res = task->get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查线程池是否已经停止
    if (stop_) {
      throw std::runtime_error("add_task on stopped ThreadPool");
    }

    // 检查任务队列是否已满
    if (taskqueue_.size() >= max_queue_size_) {
      throw std::runtime_error("Task queue is full");
    }

    taskqueue_.emplace([task]() {
      (*task)();
    });
  }
  condition_.notify_one();
  return res;
}

// 显式实例化，避免链接错误
template auto ThreadPool::add_task<>(std::function<void()> &&, void &&...) -> std::future<void>;