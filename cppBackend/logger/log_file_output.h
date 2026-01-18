#pragma once
#include "log_output.h"
#include<fstream>
class LogFileOutput :public LogOutput
{
public:
	/// 打开写入日志的文件
	bool Open(const std::string& file);

	///格式化后的日志内容
	void Output(const std::string& log) override;
	
private:
	std::ofstream ofs_;
};

