# HTTP请求解析技术文档 - 图表汇总

本文件仅提供“与源码一致”的流程图与状态机图，便于快速沟通调用链与关键边界条件。

## 1. TCP 接入与连接分配（Acceptor → TcpServer → subloop）

```mermaid
sequenceDiagram
    participant Client
    participant Acceptor
    participant TcpServer
    participant SubLoop as EventLoop(subloop)
    participant Conn as Connection
    participant TW as TimeWheel

    Client->>Acceptor: TCP 三次握手完成后发起连接
    Acceptor->>Acceptor: listenfd 可读，循环 accept()
    Acceptor->>TcpServer: newconnection(std::unique_ptr<Socket>)
    TcpServer->>TcpServer: loop_index = fd % threadnum_
    TcpServer->>Conn: new Connection(subloops_[loop_index], socket)
    TcpServer->>TW: add_connection(conn, timeout_s)
    TcpServer->>SubLoop: queueinloop(conn->connectEstablished())
    SubLoop->>Conn: enablereading() (ET)
```

## 2. HTTP 处理管线（HttpServer → HttpFacade）

```mermaid
flowchart TD
  A[Connection读回调] --> B[BufferBlock inputbuffer_追加数据]
  B --> C[HttpServer::HandleMessage]
  C --> D[request_data = inputbuffer.bufferToString]
  D --> E[HttpFacade::Process]
  E --> F{SSL启用?}
  F -->|否| G[ProcessParsing]
  F -->|是| H[握手/解密]
  H --> G
  G --> I[ProcessValidation]
  I --> J[ProcessRouting]
  J --> K[HttpServer::response Serialize]
  K --> L{HasSendFile?}
  L -->|否| M[outputbuffer.append + writev]
  L -->|是| N[open + StartSendFile + sendfile]
```

## 3. Http1Parser 状态机（真实状态）

```mermaid
stateDiagram-v2
    [*] --> kStartLine
    kStartLine --> kHeaders: 起始行解析成功
    
    kHeaders --> kBodyChunkedSize: Transfer-Encoding为chunked
    kHeaders --> kBodyContentLength: Content-Length大于0
    kHeaders --> kDone: 无body
    
    kBodyContentLength --> kDone: 读取满Content-Length
    
    kBodyChunkedSize --> kBodyChunkedData: chunkSize大于0
    kBodyChunkedData --> kBodyChunkedSize: chunk-data加CRLF
    
    kBodyChunkedSize --> kBodyChunkedEnd: chunkSize等于0
    kBodyChunkedEnd --> kDone: trailer空行结束
    
    kDone --> [*]
```

## 4. GET 静态文件（路由命中 → SetSendFile → sendfile）

```mermaid
sequenceDiagram
    participant Client
    participant Conn as Connection
    participant HS as HttpServer
    participant HF as HttpFacade
    participant R as Router
    participant SFS as StaticFileService

    Client->>Conn: GET /images/a.png HTTP/1.1 ...
    Conn->>HS: onmessagecallback_()
    HS->>HF: Process(request_data)
    HF->>R: Handle(request, response)
    R->>SFS: HandleStaticFile(request, response)
    SFS-->>R: response.SetSendFile(path, offset, length)
    R-->>HF: handler 返回 true
    HF-->>HS: SUCCESS + response
    HS->>Conn: outputbuffer.append(response.Serialize())
    HS->>Conn: StartSendFile(open(path), offset, length)
    Conn->>Client: 先发响应头(writev)
    Conn->>Client: 再发文件体(sendfile)
```

## 5. Range（单范围）处理流程

```mermaid
flowchart TD
    A[收到GET/HEAD文件请求] --> B{存在Range?}
    B -->|否| C[200 OK Content-Length=file_size]
    B -->|是| D[ParseRangeHeader range, file_size]
    D --> E{解析成功?}
    E -->|否| F[416 Content-Range = bytes */file_size]
    E -->|是| G[计算length=end-start+1]
    G --> H{HEAD请求?}
    H -->|是| I[206 仅返回头 Content-Length/Content-Range]
    H -->|否| J[206 SetSendFile path, start, length]
```

## 6. 错误回包与连接处理（真实分支）

```mermaid
flowchart TD
  A[facade->Process 返回] --> B{result}
  B -->|SUCCESS| C[消费 GetConsumedBytes]
  B -->|NEED_MORE_DATA| D[等待下次收包]
  B -->|其他| E[构造 HttpError]
  E --> F[ResponseFactory::CreateHttpError]
  F --> G[Connection: close_on_send_complete = true]
  G --> H[发送错误 JSON 响应]
```

