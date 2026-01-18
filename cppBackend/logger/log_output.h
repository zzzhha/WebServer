#pragma once
#include<string>
#include<vector>
/// 
/// 日志的输出接口，输出到设备
///

class LogOutput {
public:
	/// 
	/// 日志输出
	/// log 格式化后的输出内容
	/// 
	virtual void Output(const std::string& log) = 0;
};