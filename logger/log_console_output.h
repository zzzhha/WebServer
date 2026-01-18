#pragma once
#include "log_output.h"
class LogConsoleOutput :public LogOutput
{
public:
	//log 格式化后的日志内容
	void Output(const std::string& log) override;
};

