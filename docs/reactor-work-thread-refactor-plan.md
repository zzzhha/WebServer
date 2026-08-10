# Reactor IO/Work 线程职责拆分改造方案

## 1. 背景与目标

当前实现中，`HttpServer::HandleMessage` 在 IO 线程（sub event loop 所在线程）里直接执行了以下工作：

- HTTP 解析与路由分发
- 业务处理（含数据库访问、文件系统访问、上传下载逻辑）
- 响应拼装与部分文件打开（`open`）

这会导致 IO 线程可能被阻塞或长时间占用，影响连接读写公平性和延迟稳定性。

本次改造目标：

- 启动并使用现有 `work` 线程池处理业务逻辑。
- 将阻塞与耗时代码从 IO 线程迁移到 work 线程。
- 保证 sub event loop 仅处理 IO 相关事务（读写事件、连接状态、回写调度）。
- 补充单元测试与集成测试，验证行为正确性与线程模型约束。

## 2. 现状问题定位

关键热点路径：

- `TcpServer::message` -> `HttpServer::HandleMessage`
- `HandleMessage` 中直接调用 `facade->ProcessPending(...)` 与 `ProcessRequest(...)`
- 路由处理中调用 `AuthService / DownloadService / StaticFileService / UploadService / FileApiService`
- 文件发送分支中包含 `open(...)`

潜在问题：

- MySQL 连接池等待与 SQL 执行会阻塞 IO 线程。
- 文件元数据与打开文件等系统调用会阻塞 IO 线程。
- 上传/下载与较大请求处理会拉长单次事件处理时间，降低并发可预测性。

## 3. 总体改造思路

采用“两段式处理”模型：

1. IO 线程（快速路径）
- 从 `Connection::inputbuffer_` 读取并转移字节数据。
- 生成请求任务上下文（连接弱引用、请求序号、必要快照）。
- 投递到 `HttpServer::threadpool_`（work 线程池）。
- 立即返回事件循环，不做业务处理。

2. work 线程（慢路径）
- 在 work 线程执行 HTTP 增量解析、路由、业务调用、响应构建。
- 产出“可发送结果对象”（响应文本或 sendfile 描述、是否 keep-alive、是否关闭连接）。
- 通过 `conn->getLoop()->queueinloop(...)` 投递回原 IO 线程执行最终回写。

3. IO 线程（回写路径）
- 校验连接仍有效、请求序号未过期（避免乱序回写）。
- 将响应写入 `outputbuffer_`，设置 `StartSendFile` 或 `setCloseOnSendComplete`。
- 调用 `conn->send()` 触发写事件发送。

## 4. 核心设计细节

### 4.1 连接级上下文扩展

在连接 `context`（当前为 `std::shared_ptr<HttpFacade>`）上扩展为结构体，例如：

- `facade`: 连接级 HTTP 状态机
- `mutex`: 保护连接级 pending 缓冲（若允许多任务并行）
- `inflight` / `sequence`: 保证同连接请求处理顺序与回写顺序

原则：

- 同一连接请求默认串行处理（先保证正确性，再做并行优化）。
- 不同连接可并行利用 work 线程池。

### 4.2 请求任务对象（WorkItem）

建议定义 `WorkItem`：

- `weak_ptr<Connection>`：避免任务延迟执行时延长连接生命周期
- `request_id` / `conn_fd` / `seq`
- `raw_data`：从 inputbuffer 转移出的字节
- `recv_ts`：用于超时与可观测性统计

### 4.3 回写结果对象（WorkResult）

建议定义 `WorkResult`：

- `seq`
- `close_after_send`
- `serialized_response`
- `sendfile`（path/offset/length）
- `error_meta`（用于日志）

work 线程只负责产出数据，不直接操作 `Connection` 的 IO 行为。

### 4.4 线程安全与生命周期

