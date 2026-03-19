#include "SimpleHttpClient.h"

/*
 * 简述: 轻量级 HTTP/1.1 客户端实现
 * - 通过 getaddrinfo/connect 建立 TCP 连接，设置收发超时
 * - 仅构造必要的请求头（HEAD/GET Range），读取并解析响应头与可选的固定长度 body
 * - 超时与错误通过返回值与 error 字符串反馈
 */
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static bool SetTimeouts(int fd, int timeout_ms) {
  timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
  if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
  return true;
}

static bool ConnectTcp(const std::string& host, uint16_t port, int timeout_ms, int& out_fd, std::string& error) {
  out_fd = -1;

  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;

  addrinfo* res = nullptr;
  std::string port_str = std::to_string(port);
  int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0 || !res) {
    error = "getaddrinfo failed";
    return false;
  }

  for (addrinfo* p = res; p; p = p->ai_next) {
    int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (!SetTimeouts(fd, timeout_ms)) {
      ::close(fd);
      continue;
    }
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      ::freeaddrinfo(res);
      out_fd = fd;
      return true;
    }
    ::close(fd);
  }

  ::freeaddrinfo(res);
  error = "connect failed";
  return false;
}

static bool SendAll(int fd, const std::string& data, std::string& error) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      error = "timeout";
      return false;
    }
    error = "send failed";
    return false;
  }
  return true;
}

static bool RecvExact(int fd, size_t bytes, std::string& out, std::string& error) {
  out.clear();
  out.reserve(bytes);
  while (out.size() < bytes) {
    char buf[8192];
    size_t want = bytes - out.size();
    size_t cap = want < sizeof(buf) ? want : sizeof(buf);
    ssize_t n = ::recv(fd, buf, cap, 0);
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      error = "timeout";
      return false;
    }
    if (n == 0) {
      error = "connection closed";
      return false;
    }
    error = "recv failed";
    return false;
  }
  return true;
}

static bool ReadUntilHeadersDone(int fd, std::string& header_and_maybe_body, std::string& error) {
  header_and_maybe_body.clear();
  char buf[4096];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      header_and_maybe_body.append(buf, static_cast<size_t>(n));
      if (header_and_maybe_body.find("\r\n\r\n") != std::string::npos) return true;
      if (header_and_maybe_body.size() > 1024 * 1024) {
        error = "headers too large";
        return false;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      error = "timeout";
      return false;
    }
    if (n == 0) {
      error = "connection closed";
      return false;
    }
    error = "recv failed";
    return false;
  }
}

static std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

static std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

static bool ParseHeaders(const std::string& raw, HttpResponseData& out, size_t& body_off, std::string& error) {
  size_t pos = raw.find("\r\n\r\n");
  if (pos == std::string::npos) {
    error = "header not complete";
    return false;
  }
  body_off = pos + 4;

  size_t line_end = raw.find("\r\n");
  if (line_end == std::string::npos) {
    error = "bad status line";
    return false;
  }
  std::string status_line = raw.substr(0, line_end);
  size_t sp1 = status_line.find(' ');
  if (sp1 == std::string::npos) {
    error = "bad status line";
    return false;
  }
  size_t sp2 = status_line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) sp2 = status_line.size();
  std::string code_str = status_line.substr(sp1 + 1, sp2 - (sp1 + 1));
  out.status_code = std::atoi(code_str.c_str());
  out.reason = sp2 < status_line.size() ? status_line.substr(sp2 + 1) : "";

  out.headers.clear();
  size_t cur = line_end + 2;
  while (cur < pos) {
    size_t next = raw.find("\r\n", cur);
    if (next == std::string::npos || next > pos) break;
    std::string line = raw.substr(cur, next - cur);
    cur = next + 2;
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = ToLower(Trim(line.substr(0, colon)));
    std::string val = Trim(line.substr(colon + 1));
    out.headers[key] = val;
  }
  return true;
}

static bool ReadHttpResponse(int fd, bool want_body, HttpResponseData& out, std::string& error) {
  std::string first;
  if (!ReadUntilHeadersDone(fd, first, error)) return false;

  size_t body_off = 0;
  if (!ParseHeaders(first, out, body_off, error)) return false;

  std::string body_prefix = first.substr(body_off);

  out.body.clear();
  if (!want_body) return true;

  size_t content_len = 0;
  auto it = out.headers.find("content-length");
  if (it != out.headers.end()) {
    content_len = static_cast<size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
  }

  if (body_prefix.size() > content_len) body_prefix.resize(content_len);
  out.body = body_prefix;

  if (out.body.size() < content_len) {
    std::string rest;
    if (!RecvExact(fd, content_len - out.body.size(), rest, error)) return false;
    out.body += rest;
  }
  return true;
}

bool SimpleHttpClient::Head(const std::string& host, uint16_t port, const std::string& path, int timeout_ms,
                            HttpResponseData& out, std::string& error) {
  int fd = -1;
  if (!ConnectTcp(host, port, timeout_ms, fd, error)) return false;

  std::string req = "HEAD " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Connection: close\r\n";
  req += "X-Request-MD5: 1\r\n";
  req += "\r\n";

  bool ok = SendAll(fd, req, error) && ReadHttpResponse(fd, false, out, error);
  ::close(fd);
  return ok;
}

bool SimpleHttpClient::GetRange(const std::string& host, uint16_t port, const std::string& path, uint64_t start,
                                uint64_t end, int timeout_ms, HttpResponseData& out, std::string& error) {
  int fd = -1;
  if (!ConnectTcp(host, port, timeout_ms, fd, error)) return false;

  std::string req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Connection: close\r\n";
  req += "Range: bytes=" + std::to_string(start) + "-" + std::to_string(end) + "\r\n";
  req += "\r\n";

  bool ok = SendAll(fd, req, error) && ReadHttpResponse(fd, true, out, error);
  ::close(fd);
  return ok;
}
