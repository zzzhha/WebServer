#include"core/HttpRequest.h"

const std::unordered_map<std::string,HttpMethod> HttpRequest::s_strMethod = {
  {"GET",HttpMethod::GET},
  {"POST",HttpMethod::POST},
  {"PUT",HttpMethod::PUT},
  {"DELETE",HttpMethod::DELETE},
  {"PATCH",HttpMethod::PATCH},
  {"HEAD",HttpMethod::HEAD},
  {"OPTIONS",HttpMethod::OPTIONS},
  {"TRACE",HttpMethod::TRACE},
  {"CONNECT",HttpMethod::CONNECT}
};

HttpRequest::HttpRequest(const HttpRequest& other) 
    :version_(other.version_),
    headers_(other.headers_),
    headerIndexMap_(other.headerIndexMap_),
    body_(other.body_),
    contentEncoding_(other.contentEncoding_),
    method_(other.method_),
    url_(other.url_),
    path_(other.path_),
    queryParams_(other.queryParams_) {}


HttpRequest::HttpRequest(HttpRequest && other) noexcept 
  :version_(other.version_),
  headers_(std::move(other.headers_)),
  headerIndexMap_(std::move(other.headerIndexMap_)),
  body_(std::move(other.body_)),
  contentEncoding_(other.contentEncoding_),
  method_(other.method_),
  url_(std::move(other.url_)),
  path_(std::move(other.path_)),
  queryParams_(std::move(other.queryParams_)) {
  other.version_ = HttpVersion::HTTP_1_1;
  other.contentEncoding_ = HttpContentEncoding::IDENTITY;
  other.method_ = HttpMethod::GET;
}

HttpRequest& HttpRequest::operator=(const HttpRequest& other) {
  if(&other == this) return *this;
  version_=other.version_;
  headers_=other.headers_;
  headerIndexMap_=other.headerIndexMap_;
  body_=other.body_;
  contentEncoding_=other.contentEncoding_;
  method_=other.method_;
  url_=other.url_;
  path_=other.path_;
  queryParams_=other.queryParams_;

  return *this;
}

HttpRequest& HttpRequest::operator=(HttpRequest&& other) noexcept {
  if(&other == this) return *this;
  version_=other.version_;
  headers_=std::move(other.headers_);
  headerIndexMap_=std::move(other.headerIndexMap_);
  body_=std::move(other.body_);
  contentEncoding_=other.contentEncoding_;
  method_=std::move(other.method_);
  url_=std::move(other.url_);
  path_=std::move(other.path_);
  queryParams_=std::move(other.queryParams_);

  other.version_ = HttpVersion::HTTP_1_1;
  other.contentEncoding_ = HttpContentEncoding::IDENTITY;
  other.method_ = HttpMethod::GET;
  return *this;
}

void HttpRequest::SetHeader(const std::string& key,const std::string& value) {
  std::string normalized_key = normalizeHeaderKey(key);
  // 移除所有相同名称的头部并重建索引，避免 erase 导致的下标漂移
  headerIndexMap_.erase(normalized_key);

  for(auto it = headers_.begin(); it != headers_.end();) {
    if(it->first == normalized_key) {
      it = headers_.erase(it);
    } else {
      ++it;
    }
  }

  headers_.emplace_back(normalized_key, value);
  rebuildHeaderIndex();
}

void HttpRequest::AppendHeader(const std::string& key,const std::string& value) {
  std::string normalized_key = normalizeHeaderKey(key);
  headers_.emplace_back(normalized_key, value);
  headerIndexMap_[normalized_key].push_back(headers_.size() - 1);
}

void HttpRequest::AppendHeader(const std::vector<std::pair<std::string,std::string>>& headers) {
  headers_.reserve(headers_.size()+headers.size());

  for(auto &pair:headers) {
    std::string normalized_key = normalizeHeaderKey(pair.first);
    headers_.emplace_back(normalized_key, pair.second);
    headerIndexMap_[normalized_key].push_back(headers_.size() - 1);
  }
}

std::optional<std::string> HttpRequest::GetHeader(const std::string& key) const {
  std::string normal_key = normalizeHeaderKey(key);
  auto it = headerIndexMap_.find(normal_key);
  if(it != headerIndexMap_.end() && !it->second.empty()) {
    return headers_[it->second[0]].second;
  }
  return std::nullopt;
}

std::vector<std::string> HttpRequest::GetHeaders(const std::string& key) const {
  std::vector<std::string> heads;
  std::string normal_key = normalizeHeaderKey(key);
  
  auto it = headerIndexMap_.find(normal_key);
  if(it != headerIndexMap_.end()) {
    for(size_t index : it->second) {
      heads.emplace_back(headers_[index].second);
    }
  }
  return heads;
}


