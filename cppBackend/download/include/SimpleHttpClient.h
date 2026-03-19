#pragma once

/**
 * 模块: SimpleHttpClient
 * 作用: 提供最小化的 HTTP 客户端能力，用于 HEAD 与带 Range 的 GET 请求
 * 说明: 为避免引入第三方库，采用简单的套接字实现与基础解析，满足下载模块需求
 */
#include <cstdint>
#include <map>
#include <optional>
#include <string>

/**
 * HTTP 响应数据
 * status_code: 状态码（如 200/206/304/416 等）
 * reason: 原因短语
 * headers: 响应头键值对（大小写不敏感的处理需在实现中统一）
 * body: 响应体内容（HEAD 请求为空；Range GET 为指定字节段）
 */
struct HttpResponseData {
  int status_code{0};
  std::string reason;
  std::map<std::string, std::string> headers;
  std::string body;
};

/**
 * SimpleHttpClient
 * 支持:
 * - Head: 获取资源元信息（Content-Length、ETag、Last-Modified 等）
 * - GetRange: 按字节段下载（Content-Range/Accept-Ranges）
 */
class SimpleHttpClient {
 public:
  /**
 * 发送 HEAD 请求
 * host/port/path: 目标主机、端口与路径
 * timeout_ms: 超时毫秒
 * out: 返回响应数据（无 body）
 * error: 若失败，填充可读错误信息
 * 返回: 成功/失败
 */
static bool Head(const std::string& host, uint16_t port, const std::string& path, int timeout_ms,
                   HttpResponseData& out, std::string& error);

  /**
 * 发送带 Range 的 GET 请求
 * start/end: 字节范围闭区间 [start, end]
 * 返回: 成功/失败；成功时 out.body 为区间数据，需检查 206 状态码
 */
static bool GetRange(const std::string& host, uint16_t port, const std::string& path, uint64_t start, uint64_t end,
                       int timeout_ms, HttpResponseData& out, std::string& error);
};

