#include "logger.h"
#include <iostream>

namespace {
	void EmergencyOutput(const std::string& msg) {
		std::cerr << "[LOG-EMERGENCY] " << msg << std::endl;
	}
}

Logger::~Logger(){
	try {
		if(worker_ && worker_->joinable()){
			SetThreadStopWhile(true);
			cond_.notify_all();
			worker_->join();
		}
	} catch (const std::exception& e) {
		EmergencyOutput(std::string("Exception in Logger destructor: ") + e.what());
	} catch (...) {
		EmergencyOutput("Unknown exception in Logger destructor");
	}
}

void Logger::Write(Xlog level,
	const std::string& log,
	const std::string& file,
	int line
) {
	if (!formater_ || !out_) {
		EmergencyOutput("Logger not properly initialized: formater_ or out_ is null");
		return;
	}
	if (level < log_level_) return;
	
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

	if(isasync_){
		AddWorkLog(formater_->Format(levelstr, log, file, line));
	}
	else{
		auto str=formater_->Format(levelstr, log, file, line);
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
	if (thread_initialized_.exchange(true)){
		return;
	}
	if(!worker_){
		worker_ = std::make_unique<std::thread>([this]{
			try {
				std::vector<std::string> batch;
				while(true){
					batch.clear();
					{
						std::unique_lock<std::mutex> lock(mutex_);
						cond_.wait(lock,[this]{
							return th_stop_ || !workqueue_.empty();
						});
						
						if(th_stop_ && workqueue_.empty()) return;
						
						// 批量取出
						while(!workqueue_.empty() && batch.size() < batch_size_){
							batch.push_back(std::move(workqueue_.front()));
							workqueue_.pop();
						}
					}
					
					// 批量输出
					if (out_) {
						for (auto& str : batch) {
							out_->Output(str);
							++total_processed_;
						}
					} else {
						EmergencyOutput("out_ is null in worker thread");
					}
				}
			} catch (const std::exception& e) {
				EmergencyOutput(std::string("Exception in worker thread: ") + e.what());
			} catch (...) {
				EmergencyOutput("Unknown exception in worker thread");
			}
		});
	} else {
		thread_initialized_ = false;
	}
}

void Logger::AddWorkLog(std::string&& log){
	{
		std::unique_lock<std::mutex> lock(mutex_);
		const size_t MAX_QUEUE_SIZE = 10000;
		if (workqueue_.size() >= MAX_QUEUE_SIZE) {
			EmergencyOutput("Log queue full, dropping log");
			return;
		}
		workqueue_.push(std::move(log));
	}

	cond_.notify_one();
}

size_t Logger::GetQueueSize() const {
	std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(mutex_));
	return workqueue_.size();
}

size_t Logger::GetTotalProcessed() const {
	return total_processed_.load();
}

bool Logger::IsWorkerThreadRunning() const {
	return thread_initialized_.load() && !th_stop_.load();
}
