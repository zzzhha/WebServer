# kTLS（Kernel TLS）改造实施方案（TLS 1.2）

本文档给出一套面向本仓库现有架构（Reactor + HttpServer/HttpFacade + sendfile）的 kTLS 改造实施方案，目标是在 **TLS 1.2** 场景下把“数据面加解密”尽可能下沉到内核，以便：

- 发送侧继续复用现有 `writev()` + `sendfile()` 的高效路径；
- 在 kTLS 不可用时自动回退到用户态 OpenSSL；
- 当收到明文 HTTP 报文时能够正常解析并提供服务（按策略允许/拒绝）。

相关现状代码链路（便于对照）：

- 请求处理：HttpServer::HandleMessage → HttpFacade::Process（[HttpServer.cpp](file:///home/zsy/WebServer/cppBackend/reactor/HttpServer.cpp)，[HttpFacade.cpp](file:///home/zsy/WebServer/cppBackend/http/src/HttpFacade.cpp)）
- 发送链路：Connection::writecallback（writev + sendfile）（[Connection.cpp](file:///home/zsy/WebServer/cppBackend/reactor/Connection.cpp)）
- 现有 SSL 处理：SslHandler（内存 BIO + 解密字符串）([SslHandler.cpp](file:///home/zsy/WebServer/cppBackend/http/src/ssl/SslHandler.cpp))

---

## 1）TLS 性能瓶颈分析与技术评估

### 1.1 现有实现的瓶颈/风险点

1) **TLS 与发送链路割裂**
- 现有 SSL 逻辑主要发生在 HttpFacade 解析前（解密入站），但响应发送链路（HttpServer/Connection）没有对应的“加密出站”，导致无法形成完整 HTTPS。
- 即使未来补齐用户态 TLS 出站（SSL_write），也会与 `sendfile()` 冲突：用户态 TLS 无法直接对 socket 使用 sendfile 发送明文文件体，否则会明文出网；只能“read 文件 → SSL_write 分段”，会显著增加 CPU/内存拷贝与系统调用。

2) **多次内存拷贝**
- Reactor 侧 BufferBlock → `bufferToString()` 会拼接一次；
- 现有 Http1Parser::Parse(const char*,len) 还会再拷贝到 std::string；
- 若再叠加 TLS 用户态加解密（SSL_read/SSL_write），会带来额外拷贝与上下文切换。

3) **IO 线程被 TLS/业务阻塞的风险**
- 当前大量逻辑在 IO 线程执行（解析/校验/路由/部分业务），若 TLS 改为用户态并加入 SSL_write 分段，IO 线程压力会进一步上升，影响连接公平性与延迟尾部。

### 1.2 kTLS 的收益预期（适用于 TLS 1.2）

- **发送侧收益最大**：启用 kTLS TX 后，应用仍然向 socket 写“明文数据”，由内核完成 TLS 记录封装与加密；因此可以继续使用 `sendfile()` 发送文件体并获得“零拷贝 + 内核 TLS 加密”的组合收益。
- **CPU 降低与吞吐提升**：大文件下载、静态资源场景，用户态 TLS 的加密开销和分段写会显著占用 CPU；kTLS 可减少用户态加密开销与用户态分段逻辑。
- **架构一致性**：把 TLS 移到连接级 I/O 层后，HttpFacade/Router/Service 永远只处理明文 HTTP，业务栈更清晰。

### 1.3 kTLS 的限制与风险（必须在方案中显式兜底）

- **内核/发行版/套件支持差异**：kTLS 依赖 Linux 内核 TLS ULP（CONFIG_TLS），对 TLS 版本、加密套件、记录层行为存在支持集限制。方案必须“可启用则启用，不可用则回退”。
- **TLS 1.2 支持集**：建议只允许内核普遍支持的 TLS1.2 AEAD 套件（通常 AES-GCM 可靠，ChaCha20 依赖内核版本/配置），否则启用失败频繁。
- **接收侧不强求**：kTLS RX 支持受限更大；可选择先做 **TX 优先**（发送端）以满足 sendfile 目标，RX 继续使用 SSL_read（用户态解密）作为稳定路径。
- **与现有“内存 BIO 解密字符串”不兼容**：kTLS 必须绑定真实 socket fd，现有 SslHandler 设计需要重构/替换。

### 1.4 评估与验收指标（建议）

- 功能：
  - HTTPS（TLS1.2）完整可用：GET/POST、Range、304、错误回包，均为密文传输；
  - 明文 HTTP 仍可在允许策略下工作（或明确拒绝并返回可解释行为）。
- 性能（对比用户态 TLS）：
  - 大文件下载吞吐提升、CPU/核占用下降；
  - p99 延迟改善（尤其是 IO 线程更“轻”）。
- 稳定性：
  - kTLS 启用失败自动回退不影响服务；
  - 日志可观测：每连接是否启用 kTLS、是否回退、回退原因。

---

## 2）kTLS 内核模块集成方案（TLS 1.2）

### 2.1 总体策略：连接级 TLS，会话完成后尝试启用 kTLS

改造的核心是把 TLS 从“HttpFacade 的字符串预处理”迁移到“Connection 的收发路径”：

- 每个 `Connection` 维护一个连接级 `TlsSession`（建议放在 Connection::context_ 或直接作为成员）。
- `TlsSession` 用 OpenSSL 完成 TLS1.2 握手与证书验证。
- 握手完成后，尝试启用 kTLS：
  - **优先启用 TX**（发送侧），以确保 sendfile 可用；
  - RX 可选（先不启用也可）。

### 2.2 前置条件（部署与构建）

1) 内核与系统
- Linux 内核启用 TLS ULP（CONFIG_TLS），推荐使用较新的 LTS 内核以获得更好兼容性与稳定性。
- 运行时可检查 `/proc/net/tls_stat`（若存在）或通过 `modprobe tls` 验证模块。

