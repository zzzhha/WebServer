# 第6章 下载请求：Range/缓存头/sendfile

本章覆盖两类“文件输出”能力：

- 静态资源：`StaticFileService::HandleStaticFile`
- 下载接口：`DownloadService::HandleDownload`

两者都支持 GET/HEAD、缓存头（ETag/Last-Modified）、Range 断点续传，并通过 `HttpResponse::SetSendFile` + Reactor 层 `sendfile()` 实现文件直传。

## 6.1 路由入口：哪些请求会进入文件输出逻辑

路由注册见 [HttpServer::SetupRoutes](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L291-L533)：

- 静态文件：`/images/*`、`/video/*`、`/uploads/*`、`/assets/*` 等转发至 [StaticFileService::HandleStaticFile](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L10-L147)。
- 下载：`/download/*` 转发至 [DownloadService::HandleDownload](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L11-L211)。

## 6.2 sendfile 输出链路：响应头与文件体分离

Service 层不会把文件内容读入 `HttpResponse::body_`，而是设置 sendfile 元信息：

- `response.SetSendFile(path, offset, length)`：见 [HttpResponse.h](file:///home/zsy/WebServer/cppBackend/http/include/core/HttpResponse.h)。
- HttpServer 检测到 `response.HasSendFile()` 后：
  - `open()` 文件；
  - 把 `response.Serialize()`（仅响应头）写入输出缓冲；
  - 调用 `Connection::StartSendFile`，由写回调 `sendfile()` 推送文件体。
  - 实现见 [HttpServer::HandleMessage](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp#L189-L211) 与 [Connection::writecallback](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp#L110-L148)。

## 6.3 缓存头：ETag / Last-Modified / 304

静态文件与下载服务都遵循同一套缓存头策略：

- 生成并写入：
  - `Last-Modified = FileServeUtil::ToHttpDate(mtime)`
  - `ETag = FileServeUtil::BuildWeakEtag(mtime, size)`
  - `Cache-Control = public, max-age=3600`
- 命中缓存：
  - `If-None-Match == etag` → 304
  - `If-Modified-Since >= mtime` → 304

对应实现：

- 静态文件： [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L54-L77)
- 下载： [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L106-L139)
- 工具函数声明： [FileServeUtil.h](file:///home/zsy/WebServer/cppBackend/services/include/FileServeUtil.h#L14-L28)

## 6.4 Range：断点续传与 206/416

两类服务都支持 `Range: bytes=start-end`：

- 解析：`FileServeUtil::ParseRangeHeader(range_value, file_size, range)`
- 解析失败：返回 416，并设置 `Content-Range: bytes */<file_size>`
- 解析成功：返回 206，并设置：
  - `Content-Length = end-start+1`
  - `Content-Range = bytes start-end/file_size`
  - `SetSendFile(path, start, length)`

对应实现：

- 静态文件： [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L92-L136)
- 下载： [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L164-L208)

## 6.5 HEAD：只返回头，不发送文件体

两类服务都对 HEAD 做了特殊处理：

- HEAD 不发送 body/文件体；
- 但会根据 Range 是否启用来设置正确的 `Content-Length` 与 `Content-Range`。

对应实现：

- 静态文件： [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L104-L118)
- 下载： [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L176-L190)

## 6.6 路径与文件名安全

### 6.6.1 静态文件：限制在 static_path 根目录下

- 静态文件通过 `ResolvePathUnderRoot(static_path, req_path, full_path)` 把请求路径解析为根目录下的真实路径；失败返回 403。
- 实现见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L34-L40) 与工具函数声明 [FileServeUtil.h](file:///home/zsy/WebServer/cppBackend/services/include/FileServeUtil.h#L27-L28)。

### 6.6.2 下载：限制 folder 白名单 + 文件名字符约束

DownloadService 的路径解析与安全检查包括：

- `/download/[folder]/[filename]` 解析，并限制 folder 只能为 `images/video/uploads`（见 [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L34-L52)）。
- filename 禁止包含 `/`、`\\`、`..`、空字节等（见 [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L75-L82)）。
- `ValidateFilePath` 进一步拒绝 `../` 与 `..\\`（见 [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L213-L235)）。

## 6.7 可选的文件 MD5 输出

两类服务都支持按需返回文件 MD5：

- query：`?md5=1`
- header：`X-Request-MD5: 1`

命中后会计算并附加 `X-File-MD5`（见 [StaticFileService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/StaticFileService.cpp#L82-L90) 与 [DownloadService.cpp](file:///home/zsy/WebServer/cppBackend/services/src/DownloadService.cpp#L154-L162)）。

