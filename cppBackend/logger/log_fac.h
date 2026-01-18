#pragma once
#include<string>
#include"logger.h"
//logger工厂类
//维护logger生命周期
//构建logger

class LogFac {
public:
	static LogFac& Instance() {
		static LogFac fac;
		return fac;
	}
	/// 初始化logger对象
	void Init(bool isasync,const std::string& con_file = "log.conf");
	Logger& logger() { return logger_; }
private:
	LogFac() {}
	Logger logger_;
};


#define XLOGOUT(level,str) LogFac::Instance().logger().Write(level, str, __FILE__, __LINE__)

//通过这四个宏对日志系统进行写入，指定文件为默认为log.txt,可以在conf文件更改
#define LOGDEBUG(s)  XLOGOUT(Xlog::DEBUG,s)
#define LOGINFO(s)  XLOGOUT(Xlog::INFO,s)
#define LOGWARNING(s) XLOGOUT(Xlog::WARNING,s)
#define LOGERROR(s) XLOGOUT(Xlog::ERROR,s)
#define LOGFATAL(s) XLOGOUT(Xlog::FATAL,s)
