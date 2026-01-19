#pragma once

#include"IHttpParser.h"
#include"core/IHttpMessage.h"
#include<memory>
#include<queue>

class Http2Parser : public IHttpParser {
public:
  Http2Parser();
  int Parse(std::string&,std::unique_ptr<IHttpMessage>& out) override;
  int Parse(const char* data, size_t len, std::unique_ptr<IHttpMessage>& out) override;
  void Reset() override;
  ~Http2Parser() override;
  size_t GetConsumeBytes() const override;

/*   void SetMaxConcurrentStreams(size_t max) { maxConcurrentStreams_ = max; }
  void SetInitialWindowSize(uint32_t size) { initialWindowSize_ = size; }
  void SetEnablePush(bool enable) { enablePush_ = enable; }
  size_t GetActiveStreamCount() const;

private:
  struct Http2Stream; //前向声明（每一个流对应一个request/Response）
  
  //HTTP/2 连接级状态
  bool prefaceReceied_ = false;   //是否接收到 "PRI * HTTP/2.0\r\n"
  uint32_t nextStreamId_ = 1;     //下一个流 ID(客户端奇数，服务端偶数)
  size_t maxConcurrentStreams_ = 65535;
  uint32_t initialWindowSize_ = 100;
  bool enablePush_ = false;

  //流管理(map<stream_id,stream>)
  std::map<uint32_t,std::unique_ptr<Http2Stream>> activeStreams_;
  std::queue<std::unique_ptr<IHttpMessage>> completedMessages_; //已完成的消息队列

  //底层帧解析器
  struct FrameParser;
  std::unique_ptr<FrameParser> frameParser_;
  
  //核心逻辑
  ParseResult ProcessPreface(const char*& data,size_t& len);
  ParseResult ProcessFrame(const char*& data, size_t& len);
  void OnHeadersFrame(uint32_t stream_id,const std::vector<std::pair<std::string,std::string>>& headers, bool end_headers);
  void OnDataFrame(uint32_t stream_id, const char* data, size_t len, bool end_stream);
  void OnStreamComplete(uint32_t stream_id);

  //工具 HPACK解码（此处为占位，实际需要实现或者继承
  std::vector<std::pair<std::string,std::string>> decodeHeaders(const uint8_t* data, size_t len); */
};