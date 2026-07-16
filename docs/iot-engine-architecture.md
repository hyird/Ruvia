# iot-engine 架构总方案

> 文档状态：定稿（2026-07-14）
>
> 适用基线：Ruvia 0.1.1；并发能力按 Ruvia 0.1.2/0.1.3 演进
>
> 相关文档：[Ruvia 并发原语规范](ruvia-concurrency-primitives.md)、[实施计划](iot-engine-implementation-plan.md)

## 1. 目标与边界

iot-engine 是单进程、生产者—消费者架构的物联网平台后端，由 Ruvia Web、TimescaleDB/PostgreSQL、Redis 和应用自有的设备、事件及 Webhook runtime 组成。

职责固定如下：

- Ruvia 负责 HTTP/WebSocket/SSE runtime，以及 worker 亲和的调度、等待和结构化并发原语。
- iot-engine 负责南向 raw TCP、设备协议、事件聚合、命令持久化、Webhook 传输和产品策略；长批处理 runtime 可自持数据库连接，普通后台业务可投递到 Ruvia Web worker 查询。
- `ruvia-http` 提供 Webhook 所需的 sans-I/O HTTP client 协议能力；DNS、TCP、TLS、timeout 和连接池由 iot-engine 在应用自有 `EventLoopPool` 上驱动。
- `c.db()` 和 `c.redis()` 只服务所属 Ruvia worker；后台线程不得借用这些 handle，而应使用 `App::workerFor(key).post()`，在 `WebWorkerContext` 内重新取得目标 worker 的 DB/Redis handle。
- 单进程内事件不经 Redis 绕行。Redis 只承担跨实例协调、短期缓存、限流和分布式部署后的消息能力。

## 2. 总体结构

```text
TCP devices <-> DeviceEngine/DeviceSession
                       |
                       +-> CommandRegistry -> HTTP handlers
                       |
                       +-> pipeline shards -> RealtimeAggregator -> EventBus -> WS/SSE
                                           |
                                           +-> TimescaleDB
                                           +-> filtered webhook outbox -> WebhookRuntime
```

线程所有权：

- 每个 Ruvia Web worker 独占一个不向业务公开的 `io_context` 和它的 HTTP/WS 连接。
- iot-engine 通过 `EventLoopPool` 创建应用自有 loop；每个 loop 公开 `ioContext()`/executor，DeviceSession 从建立到关闭固定归属一个 loop。
- 每个 pipeline shard 按 `deviceId` hash 固定拥有一组设备事件。
- WebhookRuntime 使用独立 `EventLoopPool`（或与 DeviceEngine 明确分组的应用 loop），不阻塞 Ruvia Web worker 或 pipeline。
- socket、连接状态机、协议 decoder、数据库连接都不得跨所有者线程访问。

跨线程交互只能经过 Ruvia Channel/OneShot、DeviceEngine command mailbox 或其他明确的有界 mailbox。

## 3. 事件模型

### 3.1 事件分类

实时遥测和持久事件使用独立语义：

| 流 | 内容 | 队列语义 | sequence 语义 |
|---|---|---|---|
| 实时遥测流 | 温度、压力、瞬时量等全量最新状态 | 按设备合流，可覆盖旧值 | 可以携带 revision 供观察，但跳号不触发快照 |
| 持久事件流 | 告警产生/恢复、设备上线/离线、命令状态 | 有序 FIFO，不静默丢弃 | 独立 durable sequence；跳号时拉取持久状态快照 |
| UI 通知流 | 刷新提示、非关键进度 | 允许合并或丢弃旧通知 | 不承诺连续 sequence |

不同流之间不保证顺序。前端不得假设“触发告警的实时值批次”一定先于或后于告警事件到达；需要关联时使用事件时间、设备标识和业务事件 ID。

事件 envelope 不预设租户字段：

```cpp
struct DeviceEvent {
    DeviceId deviceId;
    DeviceEventType type;
    Timestamp occurredAt;
    Payload payload;
};

struct DurableDeviceEvent {
    std::uint64_t durableSequence;
    EventId eventId;
    DeviceId deviceId;
    DeviceEventType type;
    Timestamp occurredAt;
    Payload payload;
};
```

### 3.2 RealtimeAggregator

FIFO Channel 不承担 keyed conflation。每个 pipeline shard 单线程维护：

