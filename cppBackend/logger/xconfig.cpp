#include "xconfig.h"
#include<fstream>
#include<iostream>
#include <algorithm>
#include <cctype>
using namespace std;

namespace {
inline void TrimInPlace(std::string& s) {
	size_t start = 0;
	while (start < s.size() &&
		   std::isspace(static_cast<unsigned char>(s[start]))) {
		++start;
	}
	size_t end = s.size();
	while (end > start &&
		   std::isspace(static_cast<unsigned char>(s[end - 1]))) {
		--end;
	}
	if (start == 0 && end == s.size()) return;
	s = s.substr(start, end - start);
}

inline void LowerAsciiInPlace(std::string& s) {
	for (char& c : s) {
		unsigned char uc = static_cast<unsigned char>(c);
		c = static_cast<char>(std::tolower(uc));
	}
}

inline std::string NormalizeKey(std::string key) {
	TrimInPlace(key);
	LowerAsciiInPlace(key);
	return key;
}
} // namespace


//查找函数，Read读取过配置文件后，在conf_这个map结构中已经存储了配置文件的所有信息
//通过find函数查找并返回即可，如果没有就返回空
const std::string& XConfig::Get(const std::string& key) {
	auto c = conf_.find(NormalizeKey(key));
	if (c == conf_.end()) 
	{
		static std::string empty_string;
		return empty_string;
	}
	return c->second;
}

//查找函数，带默认值
std::string XConfig::Get(const std::string& key, const std::string& default_value) {
	auto c = conf_.find(NormalizeKey(key));
	if (c == conf_.end()) {
		return default_value;
	}
	return c->second;
}

//读取函数，读取配置文件，如果打开失败就返回false
bool XConfig::Read(const std::string& file) {
	ifstream ifs(file);

	if (!ifs.is_open())return false;
	string line;
//死循环读取配置文件中的每一行数据，读取结束再退出
	for (;;) {
		//读取每一行配置 例如：log_type=console
		getline(ifs, line);
		string k, t,v;
		if (!line.empty()) {
			TrimInPlace(line);
			if (line.empty()) {
				if (!ifs.good())break;
				continue;
			}
			if (line[0] == '#' || line[0] == ';') {
				if (!ifs.good())break;
				continue;
			}
			auto p = line.find('=');
			if (p <= 0) continue;
			k = line.substr(0, p);
			v = line.substr(p + 1);
			TrimInPlace(k);
			TrimInPlace(v);
			k = NormalizeKey(k);
			conf_[k] = v;
		}
		//如果出错或者读到结尾
		if (!ifs.good())break;
	}
	return true;
}
