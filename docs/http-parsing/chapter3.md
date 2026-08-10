# 第3章 GET请求处理：路由与静态文件

本章以 GET 为主线，串起“解析成功后的路由匹配 → 静态资源/页面/下载服务 → sendfile 输出”的真实调用链，并补充本项目对 URL/路径/查询参数的实际处理规则。

## 3.1 GET 请求在本项目中的位置

GET 的典型处理链路如下：

- Connection 收包 → HttpServer 从 `BufferBlock` 取出 `request_data` → HttpFacade 执行解析/校验/路由 → Router 命中 handler → handler 填充 `HttpResponse` → HttpServer 序列化响应头并按需触发 `sendfile()`。

关键入口：

- [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L80-L211)
- [HttpFacade::Process](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp#L26-L106)
- [Router::Handle](file:///home/zsy/WebServer/cppBackend/http/src/router/Router.cpp#L503-L524)

## 3.2 路由注册：哪些 GET 路径会被处理

路由表集中注册在 [HttpServer::SetupRoutes](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L291-L533)。

与 GET 相关的常见类别：

- 页面类：`/`、`/index.html`、`/login.html` 等由页面 handler 生成响应（见同函数内 `pageRouteHandler`）。
- 静态资源类：`/images/*`、`/video/*`、`/uploads/*`、`/assets/*` 等转给 `StaticFileService::HandleStaticFile`。
- 下载类：`/download/*` 转给 `DownloadService::HandleDownload`（同时支持 HEAD）。
- API 类：例如 `/api/files`、`/api/files/preview` 等转给 `FileApiService`。

## 3.3 URL/Path/QueryParams 的真实解析规则（HttpRequest）

Http/1 解析器在解析起始行后，会调用 [HttpRequest::SetRequestLine](file:///home/zsy/WebServer/cppBackend/http/include/core/HttpRequest.h#L66-L68)，其内部会触发 URL 解析逻辑（实现见 [HttpRequest.cpp](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp)）。

核心行为以 [HttpRequest::ParseUrl](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L305-L357) 为准：

- URL 为空：路径设为 `/`。
- `#fragment`：会被剥离，不参与路由与文件访问。
- `?query`：解析为 `queryParams_`，并在 `RebuildUrl()` 后重建到 `url_`。
- 路径严格解码：`UrlDecodeStrict` 遇到非法 `%` 编码直接失败，随后把 `path_` 置空（见 [UrlDecodeStrict](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L462-L492) 与 [ParseUrl](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L332-L350)）。
- UTF-8 校验：路径必须是合法 UTF-8，否则失败并置空路径（见 [IsValidUtf8](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L494-L544)）。
- 规范化与防越界：`NormalizePath` 规范化 `.`/`..` 并拒绝越界（失败同样置空路径），见 [NormalizePath](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L546-L579)。
- 小写语义：最终 `path_` 会被转为小写（`LowerAsciiInPlace(decoded_path)`），见 [ParseUrl](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L349-L350)。
- Query key 小写：`queryParams_[LowerAsciiCopy(key)]...`，因此查询参数 key 大小写不敏感，见 [ParseQueryString](file:///home/zsy/WebServer/cppBackend/http/src/core/HttpRequest.cpp#L359-L387)。

这些规则会直接影响：

- 路由匹配（Router 内部同样做了路径规范化与小写处理，见 [Router.cpp](file:///home/zsy/WebServer/cppBackend/http/src/router/Router.cpp)）。
- 静态文件/下载路径拼接（服务层会再做一次路径安全限制，见 3.4/3.5）。

## 3.4 GET 静态文件：StaticFileService 的真实行为

静态文件处理入口： [StaticFileService::HandleStaticFile](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L10-L147)。

关键规则：

- 仅支持 GET/HEAD，否则 405（见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L19-L25)）。
- 若请求路径为空或以 `/` 结尾，会追加 `index.html`（见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L27-L33)）。
- 根目录约束：通过 `ResolvePathUnderRoot(static_path, req_path, full_path)` 防止目录穿越（失败返回 403），见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L34-L40)。
- 缓存头：始终设置 `Last-Modified/ETag/Cache-Control`，并处理 `If-None-Match/If-Modified-Since` 返回 304，见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L54-L77)。
- Range：若存在 `Range`，调用 `ParseRangeHeader` 解析，非法则 416；合法则返回 206 并设置 `Content-Range/Content-Length`，同时通过 `response.SetSendFile(...)` 走文件直传，见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L92-L136)。
- HEAD：HEAD 不发送 body，但会根据 Range 与文件大小正确设置 `Content-Length`（见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L104-L118)）。

## 3.5 sendfile 输出：响应头与文件体的分离发送

本项目在大文件场景不会把文件读入内存 `body_`，而是：

- Service 层设置 `response.SetSendFile(path, offset, length)`。
- HttpServer 序列化并发送响应头，然后调用 `Connection::StartSendFile` 激活 `sendfile()` 发送文件体。实现见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L189-L211) 与 [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L110-L148)。

注意：当 `HasSendFile()` 为真时，HttpServer 会自行 `open()` 文件；若打开失败，会清除 sendfile 标记并回退为文本错误响应（见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L189-L211)）。