```cpp
std::unordered_map<DeviceId, DeviceRealtimeState> latest;
std::unordered_set<DeviceId> dirty;
```

处理规则：

1. 设备按 `deviceId` hash 固定归属 pipeline shard。
2. 新实时值覆盖 `latest[deviceId]`，并把 deviceId 加入 `dirty`。
3. 每 50–100ms 或达到批量阈值时 flush。
4. 每个 dirty device 只输出一份全量最新状态。
5. 清空 dirty 集合，发布 `RealtimeBatch`。

实时批次是全量最新状态而不是增量补丁，因此合流产生的源事件序号空洞不会触发客户端快照。

### 3.3 EventBus 第一阶段

第一阶段使用 64 或 128 个订阅分片，每个分片一个 `shared_mutex`：

- WS/SSE 建立和关闭时以独占锁增删订阅。
- pipeline publish 时以共享锁读取相应分片。
- 锁内只允许执行非阻塞 `ChannelSender::send()`；不得序列化 JSON、访问数据库或等待 I/O。
- 每连接队列有界，溢出策略按消息类别配置。

这是第一阶段明确接受的跨线程技术债，不属于普通 HTTP 请求处理路径。第二阶段在压测证明需要后改为每 worker 一个 mailbox：pipeline 每个事件至多向每个 worker 投递一次，worker 在线程内无锁 fan-out。第二阶段需要 worker service 生命周期能力，不纳入 Ruvia 0.1.2。

### 3.4 权限快照失效

订阅建立时解析用户可访问设备集合，该集合只是权限快照。

选择强制重连策略：

- `device:share:changed`、角色调整、部门设备关系变化时定位受影响的用户/部门订阅。
- WebSocket 使用私有 close code `4003` 和 `authorization changed` 原因关闭。
- SSE 直接结束流。
- 客户端重新鉴权、重连并重新提交订阅条件。

不在长连接中增量修补权限集合，以避免撤权窗口和多套权限状态并存。

## 4. WebSocket 双工

每个连接保持一个 application reader 和一个 application writer：

- reader 在 handler 主协程持续调用 `ws.read()`。
- writer 在 `TaskScope` 子任务等待 EventBus Channel 并调用 `ws.text()`。
- 两个 application writer 或两个 reader 都属于逻辑错误。
- writer 捕获任意异常后调用 `ws.abort()` 并重新抛出，以有限时间唤醒阻塞的 reader。
- reader 退出后关闭 subscription、请求 TaskScope 停止并 `join()`。
- 慢客户端不得造成无界积压；队列满按消息类别拒绝新消息或丢弃最旧 UI/实时批次。

Ruvia 必须修复 active reader 期间 `close()` 再次调用 `read()` 的双重读问题，详细规范见 [Ruvia 并发原语规范](ruvia-concurrency-primitives.md)。

正常 reader EOF 后，pending writer 应由 transport 关闭或 WS 状态变化唤醒；close-handshake timeout 是最终兜底。集成测试必须覆盖 writer 正阻塞于慢客户端写入时 reader 退出的场景。

## 5. 设备命令

### 5.1 状态模型

```text
pending -> sent -> acknowledged
                -> timeout
                -> failed
```

新增 `device_command`：

| 字段 | 说明 |
|---|---|
| `id` | 全局唯一 commandId |
| `device_id` | 目标设备 |
| `command_type` | 命令类型 |
| `request_payload` | JSONB 请求快照 |
| `response_payload` | JSONB 回执，允许空 |
| `status` | pending/sent/acknowledged/timeout/failed |
| `correlation_id` | 设备协议关联号 |
| `idempotency_key` | 调用方幂等键 |
| `created_at/sent_at/completed_at/expires_at` | 生命周期时间 |
| `error_code/error_message` | 失败摘要 |
| `version` | 乐观并发版本 |

索引至少包括 `(device_id, created_at desc)`、`(status, expires_at)`，以及存在幂等键时的 `(device_id, idempotency_key)` 唯一约束。

### 5.2 HTTP 契约

- `POST /open-api/device/command` 创建命令并返回 commandId。
- 同步窗口内收到回执时返回 `200`。
- 命令已接受但尚未回执时返回 `202`，并设置指向状态接口的 `Location`。
- `GET /open-api/device/command/{commandId}` 查询最终状态。
- HTTP 等待超时不等于取消设备命令；迟到回执仍更新 `device_command`。

### 5.3 CommandRegistry 竞争

