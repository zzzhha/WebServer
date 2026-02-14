#pragma once
#include "log_output.h"
#include<fstream>
#include<string>
#include<atomic>
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
	std::ofstream ofs_;
	std::string filename_;
	std::string base_filename_;
	std::atomic<size_t> flush_counter_{0};
	size_t flush_interval_ = 10;
	size_t max_file_size_ = 10 * 1024 * 1024;
	size_t max_files_ = 5;
	
	/// 滚动日志文件
	void RotateLog();
};

