# Ruvia

[![Build](https://github.com/hyird/Ruvia/actions/workflows/build.yml/badge.svg)](https://github.com/hyird/Ruvia/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hyird/Ruvia)](https://github.com/hyird/Ruvia/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Ruvia is a C++20 HTTP/Web framework built as three independently consumable
CMake targets. The repository is a monorepo, but its runtime foundation and
protocol library do not require the full Web framework.

## Highlights

- **Layered by design** — `ruvia::core` (runtime), `ruvia::http` (protocol),
  and `ruvia::web` (framework) install and import as independent package
  components.
- **Sans-I/O protocol library** — one HTTP/1, HTTP/2, WebSocket, and HPACK
  implementation shared by the server and the outbound client; callers feed
  bytes and consume typed events. No sockets, no Asio, no TLS inside.
- **Coroutine-first Web framework** — controllers, typed JSON
  models, validation, middleware, streaming, SSE, and WebSocket routes, all
  finalized at startup with no per-request rebuilding.
- **Bounded, application-owned runtime** — explicit workers, bounded mailboxes,
  backpressure at every producer, and deterministic shutdown cancellation and
  completion draining.
- **TLS out of the box** — server TLS with optional or required client-certificate
  policy, SNI identities, and connection metadata kept separate from the HTTP
  request model.
- **Optional integrations** — MariaDB, PostgreSQL, Redis, and JWT behind vcpkg
  features; both database drivers share one `DbHandle` API surface.
- **Verified** — RFC 9113 wire conformance suite against a real h2c server and
  guard tests that pin the public API contracts.

## Contents

- [Quick Start](#quick-start)
- [Targets](#targets)
- [Core Runtime](#core-runtime)
- [Blocking Work](#blocking-work)
- [Static Files and Compression](#static-files-and-compression)
- [Requirements](#requirements)
- [Build](#build)
- [Database Drivers](#database-drivers)
- [Conformance](#conformance)
- [Performance Baseline](#performance-baseline)
- [Install and Consume](#install-and-consume)
- [Web API Shape](#web-api-shape)
- [HTTP Protocol Library](#http-protocol-library)
- [License](#license)

## Quick Start

```cpp
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class HelloController final : public ruvia::Controller<HelloController> {
public:
    RUVIA_ROUTES_BEGIN
        RUVIA_GET("/hello", hello);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello");
    }
};

int main() {
    ruvia::app()
        .setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8080)})
        .setSignalShutdown(true)
        .run();
}
```

`App` does not install process signal handlers by default. Standalone servers
can opt in with `setSignalShutdown(true)` as above; embedded runtimes retain
ownership of SIGINT/SIGTERM and call `App::stop()` themselves.

The same route is part of the compiled
[`basic_http.cpp`](examples/web/basic_http.cpp) example. Configure once with
`-DRUVIA_BUILD_EXAMPLES=ON` as shown in [Build](#build), then:

```bash
cmake --build build --target ruvia_example_basic_http
./build/examples/ruvia_example_basic_http &
curl http://127.0.0.1:8080/hello
```

Handlers use `ruvia::Task<T>`, read HTTP input through `c.req()`, and build
responses through `Context`. Set response metadata through `c.status()`,
`c.header()`, and `c.setCookie()` before selecting one body builder such as
`c.text()` or `c.json()`; body builders do not accept a second metadata path.
HTTP status APIs use `ruvia::HttpStatusCode`: prefer named values such as
`ruvia::http_status::kCreated`, and use `HttpStatusCode::fromValue()` only for
validated extension codes.
`setListeners()` atomically installs any number of `ListenerConfig::http(...)`, `https(...)`, and `redirectHttpToHttps(...)` listeners. Redirect targets must name an HTTPS listener in the same list, ports must be unique, and total workers equal the listener count multiplied by `setWorkersPerListener()`.
Public startup configuration uses ordinary C++ values (`std::string`,
`std::vector`, paths, durations, and spans); callers never choose a PMR
resource. Ruvia copies retained configuration into process-owned storage before
workers start.
Default rate limiting is worker-local via `setDefaultRateLimitPerWorker()`; `setRateLimitSlotsPerWorker()` selects its power-of-two startup capacity (`kDefaultRateLimitSlotsPerWorker` by default), and workers with neither a default nor route-specific rule allocate no table.

Connection metadata is deliberately separate from the HTTP request model:

```cpp
const auto info = ruvia::getConnInfo(c);
const auto peerAddress = info.remote().address();
if (const auto* tls = info.tls()) {
    const auto clientSubject = tls->clientCertificateSubject();
}
```

## Targets

| Directory | CMake target | Purpose |
| --- | --- | --- |
| `ruvia-core/` | `ruvia::core` | Coroutine tasks, Asio integration, PMR memory, connection scanning, and runtime helpers. |
| `ruvia-http/` | `ruvia::http` | Pure sans-I/O HTTP, HTTP/2, WebSocket, multipart, SSE, content-coding, and outbound-client protocol primitives. |
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server I/O, TLS, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web   ->  ruvia-core + ruvia-http
```

`ruvia-http` is core-free, Asio-free, and socket-free. Applications that need
an outbound HTTP client provide their own I/O runtime and drive its sans-I/O
client APIs; `ruvia-web` intentionally does not provide `fetch`, proxy,
connection-pool, or client TLS runtime APIs.

## Core Runtime

`ruvia::EventLoopPool` creates application-owned event loops. Every
`EventLoop` owns one thread and one standalone Asio `io_context`, which is
available for application TCP, UDP, DNS, and TLS integrations. Its bounded
`post()` remains the cross-thread queue-in-loop API:

```cpp
#include <ruvia/core/EventLoopPool.h>

ruvia::EventLoopPool loops({.loopCount = 4, .mailboxCapacity = 1024});
loops.start();

auto loop = loops.loopFor("device-42");
asio::ip::tcp::socket socket(loop.ioContext());
auto posted = loop.post([] { /* runs on the selected event loop */ });
if (!posted.accepted()) {
    auto rejected = std::move(posted).takeRejected();
    // Retry or persist the rejected callable.
}

auto stopRegistration = loop.onStop([&socket] {
    std::error_code ignored;
    socket.close(ignored);
});

loops.stop();
loops.join();
```

Keep the stop registration alive while its resource is active. The callback
runs on the owning event-loop thread before that loop exits. Do not call
`run()`, `stop()`, or `restart()` on a pool-owned `io_context`; lifecycle
control belongs to `EventLoopPool`. Cross-thread application work must still
use bounded `EventLoop::post()` rather than raw `asio::post()`, so shutdown and
backpressure remain observable. Web workers deliberately expose only
`WorkerHandle`/`WebWorkerHandle`, not their `io_context` or executor.

Existing Asio applications can attach one Ruvia event loop to an externally
owned context without transferring thread or lifecycle ownership:

```cpp
asio::io_context io;
auto attachment = ruvia::attachEventLoop(io);
auto loop = attachment.loop();

std::thread thread([&] { io.run(); });
loop.post([] { /* runs on the external context */ });

attachment.stop(); // closes the mailbox, runs onStop hooks, releases its work guard
thread.join();
```

The attachment may be stopped or destroyed while another thread is inside
`run()`: its context service retains the worker state until the terminal
cleanup handler drains. The external owner still retains ownership of
`run()`, `stop()`, `restart()`, and the thread; the attachment never calls
`io_context::stop()` because the context may host unrelated work. If the
context is destroyed first, returned `EventLoop` handles become terminal and
their `ioContext()`/`executor()` access throws `std::logic_error`. A second
attachment is rejected until the first attachment's terminal cleanup has
completed.

Database and Redis integrations remain in `ruvia::web`, but they do not require
an HTTP `App`, `Context`, or server worker. `DataAccessService` does not create
a thread or `io_context`; it attaches connection pools and coroutine-job
lifetime tracking to an existing application-owned core event loop:

```cpp
#include <future>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/DataAccess.h>

ruvia::EventLoopPool loops({.loopCount = 1});
auto loop = loops.loop(0);

auto pg = ruvia::DbConfig::postgreSql();
pg.host = "127.0.0.1";
pg.database = "app";

ruvia::DataAccessOptions options;
options.databases.push_back({"default", std::move(pg)});
options.redis.push_back({"default", ruvia::RedisConfig{}});
ruvia::DataAccessService service(loop, std::move(options));

auto ready = service.connect();
loops.start();
std::promise<std::exception_ptr> completed;
auto done = completed.get_future();
ready.get();
auto posted = service.post([&completed](
    ruvia::DataAccessContext& context) -> ruvia::Task<void> {
    try {
        co_await runWorkerJob(context.db(), context.redis());
        completed.set_value(nullptr);
    } catch (...) {
        completed.set_value(std::current_exception());
    }
});
if (!posted.accepted()) {
    throw std::runtime_error("worker queue is full or stopping");
}

// Join application-owned jobs before stopping their worker resources.
auto failure = done.get();
loops.stop();
loops.join();
if (failure != nullptr) {
    std::rethrow_exception(failure);
}
```

`connect()` schedules startup and reports it through a future; `post()` is the
only public operation-scope entry point, and `close()` is worker-affine. A
`DataAccessContext` is a short-lived operation scope and must not escape its posted
coroutine. Its handles are job-scoped, while DB/Redis result values allocate
from that worker's unsynchronized resource; neither may escape the posted
coroutine or be destroyed from another thread. Database and Redis hostname
lookup runs asynchronously on the bound event loop, is subject to
`connectTimeout`, and is canceled during shutdown. Event-loop shutdown requests
stop and cancels pool I/O so accepted jobs can finish before loop teardown. An
optional `failureHandler` handles
uncaught job exceptions on the worker; without one the exception fails the loop
and is rethrown by `EventLoopPool::join()`. Enable the corresponding
`RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_MARIADB`, and `RUVIA_ENABLE_REDIS`
features and link `ruvia::web`; `ruvia::core` itself keeps no database or Redis
dependency.

Web handlers obtain their current core worker with `Context::worker()`; its reference is
borrowed for the request, so use `auto worker = c.worker()` before capturing it. Background
components select a stable Web worker with `App::workerFor()` and submit a job with worker-local DB and Redis
access without exposing the underlying executor:

```cpp
auto worker = app.workerFor("device-42");
auto result = worker.post(
    [event = std::move(event)](
        ruvia::WebWorkerContext& workerContext) mutable -> ruvia::Task<void> {
        co_await persistEvent(workerContext.db(), event);
    });
```

Web job contract:

- **Backpressure** — configure the bounded queue before startup with
  `App::setWorkerMailboxCapacity()` and handle `kQueueFull` at every producer.
- **Metrics** — `WebWorkerHandle::stats()` exposes accepted, rejected,
  completed, failed, and outstanding counts.
- **Shutdown completion** — a job accepted before shutdown remains owned until
  its coroutine completes. Shutdown rejects new jobs, requests stop, and closes
  worker I/O plus DB/Redis to wake suspended operations; their completion
  continuations drain before worker memory is destroyed.
- **Lifetimes** — captured data must own its lifetime, and `WebWorkerContext`
  must not be stored beyond the callback.
- **Producers** — `App::workers()` returns all Web worker handles; external
  producers must keep using `WebWorkerHandle::post()` so accepted jobs remain
  covered by Web shutdown ownership, failure propagation, and completion
  tracking.
- **Failures** — an unhandled job exception stops every App worker and is
  rethrown by `App::run()`; applications should still catch expected DB or
  business failures inside the job.
- **Cancellation** — jobs may use `WebWorkerContext::worker()` for worker-bound
  core primitives, and must use cancellable waits or observe
  `WebWorkerContext::stopToken()` so shutdown can finish.

`TaskScope`, worker-bound `sleepFor`, bounded `Channel`, and `OneShot` are also
provided by `ruvia::core`; their deadlines share the worker's single timer
queue. `App::onStop()` hooks run once for explicitly enabled signal shutdown,
direct `App::stop()`, and worker failure before the worker-local Web resources
are closed.

## Blocking Work

A worker is one thread serving every connection it accepted, so a handler that
blocks — password hashing, a synchronous third-party SDK, template rendering, a
slow file — freezes all of them for as long as it blocks. `BlockingPool` is the
offload path: a fixed set of long-lived threads with a bounded queue, started
once by `App::run()` and shared by every worker. Offloading enqueues a task and
wakes a waiting thread; it never spawns one per call.

```cpp
ruvia::app().setBlockingPool(ruvia::BlockingPoolOptions{
    .threadCount = 8,     // 0 selects hardware_concurrency()
    .queueCapacity = 512, // 0 selects threadCount * 64
});

ruvia::Task<ruvia::HttpResponse> hash(ruvia::Context& c) {
    const auto body = co_await c.req().text();   // borrows the request buffer
    auto digest = co_await c.runBlocking(
        [input = std::string(body)] {            // ...so copy before offloading
            return argon2Hash(input);            // blocks a pool thread, not the worker
        });
    co_return c.text(std::string_view(digest));
}
```

The handler suspends, the worker keeps serving its other connections, and the
coroutine resumes on that same worker with the result. What the callable throws
is rethrown at the `co_await`, so `onError` answers it like any other handler
failure. A pool with no free thread and no free queue slot refuses the work
rather than queueing it without bound: `runBlocking()` throws
`BlockingOperationRejected`, which the default error path answers with 503.
`c.tryRunBlocking(...)` returns a `BlockingResult<T>` with the status instead of
throwing, for handlers that would rather shed load their own way.
`WebWorkerContext::runBlocking()` offers the same to posted background jobs.

Both spellings take an optional deadline as their first argument —
`c.runBlocking(std::chrono::seconds(2), fn)` — which bounds the *wait*, not the
work: a blocking call cannot be interrupted, so the pool thread stays occupied
until the callable returns and its result is then discarded. Use it to stop one
wedged dependency from pinning a request's connection and arena indefinitely.

`App::blockingPoolStats()` reports what the two size knobs should be set from:
`queued`/`running` depth, `completed`, `rejected` (refused because the pool was
full — the overload signal), and `discarded` (never ran because the pool was
stopping — shutdown accounting, deliberately kept out of `rejected`).

The callable runs on a foreign thread: capture by value or move, and never
capture the `Context`, the request, its arena, or any other worker-owned state.
Shutdown does not wait for work that is still running — the pool handle stops
accepting work and detaches its running threads, while a suspended handler is
resumed immediately and the pool result is discarded. A callable may therefore
finish after `App::run()` or a `BlockingPool` destructor returns, so its captures
must remain self-contained. Call `BlockingPool::join()` explicitly when an
owner needs a completion barrier. `BlockingPool` is a `ruvia::core` type and
can be used directly with `ruvia::runBlocking(pool, worker, fn)` outside the Web
framework. `App::setBlockingPool()` is absent by default: an application that
never offloads should not pay for idle threads.

## Static Files and Compression

`DocumentRootConfig` builds a static-root index at startup. The default
`DocumentRootRefreshMode::kImmutable` keeps requests on the index and does not
rescan directories. For development, opt into polling; each refresh rebuilds
the complete index on the blocking pool and publishes it between requests. A
filesystem error rejects that candidate as a whole, so polling keeps the
previous complete index instead of exposing a partial directory. Each failed
poll increments `App::httpStats().documentRootRefreshFailures` without stopping
the worker:

```cpp
ruvia::DocumentRootConfig documentRoot;
documentRoot.root = "public";
documentRoot.runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;
documentRoot.runtimeOptions.refreshInterval = std::chrono::milliseconds(500);
documentRoot.runtimeOptions.enableLiveReload = true;

ruvia::app()
    .setBlockingPool(ruvia::BlockingPoolOptions{.threadCount = 2})
    .setCompression(ruvia::CompressionConfig{})
    .setDocumentRoot(std::move(documentRoot))
    .run();
```

Live reload is deliberately development-only. Include
`/__ruvia/live-reload.js` in the development HTML; the script polls the
version endpoint and reloads the page when the published index changes. The
endpoint uses a monotonic snapshot revision, so a filesystem change cannot be
hidden by a hash collision.
It is only valid with `DocumentRootRefreshMode::kPolling`; enabling it with
the immutable refresh mode is rejected during server-option validation.

Static files prefer checked-in `.br`, `.gz`, or `.zst` sidecars whose mtime is
at least as new as the identity file; an older sidecar is ignored so an update
cannot serve stale decoded bytes. If no usable sidecar is available, an accepted coding can be produced for a complete file no larger
than `DocumentRootRuntimeOptions::onDemandCompressionMaxBytes` through the blocking
pool, subject to `CompressionConfig::minBytes`. Ranges, large files, pool
rejection, and a coding that would not make the representation smaller keep the
original file response, preserving the zero-copy path. File or encoder failures
remain server errors when identity is forbidden. `setCompression()` also enables incremental gzip, Brotli, or zstd for
response streams; each handler write is flushed through the encoder so SSE and
other low-latency streams do not wait for a full buffered response.
When both a document root and compression are configured, file responses returned
by `Context::staticFile()` and `Context::file()` use the same Accept-Encoding
negotiation and deferred-compression stage as the document-root fallback. The
response plan is finalized only after that stage, so HTTP/1 and HTTP/2 advertise
the compressed length and framing consistently. A standalone `Context` created
without server services keeps `staticFile()` strict and does not defer an
unacceptable identity response.
An application-provided known `Content-Encoding` (including a stack composed only
of known codings) is treated as an already-built representation and every coding
must still be acceptable to the request's `Accept-Encoding`; otherwise the response
is rejected with `406`. Stacks containing unknown custom codings remain the
application's responsibility.
For buffered handlers, a request with no acceptable response coding is checked
after the handler status is known: representation-free `204`, `205`, and `304` responses
remain valid, while a bodyful response is `406 Not Acceptable`; streaming and
upgrade routes reject before committing their response head.

## Requirements

- CMake 3.24 or newer.
- A C++20 compiler. CI builds with GCC 13 on Ubuntu 24.04, the stock Apple
  Clang on macOS 26, and MSVC on Windows.
- vcpkg.
- Supported build platforms: Linux, macOS, and Windows 10 or newer. Windows
  builds require MSVC.
- Component dependencies: core uses Asio; HTTP uses zlib, Brotli, and zstd;
  Web adds OpenSSL.
- Optional vcpkg features: MariaDB, PostgreSQL, Redis, and JWT.

## Build

Set `VCPKG_ROOT` to the root of your vcpkg checkout. Ruvia automatically uses
its toolchain unless `CMAKE_TOOLCHAIN_FILE` was set explicitly.

Linux / macOS:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

When needed, add `-DRUVIA_BUILD_TESTS=ON` and `-DRUVIA_BUILD_EXAMPLES=ON` to
the same configuration, rebuild, and run the tests:

```bash
ctest --test-dir build --output-on-failure
```

Windows uses MSVC with static vcpkg libraries. If the two vcpkg defaults are
already configured in your environment, the first two lines can be omitted:

```powershell
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"
$env:VCPKG_DEFAULT_HOST_TRIPLET = "x64-windows-static"

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Add `-DRUVIA_BUILD_TESTS=ON` and `-DRUVIA_BUILD_EXAMPLES=ON` when needed,
then run `ctest --test-dir build -C Release --output-on-failure`.

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build `ruvia::core`. |
| `RUVIA_BUILD_HTTP` | `ON` | Build standalone `ruvia::http`. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia::web`; requires core and HTTP. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build tests for every selected target and enabled feature. |
| `RUVIA_BUILD_BENCHMARKS` | `OFF` | Build Release-oriented HTTP hot-path benchmarks; requires HTTP. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples for every enabled Web feature; requires Web. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Enable MariaDB integration in Web. |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | Enable PostgreSQL integration in Web. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Enable Redis integration in Web. |
| `RUVIA_ENABLE_JWT` | `OFF` | Enable JWT integration in Web. |

## Database Drivers

MariaDB and PostgreSQL use the same `DbHandle`, result, streaming, transaction and migration APIs. Each worker owns exactly one database connection. Select the driver when constructing its configuration:

```cpp
auto config = ruvia::DbConfig::postgreSql(); // port 5432
config.username = "app";
config.password = "secret";
config.database = "app";
app.useDb(std::move(config));
```

Enable its matching CMake feature first. PostgreSQL parameters use `$1`, `$2`,
and so on; MariaDB parameters use `?`. A `?` inside a string literal, a quoted
identifier or a comment is data, not a placeholder. For generated PostgreSQL
keys, use `INSERT ... RETURNING id` and read the returned row.

`query()` returns `DbRows`, which exposes only the row set. `execute()` returns
`DbExecResult`, which exposes `affectedRows()` and an optional
`lastInsertId()`; the latter is present only when the backend supplies that
concept. Use `query()` for PostgreSQL statements with `RETURNING`.

`DbConfig`'s timeouts are enforced by the client: an expired `queryTimeout`
fails the operation and drops the connection, whatever the server is still
doing with the statement. A timeout left unset is disabled, so a stalled
backend then waits indefinitely.

### Migrations

`DbMigrator` applies pending migrations synchronously, under a backend lock so
that concurrent deployers serialize. It runs its own event loop and blocks, so
it belongs in startup code rather than on a worker:

```cpp
static constexpr std::array migrations{
    ruvia::DbMigration{"001_create_users",
        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
        "name VARCHAR(120) NOT NULL)"},
};
const auto report = ruvia::DbMigrator::migrate(config, migrations);
```

Each `DbMigration` is exactly one statement -- neither backend accepts more
than one per call -- and its id is recorded in a migrations table so later runs
skip it. Ids that differ only in letter case are rejected, because a
case-insensitive collation would treat them as the same migration. The text is
recorded as a digest alongside the id, so editing a migration that has already
run is reported rather than silently skipped.

On PostgreSQL the statement and the row recording it commit together, so an
interruption cannot leave the schema changed and unrecorded. A statement the
backend refuses inside a transaction block names the exception, per migration:

```cpp
ruvia::DbMigration{"002_index",
    "CREATE INDEX CONCURRENTLY items_value_idx ON items (value)",
    ruvia::DbMigrationAtomicity::kUnwrapped},
```

MariaDB commits DDL implicitly, so there the two statements are always separate
and an interruption between them re-runs the migration on the next start: write
MariaDB migrations to be re-applicable.

With tests and a driver enabled, that driver's live test is compiled
automatically. Set `RUVIA_RUN_POSTGRESQL_INTEGRATION=1` or
`RUVIA_RUN_MARIADB_INTEGRATION=1` when running CTest to execute it; without the
environment variable CTest reports it as skipped.

## Conformance

The regular Linux CI runs a repository-owned wire-level suite against Ruvia's
actual cleartext HTTP/2 server. Its connection-per-case, handcrafted-frame, and
wire-error assertion model follows the proven h2spec approach, but every
expectation is maintained directly against RFC 9113 instead of filtering
RFC 7540 results. To reproduce it locally, install Python 3 and configure:

```bash
cmake -S . -B build-conformance -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRUVIA_BUILD_TESTS=ON
cmake --build build-conformance --target ruvia_http2_conformance_server
ctest --test-dir build-conformance -R ruvia_http2_conformance --output-on-failure
```

## Performance Baseline

Release benchmark scope, commands, and comparison rules live with the benchmark
sources in [tests/http/benchmarks/README.md](tests/http/benchmarks/README.md).

## Install and Consume

Install all selected targets:

```bash
cmake --install build --prefix build/install
```

For a Visual Studio build, add the selected configuration, for example
`cmake --install build --config Release --prefix build/install`.

Each library has an independent export. Consumers must request the component
they use:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(my_app PRIVATE ruvia::web)
```

Narrower consumers can request only core or HTTP:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS core)
target_link_libraries(runtime_tool PRIVATE ruvia::core)

find_package(ruvia CONFIG REQUIRED COMPONENTS http)
target_link_libraries(protocol_tool PRIVATE ruvia::http)
```

The package imports only the requested dependency closure: core and HTTP are
independent, while Web imports core and HTTP alongside its own targets.
Component-scoped installation uses `core`, `http`, `web`, and `Development`
install components.

## Web API Shape

Ruvia intentionally uses one application per process. `ruvia::app()` is the
only configuration and lifecycle entry point; applications do not construct
additional `App` instances. Controllers use CRTP and register themselves at
startup when their route macro block is declared, so there is no separate
controller list or `useController()` step. Every controller translation unit
retained in the final executable contributes to this process-wide registry.
Controller static or object libraries are linked with
`ruvia_link_controllers(application controllers)`, which preserves every
controller object across GNU, Apple, and MSVC linkers without a manual
controller list. Dynamically loaded modules must be present before `run()`.
The first `App::run()` or `TestApp::request()` seals and deduplicates that
registry. Loading a controller-bearing module after sealing is a startup error,
so every worker observes the same controller set.
Each worker then creates its own controller instances and finalized route graph
from that startup registry.
Routes and schemas use these macros:

| Concern | Macros |
| --- | --- |
| Controller grouping | `RUVIA_CONTROLLER_GROUP(...)` |
| Route table | `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END` |
| HTTP methods | `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE` |
| Streaming / SSE | `RUVIA_GET_STREAM`, `RUVIA_GET_SSE` |
| WebSocket | `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS` |
| Models | `RUVIA_MODEL`, `RUVIA_FIELD`, `RUVIA_OPTIONAL_FIELD`, `RUVIA_FIELD_NAME` |
| Validation | `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_RULE` |

Route tables, middleware chains, and controller instances are finalized before
workers start. The request path does not rebuild them or use a per-request
virtual dispatcher.

Failures inside a request become responses: `onError` receives the exception and
decides the status, and an error handler that itself throws still yields a
deterministic 500. A failure past the response's point of no return cannot become
a response — the head is already on the wire — so it is reported instead:
`App::onConnectionFailure` receives the exception with the peer address, and
without a listener it is written to stderr rather than dropped with the
connection. `App::httpStats()` sums the same events across every worker as
counters — active and shed connections, connection failures, transient accept
failures, worker failures, document-root refresh failures — so a deployment can
be monitored by polling instead of by installing callbacks.
[`docs/ruvia-exception-policy.md`](docs/ruvia-exception-policy.md)
is the full contract: what each layer raises, which failures are isolated where,
and the three kinds of callback contract.

Self-contained callbacks passed to App are owned and destroyed with the App;
request dispatch retains only internal, allocation-free callback references.
Public callback types never expose non-owning `bind()` or `borrow()` factories.

Redis time APIs avoid exposing wire-level sentinel values in application code:
`expireAt()` accepts `std::chrono::system_clock::time_point`, `ttl()` and
`pttl()` return `RedisTtl` (`missing`, `persistent`, or an expiring duration),
and scan options accept an optional continuation cursor. Scan results expose
`done()` and `nextCursor()`, so Redis's wire-level zero sentinel never doubles
as both an initial and terminal application state.

Models are ordinary structs with one schema for JSON parsing, validation, and
serialization. They support nested models and arrays; `RUVIA_FIELD` is required
and `RUVIA_OPTIONAL_FIELD` may be absent. Route middleware keeps the Hono-style
typed `c.req().valid<T>()` API, while `c.req().validJson<T>()` also exposes the
validated original bytes through `raw()` for JSONB passthrough. See the compiled
[`models_validation.cpp`](examples/web/models_validation.cpp) example for the
complete API. `SecurityHeadersOptions` defaults to `LegacyXssFilterPolicy::kDisable`, emitting `X-XSS-Protection: 0` because obsolete browser filters can create security issues; `kOmitHeader` omits that header, while Content Security Policy remains the modern content control.

## HTTP Protocol Library

`ruvia::http` can be used without the runtime or Web framework. It provides
HTTP message types and helpers, HTTP/1 request and response parsing/writing,
the server/client-role `Http2Connection` state machine, HPACK, WebSocket
protocol state, multipart parsing, SSE formatting, range and
conditional-request helpers, cookies, content negotiation, redirects, and
content decoding.

The library is sans-I/O: callers feed bytes, consume typed results/events, and drive
transport I/O themselves. It contains no App, Context, Router, socket,
TLS, connection pool, runtime timeout, static-root policy, DB, Redis, or JWT
integration. Content-Encoding parsing distinguishes identity, one supported
coding, and an unsupported coding stack; Web request decoding reports the
latter as HTTP 415.

Borrowed outbound-client models say so in their names: `HttpOriginView`,
`HttpClientRequestView`, `HttpClientRequestContentView`, and
`HttpClientRequestBytesView`. Their referenced storage must remain alive until
the external sans-I/O driver finishes using it; response-head values remain
owned PMR results.

## License

Ruvia is released under the [MIT License](LICENSE).
