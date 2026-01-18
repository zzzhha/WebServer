#include "log_console_output.h"
#include<iostream>
using namespace std;


//因为是在控制台输出，所以成员变量ofs_没啥作用，直接cout输出即可
void LogConsoleOutput::Output(const std::string& log) {
	cout << log << endl;
 }