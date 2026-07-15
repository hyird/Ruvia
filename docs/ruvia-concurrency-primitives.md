# Ruvia worker runtime 与并发原语规范

> 文档状态：定稿并开始实施（2026-07-14）
>
> 相关文档：[iot-engine 架构总方案](iot-engine-architecture.md)、[实施计划](iot-engine-implementation-plan.md)

## 1. 分层与不变量

通用 runtime 能力属于 `ruvia-core`；`ruvia-web` 只把所属 Web worker 的句柄暴露给应用。`ruvia-http` 保持 sans-I/O，不依赖 core 或 Asio。

必须保持：

- `EventLoopPool` 可以创建应用自有 event loop；`App::run()` 仍创建受限的 Web worker。
- 连接、Context、DB handle 和 Redis handle 不跨 worker。
- 外部线程通过有界 mailbox 投递，不直接恢复协程或操作连接。
- 普通请求热路径不新增锁或共享控制块分配。
- 应用自有 `EventLoop` 公开 `asio::io_context` 和 executor；Web worker 不公开二者。
- pool-owned `io_context` 的 `run/stop/restart` 生命周期只由 `EventLoopPool` 控制。
- 不提供 detached Ruvia Task，也不实现不安全的通用 `withTimeout(Task<T>)`。

## 2. EventLoopPool、EventLoop 与 WorkerHandle

```cpp
ruvia::EventLoopPool loops({
    .loopCount = 4,
    .mailboxCapacity = 1024,
});
loops.start();

auto loop = loops.loopFor("device-42");
asio::ip::tcp::socket socket(loop.ioContext());
auto result = loop.post([state = std::move(state)]() mutable {
    // 始终在目标 event loop 执行
});

auto stopRegistration = loop.onStop([&socket] {
    std::error_code ignored;
    socket.close(ignored);
});
```

`EventLoopPool` 负责应用 event loop 的创建、启动、停止和 join。每个 `EventLoop` 拥有独立 `asio::io_context` 和线程，公开 `ioContext()` 与 `executor()`，供应用创建 TCP、UDP、DNS、TLS 等异步对象。连接创建后固定归属该 loop，不得迁移。

`EventLoop::onStop()` 返回 move-only registration。业务资源存活期间必须保留 registration；pool 停止时回调在所属 loop 线程执行，应用在其中 cancel/close acceptor、socket、resolver 和 TLS stream。回调必须尽快返回且不得依赖异常传播；框架会忽略 stop callback 异常并继续关闭。应用不得对 pool-owned `io_context` 调用 `run()`、`stop()` 或 `restart()`。

`ioContext()`/`executor()` 用于构造和驱动归属于该 loop 的 Asio I/O 对象，不是无界任务入口。外部线程提交普通业务任务仍必须使用有界 `EventLoop::post()`；直接 `asio::post(loop.executor(), ...)` 绕过 mailbox 的任务不享受背压、拒绝新任务和停机可观测性保证。

已有 Asio runtime 可以接入同一套原语：

```cpp
asio::io_context io;
auto attachment = ruvia::attachEventLoop(io, {
    .mailboxCapacity = 1024,
});
auto loop = attachment.loop();

std::thread thread([&] { io.run(); });
// 使用 loop.post()/handle()/onStop()。
attachment.stop();
thread.join();
```

`EventLoopAttachment` 不创建线程，不调用外部 context 的 `run/stop/restart`，也不取得这些生命周期操作的所有权。attachment 存活期间持有 work guard；`stop()` 关闭 mailbox、在 owner thread 投递 stop callback 和 timer 清理，然后释放 work guard，但不会调用 `io_context::stop()`，以免终止 context 上与 Ruvia 无关的工作。

外部 context 必须晚于 attachment、所有 `EventLoop` 副本和其 Asio 对象销毁；一个 context 同时只能有一个 Ruvia attachment，重复绑定会抛 `std::invalid_argument`。调用方必须在 context 仍能 drain handler 时显式 `attachment.stop()`，然后才可停止或 join 自己的 runtime。外部 context 仍必须保证单线程 `run()`，以维持一个 worker 一个 owner thread 的不变量。

`EventLoop::handle()` 返回可复制的 `WorkerHandle`，供 `sleepFor`、`Channel`、`OneShot`、`TaskScope` 等 worker-bound core 原语使用。Web handler 的 `Context::worker()` 也只返回这种受限句柄：

- `post(fn)` 是通用公开 API，语义对应 event-loop 的 queue-in-loop。
- 返回 `kAccepted`、`kQueueFull` 或 `kWorkerStopping`，调用方必须处理背压。
- 支持 move-only callable。
- `isCurrent()` 用于断言线程亲和；`id()` 用于诊断。
- 自身不暴露 executor 或 `io_context`，因此不能借 Web worker 创建任意网络 runtime。
- 句柄晚于 worker 销毁仍可安全调用，返回 stopping。

