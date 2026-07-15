# iot-engine 与 Ruvia 实施计划

> 文档状态：定稿计划（2026-07-14）
>
> 相关文档：[架构总方案](iot-engine-architecture.md)、[Ruvia 并发原语规范](ruvia-concurrency-primitives.md)

## 1. 发布切分

实施分三条并行工作流：

| 工作流 | 内容 | 框架依赖 |
|---|---|---|
| Ruvia R0 | 每 Web worker 单一时间调度 timer，移除 timer-as-signal | 无 |
| Ruvia R1 | EventLoopPool、EventLoop/WorkerHandle、sleep、Channel、OneShot | R0 |
| Ruvia R2 | TaskScope、WS 单 reader/close/abort | R1 |
| iot-engine | schema、CRUD、DeviceEngine、EventBus、Webhook | 可在 R1/R2 前并行 |

版本目标：

- Ruvia 0.1.2：R1 全部能力，不包含通用 `withTimeout`。
- Ruvia 0.1.3：R2 全部能力；若决定合版，也必须保持 R1/R2 为独立提交和独立测试门禁。
- iot-engine 在 R2 前使用 SSE 和异步命令状态接口，不阻塞业务主线。

## 2. 里程碑

### M0：不依赖新框架的基础工作

状态：待开始，可立即并行。

交付物：

- iot-engine 项目脚手架、配置、日志、健康检查和 graceful shutdown。
- PostgreSQL/TimescaleDB、Redis 配置及后台连接所有权约束。
- 认证、RBAC、部门/设备共享和 CRUD。
- `device_command`、Webhook subscription/outbox schema 与迁移。
- 命令 `POST`、状态 `GET` 契约，先实现 `202 + Location`。
- DeviceEngine protocol decoder 的纯字节单测。
- RealtimeAggregator 的 keyed conflation 单测。
- SSE 事件推送过渡实现。

门禁：

- schema 可重复迁移和回滚验证。
- OpenAPI 明确定义 200/202、commandId、Location 和状态模型。
- 权限变更能终止受影响 SSE/WS session。
- 业务代码不包含 `ruvia::detail`。

### R1：Ruvia 0.1.2 并发基础设施

状态：已完成。EventLoopPool、EventLoop/WorkerHandle、WebWorkerHandle 异步作业投递、统一 deadline queue、sleep、Channel、OneShot，以及 timer-as-signal/stream timeout 迁移均已实现并通过验证。EventLoop 公开应用自有 `io_context`/executor，Web worker 保持受限。

任务：

0. 将 ConnectionScanner deadline、shutdown/stream timeout 合并到每 worker 唯一 deadline queue；HTTP/2/WS 写唤醒改用 mailbox/signal，不再创建 `steady_timer(max)`。
1. 在 `ruvia-core` 增加 EventLoopPool、EventLoop、WorkerHandle 和有界 `post()`；EventLoop 支持原生 Asio I/O 和 owner-thread `onStop()`。
2. 将 WorkerHandle 从 HttpServer worker 传入 Context；通过 `App::workerFor()`/`App::workers()` 暴露 WebWorkerHandle，使后台线程能把拥有权数据投递到稳定 Web worker，并在 `WebWorkerContext` 内使用该 worker 的 DB/Redis。
3. 实现 worker-bound sleep。
4. 实现创建期预分配、短临界区 mutex 保护的有界 Channel。
5. 实现 OneShot 和 `waitFor()` 单胜者状态机。
6. 接入 worker shutdown 注册、关闭和 mailbox drain；停止时拒绝新 Web 作业，等待已接受作业后再关闭 DB/Redis；未捕获作业异常触发 App 全局停止并由 `run()` 重抛。
7. 开放每 worker mailbox capacity，并提供 accepted/full/stopping/completed/failed/outstanding 统计。
8. 增加安装包/下游消费者编译测试。

验收：

