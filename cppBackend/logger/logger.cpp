#include "logger.h"




Logger::~Logger(){
	if(worker_  && worker_->joinable()){
		SetThreadStopWhile(true);
		cond_.notify_all();
		worker_->join();
	}
}

//write函数即为最终的输出函数
//log_fac确定了
//LogOutput* out_{ nullptr }; ->输出流
//LogFormat* formater_{ nullptr };->格式化方式
//Xlog log_level_ ->日志级别
//通过宏 传入日志级别，文件内容，文件所在地，文件行数
//        Xlog枚举类   s宏参数    __FILE__    __LINE__  
// 没有输出流是因为配置文件确定的输出流，而不是write传入输出流
void Logger::Write(Xlog level,
	const std::string& log,
	const std::string& file,
	int line
) {
	//如果传入参数的日志级别小于配置文件的日志级别
	//那么我们不存储此信息
	if (!formater_ || !out_)return;
	if (level < log_level_) return;
	
	//将日志级别枚举类型转化为字符串
	std::string levelstr = "debug";
	switch (level) {
	case Xlog::INFO:
		levelstr = "info";
		break;
	case Xlog::WARNING:
		levelstr="warning";
		break;
	case Xlog::ERROR:
		levelstr = "error";
		break; 
	case Xlog::FATAL:
		levelstr = "fatal";
		break;
	default:
		break;
	}

	//格式化日志，log_fac已经帮我们确定了formater_ 此指针的派生类指向
	//日志级别，日志内容，文件所在地，行数
	if(isasync_){
		AddWorkLog(formater_->Format(levelstr, log, file, line));
	}
	else{
		auto str=formater_->Format(levelstr, log, file, line);
		//文件格式化后就可以输出了，log_fac通过配置文件处理已经确定过了文件输出在哪里
		out_->Output(str);
	}
	
	
}

void Logger::SetAsyncMode(bool async){
	isasync_=async;
}
bool Logger::IsAsyncMode() const{
	return isasync_;
}

void Logger::SetThreadStopWhile(bool th_stop){
	th_stop_=th_stop;
}

void Logger::Init_Thread(){
	if (thread_initialized_){
    return; 
  }
	if(!worker_){
		worker_ =std::make_unique<std::thread>([this]{
			while(true){
				std::string str;
				{
					std::unique_lock<std::mutex> lock(mutex_);
					cond_.wait(lock,[this]{
						return th_stop_ || !workqueue_.empty();
					});
					if(th_stop_ && workqueue_.empty()) return;
				
					str=workqueue_.front();
					workqueue_.pop();
				}
				out_->Output(str);
			}
		});
		thread_initialized_ = true;
	}
}

void Logger::AddWorkLog(std::string&& log){
	{
		std::unique_lock<std::mutex> lock(mutex_);
		workqueue_.push(std::move(log));
	}

	cond_.notify_one();
}