2) OpenSSL
- 需要 OpenSSL 支持 TLS1.2，并具备 kTLS 集成能力（不同版本/编译选项行为不同）。
- 方案优先采用 **OpenSSL 管理 kTLS**（即 OpenSSL 在握手完成后为该 socket 安装内核 TLS keys），避免手工提取 key material。

当前仓库实现的启用开关（便于落地验证）：
- `WEBSERVER_TLS_CERT`：服务端证书 PEM 路径
- `WEBSERVER_TLS_KEY`：服务端私钥 PEM 路径
- `WEBSERVER_TLS_STRICT`：为 `1/true` 时，TLS 启用但收到明文 HTTP 会直接断开连接

### 2.3 TLS 版本与套件约束（强制 TLS 1.2）

为了满足“只支持 TLS 1.2”：

- 服务器 SSL_CTX 配置：
  - `min_proto_version = TLS1_2_VERSION`
  - `max_proto_version = TLS1_2_VERSION`
- Cipher suites 约束（示例方向，最终以 OpenSSL/内核支持集为准）：
  - 优先：ECDHE + AES_128_GCM / AES_256_GCM（TLS1.2 AEAD）
  - 禁止：CBC、RC4、3DES 等非 AEAD 或弱套件

这样可以显著提高“kTLS 启用成功率”，并避免协商到内核不支持的套件后频繁回退。

### 2.4 接入点改造（与现有模块对应）

#### 2.4.1 Connection：收包路径（onmessage）

目标：上层（HttpServer/HttpFacade）只接收明文 HTTP。

- 连接初始状态为 `AUTO`：
  - 若前若干字节判定为 TLS ClientHello/TLS Record，则进入 `TLS_HANDSHAKE`；
  - 若判定为明文 HTTP（GET/POST/PUT/HEAD/HTTP 等前缀），进入 `PLAINTEXT_HTTP`。
- PLAINTEXT_HTTP：维持现有 `read()` → `BufferBlock` 行为。
- TLS_HANDSHAKE / TLS_READY：
  - 握手阶段使用 `SSL_accept` 推进（非阻塞），并处理 WANT_READ/WANT_WRITE；
  - 应用数据阶段：
    - 若启用 kTLS RX：直接 `read()` 得到明文；
    - 否则：用 `SSL_read()` 得到明文后写入 inputbuffer_（或直接写入上层 pending 缓冲）。

#### 2.4.2 Connection：发送路径（writecallback）

目标：尽可能保持现有 `writev + sendfile`，并在回退时用 SSL_write 发送密文。

在 `TLS_READY` 状态下按能力分叉：

- **kTLS TX 启用成功**：
  - 头部/小 body：继续 `writev()` 写明文；
  - 文件体：继续 `sendfile()` 写明文；
  - 内核完成 TLS record 封装与加密出网。
- **kTLS TX 启用失败（回退用户态）**：
  - `outputbuffer_` 中的明文需改为 `SSL_write()` 发送；
  - `sendfile` 路径必须改为：读文件分段到用户态缓冲 → `SSL_write()`。

