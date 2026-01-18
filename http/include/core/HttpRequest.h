#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "IHttpMessage.h"
enum class HttpMethod{
  GET,
  POST,
  PUT,
  DELETE,
  PATCH,
  HEAD,
  OPTIONS,
  TRACE,
  CONNECT,
  UNKNOWN
};

class HttpRequest : public IHttpMessage {
public:
  HttpRequest() = default;
  HttpRequest(const HttpRequest& other);
  HttpRequest(HttpRequest && other) noexcept;
  HttpRequest& operator=(const HttpRequest& other);
  HttpRequest& operator=(HttpRequest&& other) noexcept;
  ~HttpRequest() override = default;

  void SetVersion(HttpVersion version) override { version_ = version; }
  HttpVersion GetVersion() const { return version_; }
  std::string GetVersionStr() const override;
  void SetHeader(const std::string& key,const std::string& value) override;
  void AppendHeader(const std::string& key,const std::string& value) override;
  void AppendHeader(const std::vector<std::pair<std::string,std::string>>& headers) override;
  virtual std::optional<std::string> GetHeader(const std::string& key) const override;
  virtual std::vector<std::string> GetHeaders(const std::string& key) const override;
  const std::vector<std::pair<std::string,std::string>>& GetAllHeaders() const override;
  bool HasHeader(const std::string& key) const override;
  void RemoveHeader(const std::string& key) override;
  void ClearHeaders() override;

  void SetBody(const std::string& body) override { body_ = body; }
  //void SetBinaryBody(const char* binary, size_t length) override;
  std::string GetBody() const override { return body_; }
  //const char* GetBinaryBody(size_t& length) const override;
  size_t GetBodyLength() const override { return body_.length(); }
  void ClearBody() override {body_.clear(); }

   void SetContentEncoding(HttpContentEncoding encoding) override { contentEncoding_ = encoding; }
  HttpContentEncoding GetContentEncoding() const override { return contentEncoding_; }
  std::string GetContentEncodingStr() const override;

  //parser接口
  void SetMethodString(std::string_view method) override;
  void SetUrl(std::string_view url) override;
  void SetStatusCodeInt(int code) override { statusCode_ = code; }
  int GetStatusCodeInt() const override { return statusCode_; }
  void SetRequestLine(std::string_view method,std::string_view url,HttpVersion version) override;
  void AppendBodyChunk(const char* data,size_t len) override;

  bool IsRequest() const override { return true; }
  bool IsResponse() const override { return false; }
  MessageType GetMessageType() const override { return MessageType::Request; }

  void Clear() override;
  std::string Serialize() const override;
  //HttpRequest特有的方法
  void SetMethod(HttpMethod method);
  void SetPath(std::string_view path);
  HttpMethod GetMethod() const { return method_; }

  std::string GetMethodString() const;
  std::string GetUrl() const { return url_; }
  std::string GetPath() const { return path_; }

  void AddQueryParam(const std::string& key, const std::string& value);
  void SetQueryParam(const std::string& key, const std::string& value);
  std::string GetQueryParam(const std::string& key) const;
  std::vector<std::string> GetQueryParams(const std::string& key)const;
  const std::map<std::string,std::vector<std::string>>& GetAllQueryParams() const { return queryParams_; }
  bool RemoveQueryParam(const std::string& key);
  void ClearQueryParams();

private:
  std::string normalizeHeaderKey(const std::string& key) const;
  void ParseUrl();
  void ParseQueryString(const std::string& querystr);
  void RebuildUrl();
  std::string buildQueryStr() const;
  std::string UrlEncode(const std::string& str) const;
  std::string UrlDecode(const std::string& str) const;
  std::string toHex(char c) const;
  uint8_t HexToByte(char hex) const;
  void rebuildHeaderIndex();
  
  static const std::unordered_map<std::string, HttpMethod> s_strMethod;
  HttpMethod method_ = HttpMethod::GET;
  int statusCode_ = 200;
  std::string url_;
  std::string path_;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  std::vector<std::pair<std::string,std::string>>headers_; // 保持vector以维持头部顺序
  std::unordered_map<std::string, std::vector<size_t>>headerIndexMap_; // 用于快速查找头部索引
  std::string body_;
  HttpContentEncoding contentEncoding_ = HttpContentEncoding::IDENTITY;
  std::map<std::string,std::vector<std::string>> queryParams_;


};
