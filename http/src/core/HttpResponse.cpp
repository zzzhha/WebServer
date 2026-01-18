#include"core/HttpResponse.h"
#include <cctype>

HttpResponse::HttpResponse() : 
    version_(HttpVersion::HTTP_1_1), 
    statusCode_(HttpStatusCode::OK), 
    statusReason_("OK"), 
    contentEncoding_(HttpContentEncoding::IDENTITY) {
}

HttpResponse::HttpResponse(HttpStatusCode statuscode) : 
    version_(HttpVersion::HTTP_1_1), 
    statusCode_(statuscode), 
    statusReason_(GetDefaultReason(statuscode)), 
    contentEncoding_(HttpContentEncoding::IDENTITY) {
}

void HttpResponse::SetHeader(const std::string& key,const std::string& value) {
  std::string normalized_key = normalizeHeaderKey(key);
  for(auto it = headers_.begin(); it != headers_.end();) {
    if(it->first == normalized_key) {
      it = headers_.erase(it);
    } else {
      ++it;
    }
  }
  headers_.emplace_back(normalized_key,value);
}

void HttpResponse::AppendHeader(const std::string& key,const std::string& value) {
  headers_.emplace_back(normalizeHeaderKey(key),value);
}

void HttpResponse::AppendHeader(const std::vector<std::pair<std::string,std::string>>& headers) {
  headers_.reserve(headers_.size()+headers.size());

  for(auto &pair:headers) {
    headers_.emplace_back(normalizeHeaderKey(pair.first),pair.second);
  }
}

std::optional<std::string> HttpResponse::GetHeader(const std::string& key) const {
  std::string normal_key = normalizeHeaderKey(key);
  for(auto & pair : headers_) {
    if (pair.first == normal_key) {
      return pair.second;
    }
  }
  return std::nullopt;
}

std::vector<std::string> HttpResponse::GetHeaders(const std::string& key) const {
  std::vector<std::string> heads;
  std::string normal_key = normalizeHeaderKey(key);
   for(auto & pair : headers_) {
    if (pair.first == normal_key) {
      heads.emplace_back(pair.second);
    }
  }
  return heads;
}

bool HttpResponse::HasHeader(const std::string& key)const {
  std::string normal_key = normalizeHeaderKey(key);
  for(auto & pair : headers_) {
    if(pair.first == normal_key) return true;
  }
  return false;
}

const std::vector<std::pair<std::string,std::string>>& HttpResponse::GetAllHeaders()const {
  return headers_;
}

void HttpResponse::RemoveHeader(const std::string& key) {
  std::string normal_key = normalizeHeaderKey(key);
  for(auto it = headers_.begin(); it != headers_.end();) {
    if(it->first == normal_key) it = headers_.erase(it);
    else ++it;
  }
}

void HttpResponse::ClearHeaders() {
  headers_.clear();
}

//http版本操作
void HttpResponse::SetVersion(HttpVersion version) {
  version_ = version;
}

HttpVersion HttpResponse::GetVersion() const {
  return version_;
}

std::string HttpResponse::GetVersionStr() const {
  switch(version_){
    case HttpVersion::HTTP_1_0: return "HTTP/1.0";
    case HttpVersion::HTTP_1_1: return "HTTP/1.1";
    case HttpVersion::HTTP_2: return "HTTP/2";
    case HttpVersion::HTTP_3: return "HTTP/3";
    default: return "HTTP/1.1";
  }
}

//void SetBinaryBody(const char* binary, size_t length) override;
//const char* GetBinaryBody(size_t& length) const override;

//http编码操作
void HttpResponse::SetStatusCode(HttpStatusCode statuscode) {
  statusCode_ = statuscode;
  if (statusReason_.empty()) {
    statusReason_ = GetDefaultReason(statuscode);
  }
}

void HttpResponse::Clear() {
  version_ = HttpVersion::HTTP_1_1;
  statusCode_ = HttpStatusCode::OK;
  statusReason_ = "OK";
  headers_.clear();
  body_.clear();
  contentEncoding_ = HttpContentEncoding::IDENTITY;
}

std::string HttpResponse::GetContentEncodingStr() const {
  switch(contentEncoding_) {
    case HttpContentEncoding::IDENTITY : return "identity";
    case HttpContentEncoding::GZIP : return "gzip";
    case HttpContentEncoding::DEFLATE : return "deflate";
    case HttpContentEncoding::BR : return "br";
    case HttpContentEncoding::ZSTD : return "zstd";
    default : return "";
  }
}