const std::vector<std::pair<std::string,std::string>>& HttpRequest::GetAllHeaders() const {
  return headers_;
}

bool HttpRequest::HasHeader(const std::string& key) const {
  std::string normal_key = normalizeHeaderKey(key);
  return headerIndexMap_.find(normal_key) != headerIndexMap_.end();
}

void HttpRequest::RemoveHeader(const std::string& key) {
  std::string normal_key = normalizeHeaderKey(key);
  headerIndexMap_.erase(normal_key);

  // 从headers_中移除所有相同名称的头部
  for(auto it = headers_.begin(); it != headers_.end();) {
    if(it->first == normal_key) {
      it = headers_.erase(it);
    } else {
      ++it;
    }
  }
  rebuildHeaderIndex();
}

//void HttpRequest::SetBinaryBody(const char* binary, size_t length);

//const HttpRequest::char* GetBinaryBody(size_t& length) const;

std::string HttpRequest::GetContentEncodingStr() const {
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
void HttpRequest::SetMethodString(std::string_view method) {
  if (method.empty()) {
    method_ = HttpMethod::UNKNOWN;
    return;
  }

  std::string tmpmethod;
  for(auto &ch : method) tmpmethod.push_back(std::toupper(ch));
  auto it = s_strMethod.find(tmpmethod);
  if(it != s_strMethod.end()) method_ = it->second;
  else method_ = HttpMethod::UNKNOWN;
}

void HttpRequest::SetRequestLine(std::string_view method,std::string_view url,HttpVersion version) {
  SetMethodString(method);
  SetUrl(url);
  SetVersion(version);
}

void HttpRequest::AppendBodyChunk(const char* data,size_t len) {
  body_.append(data,len);
}

void HttpRequest::Clear() {
  headers_.clear();
  headerIndexMap_.clear();
  body_.clear();
  url_.clear();
  path_ = "/";
  queryParams_.clear();
  method_ = HttpMethod::GET;
  version_ = HttpVersion::HTTP_1_1;
  contentEncoding_ = HttpContentEncoding::IDENTITY;
}

void HttpRequest::ClearHeaders() {
  headers_.clear();
  headerIndexMap_.clear();
}

void HttpRequest::rebuildHeaderIndex() {
  headerIndexMap_.clear();
  for(size_t i = 0; i < headers_.size(); ++i) {
    headerIndexMap_[headers_[i].first].push_back(i);
  }
}

std::string HttpRequest::Serialize() const {
  std::string result = GetMethodString() + " " + url_ + " " + GetVersionStr() + "\r\n";

  for(const auto& pair : headers_) {
    result += pair.first + ": " + pair.second + "\r\n";
  }

  result += "\r\n";
  
  result += body_;

  return result;
}

std::string HttpRequest::GetVersionStr() const {
  switch(version_){
    case HttpVersion::HTTP_1_0: return "HTTP/1.0";
    case HttpVersion::HTTP_1_1: return "HTTP/1.1";
    case HttpVersion::HTTP_2: return "HTTP/2";
    case HttpVersion::HTTP_3: return "HTTP/3";
    default: return "HTTP/1.1";
  }
}

std::string HttpRequest::GetMethodString() const {
  switch(method_){
    case HttpMethod::GET     : return "GET";
    case HttpMethod::POST    : return "POST";
    case HttpMethod::PUT     : return "PUT";
    case HttpMethod::DELETE  : return "DELETE";
    case HttpMethod::PATCH   : return "PATCH";
    case HttpMethod::HEAD    : return "HEAD";
    case HttpMethod::OPTIONS : return "OPTIONS";
    case HttpMethod::TRACE   : return "TRACE";
    case HttpMethod::CONNECT : return "CONNECT";
    default                  : return "UNKNOWN";
  }
}

void HttpRequest::AddQueryParam(const std::string& key, const std::string& value) {
  queryParams_[key].emplace_back(value);
  RebuildUrl();
}

void HttpRequest::SetQueryParam(const std::string& key, const std::string& value) {
  queryParams_[key].clear();
  queryParams_[key].emplace_back(value);
  RebuildUrl();
}

std::string HttpRequest::GetQueryParam(const std::string& key) const {
  auto it = queryParams_.find(key);
  if(it != queryParams_.end()) {
    if(it->second.empty()) {
      return "";
    } else {
      return it -> second[0];
    }
  } else {
    return "";
  }
}

std::vector<std::string> HttpRequest::GetQueryParams(const std::string& key) const {
  auto it = queryParams_.find(key);
    if (it != queryParams_.end()) {
        return it->second;
    }
    return {};
}

bool HttpRequest::RemoveQueryParam(const std::string& key) {
  if(queryParams_.find(key) != queryParams_.end()) {
    queryParams_.erase(queryParams_.find(key));
    RebuildUrl();
    return true;
  } 
  return false;
}

std::string HttpRequest::normalizeHeaderKey(const std::string& key) const {
  std::string normalized = key;
  bool capitalizeNext = true;
  for(char &c : normalized) {
    unsigned char uc = static_cast<unsigned char>(c);
    if(capitalizeNext && std::isalpha(uc)) {
      c              = static_cast<char>(std::toupper(uc));
      capitalizeNext = false;
    } else if (c == '-') {
      capitalizeNext = true;
    } else {
      c = static_cast<char>(std::tolower(uc));
    }
  }
  return normalized;
}

void HttpRequest::ParseUrl() {
  if(url_.empty()){
    path_ = "/";
    queryParams_.clear();
    return;
  }

  std::string tmp = url_;
  std::string path, query, fragment;
 
  size_t frag_end = tmp.find('#');
  if(frag_end != std::string::npos) {
    fragment = tmp.substr(frag_end + 1);
    tmp = tmp.substr(0,frag_end);
  }

  size_t query_str = tmp.find('?');
  if(query_str != std::string::npos) {
    query = tmp.substr(query_str + 1);
    path = tmp.substr(0, query_str);
  } else {
    path = tmp;
  }

  if(path.empty()) {
    path = "/";
  }
  path_ = path;

  queryParams_.clear();
  if(!query.empty()) {
    ParseQueryString(query);
  }
}

void HttpRequest::ParseQueryString(const std::string& querystr) {
  if(querystr.empty()) return;

  size_t pos = 0;
  while(pos < querystr.length()) {
    size_t amp_pos = querystr.find('&', pos);
    std::string param_pair;

    if(amp_pos == std::string::npos) {
      param_pair = querystr.substr(pos);
      pos = querystr.length();
    } else {
      param_pair = querystr.substr(pos, amp_pos - pos);
      pos = amp_pos + 1;
    }

    size_t eq_pos =param_pair.find('=');
    std::string key,value;

    if(eq_pos == std::string::npos) {
      key = UrlDecode(param_pair);
    } else {
      key = UrlDecode(param_pair.substr(0,eq_pos));
      value = UrlDecode(param_pair.substr(eq_pos + 1));
    }

    queryParams_[key].push_back(value);
  }
}

void HttpRequest::RebuildUrl() {
  std::string new_url = path_;

  std::string query = buildQueryStr();
  if(!query.empty()) {
    new_url += "?" + query;
  }

  url_ = new_url;
}

std::string HttpRequest::buildQueryStr() const {
  if(queryParams_.empty()) return "";

  std::string query;
  bool first = true;

  for(const auto& pair : queryParams_) {
    for(const auto& value : pair.second) {
      if(!first) {
        query += "&";
      }
      query += UrlEncode(pair.first);
      if(!value.empty()) {
        query += "=" + UrlEncode(value);
      }
      first = false;
    }
  }

  return query;
}

std::string HttpRequest::UrlEncode(const std::string& str) const {
  std::string encoded;
  
  for(unsigned char c : str) {
    if(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      // 安全字符保持不变
      encoded += c;
    } else if(c == ' ') {
      // 空格编码为+
      encoded += '+';
    } else {
      encoded += '%';
      encoded += toHex((c >> 4) & 0x0F);
      encoded += toHex(c & 0x0F);
    }
  }
  return encoded;
}

std::string HttpRequest::UrlDecode(const std::string& str) const {
  std::string result;
  int i = 0, len = str.length();

  while (i < len) {
      if (str[i] == '+') {
          result += ' ';
          i++;
      } else if (str[i] == '%' && i + 2 < len) {
          uint8_t high = HexToByte(str[i + 1]);
          uint8_t low = HexToByte(str[i + 2]);
          result += static_cast<char>((high << 4) | low);
          i += 3;
      } else {
          result += str[i];
          i++;
      }
  }
  return result;
}

std::string HttpRequest::toHex(char c) const {
  if(c < 0 || c > 15) return "0";
  if(c < 10) return std::string(1, '0'+c);
  return std::string(1, 'A' + c - 10);
}

void HttpRequest::SetMethod(HttpMethod method) {
  method_ = method;
}

void HttpRequest::SetPath(std::string_view path) {
  path_ = path;
  RebuildUrl();
}

void HttpRequest::ClearQueryParams() {
  queryParams_.clear();
  RebuildUrl();
}

void HttpRequest::SetUrl(std::string_view url) {
  url_ = url;
  ParseUrl();
}

uint8_t HttpRequest::HexToByte(char hex) const {
  if(hex >= '0' && hex <= '9') return hex - '0';
  else if(hex >= 'A' && hex <= 'F') return hex - 'A' + 10;
  else if(hex >= 'a' && hex <= 'f') return hex - 'a' + 10;
  return 0;
}