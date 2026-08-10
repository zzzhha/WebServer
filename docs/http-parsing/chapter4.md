# 第4章 POST请求体读取：Content-Length 与 chunked

本章聚焦“请求体如何从字节流进入 HttpRequest::body_”，并对照源码说明 Content-Length 与 Transfer-Encoding: chunked 两条读取路径，以及随后在业务路由中如何使用请求体。

## 4.1 请求体的落点：HttpRequest::AppendBodyChunk / GetBody

请求体最终存储在 `HttpRequest::body_` 中：

- 接口定义见 [HttpRequest.h](file:///home/zsy/WebServer/cppBackend/http/include/core/HttpRequest.h#L50-L68)（`AppendBodyChunk` / `GetBody` / `GetBodyLength`）。
- 解析器在读取 body 时会调用 `currentMessage_->AppendBodyChunk(...)`，即落到 HttpRequest 的实现上（见第 4.2/4.3 的解析路径）。

## 4.2 Content-Length 模式：按长度补齐 body

触发条件与读取过程以 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L79-L206) 为准：

- 在 headers 阶段遇到空行后，若存在 `Content-Length`：
  - 先 `std::stoull(*cl)` 得到 `contentLength_`；
  - 若超过 `maxBodySize_` 直接返回 `BODYTOOLONG`；
  - `contentLength_ > 0` 切换到 `kBodyContentLength` 状态。
- `kBodyContentLength` 状态按“剩余可用字节”和“还需要的字节”计算 `take`，逐步 `AppendBodyChunk`，直到 `bodyReceived_ >= contentLength_` 后完成消息并 `FinalizeMessage(out)`。

与“真实情况”相关的边界条件：

- 若本次输入只包含部分 body，解析器会返回 `NEEDMOREDATA`，等待后续数据拼齐（见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L184-L205)）。
- `maxBodySize_` 同时约束声明的 Content-Length 与实际累积的 body 字节数（见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L108-L112) 与 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L190-L199)）。

## 4.3 chunked 模式：按 chunk-size/数据块累积 body

触发条件与状态机以 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L82-L180) 为准：

- headers 结束后，若 `Transfer-Encoding: chunked`：
  - `isChunked_ = true`；
  - 解析 `Trailer` 声明并构建 trailer key 白名单（见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L82-L103)）；
  - 切换到 `kBodyChunkedSize`。

chunked 的关键行为（与“真实情况”最相关）：

- chunk size 解析只读取行首连续的十六进制数字，忽略后续扩展（见 [ParseChunkSize](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L415-L431)）。
- chunk data 必须完整包含 `chunkSize_` 字节及其后的 `\r\n`；若末尾不是 CRLF，直接 `ERROR`（见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L208-L234)）。
- trailer 阶段禁止出现 `Transfer-Encoding/Content-Length/Trailer`，且当白名单非空时只允许白名单内字段（见 [Http1Parser::Parse](file:///home/zsy/WebServer/cppBackend/http/src/parser/Http1Parser.cpp#L289-L300)）。

## 4.4 协议校验：冲突长度、非法 Content-Length、长度不匹配

解析成功后会进入责任链校验（见第 2 章）。其中对请求体相关的关键规则在：

- [ProtocolValidationHandler](file:///home/zsy/WebServer/cppBackend/http/src/handler/ProtocolValidationHandler.cpp#L7-L45)
  - `Content-Length` 与 `Transfer-Encoding` 同时出现 → 400
  - `Content-Length` 非数字 → 400
  - 若解析后已有 body 且 `body_length != Content-Length` → 400
- [SecurityValidationHandler::CheckBodySize](file:///home/zsy/WebServer/cppBackend/http/src/handler/SecurityValidationHandler.cpp#L156-L176)
  - 限制 `Content-Length` 声明大小与实际 body 大小

## 4.5 业务侧如何使用 POST body（真实调用点）

路由中直接读取 body 的典型写法见 [HttpServer::SetupRoutes](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L321-L410)：

- `/register`、`/login`、`/refresh-token` 等 POST handler：
  - `std::string body = request->GetBody();`
  - 使用 `ParseFormData(body)` 解析 `application/x-www-form-urlencoded`（实现见 [ParseFormData](file:///home/zsy/WebServer/cppBackend/http/src/handler/AppHandlers.cpp#L17-L82)）。

说明：

- `ParseFormData` 做了基础的 `+` 与 `%XX` 解码，但其解码策略并不等同于 `HttpRequest::UrlDecodeStrict` 的“严格路径解码”，两者用途不同（前者用于表单，后者用于 URL path）。

