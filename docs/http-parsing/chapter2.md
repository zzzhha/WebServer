# 第2章 HTTP解析入口、状态机与缓冲区

本章描述“网络字节流 → IHttpMessage(请求/响应)”的真实处理链路，涵盖缓冲区、解析器创建与消费推进、解析状态机与关键边界条件。文中涉及实现细节均以源码为准。

## 2.1 从 socket 到 HttpFacade：入口与数据流

### 2.1.1 Connection 收包并触发上层处理

- Connection 在读回调里循环 `read()`，把数据追加到 `BufferBlock inputbuffer_`，读到 `EAGAIN/EWOULDBLOCK` 后触发上层回调（并更新时间轮）。实现见 [Connection::onmessage](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L150-L188)。

### 2.1.2 HttpServer 将 BufferBlock 转为字符串并交给 HttpFacade

- HttpServer 取输入缓冲区 `bufferToString()` 得到 `request_data`，并通过连接上下文拿到“连接级”的 `HttpFacade` 实例，调用 `facade->Process(request_data, message, response, err)`。实现见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L80-L180)。
- 解析成功后，HttpServer 通过 `facade->GetConsumedBytes()` 推进消费偏移：`inputbuffer.consumeBytes(consumed_bytes)`。实现见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L130-L134) 与 [HttpFacade::GetConsumedBytes](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L455-L459)。

### 2.1.3 HttpFacade 负责编排：SSL → 解析 → 校验 → 路由

- 统一入口是 [HttpFacade::Process](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L26-L106)，其阶段划分与调用顺序在该函数中固定。
- 解析阶段由 [HttpFacade::ProcessParsing](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L163-L284) 完成，内部按需创建解析器并处理超时/错误映射。

## 2.2 BufferBlock：分块缓冲与零拷贝写出

项目的 IO 缓冲不是连续 vector，而是“多块内存块 + 读写位置”的结构：

- 定义： [Buffer.h](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.h)
- 实现： [Buffer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp)

关键点：

- `append(const char*, size_t)`：追加数据时，尾块写满则按策略扩容新块（上限 64KB），并维护 `total_size_`。实现见 [BufferBlock::append](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp#L46-L80)。
- `bufferToString()`：把分块数据拼成连续字符串，供 HTTP 解析器使用。其存在是为了适配解析器当前的 `std::string` 输入模型（参见 HttpServer 侧调用）。实现见 [BufferBlock::bufferToString](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp)。
- `consumeBytes(size_t)`：按需跨块推进 `read_pos_`，必要时弹出已读尽的头块。实现见 [BufferBlock::consumeBytes](file:///home/zsy/WebServer/cppBackend/reactor/Buffer.cpp#L130-L149)。
- 写出侧：Connection 使用 `getIOVecs()` 组装 `iovec[]` 并 `writev()`，从而避免额外拼接；文件场景再叠加 `sendfile()`。实现见 [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L80-L148)。

## 2.3 解析器接口与返回码

解析器的统一接口是 [IHttpParser](file:///home/zsy/WebServer/cppBackend/http/include/parsers/IHttpParser.h)。

```cpp
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
```

要点：

- `Parse(...)` 可能返回 `NEEDMOREDATA`，表示当前输入不足以完成一个完整 HTTP 消息，调用方应继续累积字节并重试（HttpServer 的行为见第 7 章）。
- `GetConsumeBytes()` 用于上层“推进输入缓冲区消费”，避免重复解析同一字节。
- 上限控制：`SetMaxHeaderLineSize/SetMaxHeaderCount/setMaxBodySize/setStrictHeaderCheck` 对应 Http/1 解析过程中的 Header/Body 边界策略（默认值由具体实现决定）。

## 2.4 Http1Parser 状态机（真实实现）

Http/1 解析由 [Http1Parser](file:///home/zsy/WebServer/cppBackend/http/include/parsers/Http1Parser.h) 实现，采用内部状态机：

```cpp
  enum class ParseState{
    kStartLine,
    kHeaders,
    kBodyContentLength,
    kBodyChunkedSize,
    kBodyChunkedData,
    kBodyChunkedEnd,
    kDone
  };
```

关键转换逻辑在 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L40-L330)：

- `kStartLine`：按行取 `\r\n`，解析请求行/响应行（支持 `HTTP/1.0` 与 `HTTP/1.1`，否则 `UNSUPPORTEDVERSION`），见 [ParseStartLine](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L355-L397)。
- `kHeaders`：逐行解析 `key:value`。遇到空行后：
  - 若 `Transfer-Encoding: chunked` → `kBodyChunkedSize`；
  - 若 `Content-Length` 且长度 >0 → `kBodyContentLength`；
  - 否则直接 `FinalizeMessage`。
- `kBodyContentLength`：按剩余可用字节数补齐 body，边读边 `AppendBodyChunk`；读满即 `FinalizeMessage`。
- `kBodyChunkedSize`/`kBodyChunkedData`：解析 chunk size（十六进制数字部分），再读取完整 chunk + 末尾 `\r\n`；最后 size==0 转入 `kBodyChunkedEnd` 解析 trailer。

## 2.5 “真实情况”边界条件与恢复策略

### 2.5.1 不完整行/不完整 chunk：返回 NEEDMOREDATA

- 在 `kStartLine/kHeaders/kBodyChunkedSize/kBodyChunkedEnd` 等“按行”阶段，未发现 `\r\n` 会把残留写入 `lineBuffer_` 并返回 `NEEDMOREDATA`（避免把半行当成错误）。见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L52-L58)。
- 在 `kBodyChunkedData` 阶段，若剩余字节不足 `chunkSize_ + 2`（chunk 数据 + 末尾 CRLF），同样返回 `NEEDMOREDATA`。见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L208-L216)。

### 2.5.2 重复喂入同一缓冲的重同步逻辑

- 当处于 `kHeaders` 且这是首个 header 行但该行不含冒号时，解析器会尝试把它当成新的起始行重新识别，降低“重复喂入同一缓冲导致的误判”。见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L125-L135)。