#### 2.4.3 HttpFacade/SslHandler：职责收敛

- HttpFacade 不再负责“解密字符串”，只负责 HTTP 解析/校验/路由。
- 现有 `SslHandler`（内存 BIO）不适用于 kTLS，需要替换为“socket 绑定的连接级 TLS 会话”：
  - 复用其证书加载/协议版本配置思想（SslContext/SslFactory），但删除“Decrypt(raw_data)->string”接口。

### 2.5 kTLS 启用流程（TX 优先）

每连接在握手完成后执行：

1) 记录协商结果（TLS 版本、cipher）。
2) 尝试启用 kTLS TX：
   - 若成功：标记 `ktls_tx=true`，走 writev/sendfile 明文写 socket。
   - 若失败：标记回退原因（例如 EOPNOTSUPP/套件不支持/内核不支持）。
3) 可选：尝试启用 kTLS RX（失败不影响）。

日志建议：
- 每连接在握手完成后输出一次：`tls=1.2 cipher=... ktls_tx=... ktls_rx=... fallback=...`

---

## 3）回退机制设计（kTLS 不可用 / 收到 HTTP 明文）

### 3.1 回退目标

- kTLS 不可用时：HTTPS 仍可用（用户态 OpenSSL），只是性能退化；
- 收到 HTTP 明文时：能正常解析（按策略允许），不会误当成 TLS 导致握手失败。

### 3.2 场景 A：kTLS 不可用（内核/套件不支持/启用失败）

触发条件示例：
- 内核不支持 TLS ULP 或运行时禁用；
- 协商到的 cipher 不在内核支持集；
- OpenSSL 无法为该 socket 安装 kTLS keys。

回退策略：
- 保持该连接为 “TLS 用户态模式”：
  - 读：SSL_read
  - 写：SSL_write
  - 文件发送：read 文件分段 + SSL_write（禁止 sendfile）
- 该回退是连接级的，不影响其他连接的 kTLS 启用。

### 3.3 场景 B：端口/配置要求 TLS，但实际收到 HTTP 明文

需要明确产品策略（建议二选一）：

1) **兼容模式（推荐用于开发/混合环境）**
- AUTO 探测到明文 HTTP 后，按 HTTP 正常处理。
- 适用于同一端口可能同时接入 HTTP/HTTPS 的场景（注意安全风险与运维复杂度）。

2) **严格模式（推荐用于生产 HTTPS 端口）**
- 在“HTTPS 端口/启用TLS配置”下，如果探测到明文 HTTP：
  - 直接返回一个明文 400/301（可选），或直接关闭连接；
  - 防止误配置导致的明文降级。

工程建议：通过配置项决定严格/兼容行为，并在日志中输出 `plaintext_on_tls_port` 事件。

### 3.4 场景 C：TLS 握手未完成 + 事件驱动（WANT_WRITE/WANT_READ）

回退/稳定性要点：
- 在 ET 模式下，握手过程可能需要同时关注读/写事件：
  - WANT_READ：确保监听读事件（现有已在读回调触发）；
  - WANT_WRITE：必须 enable writing，否则握手/重协商会卡住。
- 握手失败时：
  - 严格模式：关闭连接并记录错误；
  - 兼容模式：若能确定其实是 HTTP 明文（探测误判），可退回 PLAINTEXT_HTTP（需要谨慎实现，避免被探测绕过）。

---

## 4）实施步骤建议（里程碑）

### M1：架构改造（不启用 kTLS，先把 TLS 移到 Connection）
- 目标：实现完整 HTTPS（TLS1.2）用户态收发（SSL_read/SSL_write），HttpFacade 不再做解密字符串。
- 验收：功能正确、HTTP 明文仍可处理（按配置）。

### M2：启用 kTLS TX（发送侧优先，保住 sendfile）
- 目标：握手完成后尽可能启用 kTLS TX；成功连接继续使用 writev/sendfile。
- 验收：大文件下载走 sendfile 且抓包为 TLS 密文；启用失败时自动回退 SSL_write 且功能不受影响。

### M3：可选启用 kTLS RX（接收侧）
- 目标：在支持环境下启用 RX，减少 SSL_read 解密成本；失败回退不影响。

### M4：性能专项（与 8.6 相关）
- 目标：减少 BufferBlock→string 的全量拼接拷贝；引入 pending 缓冲/解析器无拷贝接口等。
