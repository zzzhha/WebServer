#include "user_format.h"
#include <iomanip>
#include <sstream>
using namespace std;



//时间函数直接用就行不需要看懂，约等于local_time 不用他是因为local_time有时会出错（不知道啥原因）
static std::string Getnow(const char* fmt = "%Y-%m-%d %H:%M:%S",int time_zone = 8){
    std::time_t unix_sec = std::time(nullptr);
    std::tm tm;
    static const int kHoursInDay = 24;
    static const int kMinutesInHour = 60;
    static const int kDaysFromUnixTime = 2472632;
    static const int kDaysFromYear = 153;
    static const int kMagicUnkonwnFirst = 146097;
    static const int kMagicUnkonwnSec = 1461;
    tm.tm_sec = unix_sec % kMinutesInHour;
    int i = (unix_sec / kMinutesInHour);
    tm.tm_min = i % kMinutesInHour; //nn
    i /= kMinutesInHour;
    tm.tm_hour = (i + time_zone) % kHoursInDay; // hh
    tm.tm_mday = (i + time_zone) / kHoursInDay;
    int a = tm.tm_mday + kDaysFromUnixTime;
    int b = (a * 4 + 3) / kMagicUnkonwnFirst;
    int c = (-b * kMagicUnkonwnFirst) / 4 + a;
    int d = ((c * 4 + 3) / kMagicUnkonwnSec);
    int e = -d * kMagicUnkonwnSec;
    e = e / 4 + c;
    int m = (5 * e + 2) / kDaysFromYear;
    tm.tm_mday = -(kDaysFromYear * m + 2) / 5 + e + 1;
    tm.tm_mon = (-m / 10) * 12 + m + 2;
    tm.tm_year = b * 100 + d - 6700 + (m / 10);
    stringstream ss;
    ss << std::put_time(&tm, fmt); //#include <iomanip>
    return ss.str();
}

//替换字符串，将传入的字符串替换成我们想要的字符串
static void ReplaceString(string& str, const string ostr, const string& nstr){
    auto pos = str.find(ostr);
    if (pos == string::npos)return;
    str.replace(
        pos, // 1 被替换字符的起始位置
        ostr.size(),// 2 要被替换掉的内容长度
        nstr// 3 要替换为的字符串
    );
}

std::string UserFormat::Format(const std::string& level,
    const std::string& log,const std::string& file,int line){
    string logout = user_format;
    //xml格式我们需要替换花括号内的内容
//<log><time>{time}</time> <level>{level}</level><content>{content}</content> <file>{file}</file><line>{line}</line></log>
    ReplaceString(logout, "{time}", Getnow());
    ReplaceString(logout, "{level}", level);
    ReplaceString(logout, "{content}", log);
    ReplaceString(logout, "{file}", file);
    ReplaceString(logout, "{line}", to_string(line));
    return logout;
}