Web 侧提供普通请求和后台作业两种入口：

```cpp
ruvia::WorkerHandle current = c.worker();
ruvia::WebWorkerHandle target = ruvia::app().workerFor(deviceId);

auto result = target.post(
    [event = std::move(event)](
        ruvia::WebWorkerContext& workerContext) mutable -> ruvia::Task<void> {
        auto db = workerContext.db();
        co_await persist(db, std::move(event));
    });
```

`App::workerFor(uint64_t/string_view)` 按 key 稳定选择 Web worker，`App::workers()` 返回全部 `WebWorkerHandle`。`WebWorkerHandle::post()` 只接受返回 `Task<void>` 的回调，回调收到仅在该作业内有效的 `WebWorkerContext`，可访问目标 worker、PMR resource、shutdown stop token，以及该 worker 自己的 DB/Redis handle。外部 producer 不暴露 core post 逃生口，确保所有已接受 Web 作业都进入 shutdown drain、失败传播和资源生命周期统计；作业内可用 `WebWorkerContext::worker()` 驱动 timer、`TaskScope` 等 worker-bound core 原语。

`App::setWorkerMailboxCapacity()` 在启动前配置每 worker 有界队列，默认 1024；所有 producer 必须处理 `kQueueFull`。`WebWorkerHandle::stats()` 提供 accepted、queue-full、worker-stopping、completed、failed 和 outstanding 计数，供应用接入指标系统。

外部线程只捕获拥有权数据，禁止跨线程捕获 `Context&`、`WebWorkerContext&`、`DbHandle`、`RedisHandle` 或连接对象。框架持有已接受作业的协程帧；作业完成后才释放。未捕获异常会关闭全部 App workers，并由 `App::run()` 重抛，禁止应用在单 worker 已失败后继续半死运行。shutdown 先停止接受 Web 作业并请求 stop，再等待已接受作业和活跃连接归零，最后关闭 DB/Redis。

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

每个 EventLoop/Web worker 只拥有一个框架 deadline queue 底层 `steady_timer`。`sleepFor`、OneShot timeout、连接扫描和 graceful drain 都向同一个 worker deadline queue 注册，不得各自为框架超时创建 Asio timer。应用通过 `ioContext()` 创建的协议 I/O 不改变该约束；业务超时应优先复用 worker-bound core 原语。deadline queue 维护最小截止时间并统一重设唯一 timer。DB/Redis deadline 和遗留 Web stream timeout 将继续迁入该队列。

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

auto event = co_await receiver.receive();
if (const auto* value = event.value()) {
    consume(*value);
} else if (event.closed()) {
    // producer 已关闭 Channel
} else if (event.workerStopping()) {
    // 绑定 worker 正在停止
}
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
if (const auto* ack = result.value()) {
    consume(*ack);
} else if (result.timedOut()) {
    recordTimeout();
}
```

- completion 可从任意线程调用。
- 重复 completion 不覆盖首个结果，返回 already-completed。
- wait/waitFor 只能在绑定 worker。
- timeout、completion、close 由控制块状态机保证单胜者。
- Channel receive 与 OneShot wait 共用封闭的 `WorkerWaitResult<T>`；只有
  `value()` alternative 暴露 payload，closed/stopping/timeout 无法携带伪值。
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

第 4 步同时等待已接受的 `WebWorkerHandle::post()` 作业。作业可观察 `WebWorkerContext::stopToken()` 协作退出；grace period 仍是最终上限。

任意 Task 没有强制取消语义，因此作业必须只等待 worker 原生、关闭时可唤醒的操作，或自行观察 stop token。永久挂在不可取消第三方 callback 上属于应用契约错误，框架不会销毁仍被 callback 引用的协程帧。

内部 continuation 与公开 mailbox 分开：graceful drain 期间公开投递已拒绝，但现有 TaskScope 子任务仍能完成并唤醒 join。

## 10. 验证门禁

- EventLoop 线程亲和、稳定分片、round-robin、原生 Asio 对象创建与 owner-thread stop callback。
- move-only post、队列满、停止后 post、过期句柄。
- posted callable 异常传播。
- Web worker 稳定选择、队列满、统计、DB/Redis context 编译面、停止后拒绝、异常触发 App 级联停止，以及 shutdown/grace deadline 排空已接受异步作业。
- TaskScope 完成、首异常、stop token、join。
- timer continuation 保持 worker 亲和。
- Channel send-before-receive、receive-before-send、full、close、跨线程 producer。
- OneShot timeout/completion/close 单胜者。
- WebSocket writer 异常后 reader 有限时间退出，慢 writer 和 HTTP/2 stream-local abort。
- Debug build、ctest、install、package consumer 与边界守卫全通过。
