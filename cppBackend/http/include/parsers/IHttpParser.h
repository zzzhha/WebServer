#pragma once

#include "core/IHttpMessage.h"
#include <string>
#include <vector>
#include <memory>

enum class ParseResult{
  NEEDMOREDATA = 0,
  SUCCESS = 1,
  ERROR = -1,
  INVALIDSTARTLINE = -2,
  INVALIDHEADER = -3,
  HEADERTOOLONG = -4,
  BODYTOOLONG = -5,
  UNSUPPORTEDVERSION = -6
};

// enum class ParseErrorType {
//     NO_ERROR,                // 无错误
//     INVALID_PROTOCOL,        // 协议格式非法（如不是 HTTP 消息）
//     INVALID_VERSION,         // HTTP 版本不支持
//     MISSING_REQUIRED_HEADER, // 缺少必填头字段（如 Host、Content-Length）
//     MALFORMED_HEADER,        // 消息头格式错误（如键值对不合法）
//     BODY_LENGTH_MISMATCH,    // 消息体长度与 Content-Length 不匹配
//     INVALID_CHUNKED_ENCODING,// 分块编码格式错误（HTTP/1.1）
//     INSUFFICIENT_DATA,       // 数据不完整（理论上不会返回，由 PARSING 状态表示）
//     UNSUPPORTED_FEATURE      // 不支持的特性（如 HTTP/2 的流帧）
// };

class IHttpParser{
public:
  virtual ~IHttpParser() = default;

  virtual int Parse(std::string&,std::unique_ptr<IHttpMessage>& out) = 0;
  virtual int Parse(const char* data, size_t len, std::unique_ptr<IHttpMessage>& out) = 0;

  virtual void Reset() = 0;

  virtual void SetMaxHeaderLineSize(size_t size) { maxHeaderLineSize_ = size; }
  virtual void SetMaxHeaderCount(size_t count) { maxHeaderCount_ = count; }
  virtual void setMaxBodySize(size_t size) { maxBodySize_ = size; }
  virtual void setStrictHeaderCheck(bool strict) { strictHeaderCheck_ = strict; }
  
  virtual size_t GetConsumeBytes() const = 0;

protected:
  size_t maxHeaderLineSize_ = 8192;
  size_t maxHeaderCount_ = 100;
  size_t maxBodySize_ = 0;
  bool strictHeaderCheck_ = true;
};