//parser接口
void HttpResponse::SetStatusCodeInt(int code) {
  switch(code) {
    case 100: statusCode_ = HttpStatusCode::CONTINUE; break;
    case 101:  statusCode_= HttpStatusCode::SWITCHING_PROTOCOLS; break;

    case 200: statusCode_ = HttpStatusCode::OK; break;
    case 201: statusCode_ = HttpStatusCode::CREATED; break;
    case 202: statusCode_ = HttpStatusCode::ACCEPTED; break;
    case 204: statusCode_ = HttpStatusCode::NO_CONTENT; break;
    case 206: statusCode_ = HttpStatusCode::PARTIAL_CONTENT; break;
    
    case 301: statusCode_ = HttpStatusCode::MOVED_PERMANENTLY; break;
    case 302: statusCode_ = HttpStatusCode::FOUND; break;
    case 303: statusCode_ = HttpStatusCode::SEE_OTHER; break;
    case 304: statusCode_ = HttpStatusCode::NOT_MODIFIED; break;
    case 307: statusCode_ = HttpStatusCode::TEMPORARY_REDIRECT; break;
    case 308: statusCode_ = HttpStatusCode::PERMANENT_REDIRECT; break;
    
    case 400: statusCode_ = HttpStatusCode::BAD_REQUEST; break;
    case 401: statusCode_ = HttpStatusCode::UNAUTHORIZED; break;
    case 403: statusCode_ = HttpStatusCode::FORBIDDEN; break;
    case 404: statusCode_ = HttpStatusCode::NOT_FOUND; break;
    case 405: statusCode_ = HttpStatusCode::METHOD_NOT_ALLOWED; break;
    case 408: statusCode_ = HttpStatusCode::REQUEST_TIMEOUT; break;
    case 413: statusCode_ = HttpStatusCode::PAYLOAD_TOO_LARGE; break;
    case 414: statusCode_ = HttpStatusCode::URI_TOO_LONG; break;
    case 415: statusCode_ = HttpStatusCode::UNSUPPORTED_MEDIA_TYPE; break;
    
    case 500: statusCode_ = HttpStatusCode::INTERNAL_SERVER_ERROR; break;
    case 501: statusCode_ = HttpStatusCode::NOT_IMPLEMENTED; break;
    case 502: statusCode_ = HttpStatusCode::BAD_GATEWAY; break;
    case 503: statusCode_ = HttpStatusCode::SERVICE_UNAVAILABLE; break;
    case 504: statusCode_ = HttpStatusCode::GATEWAY_TIMEOUT; break;
    case 505: statusCode_ = HttpStatusCode::HTTP_VERSION_NOT_SUPPORTED; break;

    default : statusCode_ = HttpStatusCode::NOT_FOUND; break;
  }
}

std::string HttpResponse::Serialize() const {
  std::string result = GetVersionStr() + " " + std::to_string(static_cast<int>(statusCode_)) + " " + statusReason_ + "\r\n";

  for(auto &pair : headers_) {
    result += pair.first + ": " + pair.second + "\r\n";
  }

  result += "\r\n";

  result += body_;

  return result;
}

void HttpResponse::SetStatusReason(std::string_view reason) {
  statusReason_ = reason;
}

void HttpResponse::SetStatusLine(HttpVersion version,int status_code,std::string_view reason) {
  version_ = version;
  SetStatusCodeInt(status_code);
  statusReason_ = reason;
}

void HttpResponse::AppendRawHeaderLine(std::string_view line) {
  size_t pos = line.find(':');
  if(pos == std::string_view::npos) return;

  std::string_view key_view = line.substr(0, pos);
  std::string_view value_view = line.substr(pos + 1);

  std::string normalized_key = normalizeHeaderKey(trim(key_view));
  std::string value_str(trim(value_view));

  headers_.emplace_back(std::move(normalized_key),std::move(value_str));
}

void HttpResponse::AppendBodyChunk(const char* data,size_t len) {
  if(!data || !len) return;

  body_.append(data,len);
}

std::string HttpResponse::GetDefaultReason(HttpStatusCode statusCode) {
  switch (statusCode) {
    case HttpStatusCode::CONTINUE: return "Continue";
    case HttpStatusCode::SWITCHING_PROTOCOLS: return "Switching Protocols";
    case HttpStatusCode::OK: return "OK";
    case HttpStatusCode::CREATED: return "Created";
    case HttpStatusCode::ACCEPTED: return "Accepted";
    case HttpStatusCode::NO_CONTENT: return "No Content";
    case HttpStatusCode::PARTIAL_CONTENT: return "Partial Content";
    case HttpStatusCode::MOVED_PERMANENTLY: return "Moved Permanently";
    case HttpStatusCode::FOUND: return "Found";
    case HttpStatusCode::SEE_OTHER: return "See Other";
    case HttpStatusCode::NOT_MODIFIED: return "Not Modified";
    case HttpStatusCode::TEMPORARY_REDIRECT: return "Temporary Redirect";
    case HttpStatusCode::PERMANENT_REDIRECT: return "Permanent Redirect";
    case HttpStatusCode::BAD_REQUEST: return "Bad Request";
    case HttpStatusCode::UNAUTHORIZED: return "Unauthorized";
    case HttpStatusCode::FORBIDDEN: return "Forbidden";
    case HttpStatusCode::NOT_FOUND: return "Not Found";
    case HttpStatusCode::METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case HttpStatusCode::REQUEST_TIMEOUT: return "Request Timeout";
    case HttpStatusCode::PAYLOAD_TOO_LARGE: return "Payload Too Large";
    case HttpStatusCode::URI_TOO_LONG: return "URI Too Long";
    case HttpStatusCode::UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
    case HttpStatusCode::INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case HttpStatusCode::NOT_IMPLEMENTED: return "Not Implemented";
    case HttpStatusCode::BAD_GATEWAY: return "Bad Gateway";
    case HttpStatusCode::SERVICE_UNAVAILABLE: return "Service Unavailable";
    case HttpStatusCode::GATEWAY_TIMEOUT: return "Gateway Timeout";
    case HttpStatusCode::HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    default: return "Unknown Status";
  }
}

std::string HttpResponse::trim(std::string_view& view) {
  size_t key_start = 0;
  while(key_start < view.length() && std::isspace(static_cast<char>(view[key_start]))) {
    ++key_start;
  } 

  size_t key_end = view.length();
  while(key_end > key_start && std::isspace(static_cast<char>(view[key_end - 1]))) {
    --key_end;
  }

  return std::string(view.substr(key_start, key_end - key_start));
}

std::string HttpResponse::normalizeHeaderKey(const std::string& key) const {
  std::string normalized = key;
  bool capitalizeNext = true;
  for(char &c : normalized) {
    if(capitalizeNext && std::isalpha(c)) {
      c              = std::toupper(c);
      capitalizeNext = false;
    } else if (c == '-') {
      capitalizeNext = true;
    } else {
      c = std::tolower(c);
    }
  }
  return normalized;
}