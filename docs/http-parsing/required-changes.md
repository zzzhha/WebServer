# 基于 问题.txt 的修改清单

本文档根据 [问题.txt](file:///home/zsy/WebServer/问题.txt) 中提出的问题与现状结论，整理“需要修改/可选优化/仅需补充说明”的事项，便于后续落实到代码与文档。

## 一、必须修改（功能正确性/一致性问题）

### 1) HTTPS：响应侧缺少加密发送链路（含 sendfile）

- 现状：当前 SSL/TLS 逻辑主要在 HttpFacade 侧完成握手/解密后再解析请求；响应发送链路（HttpServer/Connection 的 writev + sendfile）未见 TLS 加密封装，因此不具备“完整 HTTPS 响应加密能力”。
- 需要修改：
  - 明确 HTTPS 支持策略（二选一）：
    - **实现完整 HTTPS**：在 Connection 写出路径引入 SSL\_write/加密缓冲；sendfile 场景需要改为“读文件到用户态缓冲后 SSL\_write”（或实现类似 kTLS 的内核 TLS 方案，若不做内核TLS则不能直接 sendfile 明文出网）。
    - **不支持 HTTPS**：文档中明确仅支持 HTTP；若代码里存在“自动识别 SSL 连接并解密”的逻辑，也应同步说明其边界（例如仅用于解析，不保证加密回包）。
  - 明确“响应头/响应体统一加密”的语义，避免出现“头加密、体明文”这种不可接受的混合。
- 相关代码定位：
  - 解析侧：HttpFacade::ProcessSsl / ProcessParsing（cppBackend/http）
  - 发送侧：HttpServer::HandleMessage、Connection::writecallback、Connection::StartSendFile（cppBackend/reactor）

#### 1.1 使用 kTLS 的实现思路（推荐走 OpenSSL 内建 kTLS）

目标：在 TLS 会话建立后，把“加密/解密数据面”下沉到内核，从而允许继续使用 `sendfile()` 发送文件体且由内核完成 TLS 加密；应用层仍保留握手与证书验证逻辑。

**前置条件与限制**

- 仅 Linux 支持（内核需启用 CONFIG\_TLS），且对 TLS 版本/加密套件有支持集限制；若协商到不支持的套件，必须自动回退到用户态 SSL\_write/SSL\_read。
- 使用 kTLS 意味着 SSL 处理必须“绑定真实 socket fd”，不能再采用“内存 BIO + 手动 Decrypt 字符串”的模式。

**代码结构需要做的关键改造**

- 把 SSL 会话从“HttpFacade 解析阶段的字符串解密”迁移到“Connection 级别的收发路径”：
  - 每个连接维护一份 `SSL*`（以及共享的 `SSL_CTX*`），存放在 Connection 的 context 中。
  - 握手在该连接所属 IO 线程里以非阻塞方式推进（处理 `SSL_ERROR_WANT_READ/WRITE`），直到握手完成。
  - 握手完成后尝试启用 kTLS：成功则后续应用数据可走内核 TLS；失败则继续走 OpenSSL 用户态。

**启用 kTLS 的推荐落地方式（OpenSSL 自动安装密钥）**

- 工程已使用 OpenSSL（ssl/SslHandler.cpp、security/RequestProtectionManager.cpp），因此优先利用 OpenSSL 的 kTLS 集成，而不是自己从握手中提取 traffic secret 去 `setsockopt(SOL_TLS, TLS_TX/TLS_RX, ...)`（手工方式复杂且易错）。
- 在握手完成后，对该连接的 `SSL*` 启用 kTLS 选项（示意）：
  - `SSL_set_options(ssl, SSL_OP_ENABLE_KTLS);`
  - 并确保 `SSL` 的 BIO 是 socket BIO（或 OpenSSL 内部已关联 fd），从而 OpenSSL 能在合适时机为该 fd 调用 `setsockopt(TCP_ULP="tls")` 与 `SOL_TLS` 安装密钥。

**发送侧如何用回现有 writev/sendfile**

- 当 kTLS 的 TX 已启用后：
  - 头部/小 body：可继续用 `writev()`（写入明文字节到 socket），由内核 TLS 负责加密出网。
  - 文件体：可继续用 `sendfile()`（从文件 fd → socket fd），由内核 TLS 负责加密出网。
- 当 kTLS 未启用（或协商到不支持套件）：
  - 头部/小 body：需要用 `SSL_write()` 输出密文。
  - 文件体：需要“读文件到用户态缓冲，再 SSL\_write() 分段发送”，不能直接 sendfile。

**接入点建议（与现有文件对应）**

- reactor/Connection：
  - 在 `onmessage()` 中区分“握手阶段/应用数据阶段”，应用数据阶段用 `SSL_read()` 或 `recv()`（取决于是否启用 kTLS RX）得到明文再进入现有 BufferBlock → HttpServer 链路。
  - 在 `writecallback()` 中区分“明文直写(writev/sendfile)”与“SSL\_write(用户态加密)”两条路径，并保证只在所属 EventLoop 线程操作该连接的 SSL 状态。
- http/ssl：
  - 现有 `SslHandler` 基于内存 BIO 的“字符串解密”设计不适用于 kTLS，需要重构为“socket 绑定 + 连接级状态机”，或新增一个专门的 `SocketSslSession` 来替代当前模式。

### 2) TimeWheel：潜在竞态与超时参数硬编码

- 现状：
  - 时间轮是独立线程 tick（非接入主事件循环），tick 后把“关闭连接”投递到连接所属 EventLoop 执行，数据结构用 mutex 保护。
  - 仍可能出现“tick 判定超时并投递关闭”与“IO线程刚更新定时器”交错导致误关的竞态（逻辑层面竞态，不是容器并发破坏）。
  - `TimeWheel::update_connection` 内部调用 `add_connection_unsafe(conn, 360)` 存在硬编码 360 秒，与 TcpServer 的 `ts_tcp_conn_timeout_s_` 可能不一致。
- 需要修改：
  - 修正超时参数：update\_connection 应复用与 add\_connection 一致的 timeout（来自 TcpServer 的配置），避免硬编码。
  - 竞态兜底：增加“代际号/版本号/最后活跃时间戳”校验，tick 投递关闭前再次确认连接仍过期，避免刚活跃又被关闭。
- 相关代码定位：
  - cppBackend/timer/TimeWheel.cpp（update\_connection/tick）
  - cppBackend/reactor/tcpserver.cpp（ts\_tcp\_conn\_timeout\_s\_/update\_conn\_timeout\_time）
  - cppBackend/reactor/Connection.cpp（读到 EAGAIN 后触发 update）

## 二、建议修改（性能/可维护性优化）

### 3) 8.6：BufferBlock → request\_data 的全量拼接拷贝开销

- 现状：每次 HandleMessage 都将 BufferBlock 拼成连续 std::string（bufferToString），带来一次拷贝；解析器还会维护 lineBuffer\_ 处理半包。
- 建议修改方向（可组合）：
  - **连接级 pending 缓冲**：在 Connection context 中维护 pending 字符串，把本次 BufferBlock 的新增数据 append 进入 pending 后立即 consume 掉 BufferBlock；parser 在 pending 上解析并按已消费字节 erase 前缀，避免每次全量重建 request\_data。
  - *减少 Parse(const char*,len) 的额外拷贝\*：当前 Http1Parser::Parse(const char\*,len) 会先拷贝到 std::string 再解析；可改为直接在“视图/游标”上解析，或让主 Parse 支持 string\_view+游标。
  - **流式大 body**：上传/大 body 场景不要把 body 完整堆入 HttpRequest::body\_，而是在解析完 headers 后把 body 流式写入文件/临时存储，再交业务层处理。
- 相关代码定位：
  - cppBackend/reactor/HttpServer.cpp（bufferToString、consumeBytes）
  - cppBackend/reactor/Buffer.cpp（bufferToString）
  - cppBackend/http/src/parser/Http1Parser.cpp（Parse 重载）

### 4) 工作线程池未用于 HTTP 主链路（是否启用需要决策）

- 现状：HttpServer 创建了 WORKS 线程池，但 HandleMessage 未将解析/路由/业务派发到工作线程。
- 建议修改方向：
  - 若业务包含数据库/文件系统遍历等可能阻塞的操作：把“业务处理”放到工作线程，完成后 queue 回连接所属 EventLoop 做响应写出与 sendfile，避免 IO 线程被阻塞。
  - 若业务轻量：可考虑移除未使用的线程池或在文档中说明其用途/待接入点，避免误导。

### 5) 去重策略对“重复 GET 页面访问”的影响

- 现状：默认 5 秒窗口内内容级去重，命中返回 409；在某些场景可能误伤正常浏览器重复请求（重试、并发拉取资源、预加载等）。
- 建议修改方向：
  - 仅对非幂等请求启用去重（例如 POST/PUT/PATCH），或要求客户端提供幂等键（Idempotency-Key）再去重。
  - 对静态资源/页面 GET 默认不做去重，或将去重窗口/策略下放到具体路由（按路径白名单/黑名单）。
- 相关代码定位：
  - cppBackend/http/src/security/RequestProtectionManager.cpp（GenerateRequestHash/ShouldBlockRequest）

## 三、仅需补充说明（文档澄清/避免误解）

### 6) BufferBlock 单块 64KB 上限的含义

- 需要在文档明确：64KB 是“单个 block 的上限策略”，不是缓冲区总大小上限；总大小可通过多个 block 累积。

### 7) 无效头/无效报文的处理策略

- 需要在文档明确：当前解析失败通常不会继续消费并跳过，而是返回错误响应并关闭连接；不具备容错跳过“坏请求继续解析后续请求”的能力。

### 8) trailer / chunk 扩展语义

- 需要在文档明确：
  - trailer 的位置与格式（0 chunk 之后直到空行结束）。
  - chunk extension（`;foo=bar`）的语法存在，但当前实现忽略其内容，只提取行首 hex 数字作为 chunk-size。
  - trailer 白名单的来源与用途（Trailer 头声明）。

### 9) 下载速度展示来源

- 需要在文档明确：浏览器下载速度一般是客户端统计值；服务端当前不发送“速度指标字段”，也未实现限速协议交互。

## 四、建议落地顺序

1. HTTPS 响应侧加密链路（或明确不支持 HTTPS 并修正文档）
2. TimeWheel：修正 update\_connection 的 360 硬编码 + 增加误关竞态兜底
3. 8.6 性能优化（pending 缓冲/减少拷贝/流式 body）
4. 去重策略按方法/路由收敛，避免影响正常 GET 访问

