#include "log_fac.h"
#include"log_console_output.h"
#include"log_file_output.h"
#include"xlog_format.h"
#include"xconfig.h"
#include "user_format.h"
#include<iostream>
#include <cstring>
using namespace std;

#define LOGFILE "log.txt"



//初始化logger类
//我们需要初始化3个数据，此处Init只做了初始化logger类
//logger类需要我们传入	LogOutput* out_，LogFormat* formater_，Xlog log_level_
//						 输出流			  输出格式			    日志级别
//配置文件需要获取4个数据
//log_type ，log_file ，  log_level  ，log_user_format 
//文件输出流 文件所在地   文件级别      文件格式化方式
//前两个参数 确定输出流，如果是文件，则需要确定输出到哪里，如果是控制台则不管第二个参数
//第三个参数 确定日志级别
//第四个参数 确定格式化方式
void LogFac::Init(bool isasync,const std::string& con_file){
	logger_.SetAsyncMode(isasync);
	if(isasync) {
		logger_.SetThreadStopWhile(false);
		logger_.Init_Thread();
	}
	else{
		logger_.SetThreadStopWhile(true);
	}
	logger_.SetFormat(make_unique<XLogFormat>());
	//读取配置文件，配置文件决定上述四个数据
	XConfig conf;
	//这里是配置文件的处理，读取配置文件并将配置文件存储于map数据结构中
	//map<string key,string value> key值为上述四个数据 value值为配置文件中的数据
	bool re = conf.Read(con_file);
	//初始化定义文件类别
	string log_type = "console";
	string log_file = "log.txt";
	string log_level = "debug";
	string log_user_format = "";
	//如果Read函数成功读取到了配置文件的数据
	if (re) {
		//将这四个数据获取到临时变量中
		log_type = conf.Get("log_type");
		log_file = conf.Get("log_file");
		log_level = conf.Get("log_level");
		log_user_format = conf.Get("log_user_format");
	}
	//判断输出格式，并将其存储到logger对应格式中
	//如果配置文件没有确定格式化输出
	//那么我们就使用默认的格式化xlog_format
	if (log_user_format.empty()){
		logger_.SetFormat(make_unique<XLogFormat>());
	}
	//如果有那么我们就设置为此格式化，即xml格式（UserFormat）
	//因为UserFormat有格式，所以需要调用其构造函数，并将原本的格式传入
	//格式：<log><time>{time}</time> <level>{level}</level><content>{content}</content> <file>{file}</file><line>{line}</line></log>
	else{
		logger_.SetFormat(make_unique<UserFormat>(log_user_format));
	}
	//判断日志级别，并将其存储到logger中
	if (log_level == "info") {
		logger_.SetLevel(Xlog::INFO);
	}
	else if(log_level == "warning"){
		logger_.SetLevel(Xlog::WARNING);
	}
	else if (log_level == "error") {
		logger_.SetLevel(Xlog::ERROR);
	} 
	else if (log_level == "fatal") {
		logger_.SetLevel(Xlog::FATAL);
	}
	//特殊判断输出流为文件的时候
	if (log_type.find("file")!=string::npos) {
		//如果未指定输出文件，则输出到默认文件中，即输出到"log.txt"中
		if (log_file.empty()) {
			log_file = LOGFILE;
		}
		//因为输出流是文件，所以我们需要文件输出流
		auto fout = make_unique< LogFileOutput>();//new LogFileOutput();
		//如果文件打开失败，就报错
		//此处应该还会有其他操作，例如直接返回，但是我们此处不做任何操作，只是报错
		//此处能结束为什么文件输出需要这样写而不是跟控制台输出一样logger_.SetOutput(new LogFileOutput);
		//因为我们要判断是否成功打开文件
		if (!fout->Open(log_file)) {
			cerr << "open file failed " << log_file << endl;
			logger_.SetOutput(make_unique<LogConsoleOutput>());
		}
		//设置logger的输出流为 log_file文件
		//此处运用多态，logger内存储的是基类指针
		//我们传入派生类指针，形成多态
		else{
			logger_.SetOutput(move(fout));
		}
	}
	//如果不是存储到文件中，则设置在控制台输出
	//如果为控制台输出，则log_file没有用
	//不需要去管log_file
	else {
		logger_.SetOutput(make_unique<LogConsoleOutput>());
	}

}