- work 线程中禁止直接调用 `conn->sendinloop` 或操作 Channel。
- 所有 socket 写相关动作必须回到 `conn->getLoop()`。
- 回写前先 `lock weak_ptr`，失败则丢弃结果。
- 增加“序号检查”：若结果 `seq` 早于连接当前期望值，则丢弃，避免旧任务覆盖新状态。

### 4.5 背压与降级策略

- 若 work 队列过长（`ThreadPool::queue_size` 超阈值），可快速返回 `503 Service Unavailable`。
- 单连接 inflight 任务超过阈值时，拒绝新任务并记录告警。
- 记录任务排队时间、执行时间、回写延迟，便于后续调优。

## 5. 分阶段实施计划

### Phase 1：最小可用改造（先正确）

- 改造 `HttpServer::HandleMessage`：
  - 只做读缓冲提取与任务投递。
  - 业务逻辑迁移到 `HandleMessageInWorker`（新函数）。
- 新增 `PostResultToIoLoop`（或同义函数）统一回写。
- 保持“每连接串行”处理，避免并发状态机复杂度。

交付标准：

- IO 线程不再直接执行服务层业务代码。
- 基础 API 路由可正常返回。

### Phase 2：完善可靠性

- 增加序号/代际校验，避免乱序回写。
- 增加 work 队列背压策略。
- 增强日志：`request_id + queue_wait_ms + worker_exec_ms + io_flush_ms`。

### Phase 3：性能与可观测性

- 支持按路由分类统计耗时（DB/文件/序列化）。
- 对大文件下载路径继续利用 sendfile；TLS 回退路径保留现行为基线。

## 6. 单元测试方案

建议新增 `test/http_workflow_unit_tests.cpp`（或拆分多个文件），覆盖：

1. 任务投递
- `HandleMessage` 收到数据后会向 work 线程池投递任务。
- IO 线程路径不再触发业务执行。

2. 回写调度
- work 线程产出结果后，必须通过 `queueinloop` 回到 IO 线程。
- 连接失效时结果被安全丢弃，不崩溃。

3. 顺序保障
- 同连接多请求按序回写（基于 seq 校验）。
- 过期结果被拒绝。

4. 背压策略
- 模拟队列满时返回 503 或拒绝策略生效。

说明：

- 现有工程非 gtest 体系，可继续沿用当前 `main + 断言` 形式（与 `test/` 目录风格一致）。

## 7. 集成测试方案

建议在 `test/` 下新增 `http_worker_offload_integration_tests.cpp`：

1. 阻塞业务不拖慢 IO 读写
- 构造慢接口（sleep/慢 SQL 模拟）与快接口并发请求。
- 验证快接口延迟不被慢接口显著拖垮。

2. 上传/下载与普通 API 混部
- 并发执行 `/download/*` 与 `/api/*`。
- 验证响应正确、连接不异常关闭。

3. 连接关闭竞态
- 请求处理中主动断开连接。
- 验证 worker 回写阶段安全退出，无崩溃与野指针。

4. 大并发稳定性
- 多连接并发短请求，验证无死锁、无卡死、无明显超时堆积。

可复用现有测试可执行产物组织方式，在 `test/CMakeLists.txt` 增加目标并输出到 `${CMAKE_BINARY_DIR}/test`。

## 8. 风险与规避

- 风险：连接级状态机并发访问导致数据竞争。
  - 规避：先串行每连接请求；必要处加锁；所有 IO 操作回 IO 线程。

- 风险：任务堆积导致内存增长。
  - 规避：work 队列阈值、单连接 inflight 阈值、快速失败。

- 风险：发送时序错乱。
  - 规避：seq 校验 + 仅 IO 线程最终落地发送。

## 9. 验收标准

- 代码层面：IO 线程路径不再直接调用服务层阻塞逻辑。
- 功能层面：现有路由行为与状态码保持兼容。
- 并发表现：慢请求存在时，快请求 P95/P99 延迟明显优于改造前。
- 稳定性：新增单元/集成测试全部通过。

---

如果认可该方案，下一步按 Phase 1 先提交最小改造代码与对应测试，再逐步推进 Phase 2/3。