### 2.5.3 Header/Body 上限与错误码

- Http1Parser 默认上限：Header 单行 16KB、Header 总量 16KB、Body 10MB，见 [Http1Parser::Http1Parser](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L34-L38)。
- 超限返回 `HEADERTOOLONG/BODYTOOLONG`，由 HttpFacade 映射为 `PAYLOAD_TOO_LARGE` 或 `REQUEST_HEADER_FIELDS_TOO_LARGE`，见 [HttpFacade::ProcessParsing](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L225-L245)。

### 2.5.4 chunked trailer 限制与白名单

- 若请求头包含 `Trailer`，解析器会把其声明的键收集到白名单 `allowedTrailerKeys_`（统一转小写）。见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L82-L103)。
- trailer 阶段禁止出现 `Transfer-Encoding`/`Content-Length`/`Trailer`，且当白名单非空时，只允许白名单内的 trailer key；违反即 `ERROR`。见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L289-L300)。

## 2.6 责任链校验：协议一致性与安全检查

HttpFacade 解析成功后会执行责任链： [HandlerChain](file:///home/zsy/WebServer/cppBackend/http/include/handler/HandlerChain.h) / [HandlerChain.cpp](file:///home/zsy/WebServer/cppBackend/http/src/handler/HandlerChain.cpp)。

- 协议一致性： [ProtocolValidationHandler](file:///home/zsy/WebServer/cppBackend/http/src/handler/ProtocolValidationHandler.cpp#L7-L70)
  - 同时出现 `Content-Length` 与 `Transfer-Encoding` → 400
  - `Content-Length` 非数字或与已累积 body 长度不一致 → 400
  - HTTP/1.1 缺 Host → 400
  - 方法未知（UNKNOWN）→ 501
- 安全检查： [SecurityValidationHandler](file:///home/zsy/WebServer/cppBackend/http/src/handler/SecurityValidationHandler.cpp#L34-L149)
  - 方法白名单、body/url/header 上限、路径安全、可疑模式、请求防护（限流/去重）

## 2.7 消费推进与连接级状态

- HttpFacade 内部持有 `parser_` 与解析等待状态（如 `awaiting_more_data_`、`parse_wait_start_`），因此它必须是“连接级对象”，由连接上下文持有（创建点见 [HttpServer::HandleNewConnection](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L64-L73)）。
- 解析成功后消费字节数来自 `parser_->GetConsumeBytes()`；消费推进由 HttpServer 统一对 `BufferBlock` 执行（见 2.1.2）。

