# Runtime and blocking work

[Documentation index](../README.md#contents)

## Core Runtime

Use `EventLoopPool` for application-owned event loops. Submit cross-thread work
through bounded `post()` and handle rejection:

```cpp
#include <ruvia/core/EventLoopPool.h>

ruvia::EventLoopPool loops({.loopCount = 4, .queue_capacity = 1024});
loops.start();
auto loop = loops.loopFor("device-42");
auto posted = loop.post([] { /* work on the selected loop */ });
if (!posted.accepted()) {
    auto rejected = std::move(posted).takeRejected();
    // Retry later or persist the work.
}
loops.stop();
loops.join();
```

`loop.start(task)` returns a `RootTask<T>`; keep it and call `get()` to wait for
the result or exception. Within coroutines, use `co_await`. `TaskScope`,
`Channel`, `OneShot`, `sleepFor`, and `StopSource` support structured background
work and cancellation.

Register asynchronous resource cleanup with `loop.onStop()` and keep its
registration alive. The callback must close I/O and join pending operations
before returning. `loops.join()` waits for retirement and rethrows the first
runtime failure. Do not call `run()`, `stop()`, or `restart()` on a pool-owned
`io_context`.

### Existing Asio applications

```cpp
#include <ruvia/core/EventLoopAttachment.h>

asio::io_context io;
auto attachment = ruvia::attachEventLoop(io);
auto loop = attachment.loop();
std::thread thread([&] { io.run(); });
loop.post([] { /* application work */ });
attachment.stop();
thread.join();
```

Continue driving the external context until managed operations and cleanup
finish. The application owns that context and thread; destroy them only after
retirement.

### Standalone clients

HTTP, WebSocket, database, and Redis clients can bind directly to `loop`, without
an App. Construction performs no I/O. HTTP connects on its first request;
the other clients require `co_await client.connect()`.

1. Construct clients on their chosen loop and start the pool.
2. Await every client's readiness before admitting work.
3. Run operations on that loop; keep results within their documented lifetime.
4. Await `client.shutdown()` before releasing clients, then stop/join the pool.

Loop shutdown also cancels and joins registered client work. `close()` (or
WebSocket `abort()`) only requests cancellation and may be called across threads.
See [event_loop_data.cpp](../examples/web/event_loop_data.cpp) for PostgreSQL
and Redis, and [outbound clients](outbound-clients.md) for HTTP and WebSocket.

## Web workers

Configure `server_config::worker_count` and `worker_queue_capacity` before
`App::run()`. Connections stay on their assigned worker. Background producers
select a worker and post owned data:

```cpp
auto worker = ruvia::app().workerFor("device-42");
auto posted = worker.post(
    [event = std::move(event)](ruvia::WebWorkerContext& c) mutable
        -> ruvia::Task<void> {
        co_await persistEvent(c.db(), event);
    });
```

Handle `kQueueFull`; inspect `worker.stats()` for queue and completion counts.
Use `WebWorkerContext::stopToken()` or cancellable waits so shutdown can finish.
An uncaught job exception stops the App and is rethrown by `run()`.

`onStart()` runs before request admission; wait for asynchronous initialization
before returning from the hook. Failure or cancellation prevents serving.
`onStop()` runs after admission closes and before join. Both hooks run on the
thread calling `App::run()`; other threads use `App::stop()` to request shutdown.

## Blocking Work

Move blocking or CPU-heavy work to the bounded pool. Capture owned values:

```cpp
ruvia::app().blockingPool({.threadCount = 8, .queueCapacity = 512});

ruvia::Task<ruvia::HttpResponse> hash(ruvia::Context& c) {
    const auto body = co_await c.req().text();
    auto digest = co_await c.runBlocking(
        [input = std::string(body)] { return argon2Hash(input); });
    co_return c.text(std::string_view(digest));
}
```

`runBlocking()` rethrows callable errors and throws `BlockingOperationRejected`
when full (503 with the default error handler). `tryRunBlocking()` returns a
status instead. Both accept a wait deadline as the first argument, such as
`c.runBlocking(std::chrono::seconds(2), fn)`.

Timeout and shutdown stop waiting; they cannot interrupt the callable. Its
captures must survive independently of requests and workers. App shutdown does
not wait for running blocking calls. Standalone pool owners can use
`BlockingPool::join()` when they need that completion barrier.

The default pool uses half the logical CPUs, clamped to 2–8 threads, with 64
queued tasks per thread. Disable it with `blockingPool(nullptr)`; monitor
`App::blockingPoolStats()` when tuning capacity.

## Lifetime rules

- Await or join every started task, including after cancellation. An unstarted
  lazy task may be discarded.
- Keep worker-bound clients, handles, operations, and results on their worker;
  release them before their owner or memory resource expires.
- Request views and `Context` handles cannot escape the request. Copy data before
  posting or offloading it. A body chunk or WebSocket payload expires at the
  next read on that stream/connection.
- Use `c.arena()` for request-lifetime data and `c.pool()` for reclaimable
  temporary PMR containers. Moving a value does not extend its allocator lifetime.
- Cancellation is a request to stop; await cleanup before destroying borrowed
  resources. Outbound response lifetime exceptions are documented
  [with their APIs](outbound-clients.md#response-bodies).

See [workers_blocking.cpp](../examples/web/workers_blocking.cpp) and
[runtime_config.cpp](../examples/web/runtime_config.cpp).
