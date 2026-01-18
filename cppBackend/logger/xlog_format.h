#pragma once
#include "log_format.h"
class XLogFormat : public LogFormat
{
public:
	///////////////////////////////////////////////////
	/// 格式化日志转为字符串
	/// @para level 日志级别
	/// @para log 日志内容
	/// @para file 源码文件路径
	/// @para line 代码行号
	/// @return 格式化后的日志
	/// 
	std::string Format(const std::string& level,
		const std::string& log,const std::string& file,
		int line ) override;
};

