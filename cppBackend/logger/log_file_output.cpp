#include "log_file_output.h"
#include<iostream>

using namespace std;
//判断文件是否已追加的方式 打开成功，
//所以说他只做一件事
//以追加的方式打开传入的file文件，成功返回true 失败返回false
bool LogFileOutput::Open(const std::string& file) {
	//追加写入
	ofs_.open(file,std::ios::app);
	printf("追加写入\n");
	if (ofs_.is_open()) return true;
	return false;
}
//输入到文件里
void LogFileOutput::Output(const string& log) {
	ofs_ << log << "\n";
	ofs_.flush();
}

