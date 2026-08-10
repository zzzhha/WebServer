#pragma once
#include<string>
#include<mutex>
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
	/// 初始化logger对象（可重复调用：先安全停止旧的异步 worker 再重建，审计低危）
	void Init(bool isasync,const std::string& con_file = "log.conf");
	/// 停止异步 worker 并回收线程，供进程退出前显式调用，避免静态析构与残留线程竞态（审计低危）
	void Shutdown();
	Logger& logger() { return logger_; }
private:
	LogFac() {}
	Logger logger_;
	// 串行化 Init/Shutdown，防止并发调用相互踩踏
	std::mutex init_mutex_;
};


#define XLOGOUT(level,str) LogFac::Instance().logger().Write(level, str, __FILE__, __LINE__)

//通过这四个宏对日志系统进行写入，指定文件为默认为log.txt,可以在conf文件更改
#define LOGDEBUG(s)  XLOGOUT(Xlog::DEBUG,s)
#define LOGINFO(s)  XLOGOUT(Xlog::INFO,s)
#define LOGWARNING(s) XLOGOUT(Xlog::WARNING,s)
#define LOGERROR(s) XLOGOUT(Xlog::ERROR,s)
#define LOGFATAL(s) XLOGOUT(Xlog::FATAL,s)
