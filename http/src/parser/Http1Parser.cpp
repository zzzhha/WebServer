#include "parsers/Http1Parser.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <cctype>
#include <charconv>
#include <sstream>

namespace {
// HTTP/1.x 响应行的固定前缀
constexpr std::string_view kHttpPrefix = "HTTP/";

// 大小写不敏感字符串比较，供头部/编码判断使用
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}
} // namespace

Http1Parser::Http1Parser() = default;

int Http1Parser::Parse(std::string& data, std::unique_ptr<IHttpMessage>& out) {
  // 主入口：基于状态机按行/按块消费数据，可能返回 NEEDMOREDATA 让调用方补充更多字节
  if (data.empty()) return static_cast<int>(ParseResult::NEEDMOREDATA);
  size_t consumed = 0;
  ParseResult res = ParseResult::NEEDMOREDATA;

  while (consumed < data.size()) {
    switch (state_) {
      case ParseState::kStartLine:
      case ParseState::kHeaders:
      case ParseState::kBodyChunkedSize: {
        // 这些状态按行解析
        size_t lineEnd = data.find("\r\n", consumed);
        if (lineEnd == std::string::npos) {
          // 不完整行，缓存后返回 NEEDMOREDATA
          lineBuffer_.append(data.begin() + consumed, data.end());
          consumed = data.size();
          res = ParseResult::NEEDMOREDATA;
          break;
        }

        // 取出一行（包含可能之前缓存的残留）
        lineBuffer_.append(data.begin() + consumed, data.begin() + lineEnd);
        std::string line = std::move(lineBuffer_);
        lineBuffer_.clear();
        consumed = lineEnd + 2; // 跳过 \r\n

        if (state_ == ParseState::kStartLine) {
          res = ParseStartLine(line);
          if (res != ParseResult::SUCCESS) {
            data.erase(0, consumed);
            totalConsumed_ += consumed;
            return static_cast<int>(res);
          }
          state_ = ParseState::kHeaders;
          continue;
        }

        if (state_ == ParseState::kHeaders) {
          if (line.empty()) {
            // 头结束，决定 body 解析方式
            auto te = currentMessage_->GetHeader("Transfer-Encoding");
            if (te && iequals(*te, "chunked")) {
              isChunked_ = true;
              state_ = ParseState::kBodyChunkedSize;
              res = ParseResult::NEEDMOREDATA;
              continue;
            }

            auto cl = currentMessage_->GetHeader("Content-Length");
            if (cl) {
              contentLength_ = std::stoull(*cl);
              bodyReceived_ = 0;
              if (contentLength_ > 0) {
                state_ = ParseState::kBodyContentLength;
                res = ParseResult::NEEDMOREDATA;
                continue;
              }
            }
            res = FinalizeMessage(out);
            break;
          }

          if (line.size() > maxHeaderLineSize_) {
            data.erase(0, consumed);
            totalConsumed_ += consumed;
            return static_cast<int>(ParseResult::HEADERTOOLONG);
          }

          if (++headerCount_ > maxHeaderCount_) {
            data.erase(0, consumed);
            totalConsumed_ += consumed;
            return static_cast<int>(ParseResult::HEADERTOOLONG);
          }

          res = ParseHeaderLine(line);
          if (res != ParseResult::SUCCESS) {
            data.erase(0, consumed);
            totalConsumed_ += consumed;
            return static_cast<int>(res);
          }
          continue;
        }

        if (state_ == ParseState::kBodyChunkedSize) {
          res = ParseChunkSize(line);
          if (res != ParseResult::SUCCESS) {
            data.erase(0, consumed);
            totalConsumed_ += consumed;
            return static_cast<int>(res);
          }
          if (chunkSize_ == 0) {
            state_ = ParseState::kBodyChunkedEnd;
          } else {
            state_ = ParseState::kBodyChunkedData;
          }
          continue;
        }
        break;
      }

      case ParseState::kBodyContentLength: {
        // Content-Length 模式：按需读取 body 字节
        size_t remaining = data.size() - consumed;
        size_t need = contentLength_ - bodyReceived_;
        size_t take = std::min(remaining, need);
        if (take > 0) {
          currentMessage_->AppendBodyChunk(data.data() + consumed, take);
          consumed += take;
          bodyReceived_ += take;
        }
        if (bodyReceived_ >= contentLength_) {
          res = FinalizeMessage(out);
        } else {
          res = ParseResult::NEEDMOREDATA;
        }
        break;
      }

      case ParseState::kBodyChunkedData: {
        // chunked 模式：需要完整 chunk 及其结尾 CRLF
        size_t remaining = data.size() - consumed;
        if (remaining < chunkSize_ + 2) {
          // 需要完整 chunk + CRLF
          res = ParseResult::NEEDMOREDATA;
          consumed = data.size();
          break;
        }
        currentMessage_->AppendBodyChunk(data.data() + consumed, chunkSize_);
        consumed += chunkSize_;
        // 跳过 chunk 末尾的 \r\n
        if (!(data[consumed] == '\r' && data[consumed + 1] == '\n')) {
          data.erase(0, consumed);
          totalConsumed_ += consumed;
          return static_cast<int>(ParseResult::ERROR);
        }
        consumed += 2;
        state_ = ParseState::kBodyChunkedSize;
        res = ParseResult::NEEDMOREDATA;
        break;
      }

      case ParseState::kBodyChunkedEnd: {
        // 期望一个空行结束 chunked（可忽略 trailer）
        size_t lineEnd = data.find("\r\n", consumed);
        if (lineEnd == std::string::npos) {
          lineBuffer_.append(data.begin() + consumed, data.end());
          consumed = data.size();
          res = ParseResult::NEEDMOREDATA;
          break;
        }
        lineBuffer_.append(data.begin() + consumed, data.begin() + lineEnd);
        std::string line = std::move(lineBuffer_);
        lineBuffer_.clear();
        consumed = lineEnd + 2;

        if (!line.empty()) {
          // 简化：忽略 trailer，但要求空行，否则视为错误
          data.erase(0, consumed);
          totalConsumed_ += consumed;
          return static_cast<int>(ParseResult::ERROR);
        }
        res = FinalizeMessage(out);
        break;
      }

      case ParseState::kDone:
        res = ParseResult::SUCCESS;
        break;
    }

    if (res == ParseResult::SUCCESS || res == ParseResult::ERROR ||
        res == ParseResult::INVALIDSTARTLINE || res == ParseResult::INVALIDHEADER ||
        res == ParseResult::HEADERTOOLONG || res == ParseResult::BODYTOOLONG ||
        res == ParseResult::UNSUPPORTEDVERSION) {
      break;
    }
  }

  data.erase(0, consumed);
  totalConsumed_ += consumed;
  return static_cast<int>(res);
}

