# Ruvia worker runtime 与并发原语规范

> 文档状态：定稿并开始实施（2026-07-14）
>
> 相关文档：[iot-engine 架构总方案](iot-engine-architecture.md)、[实施计划](iot-engine-implementation-plan.md)

## 1. 分层与不变量

通用 runtime 能力属于 `ruvia-core`；`ruvia-web` 只把所属 Web worker 的句柄暴露给应用。`ruvia-http` 保持 sans-I/O，不依赖 core 或 Asio。

必须保持：

- `Runtime` 可以创建应用自有 worker；`App::run()` 仍创建 Web worker。
- 连接、Context、DB handle 和 Redis handle 不跨 worker。
- 外部线程通过有界 mailbox 投递，不直接恢复协程或操作连接。
- 普通请求热路径不新增锁或共享控制块分配。
- 公开 API 不暴露 `asio::io_context`、socket 或 `ruvia::detail`。
- 不提供 detached Ruvia Task，也不实现不安全的通用 `withTimeout(Task<T>)`。

## 2. Runtime 与 WorkerHandle

```cpp
ruvia::Runtime runtime({
    .workerCount = 4,
    .mailboxCapacity = 1024,
});
runtime.start();

auto worker = runtime.workerFor("device-42");
auto result = worker.post([state = std::move(state)]() mutable {
    // 始终在目标 worker 执行
});
```

`Runtime` 是应用自有运行时，负责 worker 的创建、启动、停止和 join。每个 worker 拥有独立 `asio::io_context` 和线程。

`WorkerHandle` 是可复制的安全句柄：

- `post(fn)` 是通用公开 API，语义对应 event-loop 的 queue-in-loop。
- 返回 `kAccepted`、`kQueueFull` 或 `kWorkerStopping`，调用方必须处理背压。
- 支持 move-only callable。
- `isCurrent()` 用于断言线程亲和；`id()` 用于诊断。
- 不暴露 `run()`、`stop()`、裸 executor 或 `io_context`。
- 句柄晚于 worker 销毁仍可安全调用，返回 stopping。

Web 侧提供两种入口：

```cpp
ruvia::WorkerHandle current = c.worker();
std::vector<ruvia::WorkerHandle> webWorkers = ruvia::app().workers();
```

应用可在 `onStart` 中取得 `App::workers()`，按 deviceId 稳定选择 Web worker。外部线程只捕获拥有权数据；投递到 Web worker 后再获取该 worker 自己的 DB client，禁止跨线程捕获 `Context&`、`DbHandle` 或连接对象。

## 3. Mailbox

公开 `post()` 使用创建期预分配的有界 FIFO ring。MPSC 边界允许一把短临界区 mutex：

- 锁内只做停止/容量判断、slot move 和索引更新。
- callable 在 worker 上、锁外执行。
- 一批任务由单个 drain handler 排空。
- 不提供 `drop-oldest`；满时只拒绝新任务。
- 未捕获异常离开 callable 时，worker 记录失败并进入停止流程。

这把锁只存在于显式跨线程投递边界，不进入普通 route dispatch。

## 4. Timer

```cpp
co_await ruvia::sleepFor(c.worker(), 100ms);
```

每个 Runtime worker 只拥有一个底层 `steady_timer`。`sleepFor`、OneShot timeout、连接扫描和 graceful drain 都向同一个 worker deadline queue 注册，不得各自创建 Asio timer。deadline queue 维护最小截止时间并统一重设唯一 timer。DB/Redis deadline 和遗留 Web stream timeout 将继续迁入该队列。

调用必须发生在目标 worker；duration 小于等于零立即完成。不会增加通用 `withTimeout`：现有 `Task` 没有统一取消协议，输掉超时竞争的任意 Task 不能被安全销毁。

## 5. Channel

