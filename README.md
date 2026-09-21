# Ruvia

[![Build](https://github.com/hyird/Ruvia/actions/workflows/build.yml/badge.svg)](https://github.com/hyird/Ruvia/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hyird/Ruvia)](https://github.com/hyird/Ruvia/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)

Ruvia is a C++23 HTTP/Web framework built as three independently consumable
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
  policy, SNI identities, and an outbound client with certificate verification,
  SNI, ALPN, HTTP/1.1, and HTTP/2.
- **Optional integrations** — MariaDB, PostgreSQL, Redis, and JWT behind vcpkg
  features; database drivers share typed entities, repositories, structured
  queries, and explicit schema migrations through the same `DbHandle`.

## Contents

- [Quick Start](#quick-start)
- [Targets](#targets)
- [Outbound HTTP Client](#outbound-http-client)
- [Outbound WebSocket Client](#outbound-websocket-client)
- [Core Runtime](#core-runtime)
- [Blocking Work](#blocking-work)
- [Static Files and Compression](#static-files-and-compression)
- [Requirements](#requirements)
- [Build](#build)
- [Database Drivers](#database-drivers)
- [Redis ORM](#redis-orm)
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
        .server({
            .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall,
        })
        .listen({.address = "0.0.0.0", .http = 8080})
        .run();
}
```

`App` does not install process signal handlers by default. Standalone servers
can opt in through `ServerConfig::processSignalHandlers` as above; embedded runtimes retain
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
Public configuration types are ordinary C++ aggregates. Configure them with
designated initializers. Passing a config to an optional App feature enables or
replaces it, and passing `nullptr` disables it.

`listen()` configures a numeric IPv4 or IPv6 bind address and its optional HTTP
and HTTPS ports as one value. The address is validated and normalized when the
configuration is supplied. An omitted port is disabled, and automatic
HTTP-to-HTTPS redirect is enabled in that same value:

```cpp
ruvia::app().listen({
    .address = "0.0.0.0",
    .http = 80,
    .https = 443,
    .tls = {
        .certificateChainFile = "certs/server.crt",
        .privateKeyFile = "certs/server.key",
    },
    .autoHttpsRedirect = true,
});
```

`ServerConfig::workerCount` is the total Web worker count. Every worker independently
listens on the same configured ports and owns one worker-local DB, Redis,
outbound HTTP client, and user-state set. Enabling both transports does not
multiply workers or data resources.
Policies that exist both app-wide and per route use one name and one rule: the
narrower scope may only **tighten**. `ServerConfig::maxBufferedBodyBytes` and
`rateLimit()` are
the deployment's ceilings; `ruvia::BodyLimit<N>` and
`ruvia::RateLimit<max, windowMs>` declare a route's own, named in the same
middleware list as any other route middleware. A route can never raise an
app-wide bound, and where a controller-wide and a route-specific declaration
both exist the stricter wins rather than the nearer.

```cpp
RUVIA_POST("/upload", upload, AuthMiddleware, ruvia::BodyLimit<64 * 1024>, ruvia::RateLimit<10, 1000>);
```

Entries in that list are types, so one that takes no configuration is named
bare and one that takes some is named with it -- there is no second syntax for
"configured" middleware. Rate limiting is worker-local: each worker counts
independently, so N workers admit up to N times the rule.
App-wide fixed-window rules use an options object rather than positional
arguments:

```cpp
ruvia::app().rateLimit({
    .rule = {
        .maxRequests = 100,
        .window = std::chrono::seconds(60),
    },
    .capacityPerWorker = 8192,
});
```

`capacityPerWorker` is a power-of-two startup key capacity
(`kDefaultRateLimitCapacityPerWorker` by default). Workers with neither an
app-wide nor a route-specific rule allocate no table. Pass `nullptr` to
`rateLimit()` to disable the app-wide rule.

Connection metadata is deliberately separate from the HTTP request model:

```cpp
const auto info = c.conn();
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
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server and outbound-client I/O, TLS, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web   ->  ruvia-core + ruvia-http
```

`ruvia-http` is a sans-I/O protocol library: callers feed bytes and consume
typed events. `ruvia-web` drives those primitives with worker-local DNS,
sockets, TLS/ALPN, connection reuse, timeouts, and cancellation.

## Outbound HTTP Client

Register outbound origins once before `App::run()`. Every Web worker then owns
one pool for each registered alias; no client socket, cookie jar, or protocol
state crosses worker threads, and request dispatch never mutates an origin
cache. `Context::httpClient()` returns the registered `default` client while
`Context::httpClient("billing")` selects a named one. Both are request-scoped
handles and cannot escape dispatch. Pass `nullptr` to `App::httpClient()` to
clear all registrations.

```cpp
ruvia::app().httpClient({
    .alias = "default",
    .config = {
        .scheme = ruvia::HttpScheme::kHttps,
        .host = "api.example.com",
        .connectionCount = 2,
        .requestTimeout = std::chrono::seconds(10),
    },
});
```

HTTPS negotiates HTTP/2 with ALPN and falls back to HTTP/1.1 by default.
Cleartext uses HTTP/1.1 unless `kHttp2Only` explicitly requests h2 prior
knowledge.

Handlers use an origin-bound handle and the protocol target's existing borrowed
`HttpClientRequestView`. The handle copies that view into reclaimable operation PMR memory
before returning the lazy operation, so its inputs only need to survive the
synchronous `send()` call:

```cpp
ruvia::Task<ruvia::HttpResponse> loadData(ruvia::Context& c) {
    auto client = c.httpClient();
    auto operation = client
        .withOptions({.timeout = std::chrono::seconds(2)})
        .send({.target = "/v1/data"});
    auto response = co_await std::move(operation);
    c.status(response.status());
    co_return c.body(co_await response.body().readAll());
}
```

`HttpClientResponse` owns status, protocol version, headers, trailers, and an
address-stable linear body state; it does not borrow from the request builder
or caller stack. `send()` completes when the final response head is available.
`maxResponseBytes` bounds `readAll()` and the HTTP/1 queued body window, not the
total number of bytes that may pass through `read()` or `pipeTo()`. Responses
with a non-identity `Content-Encoding` are decoded before `send()` completes
because the current content decoders are whole-representation decoders. Use
`trailers()` or `trailer()` after body completion to inspect trailing fields. On HTTP/1, a
request timeout or explicit `StopToken`
cancellation closes and discards that socket. On HTTP/2 it submits
`RST_STREAM(CANCEL)` for only the affected stream, so unrelated multiplexed
requests can continue. A connection I/O/protocol failure or `writeTimeout`
still discards the whole broken socket, and a later request reconnects
automatically.

Each HTTP/1 connection processes one exchange at a time. Each HTTP/2 connection
has a persistent reader/writer pair and multiplexes up to
`maxConcurrentHttp2StreamsPerConnection`, further constrained by the peer's
`SETTINGS_MAX_CONCURRENT_STREAMS`; PING, SETTINGS, flow-control updates, and
GOAWAY are processed even while no request is being submitted. Requests above a
peer GOAWAY `Last-Stream-ID` are known not to have been processed and are retried
once on a fresh connection under the original operation deadline. Ambiguous
requests are never retried automatically.

Use `HttpClientProtocol::kHttp1Only` or `kHttp2Only` when negotiation fallback
is not acceptable. Client certificates, a custom CA file, certificate
verification policy, connect/acquire/request/write timeouts, TCP keepalive, and
per-client connection capacity are supplied in the same `{}` configuration.
Additional operations wait in the bounded client-local queue and fail with
`kQueueFull` when it is full or `kTimeout` when `acquireTimeout` expires.

Cleartext HTTP/2 uses RFC 9113 prior knowledge when `kHttp2Only` is selected. HTTP/1.1
`Upgrade: h2c` is not performed implicitly, so a server that only accepts the
Upgrade transition must be configured for HTTP/1 or exposed through TLS/ALPN.

The Controller-facing surface provides one `send(HttpClientRequestView)`
operation, immutable `withOptions(OperationOptions)` derivation, origin
inspection, and a single `stats()` snapshot. Requests are awaited as scoped
coroutine operations; there are no blocking overloads or callback ownership model.

Every response has one linear body reader. `read()` consumes one borrowed
`std::span<const std::byte>` chunk, `readAll()` collects the remaining bytes into
an owning `std::pmr::vector<std::byte>` with a byte bound, and `pipeTo()`
forwards it to a controller response stream with backpressure. Both the client
body reader and request `BodyReader` offer `text()` for an explicit character
view of the next chunk, without charset conversion or UTF-8 validation (encoded
characters may straddle chunks). Reads share one operation lane; borrowed chunks
expire on the next body operation. `ResponseStreamWriter::write()` accepts byte
spans and copies their storage before returning the asynchronous operation. There is no
separate buffered request or streaming request entry point:

```cpp
auto response = co_await client.send({.target = "/v1/events"});
c.status(response.status());
co_await response.body().pipeTo(c.stream());
```
Pool configuration is immutable after that origin is first used. A handle automatically
observes its request or worker stop token; an explicit operation token is
combined with that ambient token rather than replacing it.

HTTPS origins verify both the peer certificate and host name by default. Test
or private self-signed origins must opt out explicitly with
`tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification`.
The `host` field accepts an ASCII DNS hostname (including IDNA A-labels), an
IPv4 address, or an unbracketed IPv6 address, with the port configured separately.
URI percent-encoding is not accepted. A DNS name's final dot is retained for
resolution and HTTP authority fields; TLS uses the name without that final dot.
TCP socket options use explicit policies: HTTP clients enable `tcpNoDelay` and
`tcpKeepAlive` by default, while `Tcp*Policy::kSystemDefault` leaves the socket
option untouched.

Received cookies are ignored by default. Set
`receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend` in
the origin configuration to retain matching `Set-Cookie` response fields and
send them on later requests. A per-request cookie is an ordinary `cookie`
header in the supplied `HttpClientRequestView`; `HttpClientConfig::cookies`
seeds each client-local jar when the origin is first used.
`maxCookies` and
`maxCookieBytes` bound each client-local jar; received cookies beyond
either bound are ignored. Invalid registration is rejected during App
configuration, before any worker starts.

## Outbound WebSocket Client

`WebSocketClient` is one long-lived `ws` or `wss` connection bound to one
`EventLoop`. Its API follows the standalone `HttpClient` shape: configuration is
a designated-initializable aggregate, construction performs no I/O, `connect()`
is lazy and worker-affine, `withOptions(OperationOptions)` derives operation
policy, and `abort()` is an idempotent immediate shutdown callable from any
thread. Use the typed `close(WebSocketCloseOptions)` overload for an awaited RFC
6455 close handshake.

```cpp
#include <ruvia/web/WebSocketClient.h>

ruvia::Task<void> consumeEvents(ruvia::WebSocketClient& client) {
    co_await client.connect();
    co_await client.text("ready");

    while (auto message = co_await client
               .withOptions({.timeout = std::chrono::seconds(30)})
               .read()) {
        if (message->text()) {
            // message->payload() is valid until the next read on this client.
        }
    }
}

ruvia::WebSocketClient client(loop, {
    .scheme = ruvia::WebSocketScheme::kWss,
    .host = "events.example.com",
    .target = "/v1/stream",
    .subprotocols = {"events.v1", "events.v2"},
    .heartbeat = {
        .pingInterval = std::chrono::seconds(30),
        .pongTimeout = std::chrono::seconds(10),
    },
});

auto completed = loop.start(consumeEvents(client));
```

The opening handshake uses HTTP/1.1 Upgrade, validates the server accept key and
selected subprotocol, and rejects unsolicited extensions. Client frames use a
cryptographically generated mask; inbound masked server frames are rejected.
The driver automatically answers Ping, completes peer-initiated Close, enforces
one concurrent read and one concurrent write, bounds complete messages, and
closes the transport when an operation is cancelled or times out. An operation's
timeout starts when it is awaited and covers waiting for the write channel as
well as transport I/O. Cancellation is checked before buffered messages are
returned. Creating and discarding an unstarted operation does not start its
timer or close the connection. `wss` verifies
the peer certificate and host name by default and supports the same CA and client
certificate fields and network `host` syntax as `HttpClientConfig`.
`heartbeat` is optional; it sends Ping
after an idle interval and aborts the transport when the matching Pong is not
observed before `pongTimeout`. Omitting `pongTimeout` uses the ping interval.
The application must keep a `read()` operation active so inbound control frames
and messages can be consumed. `abort()` only requests immediate transport
termination; use `co_await client.shutdown()` on the bound event loop when the
connect attempt, heartbeat task, and all client operations must be joined.

`WebSocketMessage::payload()` borrows the client read buffer and remains valid
only until the next `read()` on that connection. Copy it first when it must live
longer. A WebSocket connection stays on its bound loop for its complete lifetime;
it is not pooled or migrated between workers.

## Core Runtime

`ruvia::EventLoopPool` owns application runtime threads and standalone Asio
`io_context` instances. Each returned `EventLoop` is a stable handle to one of
those runtimes, which is available for application TCP, UDP, DNS, and TLS
integrations. Its bounded `post()` remains the cross-thread queue-in-loop API:

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
control belongs to `EventLoopPool`. Cross-thread application work uses bounded
`EventLoop::post()`. Web workers expose `WorkerHandle`/`WebWorkerHandle`, not
their `io_context` or executor.

A lazy `Task<T>` needs an explicit root owner. `EventLoop::start()` schedules it
on that loop and returns a move-only `RootTask<T>` completion owner:

```cpp
#include <ruvia/core/EventLoopPool.h>

ruvia::Task<ruvia::WorkerId> currentWorker(ruvia::WorkerHandle worker) {
    co_return worker.isCurrent() ? worker.id() : 0;
}

ruvia::EventLoopPool loops({.loopCount = 1});
auto loop = loops.loop(0);
auto completed = loop.start(currentWorker(loop.handle()));

loops.start();
const auto workerId = completed.get();
loops.stop();
loops.join();
```

Keep the `RootTask` and consume it before stopping resources the task may still
use. `get()` waits and rethrows the task exception. Destroying an in-flight
`RootTask` never destroys its suspended coroutine frame; an eventual unobserved
failure is routed to the loop failure sink and a pooled loop rethrows it from
`join()`. This setup-time root ownership does not replace bounded
`EventLoop::post()` for ongoing cross-thread submissions. `asAwaitable()`
remains available when an application is already inside an Asio coroutine.

Existing Asio applications can attach one Ruvia event loop to an externally
owned context without transferring thread or lifecycle ownership:

```cpp
#include <ruvia/core/EventLoopAttachment.h>

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

Outbound HTTP clients are first-class event-loop objects too. A client owns one
origin's worker-local DNS, TCP/TLS, connection pool, HTTP/1.1 and HTTP/2 state;
it can be bound directly to a pooled or attached loop without an HTTP `App` or
special worker context:

```cpp
#include <ruvia/web/HttpClient.h>

ruvia::Task<void> callUpstream(ruvia::HttpClient& client) {
    auto response = co_await client.send({.target = "/health"});
    auto body = co_await response.body().readAll();
}

ruvia::HttpClient client(loop, {
    .scheme = ruvia::HttpScheme::kHttps,
    .host = "api.example.com",
});

ruvia::Task<void> shutdownClient(ruvia::HttpClient& client) {
    co_await client.shutdown();
}

auto completed = loop.start(callUpstream(client));
loops.start();
completed.get();
auto stopped = loop.start(shutdownClient(client));
stopped.get();
```

Construction creates no thread and opens no connection. The first `send()`
connects lazily on the bound loop; retries, cancellation, timeouts and protocol
selection use the same runtime as `Context::httpClient()`. Operations must be
created and awaited on that loop. `close()` is idempotent, may be called from any
thread, and immediately requests cancellation without waiting. Use
`co_await client.shutdown()` on the bound loop when the client teardown must be
complete before the loop or its owning memory is destroyed.

Database clients are first-class event-loop objects. They do not require an HTTP
`App`, request `Context`, server worker, or an aggregate worker service. Bind
each client directly to any `EventLoop` created by `EventLoopPool` or
`attachEventLoop()`; construction does not create another thread or move
connections between workers:

```cpp
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/db/DbClient.h>

ruvia::Task<void> runWorkerJob(ruvia::DbClient& db) {
    auto rows = co_await db.query("SELECT id FROM jobs WHERE ready = $1", true);
    // Use rows on this same worker.
}

ruvia::Task<void> shutdownDatabase(ruvia::DbClient& db) {
    co_await db.shutdown();
}

ruvia::EventLoopPool loops({.loopCount = 1});
auto loop = loops.loop(0);

auto pg = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
pg.host = "127.0.0.1";
pg.database = "app";
ruvia::DbClient db(loop, std::move(pg));

auto ready = loop.start(db.connect());
loops.start();
ready.get();
auto done = loop.start(runWorkerJob(db));
done.get();
auto stopped = loop.start(shutdownDatabase(db));
stopped.get();
loops.stop();
loops.join();
```

`DbClient::connect()` is a lazy `Task<void>` and must run on its bound loop, just
like every later database operation; `EventLoop::start()` owns each top-level
completion. Result, stream, and transaction
values remain worker-affine. `close()` is idempotent, may be called from any
thread, and immediately requests cancellation without waiting; await
`db.shutdown()` when teardown must complete before the owning client or event
loop is destroyed. App handlers use
`Context::httpClient()` and `Context::db()` as worker-local convenience views.
Enable `RUVIA_ENABLE_POSTGRESQL` or `RUVIA_ENABLE_MARIADB` for `DbClient` and
link `ruvia::web`; `ruvia::core` keeps no HTTP-client or database dependency.

Redis follows the same standalone model. Enable `RUVIA_ENABLE_REDIS` and include
`<ruvia/web/redis/RedisClient.h>`:

```cpp
ruvia::RedisClient redis(loop, ruvia::RedisConfig{
    .host = "127.0.0.1",
    .poolSizePerWorker = 2,
});
// Start/await on loop, just like db.connect():
// co_await redis.connect();
// auto value = co_await redis.get("device:42");
// co_await redis.shutdown();
```

`RedisClient` exposes the same typed commands, pipelines, transactions and
`withOptions()` policies as `RedisHandle`. SQL and Redis ORM are available through
`db.getRepository<SqlEntity>()` and
`redis.getRepository<RedisEntity>(repositoryConfig)`; each uses its own entity
macros and the client's existing connections, cancellation and reclaimable memory.
Neither ORM requires App or an HTTP request. Direct SQL/Redis commands remain a
separate route, not an escape hatch on repositories.

For multiple loops, let an application-owned typed object hold one `DbClient`
and one `RedisClient` per loop. Select the loop **before** starting a business
coroutine; do not move clients, repositories, transactions, or PMR results between
loops. Redis pool capacities are per client/loop, not process-wide. Redis rejects a
second `connect()` without interrupting the first connection. Results must be destroyed on the
owner loop before the client; completing an operation or calling `shutdown()`
does not extend its allocator lifetime.

Start the loop pool, await **all** clients' readiness, then admit business work.
On startup failure, close already-started clients and observe all startup tasks.
For shutdown, stop business admission, await each client's `shutdown()` on its
loop, then stop/join the loop pool. Loop-stop hooks also cancel pending I/O if
normal shutdown is interrupted. `close()` may request Redis teardown from another
thread, but never performs socket operations there.

[`examples/web/event_loop_data.cpp`](examples/web/event_loop_data.cpp) demonstrates
this complete lifecycle with PostgreSQL and Redis ORM on two application-owned
loops, without `App`, `Context`, or a framework capability registry.

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
  `ServerConfig::workerMailboxCapacity` and handle `kQueueFull` at every producer.
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
queue. Standalone operations can create a `StopSource`, pass its `token()` to
channel, one-shot, timer, or blocking waits, and call `requestStop()` from any
thread. `App::onStart()` runs only after every worker has connected its
worker-local capabilities and started accepting on the complete listener set.
`App::onStop()` runs once for explicitly enabled process signal handlers, direct
`App::stop()`, and worker failure. Both hook sets execute on the
thread inside `App::run()`; stop callers and worker threads only request
shutdown and never run application hooks themselves.

## Blocking Work

A worker is one thread serving every connection it accepted, so a handler that
blocks — password hashing, a synchronous third-party SDK, template rendering, a
slow file — freezes all of them for as long as it blocks. `BlockingPool` is the
offload path: a fixed set of long-lived threads with a bounded queue, started
once by `App::run()` and shared by every worker. Offloading enqueues a task and
wakes a waiting thread; it never spawns one per call.

```cpp
ruvia::app().blockingPool({
    .threadCount = 8,     // 0 selects half the logical CPUs, clamped to 2..8
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
can be used directly outside Web; both layers use `runBlocking(...)` for the
throwing form and `tryRunBlocking(...)` for the status-returning form.
`App` creates a blocking pool by default using half the logical CPUs, clamped
to 2..8 threads, with 64 queued tasks per thread. Pass `nullptr` to
`App::blockingPool()` when an application intentionally needs no offload
capacity and should not pay for idle threads. Large buffered responses then
compress synchronously on their worker when response compression is enabled.

## Static Files and Compression

Response compression is disabled by default. Enable negotiated gzip, Brotli,
or zstd explicitly with `compression({})`; pass `nullptr` to disable it
again.

Buffered API responses use one bounded policy: bodies below `minBytes` remain
identity, bodies through `syncBytes` are compressed on the worker, bodies
through `maxBytes` are compressed on the default bounded blocking pool, and
larger bodies remain identity. The defaults are 1 KiB, 64 KiB, and 64 MiB.
If the pool is explicitly disabled, bodies through `maxBytes` are compressed
synchronously instead. Rejection by an enabled pool falls back to identity.
Whenever a policy fallback would use identity but the client forbids it, the
result is `406 Not Acceptable`.

`DocumentRootConfig` builds a static-root index at startup and always refreshes
it, once per second by default. The refresh cannot be disabled; a positive
`refreshInterval` may tune its cadence. Each refresh rebuilds the complete index
on the blocking pool and publishes it between requests. A filesystem error
rejects that candidate as a whole, so the server keeps the previous complete
index instead of exposing a partial directory. Each failed refresh increments
`App::httpStats().documentRootRefreshFailures` without stopping the worker.
Because directory scans never run on an event loop, configuring a document root
while explicitly disabling the blocking pool is rejected at startup:

```cpp
auto documentRoot = ruvia::DocumentRootConfig{
    .root = "public",
    .runtime = {
        .refreshInterval = std::chrono::milliseconds(500),
    },
    .precompressGzip = true,
};

ruvia::app()
    .compression({})
    .documentRoot(std::move(documentRoot))
    .run();
```

Static roots deny dotfiles by default, including files below hidden directories.
Use `StaticRootOptions::dotfiles = StaticDotfilePolicy::kServe` only for a root
that intentionally publishes hidden paths such as `.well-known/`.

When compression is enabled, static files may select checked-in `.br`, `.gz`,
or `.zst` sidecars whose mtime is at least as new as the identity file; an older
sidecar is ignored so an update cannot serve stale decoded bytes. A document
root with `precompressGzip`, `precompressBrotli`, or `precompressZstd` enabled
also builds in-memory variants during refresh for eligible text-like files
between `precompressMinBytes` and `precompressMaxBytes`. Checked-in sidecars
still win; refresh-built variants are used only when no fresh sidecar satisfies
the selected coding. Static files never perform request-time compression.
Without a usable sidecar or refresh-built variant they retain the
identity/zero-copy path when identity is acceptable, otherwise the response is
`406 Not Acceptable`. Disabling compression also disables static variant
negotiation. `Context::file()` always serves the selected file as identity;
`Context::staticFile()` and the document-root fallback can negotiate indexed
variants when the server switch is enabled.

`compression()` also enables incremental gzip, Brotli, or zstd for response
streams; each handler write is flushed through the encoder so SSE and other
low-latency streams do not wait for a full buffered response.
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
- A C++23 compiler. CI builds with GCC 13 on Ubuntu 24.04 and MSVC on Windows.
- vcpkg.
- Supported build platforms: Linux and Windows 10 or newer. Windows builds
  require MSVC.
- Component dependencies: core uses Asio; HTTP uses zlib, Brotli, and zstd;
  Web adds OpenSSL.
- Optional vcpkg features: MariaDB, PostgreSQL, Redis, and JWT.

## Build

For a standalone Ruvia build, set `VCPKG_ROOT` to the root of your vcpkg
checkout. Ruvia automatically uses its toolchain unless
`CMAKE_TOOLCHAIN_FILE` was set explicitly.

When Ruvia is included with `FetchContent` or `add_subdirectory`, the parent
project owns its toolchain, vcpkg manifest features, triplets, and cache-wide
compiler policy. Select the dependencies needed by the enabled `RUVIA_*`
options in the parent manifest. On MSVC, select the static runtime before
creating parent targets that link Ruvia; Ruvia applies `/MT` or `/MTd` only to
targets in its own directory tree.

Linux:

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
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit tests for every selected target and enabled feature. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples for every enabled Web feature; requires Web. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Enable MariaDB integration in Web. |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | Enable PostgreSQL integration in Web. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Enable Redis integration in Web. |
| `RUVIA_ENABLE_JWT` | `OFF` | Enable JWT integration in Web. |

`<ruvia/web/auth/Jwt.h>` and its declarations are available only when
`RUVIA_ENABLE_JWT=ON`. Consumers linking `ruvia::web` inherit that feature
definition from the target and should not define it themselves.

## Database Drivers

MariaDB and PostgreSQL use the same `DbHandle`, result, streaming, transaction
and migration APIs. Each worker owns exactly one database connection. Select an
enabled driver directly in `DbConfig`; omitting `port` selects that driver's
standard port:

```cpp
auto config = ruvia::DbConfig{
    .driver = ruvia::DbDriver::kPostgreSql,
    .username = "app",
    .password = "secret",
    .database = "app",
};
ruvia::app().database({.config = std::move(config)});
```

The selected driver must be enabled at build time. PostgreSQL parameters use
`$1`, `$2`, and so on; MariaDB parameters use `?`. A `?` inside a string literal, a quoted
identifier or a comment is data, not a placeholder. For generated PostgreSQL
keys, use `INSERT ... RETURNING id` and read the returned row.

Fixed SQL can check its parameter count at compile time. Parameter **values**
remain ordinary runtime values:

```cpp
// On a MariaDB handle (the default literal dialect):
auto rows = co_await db.query<"SELECT name FROM users WHERE id = ?">(userId);
// On a PostgreSQL handle:
auto rows = co_await db.query<"SELECT name FROM users WHERE id = $1",
    ruvia::DbDriver::kPostgreSql>(userId);
```

The same template form is available for `execute()` and `queryStream()` on
`DbClient` / `DbHandle`, and for `query()` / `execute()` on `DbTransaction`.
PostgreSQL uses the highest parameter index, so repeated `$1` references take
one argument. Quoted text and comments are skipped using the backend's lexical
rules; PostgreSQL dollar quotes, escape strings and nested comments are handled.
MariaDB scanning follows the existing backslash-escape convention, and
PostgreSQL ordinary strings assume `standard_conforming_strings=on`.
This checks parameter arity, not SQL syntax, column types or server SQL modes.
A literal's selected dialect must match the connection; a mismatch throws
`std::invalid_argument` before creating the operation. Runtime SQL strings and
parameter spans continue to use `query(sql, params)` and runtime validation.

`query()` returns `DbRows`, which is directly iterable and indexable. A `DbRow`
supports both positional and exact column-name lookup. `DbField::value()` returns
an optional text view so SQL NULL is distinct from an empty string, while
`as<T>()` converts strings, booleans, integers, and floating-point values and
throws `DbConversionError` on malformed input. `execute()` returns
`DbExecResult`, which exposes `affectedRows()` and an optional
`lastInsertId()`; the latter is present only when the backend supplies that
concept. Use `query()` for PostgreSQL statements with `RETURNING`.

`DbConfig` defaults connect, query, and pool-acquire timeouts to 5 seconds,
30 seconds, and 5 seconds. An expired `queryTimeout` fails the operation and
drops the connection, whatever the server is still doing with the statement;
explicit `std::nullopt` requests an unbounded wait. Database failures throw
`DbError`; it exposes a stable code, the driver when a backend was selected,
and, when available, the backend's native code, SQLSTATE, and constraint name.
Use `constraintName()` to map a PostgreSQL constraint failure to an application
error without parsing backend error text.

For one operation, bind a tighter timeout or an additional stop token to the
handle before starting it; DB, Redis, and outbound HTTP all use this same
`withOptions(OperationOptions)` shape:

```cpp
auto rows = co_await c.db().withOptions({
    .timeout = std::chrono::seconds(2),
}).query("SELECT name FROM users WHERE id = $1", userId);
```

### ORM and QueryBuilder

SQL and Redis each offer two independent data-access routes. They share connection
configuration and operation lifetimes; choosing ORM does not require using the
direct APIs, and choosing the direct APIs does not require declaring entities.

| Route | SQL | Redis |
| --- | --- | --- |
| Entity ORM | `RUVIA_DB_ENTITY`, `getRepository<Entity>()` | `RUVIA_REDIS_ENTITY`, `getRepository<Entity>(config)` |
| Direct API | `query`, `execute`, `queryStream` with SQL; `DbQuery` with raw rows | Key commands, pipelines, transactions and Lua through `RedisHandle` |

ORM operations use repositories throughout. Direct SQL queries return `DbRows`
and do not map rows into an entity. SQL ORM query builders retain their bound
entity and return entities, declared DTO projections or scalar counts. Expressions,
joins and CTEs compose within that binding; the direct SQL route owns complete
statements and raw results. Redis repositories
own their hash keys, field encoding and indexes; direct Redis commands should use
separate keys instead of editing repository-managed hashes.

The common repository operations are `find`, `findOne`, `findAndCount`, `count`,
`exists`, `insert`, `update`, `upsert`, `deleteBy` and `remove`. Both repositories
use `DbFindOptions` for queries, including `count({.where = ...})`. SQL keeps
relations, row locks and SQL-specific upsert options; Redis keeps TTL and Search
index configuration. Neither route simulates unsupported backend features.

The [SQL ORM example](examples/web/orm.cpp) and
[Redis ORM example](examples/web/redis_orm.cpp) use entity repositories. The
[direct SQL example](examples/web/database.cpp) and
[direct Redis example](examples/web/redis.cpp) demonstrate the original APIs.

Enable either database driver and include `ruvia/web/db/DbRepository.h`.
Entities declare database columns; repositories are obtained from the same
`DbClient`, `DbHandle`, or `DbTransaction` used for other database operations:

```cpp
RUVIA_DB_ENTITY(Device, "device",
    RUVIA_DB_COLUMN(id, std::int64_t,
        ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(enabled, bool))

auto devices = c.db().getRepository<Device>();
const ruvia::DbFindOptions findOptions{
    .where = (Device::column<"enabled">() == true)
        && Device::column<"name">().like("pump%"),
    .order = {{"id", ruvia::DbOrderDirection::kDesc}},
    .take = 50,
};
auto rows = co_await devices.find(findOptions);

Device changes;
changes.set<"name">("Main pump");
co_await devices.update(Device::column<"id">() == deviceId, changes);

auto transaction = co_await c.db().beginTransaction();
auto transactional = transaction.getRepository<Device>();
auto device = co_await transactional.findOne({
    .where = Device::column<"id">() == deviceId,
    .lock = ruvia::DbLockOptions{.mode = ruvia::DbRowLock::kUpdate},
});
co_await transaction.commit();
```

Pass transaction options when the operation needs a specific snapshot or access
mode. Both PostgreSQL and MariaDB apply them on the transaction's own connection:

```cpp
auto snapshot = co_await c.db().beginTransaction({
    .isolation = ruvia::DbTransactionIsolation::kRepeatableRead,
    .accessMode = ruvia::DbTransactionAccessMode::kReadOnly,
});
auto rows = co_await snapshot.getRepository<Device>().find();
co_await snapshot.commit();
```

The default options retain the server's defaults. Isolation also supports read
uncommitted, read committed, and serializable; access mode can explicitly select
read-write. The database's isolation semantics still apply. A failed operation
retires its transaction lease; start a new transaction before issuing more work.

`find`, `findOne`, `findAndCount`, `count`, `exists`, `insert`, `update`,
`increment`, `decrement`, `upsert`, `deleteBy`, and `remove` return cold
`ScopedOperation` objects for `co_await`. `findOne`
returns `std::optional<Entity>`. `insert` and `upsert` also accept a span of
entities. `upsert` takes `DbUpsertOptions{.conflictPaths = {"id"}}` on
PostgreSQL. MariaDB uses its distinct any-unique-key behavior, selected with
`.anyUniqueKey = true`. `remove(entity)` requires every primary-key field;
`update` and `deleteBy` require a condition.
Bulk upserts infer updated columns only when the input entities set the same
fields; otherwise specify `updateColumns` explicitly, including any deliberate
updates from database defaults.

Repository and query-builder names follow TypeORM's corresponding operations.
`findAndCount(options)` and `getManyAndCount()` return a pair suitable for
structured bindings: the selected entities and the total matching count before
`skip`/`take`. Relation joins count distinct root entities, and an empty page
still returns the total. The page and count execute sequentially; use a
repeatable-read transaction when they must observe one snapshot. Both statements
and all their parameters are owned before the cold operation is returned.

```cpp
const ruvia::DbFindOptions options{
    .where = Device::column<"enabled">() == true,
    .order = {{"id", ruvia::DbOrderDirection::kDesc}},
    .skip = 20,
    .take = 20,
};
auto [devices, total] = co_await repository.findAndCount(options);
const bool present = co_await repository.exists({
    .where = Device::column<"id">() == deviceId,
});
```

`exists(options)` and the builder's `getExists()` ignore pagination and use an
existence query. `increment(condition, "column", amount)` and `decrement(...)`
perform an atomic database update; they require a condition and a writable
numeric column. They do not read a value into the application before updating it.
`column<"field">().between(lower, upper)` expresses an inclusive range. Array
fields support `arrayContains`, `arrayContainedBy`, and `arrayOverlap` on
PostgreSQL, including typed empty arrays. Values are bound parameters and are
owned when the predicate is constructed.

PostgreSQL upserts accept `skipUpdateIfNoValuesChanged = true` and a typed
`indexPredicate` for partial unique indexes. The former compares writable update
columns with `IS DISTINCT FROM`, including NULL changes. On PostgreSQL, when there are no
columns to update, an upsert becomes insert-or-ignore. MariaDB rejects these
PostgreSQL-specific options.

See the runnable [ORM example](examples/web/orm.cpp) for pagination, existence,
counter updates, array conditions and transaction usage.

Relations are declared in the entity macro with one owning side and an optional
inverse side. `ManyToOne` stores the foreign-key column on the source table;
`OneToMany` names that owning relation. An owning `OneToOne` also stores a
foreign key and gets a unique constraint, while its inverse uses
`RUVIA_DB_INVERSE`. `ManyToMany` uses an owning `RUVIA_DB_JOIN_TABLE` mapping;
the inverse points back to the owning relation. Create referenced tables first,
then call `createRelationTables<OwningEntity>()` for junction tables. See
[`examples/web/orm_relations.cpp`](examples/web/orm_relations.cpp) for all four
mapping forms and nested relation queries.

Relations load data but do not persist it automatically. Write the owning
foreign-key column yourself, or declare a junction entity and use its repository;
there is no cascade, lazy/eager proxy, or schema-diff behavior. Loaded relations
use the same `isSet`, `isNull`, and `get` state model as fields: to-one relations
can be unset, NULL, or loaded, while a loaded collection is empty or contains
entities. Relation loading requires primary keys on the root and each selected
entity, including all components of a composite key. Root `.take`/`.skip`
pagination limits entities while retaining their complete selected collections:

```cpp
auto rows = co_await employees.find({
    .relations = {"department", "department.employees"},
    .take = 25,
});
auto builder = employees.createQueryBuilder("employee");
builder.leftJoinAndSelect("department", "department")
    .leftJoinAndSelect("department.employees", "colleague");
auto joined = co_await builder.getMany();
```

Each field distinguishes unset, SQL NULL, and a value. Unset insert fields use
database defaults; unset update fields are untouched. Declare nullable fields
with `.nullable = true` and use `setNull<"field">()` explicitly. Owning text
uses `std::pmr::string` or `ruvia::String`; PostgreSQL one-dimensional arrays
use `std::pmr::vector<T>`, with `std::optional<T>` elements when array elements
may be NULL. Column options select UUID, JSONB, timestamp, network, and other
database types independently of their owning C++ representation.

Use the entity query builder for conditions, ordering, pagination and declared
relation joins:

```cpp
auto builder = devices.createQueryBuilder("d");
builder.where(Device::column<"enabled">() == true)
    .orderBy("id", ruvia::DbOrderDirection::kDesc)
    .take(20);
auto selected = co_await builder.getMany();
```

`getMany()` and `getOne()` map all declared entity columns by default. `orderBy` and
`addOrderBy` accept declared entity column names and reject unknown names.
`getCount()` counts matching root entities without pagination. Generated SQL and
parameters can be inspected with `getQueryAndParameters()`; the inspection result
does not expose a mutable ORM statement. Writes use repository methods such as
`insert`, `update` and `deleteBy`.

### SQL expressions, projections and returning writes

`DbExpressions` owns expression nodes without an executable statement. It provides
`value`, `column`, `call`, `cast`, JSON operators, CASE, aggregates and window
functions. Pass application data through `value()`; `sql()` accepts trusted SQL
syntax, with arguments inserted between syntax parts and bound by the compiler:

```cpp
ruvia::DbExpressions x;
auto now = x.call("now");
auto merged = x.binary(x.column("payload"), ruvia::DbBinaryOperator::kJsonConcat,
    x.cast(x.value(jsonText), ruvia::DbDataType::kJsonb));
auto custom = x.sql({"jsonb_set(", ", '{label}', to_jsonb(", "::text))"},
    {x.column("payload"), x.value(label)});
const std::array changes{
    ruvia::DbAssignment{"updated_at", now},
    ruvia::DbAssignment{"payload", merged},
};
co_await events.update(Event::column<"id">() == id, changes);
```

Expression views borrow their `DbExpressions` owner until consumed. Builder and
repository methods copy expressions and values before returning; the owner and
input strings may then be destroyed. SQL syntax is developer-authored code and
must not contain interpolated request data. Dialect-specific syntax remains the
application's responsibility.

`select` declares output fields. An empty expression selects the named root-entity
column; an expression can select a joined column or compute a value. Output names
must be unique and belong to the requested result schema. A partial entity keeps
unselected fields unset (`isSet<"field">() == false`); NULL remains distinct from
unset. Use `RUVIA_DB_PROJECTION` for a DTO without a table binding:

```cpp
RUVIA_DB_PROJECTION(DeviceSummary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::pmr::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

ruvia::DbExpressions x;
auto query = devices.createQueryBuilder("d");
query.select({
    {"id"},
    {"label", x.call("upper", {x.column("name", "d")})},
    {"rank", x.over(x.call("row_number"),
        {.orderBy = {{x.column("id", "d")}}})},
});
auto summaries = co_await query.getMany<DeviceSummary>();
// getOne<DeviceSummary>() and getManyAndCount<DeviceSummary>() use the same mapping.

auto partial = devices.createQueryBuilder();
partial.select({{"id"}, {"name"}});
auto entities = co_await partial.getMany();
```

`insertReturning`, `updateReturning`, `upsertReturning` and `deleteReturning`
return `DbEntityRows<Output>`. The default output is the repository entity and the
default RETURNING list is its complete column set. An explicit list supports
partial entities and computed DTO fields. These operations require PostgreSQL;
unsupported drivers fail before starting I/O. Ordinary write methods continue
returning `DbExecResult`.

```cpp
const std::array fields{
    ruvia::DbSelection{"id"},
    ruvia::DbSelection{"label", x.call("upper", {x.column("name")})},
};
auto changed = co_await devices.updateReturning<DeviceSummary>(
    Device::column<"id">() == id, patch, fields);
// rank is unset because it was not requested.

const ruvia::DbUpsertOptions options{
    .conflictPaths = {"id"},
    .skipUpdateIfNoValuesChanged = true,
    .updateExpressions = {{"revision", x.binary(x.column("revision", "devices"),
        ruvia::DbBinaryOperator::kAdd, x.value(1))}},
    .updateWhere = x.binary(x.excluded("revision"),
        ruvia::DbBinaryOperator::kGreater, x.column("revision", "devices")),
};
auto saved = co_await devices.upsertReturning(entity, options);
```

Custom upsert expressions override inferred or explicit `updateColumns` for the
same column and may add another writable column. `updateWhere` is combined with
the optional change-detection condition using AND. Change detection compares
against each actual update expression. A conflict whose update condition is false
produces no RETURNING row; do not assume one returned entity per input entity.

Entity-bound builders support unrelated entity joins, derived queries and table
functions. CTE and subquery inputs are other repository builders:

```cpp
auto recent = devices.createQueryBuilder("recent_device");
recent.where(Device::column<"revision">() > 2);
auto query = devices.createQueryBuilder("d");
query.with("recent", recent, {.materialization = ruvia::DbMaterialization::kMaterialized});
query.joinCte(ruvia::DbJoinType::kInner, "recent", "r",
    x.binary(x.column("id", "d"), ruvia::DbBinaryOperator::kEqual, x.column("id", "r")));

auto latest = devices.createQueryBuilder("candidate");
latest.where(x.binary(x.column("id", "candidate"), ruvia::DbBinaryOperator::kEqual,
    x.column("id", "d"))).take(1);
query.join(ruvia::DbJoinType::kLeft, latest, "latest", x.value(true), {.lateral = true});
auto rows = co_await query.getMany();
```

Use `join<OtherEntity>(type, alias, on)` for an unrelated table and `joinFunction`
for table-valued functions. `subquery(expressions)` and `exists(expressions)`
produce expressions from an entity builder. `combine` supports set operations;
`with(..., {.recursive = true})` declares recursive CTEs, whose recursive branches
may reference their name through `joinCte`. `groupBy`, `having`, expression
`orderBy`, and `DbExpressions::over` compose grouped and window queries.
Explicit projections use flat row mapping and joins without automatic relation
hydration; `leftJoinAndSelect` / `innerJoinAndSelect` retain full-entity relation
loading. Ordinary joins preserve SQL row multiplicity, and counts reflect those
rows or groups. Only relation-loading joins deduplicate root entities.

Declare a PostgreSQL enum type name and a SQL default directly on entity columns:

```cpp
RUVIA_DB_ENTITY(Event, "events",
    RUVIA_DB_COLUMN(id, std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(state, std::pmr::string, ruvia::DbColumnOptions{
        .enumName = ruvia::FixedString{"app.event_state"},
        .defaultExpression = ruvia::FixedString{"'pending'::app.event_state"}}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string, ruvia::DbColumnOptions{
        .dataType = ruvia::DbDataType::kTimestampTz,
        .defaultExpression = ruvia::FixedString{"now()"}}))
```

`enumName` references an existing enum type, including its schema; create the type
in a migration before creating the table. It cannot be combined with a scalar
`dataType` or size modifiers. `FixedString` preserves literal length at compile time
without a fixed metadata length limit. Declared defaults are applied by
`DbSchema::createTable<Entity>()`; duplicate defaults in table options are rejected.
Defaults remain database expressions and are not evaluated when constructing a
C++ entity.

### Composable entity writes

`update` and `updateReturning` accept either `DbPredicate` or `DbExpression`
conditions, including `IS DISTINCT FROM`, subquery conditions and trusted SQL
expression fragments. Empty conditions are rejected. For example:

```cpp
ruvia::DbExpressions x;
auto changed = x.binary(x.column("state"), ruvia::DbBinaryOperator::kIsDistinctFrom,
    x.value(nextState));
co_await commands.update(changed, patch);
```

Repositories provide `createUpdateBuilder(alias)`, `createDeleteBuilder(alias)`
and `createInsertBuilder()`; the insert factory also accepts an entity or a span
of entities. Each builder keeps its repository's target entity and executor.
`set` accepts an entity patch or a declared column plus an expression. Update and
delete builders require a nonempty `where`; `andWhere` and `orWhere` compose typed
predicates or expressions. Inspect SQL with `getQueryAndParameters()`, call
`execute()` for `DbExecResult`, or declare `returning(...)` and use
`getMany<EntityOrDto>()` for owning typed results.

`updateFrom<SourceEntity>(alias)` reads an entity table;
`updateFrom(selectBuilder, alias)` reads a derived query; and
`updateFromCte(name, alias)` reads a CTE. Expressions use these explicit aliases.
`insertFrom({targetColumns...}, selectBuilder)` maps selected values by position,
validates the column count and rejects unknown or computed target columns.
The query builder's `fromCte(name)` reads a CTE using its existing root alias and
result schema. These methods copy their inputs immediately.

Both read and write builders accept another write builder in `with(name, writer)`.
Attach all data-modifying CTEs to the outermost statement, in dependency order;
read their `RETURNING` rows through CTE references. This follows the composable
write-builder approach in [TypeORM's CTE API](https://typeorm.io/docs/query-builder/select-query-builder/#common-table-expressions).
The following PostgreSQL statement claims pending commands and persists their
returned fields into an archive atomically:

```cpp
ruvia::DbExpressions x;
auto candidates = commands.createQueryBuilder("candidate");
candidates.where(Command::column<"state">() == "pending")
    .orderBy("id").take(100)
    .setLock({.mode = ruvia::DbRowLock::kUpdate, .skipLocked = true});

auto claim = commands.createUpdateBuilder("command");
claim.updateFromCte("candidates", "candidate")
    .set("state", x.value("claimed"))
    .where(x.binary(x.column("id", "command"), ruvia::DbBinaryOperator::kEqual,
        x.column("id", "candidate")))
    .returning({{"id"}, {"state"}});

auto claimed = commands.createQueryBuilder("claimed");
claimed.fromCte("claimed").select({{"id"}, {"state"}});
auto persist = archive.createInsertBuilder();
persist.with("candidates", candidates)
    .with("claimed", claim)
    .insertFrom({"command_id", "state"}, claimed)
    .returning();
auto saved = co_await persist.getMany();
```

The outer `getMany()` submits one statement; constructing or attaching a builder
does not execute it. The same builders work with transaction repositories.
Use unique source keys when one source row must determine each updated target.
`UPDATE FROM`, data-modifying CTEs and `RETURNING` require PostgreSQL in this API;
unsupported dialects fail during compilation, before I/O.

A read builder containing a write CTE supports flat `getMany`/`getOne` mapping.
`getManyAndCount` rejects it because that API executes two statements. Count,
existence and subquery wrappers also reject write CTEs; reference their returned
rows explicitly in the outer query instead. A SELECT limit controls returned
rows, not how many rows the CTE modifies. PostgreSQL executes data-modifying CTEs
once to completion under one statement snapshot; pass data between writes through
`RETURNING`, and avoid modifying the same row twice in one statement. See
[PostgreSQL's data-modifying WITH rules](https://www.postgresql.org/docs/current/queries-with.html#QUERIES-WITH-MODIFYING).

### Direct SQL and structured queries

This route operates on statements and raw rows independently of entity ORM:

```cpp
ruvia::DbQuery query;
query.select(query.column("name")).from("devices");
query.where(query.binary(query.column("enabled"),
    ruvia::DbBinaryOperator::kEqual, query.value(true)));
auto rows = co_await c.db().query(query);
```

`DbQuery` owns a relational statement and generates SQL for the selected
driver. Its input is identifiers, values, expressions, and nested statements:
`value()` binds data, `column()` quotes names, and `call()` invokes a named
database function. `sql(parts, arguments)` supports trusted expression syntax
interleaved with bound expression arguments. `coalesce`,
`nullIf`, casts, CASE, tuples, arrays, JSON/network operators, window frames,
aggregate FILTER/order, and table functions are explicit expression nodes.
Reusing an expression also reuses its PostgreSQL parameter positions.

| Query requirement | Structured API |
| --- | --- |
| Recursive, materialized, and data-changing CTEs | `with`, `DbCteOptions` |
| Bulk input, conflict updates, partial conflict targets | `values`, `insertFrom`, `onConflict`, `excluded` |
| Joins, correlated subqueries, lateral JSON expansion | `join`, `from`, `exists`, `subquery`, `joinFunction` |
| Latest values and time-series aggregation | `distinctOn`, `over`, `aggregate`, `filter`, `call` |
| Unions and grouped result sets | `combine`, `groupBy`, `having` |
| Queue claims and conditional writes | `lock`, `updateFrom`, `deleteUsing`, `returning` |

Common SQL operations compile for PostgreSQL and MariaDB. PostgreSQL-specific
operators, partial indexes, procedures, and TimescaleDB functions retain their
database requirements; unsupported dialect combinations fail during
compilation. These capabilities do not make a TimescaleDB workload portable
to a database without equivalent features.

Queries, predicates, and entities consume input values synchronously. An
operation owns its SQL and parameters before it is returned, so its builder
and input strings may be destroyed before awaiting it. Returned entities own
their fields independently of backend rows and subsequent operations. Keep
the originating client/worker and its memory resource alive until its results
are destroyed. A transaction repository borrows its transaction; use it before
that transaction is moved or destroyed.

### Redis query result caching

Enable `RUVIA_ENABLE_REDIS` together with either database driver. Set
`DbConfig::cache` to configure a worker-local Redis cache for that database;
App registrations and standalone `DbClient` use the same implementation.

```cpp
using namespace std::chrono_literals;
ruvia::DbConfig settings{
    .driver = ruvia::DbDriver::kPostgreSql,
    .username = "app",
    .database = "devices",
    .cache = ruvia::DbCacheConfig{
        .options = {.host = "127.0.0.1", .port = 6379},
        .duration = 1s,
        .nameSpace = "devices"}};
```

The API follows TypeORM's query-cache conventions. Configuring Redis makes the
cache available; individual queries opt in unless `alwaysEnabled = true`.
The default duration is one second. Use `std::chrono::milliseconds` or another
convertible duration instead of TypeScript's bare millisecond numbers.

```cpp
auto devices = db.getRepository<Device>();
const ruvia::DbFindOptions options{
    .order = {{"id"}},
    .take = 20,
    .cache = ruvia::DbCacheOptions{.id = "device-page", .milliseconds = 30s}};
auto [page, total] = co_await devices.findAndCount(options);
auto count = co_await devices.count(ruvia::DbFindOptions{.cache = 5s});

auto query = devices.createQueryBuilder("device");
query.where(Device::column<"id">() == 1).cache("device-one", 10s);
auto device = co_await query.getOne();
query.cache(false); // bypass even when alwaysEnabled is true
```

`cache(true)` uses the configured duration; `cache(5s)` uses an automatic key;
`cache("id", 5s)` selects an explicit ID. Find options accept `true`, `false`,
a duration, or `DbCacheOptions`. Automatic keys distinguish SQL, bound parameter
values and types, and database driver. Explicit IDs deliberately identify a
result independently of SQL: use a different ID for each filter, page, tenant,
or result shape. Choose distinct namespaces for databases sharing Redis.

Caching applies to entity reads, raw row reads, counts, existence checks, and
relation loading. `findAndCount()` / `getManyAndCount()` cache the page and total
separately; an explicit ID uses `"<id>-count"` for the total. These remain two
sequential reads and can have different cache ages.

Writes do not automatically invalidate cached results. Remove IDs explicitly
when freshness matters, or wait for their TTL:

```cpp
const std::array<std::string_view, 2> ids{"device-page", "device-page-count"};
co_await db.queryResultCache().remove(ids);
co_await db.queryResultCache().clear();
```

`queryResultCache()` is available on `DbClient` and `DbHandle` (including
`c.db()`). `clear()` scans and removes only this cache namespace, preserving
unrelated Redis keys. Like ID removal, it is not a barrier against concurrent
queries refilling the cache.

`ignoreErrors = true` falls back to the database on cache read/decode failures
and returns database results when cache writes fail. Explicit cancellation and
shutdown still propagate; database errors are never suppressed. Redis connection
validation at startup and explicit remove/clear failures also propagate.
`OperationOptions::timeout` bounds the entire operation, including cache access,
SQL execution, and cache writes; pagination pairs and remove/clear loops share
that same budget. Exhausting it is never ignored by `ignoreErrors`.

Cache settings also apply to transaction repository reads. Cached results are
shared outside the transaction and do not provide its snapshot guarantees;
cache misses can publish the transaction's uncommitted reads. Set `cache(false)`
for reads that must observe transaction isolation or read-your-writes behavior.
Locking reads, DML statements, and queries with DML CTEs bypass caching. Raw SQL
`query()` and streaming reads do not use this cache.

Queries synchronously own their parameters and cache IDs before returning a
cold operation. Cache hits skip database execution. Returned rows and mapped
entities own their result storage until destruction, independently of Redis
replies, subsequent queries, and cache invalidation.

The ORM example enables caching when `RUVIA_ORM_CACHE_REDIS_PORT` is set; optional
`RUVIA_ORM_CACHE_REDIS_HOST` selects the Redis host:

```bash
RUVIA_ORM_CACHE_REDIS_PORT=6379 ./build/examples/ruvia_example_orm --migrate --run
```

### Generated schema and migrations

Include `ruvia/web/db/DbSchema.h` to build versioned migrations without SQL:

```cpp
ruvia::DbSchema schema({.driver = ruvia::DbDriver::kPostgreSql});
schema.createTable<Device>({.ifNotExists = true});
schema.createIndex({.name = "device_name_idx", .table = "device",
    .keys = {{.column = "name"}}});
const auto migrations = schema.compile("001_devices");
const auto report = ruvia::DbMigrator::migrate(config, migrations);
```

`DbTableDefinition` also describes tables directly. Schema operations cover
columns, defaults, identity, composite keys, foreign keys, checks, enums,
extensions, views, expression/partial/GIN indexes, and explicit data changes.

Fixed-length character columns use `DbDataType::kChar` with an explicit positive
length; `kVarchar` selects variable-length character columns:

```cpp
RUVIA_DB_ENTITY(Token, "tokens",
    RUVIA_DB_COLUMN(digest, ruvia::String,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kChar, .length = 64}))
```

This produces `CHAR(64)` in PostgreSQL and MariaDB schemas. The same type and
length can be used in `DbTypeDefinition` for column changes and casts. PostgreSQL also
supports character arrays. Length is measured in database characters; padding
and comparison follow the database's character-type semantics.

Computed columns use `DbGeneratedType` independently of auto-generated identity
values. Declare the field's storage mode in its entity metadata and supply its
expression when creating the table:

```cpp
RUVIA_DB_ENTITY(LineItem, "line_item",
    RUVIA_DB_COLUMN(id, std::int64_t,
        ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(quantity, std::int64_t),
    RUVIA_DB_COLUMN(unit_price, std::int64_t),
    RUVIA_DB_COLUMN(total, std::int64_t,
        ruvia::DbColumnOptions{.generatedType = ruvia::DbGeneratedType::kStored}))

ruvia::DbQuery expressions;
schema.createTable<LineItem>({.generatedColumns = {{"total",
    expressions.binary(expressions.column("quantity"),
        ruvia::DbBinaryOperator::kMultiply, expressions.column("unit_price"))}}});
```

Computed fields are read normally and excluded from repository inserts, updates,
and upserts, including explicitly assigned values. They cannot also have an
identity or default. Direct table definitions use `DbSchemaColumn::generatedType`
and `asExpression`. PostgreSQL compilation supports stored columns; MariaDB also
supports `kVirtual`. See [orm_columns.cpp](examples/web/orm_columns.cpp) for a
runnable example of computed writes, retained results, and read-only snapshots.
MariaDB generated fields require `.nullable = true` and cannot be primary keys;
use an explicit table CHECK constraint when their expression must never be NULL.

`DbProcedure` supplies declarations, assignments, SELECT INTO, conditional
branches, exception handlers, and returns for migration blocks and trigger
functions. `apply(schema)` places generated schema changes inside such a
block; `createTriggerFunction` and `createTrigger` install trigger behavior.

TimescaleDB operations include `createHypertable` (the `by_range` API available
since TimescaleDB 2.13), `setChunkTimeInterval`, `setCompression`, compression
and retention policies. Pass interval expressions such as
`q.cast(q.value("7 days"), ruvia::DbDataType::kInterval)`; integer time columns
can use integer interval values. The extension must be installed on the server.

PostgreSQL batches compile to one atomic migration. An unwrapped operation,
such as a concurrent index, occupies its own batch. MariaDB DDL commits
implicitly, so each generated statement receives its own numbered migration
ID. Schema changes are explicit; obtaining a repository does not synchronize
or alter tables.

The PostgreSQL example [orm.cpp](examples/web/orm.cpp) prints its generated
schema and queries by default. `ruvia_example_orm --migrate --run` applies and
executes it against `RUVIA_DB_HOST`, `RUVIA_DB_PORT`, `RUVIA_DB_USER`,
`RUVIA_DB_PASSWORD`, and `RUVIA_DB_DATABASE`.

### SQL migrations

`DbMigrator` applies pending migrations synchronously, under a backend lock so
that concurrent deployers serialize. It runs its own event loop and blocks, so
it belongs in startup code rather than on a worker:

```cpp
static constexpr std::array migrations{
    ruvia::DbMigration{{.id = "001_create_users",
        .sql = "CREATE TABLE IF NOT EXISTS users ("
               "id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
               "name VARCHAR(120) NOT NULL)"}},
};
const auto report = ruvia::DbMigrator::migrate(config, migrations);
```

Each `DbMigration` is exactly one statement -- neither backend accepts more
than one per call -- and its id is recorded in a migrations table so later runs
skip it. The default table is `ruvia_schema_migrations`; use
`DbMigratorOptions{.table = "..."}` only when deliberately adopting another
table. Ids that differ only in letter case are rejected, because a
case-insensitive collation would treat them as the same migration. The text is
recorded as a digest alongside the id, so editing a migration that has already
run is reported rather than silently skipped.

Use SQL migrations for database features without a structured schema operation,
including independent sequences, constraint renaming, ordinary SQL functions,
procedural loops, and transition-table triggers. A PostgreSQL dollar-quoted
function or `DO` body is one statement and can contain its own SQL statements.
Runtime table locks and temporary tables belong on the same `DbTransaction` as
the work that uses them. PostgreSQL `LISTEN` notification consumption requires a
dedicated driver connection; the ORM does not provide a notification subscriber.

On PostgreSQL the statement and the row recording it commit together, so an
interruption cannot leave the schema changed and unrecorded. A statement the
backend refuses inside a transaction block names the exception, per migration:

```cpp
ruvia::DbMigration{{.id = "002_index",
    .sql = "CREATE INDEX CONCURRENTLY items_value_idx ON items (value)",
    .atomicity = ruvia::DbMigrationAtomicity::kUnwrapped}},
```

MariaDB commits DDL implicitly, so there the two statements are always separate
and an interruption between them re-runs the migration on the next start: write
MariaDB migrations to be re-applicable.

## Redis ORM

Enable `RUVIA_ENABLE_REDIS` and include `ruvia/web/redis/RedisRepository.h`.
This selects the entity ORM route. The original `RedisHandle` commands,
pipelines, transactions and Lua API remain available as the separate direct
route, illustrated in [redis.cpp](examples/web/redis.cpp).
Declare Redis entities with `RUVIA_REDIS_ENTITY` and `RUVIA_REDIS_COLUMN`;
SQL entities use `RUVIA_DB_ENTITY` and `RUVIA_DB_COLUMN`. Repositories reject
entities declared for the other backend. The two entity types share field-access
and predicate syntax, `DbFindOptions`, `DbEntityRows` and `DbExecResult`.
`RedisColumnOptions` exposes only `primaryKey` and `nullable`; Redis prefix and
Search indexes are configured separately:

```cpp
RUVIA_REDIS_ENTITY(User, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::String,
        ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::String),
    RUVIA_REDIS_COLUMN(age, std::uint32_t),
    RUVIA_REDIS_COLUMN(note, ruvia::String,
        ruvia::RedisColumnOptions{.nullable = true}));

const ruvia::RedisRepositoryConfig userRedisConfig{
    .prefix = "app:users",
    .indexes = {
        {.column = "name", .kind = ruvia::RedisIndexKind::kTag, .sortable = true},
        {.column = "age", .kind = ruvia::RedisIndexKind::kNumeric},
    },
};

auto users = c.redis().getRepository<User>(userRedisConfig);
User user(c.pool());
user.set<"id">("u-42");
user.set<"name">("Alice");
user.set<"age">(25);
const auto inserted = co_await users.insert(user, {.ttl = std::chrono::hours(1)});
if (inserted.affectedRows() == 0) {
    // The primary key already exists; neither fields nor TTL were changed.
}

auto loaded = co_await users.findOne({.where = User::column<"id">() == "u-42"});
User changes(c.pool());
changes.set<"name">("Alice Smith");
changes.setNull<"note">();
co_await users.update(User::column<"id">() == "u-42", changes);
const auto expiration = co_await users.ttl(User::column<"id">() == "u-42");
```

Each Redis entity needs exactly one non-nullable string or integer primary key;
applications supply the key explicitly. The second argument of
`RUVIA_REDIS_ENTITY` provides the default prefix. Keys are
`ruvia:orm:<hex-encoded-prefix>:<id>`, so namespaces remain isolated when IDs
contain colons. Use a distinct prefix per entity schema and reserve its keys for
the repository. Column names beginning with `__ruvia_` are reserved.

Hash mapping supports owning strings (`ruvia::String` or `std::pmr::string`),
booleans, supported native integer/floating types and Ruvia scalar wrappers.
Array, JSON, nested columns and SQL relation descriptors are unsupported.
Redis rejects relation loading, SQL locking and enabled query caching.

`insert` creates absent entities; `update` changes existing entities; `upsert`
merges supplied fields and inserts when absent. `deleteBy` and `remove` delete an
entity. Mutations return `DbExecResult`: `affectedRows()` is zero or one and
`lastInsertId()` is empty. `update`, `deleteBy`, `expire` and `ttl` require an exact
single-primary-key equality predicate. Unset fields are preserved on updates;
`setNull` removes a nullable hash field. Empty strings remain values. New entities
require all non-nullable fields. Updates cannot change their primary key.

Writes and TTL changes execute atomically in one single-key Lua script. Omitted
TTL preserves existing expiration; new entities are persistent. Set `.ttl` to a
positive duration or `.persist = true` to remove expiration, but not both.
`expire(predicate, seconds)` changes expiration and `ttl(predicate)` returns
status-bearing `RedisTtl` with millisecond precision. There is no implicit dirty
tracking, optimistic version check or cross-key transaction. Redis Cluster
routing follows the capabilities of the existing Redis driver.

### Redis Search queries

Primary-key `findOne` and mutations work with ordinary Redis. Field queries
require Redis Search with HASH indexing and query dialect 2. Create an index
explicitly once during deployment through a request or `WebWorkerContext` handle:

```cpp
co_await users.createIndex();
auto [matches, total] = co_await users.findAndCount({
    .where = (User::column<"name">() == "Alice Smith")
        && (User::column<"age">() >= 18),
    .order = {{.column = "name", .direction = ruvia::DbOrderDirection::kAsc}},
    .skip = 0,
    .take = 20,
});
```

`find` returns `DbEntityRows<User>`, `findOne` returns `std::optional<User>`, and
`count` counts matches independently of pagination. `exists` accepts the same
`DbFindOptions`. `findAndCount` returns the page and search engine total from one
command. Results may omit documents that expire while Search loads them. Empty
predicates match the whole index; `find` defaults to a page of 100. Search supports
one sortable column per query; equal values do not guarantee stable page order.

`kTag` provides exact case-sensitive string/boolean matching using encoded shadow
fields, preserving punctuation, commas, empty strings and binary values.
`kNumeric` provides comparisons and ranges; indexed values and query operands
must be finite and within `[-(2^53-1), 2^53-1]`. Unindexed integer columns retain
their full native range. `kText` creates a text index for external Search clients;
SQL `like`/`ilike` are not translated into text-search semantics. Unsupported SQL
expressions fail before sending a command. Null predicates use internal presence
fields; inequality comparisons exclude absent fields.

Index creation is explicit; an existing index or missing Search capability
produces a Redis error. `dropIndex()` retains entity hashes. Repositories inherit
the Redis handle's scope, worker affinity, timeout and cancellation. Configuration
and operation inputs are copied into owned worker PMR storage before asynchronous
execution. Results retain their own reclaimable storage independently of later
operations and must be destroyed before that worker resource expires.

[redis_orm.cpp](examples/web/redis_orm.cpp) demonstrates validated JSON input,
insertion with TTL, primary-key lookup, indexed queries and response models.
Build `ruvia_example_redis_orm`; run once with `--create-index` against Redis Search,
then without arguments to serve on `127.0.0.1:8091`. It reads `RUVIA_REDIS_HOST`,
`RUVIA_REDIS_PORT`, `RUVIA_REDIS_USER` and `RUVIA_REDIS_PASSWORD` from environment
variables or `.env`.

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
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(my_app PRIVATE ruvia::web)
```

Ruvia's Windows archives use the static MSVC runtime (`/MT`, or `/MTd` for
Debug), so Windows consumers must select the same runtime before creating
targets that link them.

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
only configuration and lifecycle entry point. Controllers use CRTP and register
themselves at startup when their route macro block is declared. Controller
static or object libraries are linked with
`ruvia_link_controllers(application controllers)`. Dynamically loaded modules
must be present before `run()`.
Routes and schemas use these macros:

| Concern | Macros |
| --- | --- |
| Controller grouping | `RUVIA_CONTROLLER_GROUP(...)` |
| Route table | `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END` |
| HTTP methods | `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE` |
| Streaming / SSE | `RUVIA_GET_STREAM`, `RUVIA_GET_SSE` |
| WebSocket | `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS` |
| Models | `RUVIA_REQUEST_MODEL`, `RUVIA_RESPONSE_MODEL`, `RUVIA_REQUIRED_FIELD`, `RUVIA_OPTIONAL_FIELD` |
| Validation | Field rules on `RUVIA_REQUIRED_FIELD` / `RUVIA_OPTIONAL_FIELD`; route bindings `JsonBody<T>` / `QueryModel<T>` / `PathModel<T>` |

Route tables, middleware chains, and controller instances are finalized before
workers start.

After a WebSocket upgrade, `ServerConfig::idleTimeout` no longer applies to
that connection. Use `WebSocketRouteConfig::lifecycle.heartbeat` to configure
idle Ping and matching-Pong deadlines; the route's `closeHandshakeTimeout`
controls close completion independently. Without heartbeat, an idle WebSocket
has no framework idle deadline: the application owns any required liveness
policy. Ordinary HTTP connection timeouts remain unchanged.

`Context::arena()` and `allocator()` use the request arena. For WebSocket
and response-stream routes, that arena stays alive for the whole handler,
including its handshake and middleware state. Destroying an arena-backed object
does not reclaim its individual allocation.

DB, Redis, HTTP client, response-stream, SSE, and WebSocket operations select
their owning worker's reclaimable pool automatically. Borrowed inputs are
copied before the operation is returned, so the source only needs to survive
the synchronous call. Stream and WebSocket writes can take an owned PMR string:
compatible storage is moved without copying, while storage from another
resource is copied into the writer's pool before the call returns.

Operation arguments are released with their operation. Owned return values hold
their storage independently and remain valid across later operations until
they are destroyed. Keep operations and results on their owning worker and
within their owner's scope; Context handles and results belong to the
Context's scope. These rules also apply to handles obtained before an upgrade.
Borrowing accessors on owning models, form data and result containers use
`RUVIA_LIFETIMEBOUND` where supported and reject temporary owners where applicable.
These annotations assist compiler diagnostics; they do not extend storage lifetime
or guarantee detection of asynchronous escapes. `c.req()` is a temporary facade:
its views borrow the request, not the facade object. Copy data that must outlive
the request or the next buffer-invalidating operation.

Borrowed body chunks and WebSocket payload views retain their documented
validity until the next read on the same stream or connection.

Ordinary code does not need to select an allocator. For example, a handler can
transfer an owned Redis value directly to its response stream:

```cpp
auto value = co_await c.redis().get("status");
if (value) {
    co_await c.stream().write(std::move(*value));
}
```

When constructing temporary owning PMR data yourself, use `c.pool()`.
Choose by storage lifetime: request metadata and response storage use the arena;
scratch buffers that can be discarded after a call or loop iteration use the
pool.

Each pool-backed object's destruction returns its storage for reuse without
invalidating other live objects. Cached pool storage may remain allocated until
the pool is destroyed; reclamation does not promise a drop in process RSS.
Clients can own separate worker-local pools, so `c.pool()` is not guaranteed to
equal a client's result resource. A transfer avoids copying only when the source
and destination resources are compatible. Moving an object originally
allocated in the request arena does not reclaim its arena storage.
Both `c.arena()` and `c.pool()` return `std::pmr::memory_resource*` for use
with PMR containers. Objects allocated from either must stay on the owning
worker and be destroyed within the Context's scope. Posted jobs use
`WebWorkerContext::pool()` for that same worker pool; they have no request
arena.

Default error responses use RFC 9457 `application/problem+json`: `type` is
`about:blank`, `title` describes the HTTP status, `status` matches the response,
and `detail` describes the failure. The `code` extension is a stable application
error code. Validation failures also include an `errors` array of
`{ "field": "...", "code": "...", "message": "..." }` entries. No `instance`
is generated or request URL echoed. A custom `onError` can replace this document.

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
Self-contained callbacks passed to App are owned and destroyed with the App.

Redis time APIs avoid exposing wire-level sentinel values in application code:
`expireAt()` accepts `std::chrono::system_clock::time_point`, `ttl()` and
`pttl()` return `RedisTtl` (`missing`, `persistent`, or an expiring duration),
and scan options accept an optional continuation cursor. Scan results expose
`done()` and `nextCursor()`, so Redis's wire-level zero sentinel never doubles
as both an initial and terminal application state.

Each Redis alias owns an ordinary pool plus a lazy blocking pool. Typed blocking
commands (`blpop()`, `brpop()`, and blocking `xreadGroup()`) and blocking raw
commands are routed to the isolated pool automatically, so application code does
not maintain a second alias. `RedisHandle::withOptions()` applies an end-to-end
timeout and `StopToken` to typed commands and is inherited by pipelines and
transactions. Commands and batch execution do not accept a second per-call
operation policy. Redis defaults bound connect,
pool acquisition, and command execution, while `std::nullopt` explicitly disables
an individual default. Redis enables TCP no-delay by default and leaves TCP
keepalive at the system default unless `tcpKeepAlive` is set explicitly.
Deadlines use worker timers rather than maintenance-scan
granularity. Cancelling active I/O closes and discards only its socket, and that
pool slot reconnects before its next command. An infinite block therefore
requires either a stoppable token or a finite command timeout. A request handler
can pass `c.stopToken()` to stop work when its server worker shuts down.

All SET modes use `set(key, value, RedisSetOptions)` and return
`RedisSetResult`: `applied()` reports whether the write happened and
`previous()` carries the old value when `previousValue` is
`RedisSetPreviousValuePolicy::kReturn`. Expiry, NX/XX, and GET behavior are
options rather than separate `setEx()`, `setNx()`, or `getSet()` commands.
`xreadGroup()` uses `RedisXReadGroupAcknowledgementPolicy` for pending-entry
tracking versus Redis `NOACK`. `blpop()` and `brpop()` take `RedisBlockWait`,
using `forDuration()` for a finite Redis wait or `indefinitely()` for an
explicit unbounded wait.

Request and response models have separate roles and a compact declaration:

```cpp
RUVIA_REQUEST_MODEL(AddressRequest,
    RUVIA_REQUIRED_FIELD(city, ruvia::String));

RUVIA_REQUEST_MODEL(CreateUserRequest,
    RUVIA_REQUIRED_FIELD_NAME("user_name", username, ruvia::String),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32),
    RUVIA_REQUIRED_FIELD(address, AddressRequest),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

RUVIA_RESPONSE_MODEL(UserResponse,
    RUVIA_REQUIRED_FIELD(id, ruvia::UInt64),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(avatar, ruvia::String, RUVIA_EMIT_NULL));
```

`RUVIA_REQUEST_MODEL` supports `fromJson<T>()`, `fromForm<T>()` (parse only, no
field rules), route parsing, and validation. Every JSON response is first
represented by a `RUVIA_RESPONSE_MODEL`; `toJson()` and `c.json()` accept
response models only, and there is no dynamic object/array writer API.
Runtime-sized collections are declared as `ruvia::Array<T>` fields. A request
model may only nest request models, and a response model may only nest response
models. Both roles support `ruvia::Array<T>` and recursive `ruvia::BoxedArray<T>`
fields. Form, query, param, header, and cookie binding remain flat scalar inputs.

A model's allocation resource stays fixed. Public field assignment and collection
insertion own strings and recursively normalize nested values to that resource.
`Array<T>` and `BoxedArray<T>` also use their resource for newly constructed
elements. Move construction transfers the complete value; move assignment keeps
the destination resource and can allocate. JSON view parsing still borrows its
input, which must outlive the parsed view.

Fields use compile-time accessors: `model.get<"username">()`,
`model.set<"name">("Ada")`, `model.ensure<"tags">()`, and
`model.reset<"avatar">()`. Required `get` returns `const T&`; optional `get`
returns `const std::optional<T>&`. A missing optional request property stays
empty. An explicit JSON `null` on an optional field is also empty (`kNull`) and
does not apply `RUVIA_DEFAULT`; on a required field it is `invalid_type`. An unset
optional response property is omitted by default; `RUVIA_EMIT_NULL` writes it as
`null`, and `RUVIA_OMIT_EMPTY` omits present empty values. The source field name
(`username`) is used by `get`/`set`; a `*_FIELD_NAME` wire name (`user_name`) is
used in JSON and validation paths.

`ValidationError` owns its message, code, and all issue details independently of
the validator or request arena, including when the exception is copied or moved.

Request and response models bind JSON through schema. `JsonValue` and
`JsonObject` may be model fields; they also parse a complete JSON document for
`isObject()` / `isArray()` / `isNull()`, `view()`, and `get<T>("field")`. They
are not a `c.json()` / `toJson()` writer. URL-encoded form binding stays schema-based. Raw `bytes()` /
`text()` remain available for custom formats. Buffered `multipart()` and
streaming `multipartReader()` expose flat protocol parts, preserving repeated
names and file metadata without interpreting dotted names or array suffixes.

Request models declare field rules on `RUVIA_REQUIRED_FIELD` / `RUVIA_OPTIONAL_FIELD`.
Routes select the source with `ruvia::JsonBody<T>`, `FormBody<T>`,
`QueryModel<T>`, `PathModel<T>`, `HeaderModel<T>`, or `CookieModel<T>`. A
handler returns `Task<HttpResponse>` and explicitly serializes a response model with `c.json(model)`.
Core and Web use the same `Task<T>` with an explicit result type;
operations without a result use `Task<void>`. Ordinary route
handlers return HTTP responses, while services can return typed model values:

```cpp
ruvia::Task<UserResponse> getUser(std::uint64_t id,
    std::pmr::memory_resource* requestResource) {
    UserResponse response({.resource = requestResource});
    response.set<"id">(ruvia::UInt64{id});
    response.set<"name">("Alice");
    co_return response;
}

ruvia::Task<ruvia::HttpResponse> handler(ruvia::Context& c) {
    auto response = co_await getUser(1, c.arena());
    co_return c.json(response);
}
```

The model uses the supplied request arena and must not outlive the request.
`jsonIf` / `formIf` still parse without running those field rules. JSONB
passthrough uses `c.req().validatedJson<T>().raw()`.

Route middleware keeps the typed `c.req().validated<T>()` API, while
`c.req().validatedJson<T>()` also exposes the validated original bytes through
`raw()` for JSONB passthrough. See the compiled
[`models_validation.cpp`](examples/web/models_validation.cpp) example for a
complete request/response, nested, array, default, and validation example.

`TestApp` uses the production route graph and one real Ruvia worker, preserving
worker-local state, route body and rate limits, and `Deadline` cancellation.
`request()` remains a synchronous test facade: it waits for the worker to finish
dispatch and copies the response out of request-owned storage before returning.

`SecurityHeadersConfig` uses `DefaultSecurityHeaderPolicy::kEmitDefault` for
the built-in `nosniff`, `DENY`, and TLS-only HSTS defaults; use `kOmit` for any
of those headers you want to supply yourself. It also defaults to
`XssProtectionHeaderPolicy::kEmitDisabled`, emitting `X-XSS-Protection: 0`
because obsolete browser filters can create security issues; `kOmit` omits that
header, while Content Security Policy remains the modern content control. The
default `SecurityHeaderConflictPolicy::kPreserveExisting` leaves handler-supplied
headers in place; use `kReplaceExisting` when security defaults should override
them.

With Redis enabled, `SessionMiddleware` binds one typed request capability.
Use `auto session = c.session()` followed by `data()`, `set()`, `clear()`, or
`regenerate()`; `trySession()` returns `std::nullopt` when the middleware is not
present. `SessionConfig` owns its Redis alias, cookie name, key prefix, and TTL,
so a designated-initialized temporary is safe to register. Session changes are
saved before the response head is sent, including the first SSE/stream write and
WebSocket handshake. Once submission begins, `set()`, `clear()`, and `regenerate()`
throw `std::logic_error`; `data()` remains readable for the request or WebSocket
session lifetime. Modify WebSocket sessions in middleware before `next()`.
Storage failure prevents publication of a new session cookie.

### Strict integer conversion

`<ruvia/core/Integer.h>` provides `parseInteger<T>(text)`, returning
`std::expected<T, IntegerParseError>`. It accepts complete decimal integers,
rejects whitespace, a leading `+`, trailing bytes and unsigned negative values,
and distinguishes `kInvalidFormat` from `kOutOfRange`. Missing request parameters
are separate from malformed values:

```cpp
std::uint32_t page = 1;
if (auto raw = c.req().query("page")) {
    auto parsed = ruvia::parseInteger<std::uint32_t>(*raw)
        .transform([](auto value) { return std::max(std::uint32_t{1}, value); });
    if (!parsed) {
        co_return c.error({.status = ruvia::http_status::kBadRequest,
            .code = "invalid_page", .message = "invalid page number"});
    }
    page = *parsed;
}
```

Use `QueryModel<T>` for structured request validation; use `and_then()` and
`transform()` for short conversions without discarding error information.

## HTTP Protocol Library

`ruvia::http` can be used without the runtime or Web framework. It provides
HTTP message types and helpers, HTTP/1 request and response parsing/writing,
multipart parsing, range and conditional-request helpers, cookies, content
negotiation, redirects, and content coding. Parse `Content-Encoding` with
`ruvia::parseHttpContentCoding()` from `<ruvia/http/HttpContentCoding.h>`, and
use the bounded complete-buffer codecs in `<ruvia/http/HttpContentCodec.h>`.
`ruvia::parseMultipartBoundary()` and the multipart parsers are declared by
`<ruvia/http/MultipartParser.h>`. The supported protocol-driver entry points
are `<ruvia/http/Http2Connection.h>` and
`<ruvia/http/Http2Framing.h>` for HTTP/2, `<ruvia/http/Hpack.h>` for HPACK,
`<ruvia/http/WebSocketHandshake.h>` for the HTTP/1.1 server handshake,
`<ruvia/http/Http1WebSocketClientHandshake.h>` for client handshake request
preparation and response validation, and
`<ruvia/http/WebSocketConnection.h>` for the WebSocket driver and its typed
events. `WebSocketConnectionOptions::role` selects server (default) or client
masking and inbound validation. Client connections require `maskKeyGenerator`
and an optional borrowed `maskKeyContext`; the transport supplies a fresh
cryptographically random four-byte key for every frame, including automatic
Pong and Close responses. The context must outlive the connection. Generator
failure throws; abort the connection rather than retrying with a weak key.
This replaces the former `WebSocketServerConnection` header, type, and options:
server consumers rename these to `WebSocketConnection` and retain defaults.
SSE messages are
formatted through `ruvia::formatSseMessage()` from `<ruvia/http/Sse.h>`.

`Http1WebSocketClientHandshake` prepares an HTTP/1.1 upgrade request from a
caller-generated random nonce and validates the peer's response against that
request's key and offered subprotocols. It does not negotiate extensions.
The caller supplies the transport and drives `Http1ClientResponseParser`;
handshake acceptance is required before exchanging WebSocket frames.

`Http2Connection` delivers owned response heads and owned request/response
trailers through its events, using `HttpHeader` values. Move them out with
`takeHead()` and `takeTrailers()` when retaining them beyond event processing.
DATA events carry linear flow-control credits: retain a credit to apply
backpressure, merge credits from the same stream without allocation, and
acknowledge or destroy them when their bytes have been consumed.
The supplied PMR resource must outlive the connection and all retained events,
credits, response heads, and trailers allocated from it.

The library is sans-I/O: callers feed bytes, consume typed results/events, and
drive transport I/O themselves. Content-Encoding parsing distinguishes identity,
one supported coding, and an unsupported coding stack; Web request decoding
reports the latter as HTTP 415.

`HttpRequest` is move-only and owns a compact PMR header descriptor block;
its strings still borrow the protocol input. HTTP/1 callers may select storage
with `parser.parse(input, {.resource = &resource})`; that resource must outlive
the result. The default is the default PMR resource. HTTP/2 events use the
connection's configured resource. Web requests use their request/stream arena.
The field-count limit remains 64; moving requests does not allocate or copy
the header block. `headerFields()` preserves semantic name spelling, and both
header-list lookup and `HeaderModel<T>` matching are ASCII case-insensitive.

`HttpRequest` preserves `scheme()`, `authority()`, and `targetForm()` alongside
the original target. Its header view is protocol-semantic rather than a raw
wire block: HTTP/1 target authority can replace Host, while HTTP/2 pseudo-fields
are exposed separately and Cookie fields are coalesced. Query lookup is explicit:
`lastRawQueryValue()` compares encoded keys, returns the encoded value, performs
no form-style `+` conversion, and chooses the last duplicate.

Borrowed outbound-client models say so in their names: `HttpOriginView`,
`HttpClientRequestView`, `HttpClientRequestContentView`, and
`HttpClientRequestBytesView`. Their referenced storage must remain alive until
the external sans-I/O driver finishes using it; response-head values remain
owned PMR results. `parseSetCookie()` likewise returns views into its input, so
owning string temporaries are rejected at compile time.

Headers below `ruvia/http/detail/` are internal component contracts used by
Ruvia's own targets and are not a supported application API.

## License

Ruvia is released under the [MIT License](LICENSE).
