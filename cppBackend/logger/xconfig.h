#pragma once
#include<string>
#include<map>
//读取ini的配置文件
/*
* 
log.conf

log_type=console
log_file=log.txt
log_level=debug
*/
class XConfig
{
///  读取配置文件写入内部
public:
	bool Read(const std::string& file);
	const std::string& Get(const std::string& key);
	std::string Get(const std::string& key, const std::string& default_value);
private:
	std::map<std::string, std::string>conf_;
};

