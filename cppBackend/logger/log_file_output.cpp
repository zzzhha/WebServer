#include "log_file_output.h"
#include<iostream>
#include<atomic>
#include<cstdio>

using namespace std;

LogFileOutput::LogFileOutput() {
}

LogFileOutput::~LogFileOutput() {
	if (ofs_.is_open()) {
		ofs_.flush();
		ofs_.close();
	}
}

void LogFileOutput::RotateLog() {
	if (!ofs_.is_open()) {
		return;
	}
	
	ofs_.flush();
	ofs_.close();
	
	// 删除最旧的文件
	if (max_files_ > 0) {
		string oldest_file = base_filename_ + "." + to_string(max_files_ - 1);
		remove(oldest_file.c_str());
		
		// 重命名现有文件
		for (int i = max_files_ - 2; i >= 0; --i) {
			string src = (i == 0) ? base_filename_ : base_filename_ + "." + to_string(i);
			string dst = base_filename_ + "." + to_string(i + 1);
			
			// 检查源文件是否存在
			FILE* test = fopen(src.c_str(), "r");
			if (test != nullptr) {
				fclose(test);
				rename(src.c_str(), dst.c_str());
			}
		}
	}
	
	// 重新打开新文件
	ofs_.open(filename_, ios::app);
	flush_counter_.store(0);
}

bool LogFileOutput::Open(const std::string& file) {
	if (ofs_.is_open()) {
		ofs_.flush();
		ofs_.close();
	}
	
	filename_ = file;
	base_filename_ = file;
	ofs_.open(file, std::ios::app);
	flush_counter_.store(0);
	
	if (!ofs_.is_open()) {
		cerr << "[LOG-ERROR] Failed to open log file: " << file << endl;
		return false;
	}
	
	return true;
}

void LogFileOutput::Output(const string& log) {
	if (!ofs_.is_open()) {
		return;
	}
	
	ofs_ << log << "\n";
	
	if (++flush_counter_ >= flush_interval_) {
		ofs_.flush();
		flush_counter_.store(0);
	}
	
	// 检查文件大小
	if (max_file_size_ > 0) {
		try {
			auto pos = ofs_.tellp();
			if (pos >= static_cast<streamoff>(max_file_size_)) {
				RotateLog();
			}
		} catch (...) {
			// 忽略文件大小检查错误
		}
	}
}
