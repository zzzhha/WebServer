#pragma once
#include "log_output.h"
#include<fstream>
#include<string>
#include<atomic>
#include<mutex>
class LogFileOutput :public LogOutput
{
public:
	LogFileOutput();
	~LogFileOutput() override;

	/// 打开写入日志的文件
	bool Open(const std::string& file);

	///格式化后的日志内容
	void Output(const std::string& log) override;
	
	/// 设置刷新间隔（行数）
	void SetFlushInterval(size_t interval) { flush_interval_ = interval; }
	
	/// 设置最大文件大小（字节）
	void SetMaxFileSize(size_t size) { max_file_size_ = size; }
	
	/// 设置最大文件数量
	void SetMaxFiles(size_t count) { max_files_ = count; }
	
private:
	// P2 修复：同步模式下多线程并发写 ofs_ / RotateLog close-reopen 竞态 → 互斥串行化
	std::mutex mu_;
	std::ofstream ofs_;
	std::string filename_;
	std::string base_filename_;
	std::atomic<size_t> flush_counter_{0};
	size_t flush_interval_ = 10;
	size_t max_file_size_ = 10 * 1024 * 1024;
	size_t max_files_ = 5;
	
	/// 滚动日志文件（带锁包装）
	void RotateLog();
	/// 滚动日志文件（无锁内部实现，调用方需持有 mu_）
	void DoRotateLog();
};

