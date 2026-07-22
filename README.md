# Ruvia

[![Build](https://github.com/hyird/Ruvia/actions/workflows/build.yml/badge.svg)](https://github.com/hyird/Ruvia/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hyird/Ruvia)](https://github.com/hyird/Ruvia/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Ruvia is a C++20 HTTP/Web framework built as four independently consumable
CMake targets. The repository is a monorepo, but its runtime foundation and
protocol library do not require the full Web framework.

## Highlights

- **Layered by design** — `ruvia::core` (runtime), `ruvia::http` (protocol),
  `ruvia::web` (framework), and the opt-in `ruvia::edge` (CDN edge node)
  install and import as independent package components.
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
        .setServerTopology(ruvia::ServerTopology::http(8080))
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
`ServerTopology` atomically selects HTTP, HTTPS, dual-listener, or redirect operation; HTTPS requires a validated `TlsIdentity`, and `setWorkersPerListener()` makes dual-listener topologies own twice the configured workers.
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
| `ruvia-edge/` | `ruvia::edge` | Opt-in CDN edge node: a caching reverse proxy with its own event loop and a thread-safe embedding control plane. |

Dependency direction is fixed:

```text
ruvia-web   ->  ruvia-core + ruvia-http
ruvia-edge  ->  ruvia-core + ruvia-http
```

`ruvia-http` is core-free, Asio-free, and socket-free. Applications that need
an outbound HTTP client provide their own I/O runtime and drive its sans-I/O
client APIs; `ruvia-web` intentionally does not provide `fetch`, proxy,
connection-pool, or client TLS runtime APIs.

### Edge Node

`ruvia::edge` exposes a runtime-independent product surface through
`EdgeServer.h`; Asio sockets, protocol writers, cache storage and origin-fetch
machinery remain implementation details. The listener and origin configuration
use owned value types:

```cpp
#include <ruvia/edge/EdgeServer.h>

ruvia::edge::EdgeServer edge({"0.0.0.0", 8080});
if (!edge.addOrigin(
        "www.example.com",
        ruvia::edge::OriginSettings{"origin.internal", 8443, true})) {
    throw std::runtime_error("duplicate edge origin");
}
edge.start();

// From the embedding application's control thread:
const bool removed = edge.purge("www.example.com", "/assets/app.js");
edge.stop();
```

Runtime `addOrigin`, `removeOrigin`, certificate rotation and cache controls are
synchronously serialized onto the Edge worker. An in-flight request retains a
stable origin/cache lease across suspension, so later replacement, purge or
removal cannot invalidate memory it is still using.

Setting `EdgeServerOptions::cacheDirectory` enables a persistent disk tier. A
live server exclusively leases that directory; records are checksummed and
published by atomic replacement, and restart recovery ignores uncommitted or
corrupt files. Do not point two live edge instances at the same directory.

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

The external `io_context` must outlive the attachment, every returned
`EventLoop`, and their Asio objects. Attach at most once per context, call
`attachment.stop()` while the context can still drain handlers, and retain
ownership of `run()`, `stop()`, `restart()`, and the thread. The attachment
never calls `io_context::stop()` because the context may host unrelated work.

Database and Redis integrations remain in `ruvia::web`, but they do not require
an HTTP `App`, `Context`, or server worker. Bind one `WorkerDataRuntime` to each
application-owned core event loop that needs its own pools:

```cpp
#include <future>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/WorkerData.h>

ruvia::EventLoopPool loops({.loopCount = 1});
auto loop = loops.loop(0);

auto pg = ruvia::DbConfig::postgreSql();
pg.host = "127.0.0.1";
pg.database = "app";

ruvia::WorkerDataOptions options;
options.databases.push_back({"default", std::move(pg)});
options.redis.push_back({"default", ruvia::RedisConfig{}});
ruvia::WorkerDataRuntime data(loop, std::move(options));

auto ready = data.connect();
loops.start();
std::promise<std::exception_ptr> completed;
auto done = completed.get_future();
ready.get();
auto posted = data.post([&completed](
    ruvia::WorkerDataContext& context) -> ruvia::Task<void> {
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
only public operation-scope entry point, and `close()` is worker-affine. A data
context is a short-lived operation scope and must not escape its posted
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

## Requirements

- CMake 3.24 or newer.
- A C++20 compiler. CI builds with GCC 13 on Ubuntu 24.04 and with the stock
  Apple Clang on macOS 26.
- vcpkg.
- Supported build platforms: Linux and macOS.
- Component dependencies: core uses Asio; HTTP uses zlib, Brotli, and zstd;
  Web adds OpenSSL.
- Optional vcpkg features: MariaDB, PostgreSQL, Redis, and JWT.

## Build

Linux / macOS:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

When needed, add `-DRUVIA_BUILD_TESTS=ON` and `-DRUVIA_BUILD_EXAMPLES=ON` to
the same configuration, rebuild, and run the tests:

```bash
ctest --test-dir build --output-on-failure
```

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build `ruvia::core`. |
| `RUVIA_BUILD_HTTP` | `ON` | Build standalone `ruvia::http`. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia::web`; requires core and HTTP. |
| `RUVIA_BUILD_EDGE` | `OFF` | Build the `ruvia::edge` CDN edge node; requires core and HTTP. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build the unit and guard tests. |
| `RUVIA_BUILD_BENCHMARKS` | `OFF` | Build Release-oriented HTTP hot-path benchmarks; requires HTTP. |
| `RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS` | `OFF` | Add the repository-owned RFC 9113 wire conformance suite against a real Ruvia h2c server; requires tests, Web, and Python 3. |
| `RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS` | `OFF` | Add live PostgreSQL driver tests; requires tests and PostgreSQL support. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples; requires Web. |
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
and so on; MariaDB parameters use `?`. For generated PostgreSQL keys, use
`INSERT ... RETURNING id` and read the returned row.

## Conformance

The regular Linux CI runs a repository-owned wire-level suite against Ruvia's
actual cleartext HTTP/2 server. Its connection-per-case, handcrafted-frame, and
wire-error assertion model follows the proven h2spec approach, but every
expectation is maintained directly against RFC 9113 instead of filtering
RFC 7540 results. To reproduce it locally, install Python 3 and configure:

```bash
cmake -S . -B build-conformance -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRUVIA_BUILD_TESTS=ON \
  -DRUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS=ON
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

Each library has an independent export. Consumers request the component they
use:

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
independent, while Web and Edge each import core and HTTP alongside their own
targets. Component-scoped installation uses `core`, `http`, `web`, `edge`, and
`Development` install components.

## Web API Shape

Controllers use CRTP and register themselves at startup when their route macro
block is declared; applications do not maintain a separate controller list.
`ruvia::app()` is the process-level configuration and lifecycle entry point,
while every worker owns its controller instances and finalized route graph.
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

## License

Ruvia is released under the [MIT License](LICENSE).