int Http1Parser::Parse(const char* data, size_t len, std::unique_ptr<IHttpMessage>& out) {
  std::string buf(data, data + len);
  return Parse(buf, out);
}

void Http1Parser::Reset() {
  // 重置状态机与缓存，方便复用同一解析器实例
  state_ = ParseState::kStartLine;
  currentMessage_.reset();
  lineBuffer_.clear();
  contentLength_ = 0;
  chunkSize_ = 0;
  bodyReceived_ = 0;
  isChunked_ = false;
  totalConsumed_ = 0;
  headerCount_ = 0;
}

Http1Parser::~Http1Parser() = default;

ParseResult Http1Parser::ParseStartLine(std::string_view line) {
  // 解析请求/响应起始行，自动生成 HttpRequest 或 HttpResponse
  if (line.empty()) return ParseResult::INVALIDSTARTLINE;

  if (line.rfind(kHttpPrefix, 0) == 0) {
    // Response start line: HTTP/1.1 200 OK
    std::string line_str(line);
    std::istringstream iss(line_str);
    std::string version_str;
    int status = 0;
    std::string reason;
    if (!(iss >> version_str >> status)) {
      return ParseResult::INVALIDSTARTLINE;
    }
    std::getline(iss, reason);
    if (!reason.empty() && reason.front() == ' ') reason.erase(0, 1);

    HttpVersion version = HttpVersion::HTTP_1_1;
    if (version_str == "HTTP/1.0") version = HttpVersion::HTTP_1_0;
    else if (version_str == "HTTP/1.1") version = HttpVersion::HTTP_1_1;
    else return ParseResult::UNSUPPORTEDVERSION;

    currentMessage_ = std::make_unique<HttpResponse>();
    currentMessage_->SetStatusLine(version, status, reason);
    return ParseResult::SUCCESS;
  }

  // Request start line: METHOD SP URL SP HTTP/x.y
  std::string line_str_req(line);
  std::istringstream iss_req(line_str_req);
  std::string method, url, version_str;
  if (!(iss_req >> method >> url >> version_str)) {
    return ParseResult::INVALIDSTARTLINE;
  }
  HttpVersion version = HttpVersion::HTTP_1_1;
  if (version_str == "HTTP/1.0") version = HttpVersion::HTTP_1_0;
  else if (version_str == "HTTP/1.1") version = HttpVersion::HTTP_1_1;
  else return ParseResult::UNSUPPORTEDVERSION;

  currentMessage_ = std::make_unique<HttpRequest>();
  currentMessage_->SetRequestLine(method, url, version);
  return ParseResult::SUCCESS;
}