- 满足 [原语规范第 10 节](ruvia-concurrency-primitives.md#10-验证门禁) 的全部 0.1.2 测试。
- 不新增通用 `withTimeout` 或 detached spawn；应用自有 `EventLoop` 可取得 executor/`io_context`，Web worker 只能通过有界 `WorkerHandle::post()`/`WebWorkerHandle::post()` 投递。
- 普通 HTTP route dispatch 不新增 mutex 或共享控制块分配。
- `git diff --check`、Debug build、ctest、install、package consumer 全通过。

### I1：DeviceEngine 与命令闭环

状态：待开始；decoder 可与 R1 并行，OneShot 接入依赖 R1。

任务：

- 使用 `EventLoopPool` 建立 engine loops，在 `EventLoop::ioContext()` 上创建 acceptor/client session 和串行 write queue，并通过 `onStop()` 关闭网络资源。
- DeviceSession 固定所属 EventLoop，完成心跳、idle、重连和 framing。
- CommandRegistry 使用分片线程安全 map。
- 接入 `device_command` 持久状态。
- HTTP 快速回执路径使用 OneShot；超过同步窗口返回 202。
- 处理 timeout/complete 并发、迟到回执和重复回执。
- 增加完成态 90 天归档/删除任务和 pending/sent 修复任务。

验收：

- 正常、超时、迟到、重复、离线和重连回执均有确定状态。
- registry erase/complete 压力测试无泄漏、死锁或丢持久回执。
- HTTP timeout 不取消已经发往设备的命令。

### I2：RealtimeAggregator 与 EventBus 第一阶段

状态：待开始；Aggregator 可与 R1 并行，WS Channel 接入依赖 R1。

任务：

- deviceId hash 到固定 pipeline shard。
- shard 内 latest map + dirty set，50–100ms/批量阈值 flush。
- 区分实时流、持久事件流和 UI 通知流。
- 实现 64/128 分片订阅表和 shared_mutex 技术债。
- 锁内只做非阻塞 send。
- 权限快照变更时关闭受影响连接并要求重连。
- 前端契约声明实时流不做 sequence gap 恢复、持久流跳号拉快照、跨流无序。

验收：

- 高频单设备只在每批输出一份最新状态。
- 多设备不会因合流互相覆盖。
- 告警/在线状态保持 durable sequence。
- 慢消费者内存有界，溢出有指标和确定策略。

### R2：Ruvia 0.1.3 结构化并发与 WS

状态：已完成。TaskScope、WebSocket 单 reader、close 复用 reader、幂等 abort 和 HTTP/2 stream-local reset 均已实现并通过回归测试。

任务：

1. 实现 TaskScope spawn/stop token/join 和异常传播。
2. WebSocket 增加单 reader guard 和 reader-state signal。
3. 修复 active reader 期间 close 的双重读。
4. 增加 worker-only、幂等的 `WebSocket::abort()`。
5. 保证 HTTP/2 abort 只 reset 当前 stream。
6. 覆盖慢 writer、reader EOF、close timeout 和 shutdown。

验收：

- 满足 [原语规范第 10 节](ruvia-concurrency-primitives.md#10-验证门禁) 的全部 0.1.3 测试。
- handler 返回前所有 WS 子任务已 join。
- writer 异常后 reader 在测试 deadline 内退出。
- HTTP/1 和 HTTP/2 行为一致且不扩大 abort 范围。

### I3：WebSocket 正式推送

状态：待开始；R2 前置条件已满足。

任务：

- 每连接一个 reader 主协程和一个 writer TaskScope 子任务。
- writer catch 后 `ws.abort()` 并重新抛出。
- reader 退出后关闭 subscription、requestStop、join。
- 增加批量 JSON 序列化和写出指标。
- 保留 SSE 作为降级和单向订阅接口。

验收：

- 实时批次、持久事件、客户端控制帧和权限失效均正确。
- writer 阻塞、客户端不读、客户端半关闭和网络中断不悬挂连接。
- 长时间压测无 unbounded queue、遗留 Task 或连接泄漏。

### I4：WebhookRuntime

状态：schema/outbox 可在 M0 开始，transport 可独立并行。

任务：

- event_types 过滤后写 transactional outbox。
- 实现 Asio DNS/TCP/TLS transport 和 per-origin HTTP/1.1 pool。
- 使用 `asio::ssl::host_name_verification` 和 SNI。
- 使用 `ruvia-http` writer/parser 和 body framing plan。
- 实现 DNS/connect、TLS handshake、write、read、total timeout。
- 实现同 origin redirect、SSRF 防护、body limit、retry、dead-letter。
- 保存 URL/签名配置快照或不可变版本引用。
- 如果自研 transport 阻塞主线，临时以 libcurl adapter 替代。

验收：

- HTTPS hostname、证书失败、TLS timeout、chunked、content-length、close-delimited 响应均覆盖。
- 网络错误、408、429、5xx 和普通 4xx 使用正确重试策略。
- 服务重启后未完成 outbox 可恢复。
- 无匹配订阅的遥测不产生 outbox 行。
- SSRF 和 redirect 绕过测试通过。

### I5：性能收敛与第二阶段评估

状态：I2/I3 稳定后按指标决定，不默认实施。

采集：

- Channel lock 等待时间、full/drop 计数和 batch drain 大小。
- 每事件 fan-out 次数、每 worker wake 次数。
- WS 队列长度、写延迟和慢消费者断开数。
- pipeline 合流前后事件数量。
- Webhook pool 命中率、请求延迟、重试和 dead-letter。

只有端到端压测证明第一阶段订阅锁或 per-connection fan-out 是瓶颈时，才设计 worker service 生命周期和每 worker mailbox；不能仅凭微基准扩大公开 API。

## 3. 并行关系

```text
M0 ───────────────┬───────────────┬─────────────┐
                  │               │             │
R1 -> I1 command  │               │             │
  \-> I2 channel ├-> R2 -> I3 WS │             │
                  │               │             │
Webhook schema ───┴-> I4 runtime  │             │
                                  └-> I5 measure
```

关键路径只有正式 WS 双工依赖 R2；CRUD、schema、DeviceEngine decoder、SSE、命令 202、RealtimeAggregator 和 Webhook outbox 都可以提前完成。

## 4. 统一验证矩阵

| 风险 | 必测场景 |
|---|---|
| worker 亲和 | completion 来自 engine 线程，handler 仍在原 worker 恢复 |
| 生命周期 | receiver、worker、Sender 以所有顺序销毁 |
| Channel | reject/drop-oldest、多 producer、慢 consumer、shutdown |
| OneShot | timeout/complete 同时、迟到、重复、registry 已 erase |
| WS | writer 异常、reader EOF、慢 write、graceful close、abort、HTTP/2 stream |
| 合流 | 单设备高频、多设备并行、批量阈值、实时流无快照风暴 |
| 持久事件 | durable sequence 跳号恢复、跨流无序 |
| 权限 | 共享/角色变化后旧连接被终止并重新鉴权 |
| 命令 | 200/202、Location、离线、超时、迟到、保留清理 |
| Webhook | TLS、timeout、framing、retry、outbox 恢复、SSRF |

## 5. 完成定义

一个里程碑只有同时满足以下条件才能标记完成：

- 公开 API、线程所有权和错误状态与定稿文档一致。
- 不包含未记录的 `detail` 依赖或跨线程对象借用。
- 任务相关单元、集成和压力测试通过。
- `git diff --check` 通过。
- Ruvia 改动完成 Debug build、ctest、install 和 package-consumer 验证。
- schema/API 改动同步迁移、OpenAPI、保留策略和运维指标。
- shutdown、timeout、慢消费者和迟到回调均有有限生命周期。
