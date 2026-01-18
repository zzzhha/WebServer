#pragma once
#include<string>
#include<map>
#include<unordered_map>
#include<memory>
#include<vector>
#include<optional>

  enum class HttpVersion{
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2,
    HTTP_3,
  };

enum class HttpContentEncoding {
    IDENTITY,  // 无编码（默认）
    GZIP,      // gzip 压缩
    DEFLATE,   // deflate 压缩
    BR,        // Brotli 压缩
    ZSTD       // Zstandard 压缩
};

class IHttpMessage{
public:
  enum class MessageType {
    UnKnown,
    Request,
    Response
  };

  virtual ~IHttpMessage() = default;
  
  //http头的操作
  virtual void SetHeader(const std::string& key,const std::string& value) = 0;
  virtual void AppendHeader(const std::string& key,const std::string& value) = 0;
  virtual void AppendHeader(const std::vector<std::pair<std::string,std::string>>& headers) = 0;
  virtual std::optional<std::string> GetHeader(const std::string& key) const = 0;
  virtual std::vector<std::string> GetHeaders(const std::string& key) const = 0;
  virtual bool HasHeader(const std::string& key)const = 0;
  virtual const std::vector<std::pair<std::string,std::string>>& GetAllHeaders() const = 0;
  virtual void RemoveHeader(const std::string& key) = 0;
  virtual void ClearHeaders() = 0;

  //http版本操作
  virtual void SetVersion(HttpVersion version) = 0;
  virtual HttpVersion GetVersion() const = 0;
  virtual std::string GetVersionStr() const = 0;

  //http body操作
  virtual void SetBody(const std::string& body) = 0;
  //virtual void SetBinaryBody(const char* binary, size_t length) = 0;
  virtual std::string GetBody() const = 0;
  //virtual const char* GetBinaryBody(size_t& length) const = 0;
  virtual size_t GetBodyLength() const = 0;
  virtual void ClearBody() = 0;


  virtual void SetContentEncoding(HttpContentEncoding encoding) = 0;
  virtual HttpContentEncoding GetContentEncoding() const = 0;
  virtual std::string GetContentEncodingStr() const = 0;
  
  virtual void Clear() = 0;
  virtual std::string Serialize() const = 0;

  virtual void SetMethodString(std::string_view method) {
    (void)method;
  }

  virtual void SetStatusCodeInt(int code) {
    (void)code;
  }

  virtual int GetStatusCodeInt() const {
    return 0;
  }

  virtual void SetStatusReason(std::string_view reason) {
    (void)reason;
  }

  virtual void SetUrl(std::string_view url) {
    (void)url;
  }

  virtual void SetRequestLine(std::string_view method, std::string_view url,HttpVersion version){
    SetMethodString(method);
    SetUrl(url);
    SetVersion(version);
  }

  virtual void SetStatusLine(HttpVersion version,int status_code,std::string_view reason) {
    SetVersion(version);
    SetStatusCodeInt(status_code);
    SetStatusReason(reason);  
  }

  virtual void AppendRawHeaderLine(std::string_view key_header_value) {
    size_t colon = key_header_value.find(':');
    if(colon != std::string_view::npos) {
      std::string key(key_header_value.substr(0,colon));
      std::string value;
      if(colon + 1 < key_header_value.size()) {
        size_t start = colon + 1;
        while(start < key_header_value.size() &&
             (key_header_value[start]==' ' || key_header_value[start]=='\t')) {
          ++start;
        }
        value = std::string(key_header_value.substr(start));
      }
      AppendHeader(key,value);
    }
  }

  virtual void AppendBodyChunk(const char* data,size_t len) {
    std::string chunk(data,len);
    std::string current = GetBody();
    current.append(chunk);
    SetBody(std::move(current));
  }

  virtual bool IsRequest() const { return false; }
  virtual bool IsResponse() const { return true; }
  virtual MessageType GetMessageType() const {
    if(IsRequest()) return MessageType::Request;
    if(IsResponse()) return MessageType::Response;
    return MessageType::UnKnown;
  }

};