CommandRegistry 使用分片锁或等价线程安全 map；`erase`、`take/complete` 必须幂等：

- handler timeout 只移除内存 waiter，不删除持久命令。
- engine 回执以原子 map 操作取出 waiter，在锁外调用 OneShot completion。
- timeout 和 complete 同时发生时，由 OneShot 决定唯一胜者。
- complete 发现 waiter 已移除时记录 late-reply 指标，仍写入 `device_command`。
- erase 发现条目已被 complete 取走时直接成功返回。

### 5.4 保留策略

- acknowledged/timeout/failed 完成态默认在线保留 90 天。
- 清理作业按月归档或删除过期完成态，保留必要审计字段和聚合指标。
- pending/sent 不参与按年龄直接删除；先由超时修复任务转为终态。
- 清理与现有 device data 归档作业使用相同的调度、指标和失败告警机制。

## 6. DeviceEngine

DeviceEngine 使用应用自有 Asio runtime：

- 每个 DeviceSession 固定 executor。
- 每连接一个 read loop 和一个串行 write queue。
- protocol decoder 只借用当前 read buffer；交给 pipeline 前必须复制或转成拥有权对象。
- HTTP worker 通过 command mailbox 投递命令。
- DeviceSession 通过 OneShot completion 返回回执，不直接操作 Context 或 WebSocket。
- 连接、心跳、idle timeout、重连和半包/粘包处理全部属于 DeviceEngine。

## 7. Webhook

### 7.1 可靠投递

Webhook 使用 PostgreSQL transactional outbox。只有事件类型匹配启用订阅的 `event_types` 时才写 outbox，普通遥测没有匹配订阅时不得产生 outbox 行。

`webhook_outbox` 至少包含：subscription ID、event ID/type、目标 URL 快照、签名配置版本、payload、状态、尝试次数、下次重试时间、最后 HTTP 状态/错误、创建和完成时间。

目标 URL 和签名配置必须快照或引用不可变版本，避免重试期间配置变化改变既有投递语义。

### 7.2 Client runtime

WebhookRuntime 使用：

- Asio resolver、TCP socket 和 timer。
- `asio::ssl::stream`、SNI 和 `asio::ssl::host_name_verification`。
- `ruvia-http` 的 `HttpClientRequest`、`Http1ClientRequestWriter`、`Http1ClientResponseParser`、body framing 和 redirect helper。

首版范围：HTTP/1.1、POST JSON、固定 endpoint、默认只允许 HTTPS、同 origin redirect 最多 3 次、response body 最大 1MB、每 origin 最多 4 条连接。

超时必须分别覆盖 DNS/connect、TLS handshake、write、read 和 total deadline。网络错误、408、429、5xx 可重试；普通 4xx 不重试。POST 使用稳定的 Idempotency-Key。

必须防御 SSRF：解析前和 DNS 后检查地址，默认拒绝 loopback、link-local、metadata 和未授权私网；每次 redirect 重新校验。

若自研 runtime 影响主线排期，可以用 libcurl 实现同一 transport adapter；outbox、重试、安全和上层接口不变。

## 8. 数据库和 Redis

- HTTP handler 使用 `c.db()`/`c.redis()`。
- 普通后台业务按 deviceId 等稳定 key 投递到 `WebWorkerHandle`，在 `WebWorkerContext` 内使用该 Web worker 的 DB/Redis；必须处理 mailbox 满和 worker stopping。
- 持续高吞吐、长事务或需要独立并发/背压预算的 pipeline、DeviceEngine、WebhookRuntime 仍使用应用自持的 PostgreSQL/Redis 连接或连接池，避免占满 Web worker。
- 每个后台连接槽固定 executor，不跨线程共享 libpq connection。
- Redis 不用于进程内普通 WS fan-out、每连接 BLPOP、timer 或命令 OneShot。

## 9. Ruvia 0.1.1 过渡方案

Ruvia 新原语发布前：

- 实时推送使用 SSE 和公开的 stream sleep。
- 命令接口返回 `202 + commandId`，客户端轮询或通过 SSE 接收结果。
- Webhook outbox、DeviceEngine、schema、认证、RBAC 和 CRUD 可以并行开发。
- 业务代码不得包含 `ruvia::detail` 或调用 `detail::asyncStartTask`。

完整交付顺序和验收门禁见 [实施计划](iot-engine-implementation-plan.md)。
