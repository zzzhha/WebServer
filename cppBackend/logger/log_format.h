#pragma once
#include<string>

class LogFormat {

public:
	/// 格式化日志转为字符串
	/// evel 日志级别
	/// log 日志内容
	/// file 源码文件路径
	/// line 代码行号
	virtual std::string Format(const std::string& level,
		const std::string& log,const std::string& file,int line) = 0;
};
