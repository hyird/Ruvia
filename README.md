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
  policy, SNI identities, and an outbound client with certificate verification,
  SNI, ALPN, HTTP/1.1, and HTTP/2.
- **Optional integrations** — MariaDB, PostgreSQL, Redis, and JWT behind vcpkg
  features; both database drivers share one `DbHandle` API surface.
- **Verified** — RFC 9113 wire conformance suite against a real h2c server and
  guard tests that pin the public API contracts.

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
Public configuration types are ordinary C++ aggregates. Configure them directly
with designated initializers; there are no configuration factories, builders,
or identity wrappers. Passing a config to an optional App feature enables or
replaces it, and passing `nullptr` disables it. Ruvia validates each complete
value before atomically copying retained data into process-owned PMR storage.

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
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server and outbound-client I/O, TLS, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web   ->  ruvia-core + ruvia-http
```

`ruvia-http` is core-free, Asio-free, and socket-free. It owns the outbound
HTTP/1 and HTTP/2 protocol state machines. `ruvia-web` drives those primitives
with worker-local DNS, sockets, TLS/ALPN, connection reuse, timeouts, and
cancellation. It deliberately does not add a second `fetch` name, proxy, or
reverse-proxy product API.

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
`HttpClientRequestView`. The handle copies that view into request PMR memory
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
chunk, `readAll()` collects that same stream with a byte bound, and `pipeTo()`
forwards it to a controller response stream with backpressure. There is no
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
policy, and `close()` is an idempotent immediate shutdown callable from any
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
});

auto completed = loop.start(consumeEvents(client));
```

The opening handshake uses HTTP/1.1 Upgrade, validates the server accept key and
selected subprotocol, and rejects unsolicited extensions. Client frames use a
cryptographically generated mask; inbound masked server frames are rejected.
The driver automatically answers Ping, completes peer-initiated Close, enforces
one concurrent read and one concurrent write, bounds complete messages, and
closes the transport when an operation is cancelled or times out. `wss` verifies
the peer certificate and host name by default and supports the same CA and client
certificate fields as `HttpClientConfig`.

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
control belongs to `EventLoopPool`. Cross-thread application work must still
use bounded `EventLoop::post()` rather than raw `asio::post()`, so shutdown and
backpressure remain observable. Web workers deliberately expose only
`WorkerHandle`/`WebWorkerHandle`, not their `io_context` or executor.

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

auto completed = loop.start(callUpstream(client));
loops.start();
completed.get();
```

Construction creates no thread and opens no connection. The first `send()`
connects lazily on the bound loop; retries, cancellation, timeouts and protocol
selection use the same runtime as `Context::httpClient()`. Operations must be
created and awaited on that loop. `close()` is idempotent and may be called from
any thread; draining the event loop is the shutdown barrier.

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

db.close();
loops.stop();
loops.join();
```

`DbClient::connect()` is a lazy `Task<void>` and must run on its bound loop, just
like every later database operation; `EventLoop::start()` owns each top-level
completion. Result, stream, and transaction
values remain worker-affine and must finish before the owning client and event
loop are destroyed. `close()` is idempotent; `EventLoopPool::join()` (or the
attached context owner's equivalent drain) is the shutdown barrier. App handlers use
`Context::httpClient()` and `Context::db()` as worker-local convenience views.
Enable `RUVIA_ENABLE_POSTGRESQL` or `RUVIA_ENABLE_MARIADB` for `DbClient` and
link `ruvia::web`; `ruvia::core` keeps no HTTP-client or database dependency.

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
- A C++20 compiler. CI builds with GCC 13 on Ubuntu 24.04, the stock Apple
  Clang on macOS 26, and MSVC on Windows.
- vcpkg.
- Supported build platforms: Linux, macOS, and Windows 10 or newer. Windows
  builds require MSVC.
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

### Migrations

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

With tests and a driver enabled, that driver's live test is compiled
automatically. Set `RUVIA_RUN_POSTGRESQL_INTEGRATION=1` or
`RUVIA_RUN_MARIADB_INTEGRATION=1` when running CTest to execute it; without the
environment variable CTest reports it as skipped. Linux CI starts isolated
PostgreSQL and MariaDB services and sets both variables, so driver integration
is a required gate there rather than a skipped test.

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
| Models | `RUVIA_REQUEST_MODEL`, `RUVIA_RESPONSE_MODEL`, `RUVIA_REQUIRED_FIELD`, `RUVIA_OPTIONAL_FIELD` |
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
Self-contained callbacks passed to App are owned and destroyed with the App;
request dispatch retains only internal, allocation-free callback references.
Public callback types never expose non-owning `bind()` or `borrow()` factories.

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

`RUVIA_REQUEST_MODEL` supports `fromJson<T>()`, `fromForm<T>()`, route parsing,
and validation. Every JSON response is first represented by a
`RUVIA_RESPONSE_MODEL`; `toJson()` and `c.json()` accept response models only,
and there is no dynamic object/array writer API. Runtime-sized collections are
declared as `ruvia::Array<T>` fields. Request handlers parse typed JSON with
`c.req().json<T>()`; handlers that explicitly need a dynamic borrowed JSON view
use `c.req().jsonValue()`. A request model may only nest request models, and a
response model may only nest response models. Both roles support
`ruvia::Array<T>` and recursive `ruvia::BoxedArray<T>` fields. Form, query,
param, header, and cookie binding remain flat scalar inputs.

Fields use compile-time accessors: `model.get<"username">()`,
`model.set<"name">("Ada")`, `model.ensure<"tags">()`, and
`model.reset<"avatar">()`. Required `get` returns `const T&`; optional `get`
returns `const std::optional<T>&`. A missing optional request property stays
empty, while an explicit JSON `null` is an `invalid_type` error. An unset
optional response property is omitted by default; `RUVIA_EMIT_NULL` writes it as
`null`, and `RUVIA_OMIT_EMPTY` omits present empty values. The source field name
(`username`) is used by `get`/`set`; a `*_FIELD_NAME` wire name (`user_name`) is
used in JSON and validation paths.

Model field descriptors are passed directly to a C++ variadic template. Model
registration does not use a preprocessor argument counter or `FOR_EACH`, so
Ruvia defines no fixed field-count limit; only the compiler's normal template
resource limits apply. The compiled model guard declares more than 64 fields to
protect this contract. Route middleware keeps the typed
`c.req().validated<T>()` API, while `c.req().validatedJson<T>()` also exposes the
validated original bytes through `raw()` for JSONB passthrough. See the compiled
[`models_validation.cpp`](examples/web/models_validation.cpp) example for a
complete request/response, nested, array, default, and validation example.
`RUVIA_RULE` adds route-level business constraints after structural validation.

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
so a designated-initialized temporary is safe to register.

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
`<ruvia/http/WebSocketHandshake.h>` for the HTTP/1.1 server handshake, and
`<ruvia/http/WebSocketServerConnection.h>` for the server-side WebSocket driver
and its typed events. The WebSocket driver accepts masked client frames and
emits unmasked server frames; it does not claim a client role. SSE messages are
formatted through `ruvia::formatSseMessage()` from `<ruvia/http/Sse.h>`.

The library is sans-I/O: callers feed bytes, consume typed results/events, and drive
transport I/O themselves. It contains no App, Context, Router, socket,
TLS, connection pool, runtime timeout, static-root policy, DB, Redis, or JWT
integration. Content-Encoding parsing distinguishes identity, one supported
coding, and an unsupported coding stack; Web request decoding reports the
latter as HTTP 415.

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
