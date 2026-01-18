#pragma once

#include<string>
#include<map>
#include<vector>
#include<memory>
#include"IHttpMessage.h"

enum class HttpStatusCode{
  CONTINUE = 100,
  SWITCHING_PROTOCOLS = 101,

  OK = 200,
  CREATED = 201,
  ACCEPTED = 202,
  NO_CONTENT = 204,
  PARTIAL_CONTENT = 206,

  MOVED_PERMANENTLY = 301,
  FOUND = 302,
  SEE_OTHER = 303,
  NOT_MODIFIED = 304,
  TEMPORARY_REDIRECT = 307,
  PERMANENT_REDIRECT = 308,

  BAD_REQUEST = 400,
  UNAUTHORIZED = 401,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  METHOD_NOT_ALLOWED = 405,
  REQUEST_TIMEOUT = 408,
  PAYLOAD_TOO_LARGE = 413,
  URI_TOO_LONG = 414,
  UNSUPPORTED_MEDIA_TYPE = 415,

  INTERNAL_SERVER_ERROR = 500,
  NOT_IMPLEMENTED = 501,
  BAD_GATEWAY = 502,
  SERVICE_UNAVAILABLE = 503,
  GATEWAY_TIMEOUT = 504,
  HTTP_VERSION_NOT_SUPPORTED = 505 
};

class HttpResponse : public IHttpMessage{
public:
  HttpResponse();
  explicit HttpResponse(HttpStatusCode statuscode);
  ~HttpResponse() override = default;

  void SetHeader(const std::string& key,const std::string& value) override;
  void AppendHeader(const std::string& key,const std::string& value) override;
  void AppendHeader(const std::vector<std::pair<std::string,std::string>>& headers) override;
  virtual std::optional<std::string> GetHeader(const std::string& key) const override;
  virtual std::vector<std::string> GetHeaders(const std::string& key) const override;
  bool HasHeader(const std::string& key)const override;

  const std::vector<std::pair<std::string,std::string>>& GetAllHeaders()const override;
  void RemoveHeader(const std::string& key) override;
  void ClearHeaders() override;

  //http版本操作
  void SetVersion(HttpVersion version) override;
  HttpVersion GetVersion() const override;
  std::string GetVersionStr() const override;

  //http body操作
  void SetBody(const std::string& body) override { body_ = body; }
  //void SetBinaryBody(const char* binary, size_t length) override;
  std::string GetBody() const override { return body_; }
  //const char* GetBinaryBody(size_t& length) const override;
  size_t GetBodyLength() const override { return body_.length(); }
  void ClearBody() override { body_.clear(); }

  //http编码操作
  void SetContentEncoding(HttpContentEncoding encoding) override { contentEncoding_ = encoding; }
  HttpContentEncoding GetContentEncoding() const override { return contentEncoding_; }
  std::string GetContentEncodingStr() const override;
  void Clear() override;
  std::string Serialize() const override;

  //parser接口
  void SetStatusCodeInt(int code) override;
  void SetStatusReason(std::string_view reason) override;
  void SetStatusLine(HttpVersion version,int status_code,std::string_view reason) override;
  void AppendRawHeaderLine(std::string_view line) override;
  void AppendBodyChunk(const char* data,size_t len) override;

  bool IsResponse() const override { return true; }
  MessageType GetMessageType() const override { return MessageType::Response; }

  void SetStatusCode(HttpStatusCode statuscode);
  void SetStatusCodeWithReason(HttpStatusCode statusCode, const std::string& customReason) { statusCode_ = statusCode; statusReason_ = customReason; }
  HttpStatusCode getStatusCode() const { return statusCode_; }
  int getStatusCodeInt() const { return static_cast<int>(statusCode_); }
  std::string getStatusReason() const { return statusReason_; }
  bool isSuccess() const { return static_cast<int>(statusCode_) >= 200 && static_cast<int>(statusCode_) < 300; }
  bool isRedirect() const { return static_cast<int>(statusCode_) >= 300 && static_cast<int>(statusCode_) < 400;}
  bool isClientError() const { return static_cast<int>(statusCode_) >=400 && static_cast<int>(statusCode_)< 500; }
  bool isServerError() const { return static_cast<int>(statusCode_) >=500 && static_cast<int>(statusCode_)< 600; }

private:
  static std::string GetDefaultReason(HttpStatusCode statusCode);
  static std::string trim(std::string_view& view);
  std::string normalizeHeaderKey(const std::string& key) const;
  
  HttpVersion version_;
  HttpStatusCode statusCode_;
  std::string statusReason_;
  std::vector<std::pair<std::string,std::string>>headers_;
  std::string body_;
  HttpContentEncoding contentEncoding_;
};