#include "parsers/Http2Parser.h"

#include <utility>

Http2Parser::Http2Parser() = default;
Http2Parser::~Http2Parser() = default;

int Http2Parser::Parse(std::string&, std::unique_ptr<IHttpMessage>& out) {
  // 占位实现：当前未提供 HTTP/2 解析，返回不支持版本
  (void)out;
  return static_cast<int>(ParseResult::UNSUPPORTEDVERSION);
}

int Http2Parser::Parse(const char* data, size_t len, std::unique_ptr<IHttpMessage>& out) {
  std::string buf(data, data + len);
  return Parse(buf, out);
}

void Http2Parser::Reset() {
/*   activeStreams_.clear();
  while(!completedMessages_.empty()) completedMessages_.pop();
  prefaceReceied_ = false;
  nextStreamId_ = 1; */
}
size_t Http2Parser::GetConsumeBytes() const {

}
/* size_t Http2Parser::GetActiveStreamCount() const {
  return activeStreams_.size(); 
} */
