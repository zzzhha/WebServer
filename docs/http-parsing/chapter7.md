# 第7章 错误处理与超时控制

本章说明“解析失败/校验失败/路由失败”在本项目中的真实传播路径：`ParseResult → HttpServerResult → HttpError/HttpResponse`，并覆盖两类超时：HTTP 解析等待超时与 TCP 空闲连接超时。

## 7.1 错误分层与返回类型

核心分层来自 HttpFacade：

- 返回给上层的阶段性结果： [HttpServerResult](file:///home/zsy/WebServer/cppBackend/http/include/HttpFacade.h#L20-L34)
- 结构化错误： [HttpError](file:///home/zsy/WebServer/cppBackend/http/include/error/HttpError.h#L68-L79)

HttpFacade 在 [HttpFacade::Process](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L26-L106) 中按阶段设置 `last_error_` 并返回对应 `HttpServerResult`。

## 7.2 解析错误：ParseResult 到 HttpServerResult/HttpError

解析器返回码来自 [ParseResult](file:///home/zsy/WebServer/cppBackend/http/include/parsers/IHttpParser.h#L8-L16)。

HttpFacade 对解析结果的映射逻辑集中在 [HttpFacade::ProcessParsing](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L163-L284)：

- 空输入：`PARSE_EMPTY_INPUT`，HTTP 400（见 [HttpFacade::Process](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L41-L51)）。
- `NEEDMOREDATA`：返回 `HttpServerResult::NEED_MORE_DATA`，由上层继续累积字节（见 7.3）。
- `UNSUPPORTEDVERSION`：映射为 `PARSE_UNSUPPORTED_VERSION`，HTTP 505（见 [HttpFacade.cpp](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L211-L224)）。
- `HEADERTOOLONG/BODYTOOLONG`：映射为 `PARSE_HEADER_TOO_LARGE/PARSE_BODY_TOO_LARGE`，HTTP 431 或 413，返回 `PAYLOAD_TOO_LARGE`（见 [HttpFacade.cpp](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L225-L245)）。
- 其他解析失败：`INVALIDSTARTLINE/INVALIDHEADER/ERROR` 分别映射到更细的 `HttpErrc`，HTTP 400（见 [HttpFacade.cpp](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L246-L268)）。

## 7.3 HTTP 解析等待超时（Parse Timeout）

当解析器返回 `NEEDMOREDATA`，HttpFacade 会进入“等待更多数据”的计时状态：

- 第一次返回 `NEEDMOREDATA` 时记录 `parse_wait_start_`；
- 若后续仍 `NEEDMOREDATA` 且超过 `parse_timeout_ms_`，则：
  - `HttpErrc::PARSE_TIMEOUT`
  - HTTP 408（Request Timeout）
  - `parser_->Reset()` 并清理等待状态

实现见 [HttpFacade::ProcessParsing](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L187-L209)；默认 `parse_timeout_ms_{5000}` 见 [HttpFacade.h](file:///home/zsy/WebServer/cppBackend/http/include/HttpFacade.h#L123-L126)。

上层行为：

- HttpServer 收到 `HttpServerResult::NEED_MORE_DATA` 时不会立即回包，只记录日志并等待下一次收包（见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L239-L242)）。

## 7.4 校验失败：Protocol/SecurityValidationHandler

解析成功后进入责任链校验：

- 协议一致性： [ProtocolValidationHandler](file:///home/zsy/WebServer/cppBackend/http/src/handler/ProtocolValidationHandler.cpp#L7-L70)
  - 冲突长度/非法 Content-Length/长度不匹配/缺 Host/未知方法等
- 安全检查： [SecurityValidationHandler](file:///home/zsy/WebServer/cppBackend/http/src/handler/SecurityValidationHandler.cpp#L34-L149)
  - 方法白名单、body/url/header 上限、路径安全、可疑模式、请求防护

校验失败时，HttpFacade 返回 `VALIDATION_FAILED/NOT_IMPLEMENTED/PAYLOAD_TOO_LARGE` 等结果，并保留结构化 `HttpError`（见 [HttpFacade::ProcessValidation](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L286-L311)）。

## 7.5 路由失败：当前实现的真实响应语义

HttpFacade 路由阶段调用 `router_->Handle(message, response)`：

- 若返回 `false`，HttpFacade 当前统一按“未处理”生成 404（`HttpErrc::ROUTE_NOT_FOUND`，HTTP 404），见 [HttpFacade::ProcessRouting](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L314-L348)。

说明：

- Router 内部确实能区分“路径存在但方法不允许”的情况（见 [Router::MatchRoute](file:///home/zsy/WebServer/cppBackend/http/src/router/Router.cpp#L488-L499)），但 `Router::Handle` 未把该信息上抛到 HttpFacade，因此 HttpFacade 侧目前无法自动生成 405。
- 对于静态文件/下载服务，这两类 handler 自身会在方法不匹配时返回 405（见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L19-L25) 与 [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L20-L27)）。

## 7.6 HttpServer 的错误回包：ResponseFactory::CreateHttpError

当 `facade->Process(...)` 返回非 SUCCESS 且非 NEED_MORE_DATA 时，HttpServer 会：

- 记录结构化错误日志（含 request_id、stage、code、detail、必要时 stack），见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L243-L269)。
- 用 [ResponseFactory::CreateHttpError](file:///home/zsy/WebServer/cppBackend/http/src/factory/ResponseFactory.cpp#L138-L148) 构建 JSON 错误响应，并强制 `Connection: close`，见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L270-L280)。

## 7.7 TCP 空闲连接超时：TimeWheel

除 HTTP 解析等待超时外，TcpServer 还启用了 TCP 空闲连接回收：

- TcpServer 持有 `TimeWheel time_wheel_`，启动时调用 `time_wheel_.start()`（见 [TcpServer::start](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L31-L34)）。
- 新连接建立后加入时间轮：`add_new_conn_timernode(conn)`（见 [TcpServer::newconnection](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L45-L74) 与 [TcpServer::add_new_conn_timernode](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L175-L182)）。
- Connection 每次读到 EAGAIN（认为本轮数据读尽）会触发 `updatetimercallback_` 更新时间轮（见 [Connection::onmessage](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L166-L176) 与 [TcpServer::update_conn_timeout_time](file:///home/zsy/WebServer/cppBackend/reactor/tcpserver.cpp#L179-L182)）。