ParseResult Http1Parser::ParseHeaderLine(std::string_view line) {
  // 解析单个头部行：key:value，必要时做首字符合法性校验
  size_t colon = line.find(':');
  if (colon == std::string_view::npos) return ParseResult::INVALIDHEADER;

  std::string key(line.substr(0, colon));
  std::string value = trimLWS(line.substr(colon + 1));

  if (strictHeaderCheck_ && !isTokenChar(key[0])) {
    return ParseResult::INVALIDHEADER;
  }

  currentMessage_->AppendHeader(key, value);
  return ParseResult::SUCCESS;
}

ParseResult Http1Parser::ParseChunkSize(std::string_view line) {
  // chunk size 是 16 进制，允许紧跟扩展但此处只解析数字部分
  size_t idx = 0;
  while (idx < line.size() && std::isxdigit(static_cast<unsigned char>(line[idx]))) {
    ++idx;
  }
  if (idx == 0) return ParseResult::INVALIDHEADER;

  std::string_view hexPart = line.substr(0, idx);
  unsigned long size = 0;
  auto res = std::from_chars(hexPart.data(), hexPart.data() + hexPart.size(), size, 16);
  if (res.ec != std::errc()) return ParseResult::INVALIDHEADER;

  chunkSize_ = static_cast<size_t>(size);
  bodyReceived_ = 0;
  return ParseResult::SUCCESS;
}

ParseResult Http1Parser::FinalizeMessage(std::unique_ptr<IHttpMessage>& out) {
  // 将已完成的消息移交给调用方，并重置解析器
  out = std::move(currentMessage_);
  Reset();
  return ParseResult::SUCCESS;
}

bool Http1Parser::isTokenChar(char c) {
  // RFC 7230 token
  return std::isalpha(static_cast<unsigned char>(c)) ||
         std::isdigit(static_cast<unsigned char>(c)) ||
         c == '!' || c == '#' || c == '$' || c == '%' ||
         c == '&' || c == '\''|| c == '*' || c == '+' ||
         c == '-' || c == '.' || c == '^' || c == '_' ||
         c == '`' || c == '|' || c == '~';
}

std::string Http1Parser::trimLWS(std::string_view s) {
  // 去除头部值左右的空白 (LWS)
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return std::string(s.substr(start, end - start));
}
