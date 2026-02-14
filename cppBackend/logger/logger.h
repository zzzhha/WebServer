#pragma once
#include<string>
#include"log_format.h"
#include"log_output.h"
#include<memory>
#include<thread>
#include<mutex>
#include<atomic>
#include<queue>
#include<condition_variable>
#include<vector>
enum class Xlog {
	DEBUG,
	INFO,
	WARNING,
	ERROR,
	FATAL
};
class Logger
{
public:
	///////////////////////////////////////////////////
	/// 格式化并写入日志
	/// @para level 日志级别
	/// @para log 日志内容
	/// @para file 源码文件路径
	/// @para line 代码行号
	void Write(Xlog level,
		const std::string& log,
		const std::string& file,
		int line);
	//设置文件输出流接口
	void SetOutput(std::unique_ptr<LogOutput> o) {
		out_ = move(o);
	}
	//设置格式化接口
	void SetFormat(std::unique_ptr<LogFormat> f) {
		formater_ = move(f);
	}
	//设置日志级别接口
	void SetLevel(Xlog level) {
		log_level_ = level;
	}
	~Logger();

	void SetAsyncMode(bool async);
	bool IsAsyncMode() const;

	void SetThreadStopWhile(bool th_stop);
	void Init_Thread();
	void AddWorkLog(std::string&& log);
	
	/// 获取当前队列中的日志数量
	size_t GetQueueSize() const;
	
	/// 获取处理的日志总数
	size_t GetTotalProcessed() const;
	
	/// 检查工作线程是否正在运行
	bool IsWorkerThreadRunning() const;
	
	/// 设置批量处理大小
	void SetBatchSize(size_t size) { batch_size_ = size; }

private:
	//设置文件输出流
	std::unique_ptr<LogOutput>out_;
	//设置文件格式
	std::unique_ptr<LogFormat>formater_;
	//最低日志级别
	Xlog log_level_{ Xlog::DEBUG };

	//是否需要异步
	bool isasync_{false};
	//异步的工作线程
	std::unique_ptr<std::thread> worker_;
	//工作线程是否被初始化
	std::atomic_bool thread_initialized_{false};
	//工作线程是否关闭
	std::atomic_bool th_stop_;
	//工作线程锁
	std::mutex mutex_;
	//给子进程发送日志的队列
	std::queue<std::string>workqueue_;
	//条件变量，用于通知子进程有任务到达
	std::condition_variable cond_;
	//批量处理大小
	size_t batch_size_{100};
	//处理的日志总数
	std::atomic<size_t> total_processed_{0};
};