```cpp
auto [sender, receiver] = ruvia::makeChannel<Event>(worker, 256, c.resource());

switch (sender.send(std::move(event))) {
case ruvia::ChannelSendResult::kSent: break;
case ruvia::ChannelSendResult::kFull: /* 慢消费者策略 */ break;
case ruvia::ChannelSendResult::kClosed: break;
case ruvia::ChannelSendResult::kWorkerStopping: break;
}

auto event = co_await receiver.receive(); // 明确区分 value/closed/worker-stopping
```

首版是单 consumer、有界 FIFO、`kRejectNewest`：

- 控制块和 ring 在创建期一次分配，默认使用进程级 mimalloc PMR，也可传请求/业务 PMR。
- producer 可来自任意线程；receiver 只能在绑定 worker 使用。
- `send()` 不直接恢复协程，而是由 worker continuation 唤醒。
- receiver close/析构幂等并唤醒 pending receive。
- Channel 不做按 key 合流；实时设备状态由 `RealtimeAggregator` 的 latest map + dirty set 完成。

后续的 `receiveFor()` 必须在 Channel 自有 waiter 内完成 timeout/message 单胜者，不复用通用 Task 超时包装器。

## 6. OneShot

已公开：

```cpp
auto [completion, receiver] = ruvia::makeOneShot<Ack>(worker);
auto result = co_await receiver.waitFor(2s);
```

- completion 可从任意线程调用。
- 重复 completion 不覆盖首个结果，返回 already-completed。
- wait/waitFor 只能在绑定 worker。
- timeout、completion、close 由控制块状态机保证单胜者。
- 超时不取消已经发往设备的命令；迟到回执仍持久化并计指标。

## 7. TaskScope

```cpp
ruvia::TaskScope scope(c.worker(), c.resource());
scope.spawn(writerLoop(scope.stopToken()));

// reader 结束或连接失败
scope.requestStop();
co_await scope.join();
```

`TaskScope` 属于 core，只管理协程生命周期：

- `spawn()` 只能在绑定 worker，子 Task 由 scope 持有，不 detached。
- 首个子任务异常会请求协作停止；`join()` 等待全部子任务并重抛首个异常。
- `requestStop()` 通过 `std::stop_token` 表达协作停止，不强制销毁仍被底层 callback 引用的帧。
- scope 有活动任务却析构会终止进程，强制调用方在拥有的 Context/WebSocket 销毁前 join。
- TaskScope 不知道 HTTP、WebSocket 或 DB；transport 中断由 Web 层负责。

## 8. WebSocket 双工约束

`ruvia-web` 已实现：

- 增加单 reader guard，第二个 application read 明确失败。
- close 复用现存 reader，禁止关闭路径发起第二个 read。
- 增加 worker-only、幂等的 `WebSocket::abort()`。
- HTTP/1 abort 当前 socket；HTTP/2 只 reset 当前 stream。
- writer 捕获异常后先 `ws.abort()` 唤醒 reader，再把异常交给 TaskScope。

## 9. Shutdown

顺序固定为：

1. mailbox 停止接受新的公开 post/send。
2. 停止 accept，新连接不再进入。
3. 活跃 handler 通过内部 continuation 完成 close/join。
4. 排空已接受 mailbox 工作和连接。
5. 关闭 DB/Redis、销毁 `io_context` 与 worker memory。

内部 continuation 与公开 mailbox 分开：graceful drain 期间公开投递已拒绝，但现有 TaskScope 子任务仍能完成并唤醒 join。

## 10. 验证门禁

- Runtime worker 线程亲和、稳定分片、round-robin。
- move-only post、队列满、停止后 post、过期句柄。
- posted callable 异常传播。
- TaskScope 完成、首异常、stop token、join。
- timer continuation 保持 worker 亲和。
- Channel send-before-receive、receive-before-send、full、close、跨线程 producer。
- OneShot timeout/completion/close 单胜者。
- WebSocket writer 异常后 reader 有限时间退出，慢 writer 和 HTTP/2 stream-local abort。
- Debug build、ctest、install、package consumer 与边界守卫全通过。
