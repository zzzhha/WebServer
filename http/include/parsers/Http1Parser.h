#pragma once

#include"IHttpParser.h"
#include<string>
#include<memory>
#include<algorithm>

class HttpRequest;
class HttpResponse;

class Http1Parser : public IHttpParser {
public:
  Http1Parser();
  int Parse(std::string&,std::unique_ptr<IHttpMessage>& out) override;
  int Parse(const char* data, size_t len, std::unique_ptr<IHttpMessage>& out) override;
  void Reset() override;

  size_t GetConsumeBytes() const { return totalConsumed_; }
  ~Http1Parser();

private:
  enum class ParseState{
    kStartLine,
    kHeaders,
    kBodyContentLength,
    kBodyChunkedSize,
    kBodyChunkedData,
    kBodyChunkedEnd,
    kDone
  };

  ParseResult ParseStartLine(std::string_view line);
  ParseResult ParseHeaderLine(std::string_view line);
  ParseResult ParseChunkSize(std::string_view line);
  ParseResult FinalizeMessage(std::unique_ptr<IHttpMessage>& out);

  static bool isTokenChar(char c);
  static std::string trimLWS(std::string_view s); //LWS = Linear White space(线性空白)

  ParseState state_ = ParseState::kStartLine;
  std::unique_ptr<IHttpMessage> currentMessage_;
  std::string lineBuffer_;    //拼接不完整行
  size_t contentLength_;      //当前body长度
  size_t chunkSize_;          // 当前chunk大小
  size_t bodyReceived_;       //已接收 body 字节数
  bool isChunked_ = false;    //Transfer-Encoding:chuned
  size_t totalConsumed_ = 0;  //总消费字节数
  size_t headerCount_ = 0;    //已解析 header数
};