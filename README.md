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
- **Sans-I/O protocol library** — HTTP/1, HTTP/2, HTTP/3/QUIC, WebSocket, and
  HPACK implementations shared by the server and outbound clients; callers feed
  protocol input and drive external I/O. No sockets, Asio, or TLS inside.
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
- [Models and JSON](#models-and-json)
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

Call `listen()` explicitly before `run()`; the App does not create a default
listener or choose a default port. Calling `run()` without a listener throws
`std::invalid_argument`, and you can configure a listener and retry. `listen()`
configures a numeric IPv4 or IPv6 bind address and its optional HTTP and HTTPS
ports as one value. The address is validated and normalized when the
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

HTTPS automatically enables QUIC/HTTP/3 over UDP on the same address and
numeric port; an HTTP-only listener does not enable HTTP/3 automatically. Set
`.http3.mode = ruvia::Http3Mode::kDisabled` to opt out, or use
`kEnabled` to require HTTP/3 explicitly; the remaining HTTP/3 fields override
its handshake and drain timeouts. HTTP/3 always uses the same QUIC protocol
implementation; there is no backend selection. TCP listener and UDP/QUIC
preparation is atomic: startup fails instead of serving HTTPS while advertising
an unavailable HTTP/3 endpoint.

`ServerConfig::workerCount` is the number of business workers. The runtime also
uses one dedicated server network thread (in addition to any `BlockingPool` and
signal threads). It binds the configured HTTP/HTTPS TCP ports and the automatic
HTTP/3 UDP endpoint. There is no separate configurable UDP packet-rate limit.

`ServerConfig::maxRequestsPerConnection = N` limits the cumulative number of
admitted requests on each HTTP/1, HTTP/2, or HTTP/3 connection. The default is
1000, and the value is not silently clamped. For HTTP/3, the fixed stream-ID
cutoff is `4*N`. If a request stream at or above that cutoff arrives before N
requests are admitted, the server announces `GOAWAY(4*N)` early and rejects that
stream; request streams at or above the cutoff are rejected individually. That
announcement does not seal admission of lower IDs: undecided requests below the
cutoff may still be admitted until the connection reaches N admitted requests.
At N, admission is sealed and already-admitted requests are drained.
`Http3ListenConfig::handshakeTimeout` (default 10 seconds) bounds completion of
the QUIC/TLS handshake for each new QUIC connection. The
`Http3ListenConfig::drainTimeout` (default 30 seconds) limits the HTTP/3 drain
and connection-closing phase. Request-header, request-body, and response write
inactivity timeouts apply per stream; an expiry cancels only that stream. Once
a WebSocket CONNECT response is accepted by QUIC, request-body timeout no
longer applies to its tunnel; write timeout applies only while output is
pending. WebSocket heartbeat and close-handshake settings govern its liveness.
The idle timeout applies to the QUIC connection.

The HTTP/3 server supports buffered and streaming request routes, byte and file
responses, response streams, SSE, ordinary CONNECT and Extended CONNECT. The
same WebSocket route serves RFC 6455 over HTTP/1.1, RFC 8441 over HTTP/2, and RFC
9220 over HTTP/3. HTTP/2 and HTTP/3 server push, dynamic QPACK, informational
responses, trailers and RFC 9218 priority updates are available through the
same application capabilities. `Http3ListenConfig::qpack` configures receive
limits; setting `maxTableCapacity` to zero disables dynamic entries and setting
`maxBlockedStreams` to zero forbids blocked field sections.

CONNECT-UDP (RFC 9298) uses `RUVIA_CONNECT_PROTOCOL("connect-udp", path, handler)`.
The framework negotiates GET Upgrade/101 over HTTP/1.1 and Extended CONNECT/2xx
over HTTP/2 or HTTP/3, including `Capsule-Protocol: ?1`. The handler can use
`HttpUdpTunnel` with `c.tunnel().datagrams()` for one receive path that accepts
both native QUIC DATAGRAM and reliable DATAGRAM capsules; the default automatic
send policy uses QUIC only when it was negotiated and the packet fits, otherwise
it sends a reliable capsule. Native sends are best-effort: a full bounded send
queue may drop a packet, with no delivery or retry guarantee. The application
middleware controls authorization and target policy, and opens and drives its
UDP socket. WebTransport, 0-RTT and explicit connection migration APIs are not
exposed.

When HTTP/3 is active, TLS HTTP/1.1 and HTTP/2 responses automatically advertise
its real port, for example `Alt-Svc: h3=":443"; ma=86400`; HTTP/3 responses do
not repeat that field. `ListenConfig::altSvc` can disable the advertisement,
emit `clear`, change `maxAge`/`persist`, or advertise an external port. An
application-set `Alt-Svc` value, including explicit removal, takes precedence
for that response. HTTP/3 does not alter the HTTPS TCP ALPN policy.

Each business worker separately owns TLS, its router, and its capabilities,
including DB, Redis, outbound HTTP client, and user-state set.

`ServerConfig::max_inbound_buffer_bytes_per_worker` (256 MiB) and
`max_inbound_buffer_bytes_per_connection` (64 MiB) bound live inbound body and
WebSocket allocations across requests and streams. Container capacity,
reallocation overlap and decompression output count toward these positive limits;
PMR pool caches and TLS/QUIC transport storage are separate. Buffered HTTP/1
bodies grow with received payload instead of allocating the declared length.
Capacity exhaustion rejects the affected request or terminates its stream/session.

HTTP/1 also has absolute `header_completion_timeout` (30 seconds) and
`body_completion_timeout` (120 seconds). Successful reads renew inactivity
timeouts but do not extend these completion deadlines; `std::nullopt` explicitly
disables an absolute deadline.

`App::trustedProxies({.cidrs = {...}})` enables client address resolution only
for configured peers. `X-Forwarded-Proto` is ignored unless
`trust_x_forwarded_proto = true`: enable it only when the trusted proxies remove
untrusted forwarding fields and maintain matching XFF/XFP chains. Both chains
must have the same number of elements; scheme comes from the selected client
hop. Mismatches keep the transport scheme, and XFF is never combined with
scheme from an unrelated `Forwarded` chain. Proxies that supply RFC 7239
`Forwarded` must likewise sanitize incoming fields.
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
| `ruvia-http/` | `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3 and QUIC protocol core, WebSocket, multipart, SSE, content-coding, and outbound-client protocol primitives. |
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server and outbound-client I/O, TLS/EVP callbacks, UDP runtime, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web   ->  ruvia-core + ruvia-http
```

`ruvia-http` owns HTTP and QUIC protocol processing, using ngtcp2's QUIC core;
callers provide input and consume typed protocol results. `ruvia-web` directly
depends on `ruvia::core`, `ruvia::http`, and OpenSSL, and drives the public HTTP
interfaces with worker-local DNS, sockets, TLS/ALPN and EVP callbacks, UDP,
connection reuse, timeouts, and cancellation. ngtcp2 is an HTTP component
dependency, not a direct Web dependency.

Custom HTTP/3 runtimes can use `ruvia::http3_buffered_response_cursor` from
`ruvia/http/http3_buffered_response_cursor.h` to emit buffered response frames,
and `ruvia::http3_critical_stream_output` from
`ruvia/http/http3_critical_stream_output.h` for control/QPACK prefixes and a
server GOAWAY. Both expose stable byte spans and explicit acceptance; callers
provide transport I/O and keep borrowed storage alive until acceptance.

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
knowledge. `HttpClientProtocol::kHttp3Only` instead opens a QUIC connection over
UDP for an HTTPS origin; it is explicit, never enters the TCP pool, and never
silently falls back to HTTP/2 or HTTP/1.1:

```cpp
ruvia::app().httpClient({
    .alias = "h3-api",
    .config = {
        .scheme = ruvia::HttpScheme::kHttps,
        .host = "api.example.com",
        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
    },
});
```

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
    auto body = co_await response.body().readAll();
    co_return c.body(body.bytes());
}
```

`HttpClientResponse` owns status, protocol version, headers, trailers, and an
address-stable linear body state; it does not borrow from the request builder
or caller stack. Its worker-affine storage survives client shutdown and destruction
on that worker. Client shutdown cancels and joins producers and send operations,
then detaches their transport; it does not join response body consumers (which
may be waiting on a downstream `pipeTo()` write). Keep the response and downstream
writer alive until those operations finish, and destroy the response on its bound
worker before the EventLoop retires. Metadata views borrow the response; body
chunks expire on the next body operation. Retaining a response also retains its
storage pool's caches until the last response state is released.
`send()` completes when the final response head is available.
`maxResponseBytes` bounds each `readAll()` result and the HTTP/1 queued body
window, not the total number of bytes that may pass through `read()` or
`pipeTo()`. Responses with a non-identity `Content-Encoding` are decoded before
`send()` completes because the current content decoders are whole-representation
decoders. Use
`trailers()` or `trailer()` after body completion to inspect trailing fields. On HTTP/1, a
request timeout or explicit `StopToken`
cancellation closes and discards that socket. On HTTP/2 it submits
`RST_STREAM(CANCEL)` for only the affected stream, so unrelated multiplexed
requests can continue. A connection I/O/protocol failure or `writeTimeout`
still discards the whole broken socket, and a later request reconnects
automatically. HTTP/3 cancellation resets only that request stream. A peer
GOAWAY rejection that proves a request was not processed is retried once on a
fresh QUIC connection under the original deadline; ambiguous requests are not
retried.

Each HTTP/1 connection processes one exchange at a time. Each HTTP/2 connection
has a persistent reader/writer pair and multiplexes up to
`maxConcurrentHttp2StreamsPerConnection`, further constrained by the peer's
`SETTINGS_MAX_CONCURRENT_STREAMS`; PING, SETTINGS, flow-control updates, and
GOAWAY are processed even while no request is being submitted. Requests above a
peer GOAWAY `Last-Stream-ID` are known not to have been processed and are retried
once on a fresh connection under the original operation deadline. Ambiguous
requests are never retried automatically.

Use `HttpClientProtocol::kHttp1Only`, `kHttp2Only`, or `kHttp3Only` when
negotiation fallback is not acceptable. HTTP/3 requires HTTPS; selecting it for
a cleartext origin is rejected during configuration. Client certificates, a custom CA file, certificate
verification policy, connect/acquire/request/write timeouts, TCP keepalive, and
per-client connection capacity are supplied in the same `{}` configuration.
Additional operations wait in the bounded client-local queue and fail with
`kQueueFull` when it is full or `kTimeout` when `acquireTimeout` expires.

Cleartext HTTP/2 uses RFC 9113 prior knowledge when `kHttp2Only` is selected. HTTP/1.1
`Upgrade: h2c` is not performed implicitly, so a server that only accepts the
Upgrade transition must be configured for HTTP/1 or exposed through TLS/ALPN.

`HttpClient` and Controller handles expose `send(HttpClientRequestView)`,
`openRequest()` for streaming uploads, `openTunnel()` for CONNECT and
`openUdpTunnel()` for CONNECT-UDP. `withOptions(OperationOptions)` derives
immutable operation policy. Requests are awaited as scoped coroutine operations;
inputs are owned before each operation is returned. Origin inspection and
`stats()` describe the same registered client. There are no blocking overloads.

`openRequest()` returns an `HttpClientExchange` whose `body().write()` accepts
bounded owned chunks and whose `body().end(trailers)` sends request completion.
`response()` can run concurrently with upload, so an early final response is
observable without finishing the request body. `HttpClientUploadConfig` selects
chunk bounds, optional content length and `100-continue` policy. Discarding a cold
operation performs no I/O; moving the exchange preserves existing operations.

`response.informationalResponses()` retains interim status/header sections.
`response.reprioritize(HttpPriority{...})` sends live HTTP/2 or HTTP/3 priority
updates. Enable `.push = {.enabled = true}` on `HttpClientConfig` to receive
bounded push offers with `nextPush()`; a pushed response has its own body and
cancellation lifetime. `.advertisements` selects bounded ORIGIN/ALTSVC
observations obtained through `nextAdvertisement()`. Observations do not change
the client's registered origin or TLS identity. HTTP/3 receive QPACK limits use
`HttpClientConfig::qpack`.

### CONNECT and Capsule channels

```cpp
#include <ruvia/web/HttpClient.h>
#include <ruvia/web/HttpUdpTunnel.h>

ruvia::Task<void> exchangePackets(ruvia::HttpClient& proxy) {
    // Already-expanded URI template path; encodeHttpConnectUdpPath() expands
    // the RFC 9298 default template from a host and port when needed.
    auto opened = co_await proxy.openUdpTunnel({
        .target = "/.well-known/masque/udp/target.example/443/",
    });
    if (auto* rejection = opened.response()) {
        auto body = co_await rejection->body().readAll();
        co_return;
    }
    auto udp = std::move(*opened.tunnel()).udp();
    co_await udp.send("packet");
    co_await udp.finish();
    while (auto packet = co_await udp.read()) {
        // packet owns its bytes; empty payload is a UDP packet, not EOF.
    }
}
```

Ordinary `openTunnel({.authority = "target.example:443"})` uses CONNECT;
extended tunnels additionally supply `.protocol` and `.target`. Accepted results
expose `HttpClientTunnel`, while rejections retain ordinary HTTP status, headers
and body. Extended CONNECT waits for the peer's enabling SETTINGS. The
CONNECT-UDP entry supplies its proxy authority and required Capsule field and
validates the response before exposing a tunnel.

`HttpClientTunnel::read()` borrows bytes until the next read; `write()` owns its
input before returning. `finish()` closes only the sending direction, allowing
reads to continue. Consume an established client tunnel with
`std::move(tunnel).capsules()` to obtain an `HttpCapsuleStream`, or with
`std::move(tunnel).datagrams()` / `.udp()` after CONNECT-UDP negotiation. The
datagram stream can mix native QUIC DATAGRAM and capsule receives and uses the
same automatic send policy described above; set `HttpDatagramConfig::sendPolicy`
to `kCapsule` or `kQuic` to choose explicitly. Capsules and UDP datagram results
own their PMR storage across subsequent reads and client shutdown. Destroy them
on their worker before that EventLoop retires. A null optional is EOF; an empty
capsule or datagram remains data. `abort()` wakes pending I/O; await/join all
started operations before destroying their channel. One read and one
write/finish lane may coexist, and moving a channel preserves cold operations.
Use a Capsule adapter exclusively, without interleaving raw tunnel reads or
writes.

Server handlers register ordinary CONNECT with `RUVIA_CONNECT(authority,
handler)` and Extended CONNECT with `RUVIA_CONNECT_PROTOCOL(protocol, path,
handler)`. They return `Task<void>` and use `c.tunnel()`; authorization middleware
can reject before the handshake is committed. Ordinary authorities match exactly
or use `"*"`; extended paths support normal route parameters and group prefixes.
`HttpTunnelRouteConfig::peerTransportFinTimeout` bounds transport cleanup after
the handler returns. Within the handler, send FIN does not end the receive lane.

`Context::inform()` sends interim responses; `Context::push()` dispatches promised
GET/HEAD requests through normal routing and middleware when the peer permits
push. `advertiseOrigins()` and `advertiseAlternativeService()` publish explicit
connection advertisements, and request trailers are available through
`c.req().trailers()` after body completion.

Every response has one linear body reader. `read()` consumes one borrowed
`std::span<const std::byte>` chunk, `readAll()` collects the remaining bytes into
a move-only `ruvia::HttpClientResponseBytes` with a per-response byte bound, and
`pipeTo()` forwards them to a controller response stream with backpressure.
`HttpClientResponseBytes` owns address-stable PMR storage backed by the
thread-safe `std::pmr::new_delete_resource`; it survives response, client, and
worker teardown and may be destroyed on another thread. Use its left-value
`bytes()` span while keeping the result alive. In addition to the per-response
limit, each client pool has a bounded retained-result byte budget (64 MiB by
default). Standalone `HttpClient` callers may override it with the third
`HttpClient` constructor argument, `HttpClientResultBudgetConfig{.maxRetainedBytes = ...}`.
For App clients, `ServerConfig::httpClientResultBudget.maxRetainedBytes` sets
one budget domain per business worker, shared by all registered aliases; worker
domains are independent. When a budget is full, `readAll()` throws
`kResultBudgetExceeded` and leaves the response body available for retry after
retained results are released. HTTP/1, HTTP/2, and HTTP/3 responses use the same
worker budget. `HttpClientResultBudgetConfig::max_in_flight_bytes` separately
bounds live response allocations before delivery (64 MiB by default), including
compressed input, decompression output, metadata and buffer growth. App aliases
share that receive budget per worker. Receive exhaustion fails the operation
with `kResultBudgetExceeded`; release retained responses before starting more.
Both limits apply during the copy into an escaped result. PMR pool caches and
OpenSSL/TLS/QUIC transport storage are excluded. Both the client
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

`WebSocketClientConfig::protocol` selects `kHttp1`, `kHttp2` or `kHttp3`.
HTTP/1.1 uses Upgrade and validates the accept key; HTTP/2 and HTTP/3 use Extended
CONNECT after the peer's enabling SETTINGS. Each validates the selected
subprotocol and extension response. `.deflate` negotiates RFC 7692 compression
on all three versions. Client frames use cryptographic masks and masked server
frames are rejected on all three versions.
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

auto stopRegistration = loop.onStop([&socket]() -> ruvia::Task<void> {
    std::error_code ignored;
    socket.close(ignored);
    co_return;
});

loops.stop();
loops.join();
```

Keep the stop registration alive while its resource is active. `onStop()`
callbacks return `Task<void>` and run on the owning event-loop thread. All
callbacks are started before awaiting their completion. Cancel resource work
and join its pending operations before returning, including exception paths;
stopping does not wait for a graceful application-request drain. A callback
already started remains owned until completion even if its registration is reset.

Stopping closes external submission and cancels timers, but retains the runtime
until its stop callbacks and admitted root tasks finish. Once retired, escaped
`EventLoop` and `WorkerHandle` values are invalid; `ioContext()` and `executor()`
throw `std::logic_error`. Do not call `run()`, `stop()`, or `restart()` on a
pool-owned `io_context`; lifecycle control belongs to `EventLoopPool`.
Cross-thread application work uses bounded `EventLoop::post()`. Web workers
expose `WorkerHandle`/`WebWorkerHandle`, not their `io_context` or executor.

Native completion handlers can submit terminal failures with
`loop.reportFailure(std::exception_ptr)`. This requests runtime-owner stop
without throwing through the native handler or stopping an external context.
A pool records its first failure and rethrows it from `join()`; an attachment
reports the failure through its diagnostic outlet while retiring its runtime.

Integrations that own a `WorkerRuntimeContext` can use its `submission()` to
obtain a `WorkerSubmissionView` for bounded submission without retaining endpoint
ownership. The runtime must outlive the entire synchronous `post()` call,
including callable construction, movement, and rejection cleanup. A closed or
detached runtime that remains alive rejects submissions; the view is invalid
once its runtime is destroyed. Use `WorkerHandle` when endpoint ownership must
escape the runtime's lifetime. Queued callables must independently preserve any
data they borrow until execution or destruction.

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
use. Register resource cancellation with `onStop()` so admitted roots can finish
when the loop stops; a root waiting indefinitely without a cancellation path
prevents retirement. `get()` waits and rethrows the task exception. Destroying an in-flight
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

attachment.stop(); // requests cancellation and asynchronous runtime retirement
thread.join();
```

The attachment may be stopped or destroyed while another thread is inside
`run()`: its context service retains the worker state and work guard until
asynchronous cleanup and admitted roots finish. Continue driving the external
context until that retirement completes, using either its native `run()` or
`EventLoopAttachment::run()`. The external owner retains ownership of `stop()`,
`restart()`, and the thread; the attachment never calls `io_context::stop()` or
waits for unrelated native work. A second attachment is rejected until the
first runtime has retired.

Complete managed tasks and resource cleanup before destroying the external
context. Destroying it with active structured obligations is a contract
violation. Without those obligations, destroying the context first safely
invalidates escaped handles; `ioContext()` and `executor()` then throw
`std::logic_error`.

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
`co_await client.shutdown()` on the bound loop to complete teardown before
releasing a client's resources. HTTP, database, Redis, and WebSocket clients
also participate in their loop's asynchronous stop, using that same shutdown
path; no separate manual shutdown is required merely to stop the loop. An
internal teardown failure is terminal and is reported to the loop even without
a shutdown waiter. Explicit `shutdown()` still rethrows that same failure;
ordinary request or connection-attempt errors are not teardown failures.

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
thread, but never performs socket operations there. Even a never-connected SQL
or Redis client completes teardown on its owning loop. If a pool was never
started, `EventLoopPool::join()` (also called by its destructor) drains this
accepted cleanup work; do not abandon an attached external loop before draining
its accepted work.

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
thread. `App::onStart()` runs only after every business worker has initialized its
worker-local capabilities and the server network runtime has entered serving state.
`App::onStop()` runs once for explicitly enabled process signal handlers, direct
`App::stop()`, and worker failure. Both hook sets execute on the
thread inside `App::run()`; stop callers, worker threads, and the server network
runtime only request shutdown and never run application hooks themselves.

## Blocking Work

Each business worker runs its event loop and serves TCP connections dispatched
to it by the server network runtime, so a handler that blocks — password
hashing, a synchronous third-party SDK, template rendering, or a slow file —
freezes all of them for as long as it blocks. `BlockingPool` is the
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

File responses evaluate request preconditions against the selected representation
and the handler's normal status. Redirects and errors other than `412` retain
that status instead of becoming `304` or a new precondition failure. A byte range
is considered only for a GET whose response would otherwise be `200`; HEAD
retains full-representation metadata. Unsupported multiple ranges are ignored.

`compression()` also enables incremental gzip, Brotli, or zstd for response
streams; each handler write is flushed through the encoder so SSE and other
low-latency streams do not wait for a full buffered response.
Buffered and streaming representations advertise `Vary: Accept-Encoding` when
this policy can select their coding, including negotiated identity and HEAD
metadata. Fixed representations such as `no-transform` responses are not marked
as varying by this policy.
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
- Component dependencies: core uses Asio; HTTP uses zlib, Brotli, zstd, and
  the QUIC core from ngtcp2 1.25 or newer (without ngtcp2's OpenSSL feature).
  Web adds OpenSSL 3.5 or newer for TLS and the QUIC crypto-provider callbacks.
  HTTP-only builds do not require OpenSSL; Web receives ngtcp2 transitively
  through `ruvia::http` and does not directly depend on it.
- Optional vcpkg features: MariaDB, PostgreSQL, Redis, and JWT.

## Build

For a standalone Ruvia build, set `VCPKG_ROOT` to the root of your vcpkg
checkout. Ruvia automatically uses its toolchain unless
`CMAKE_TOOLCHAIN_FILE` was set explicitly. Its manifest selects ngtcp2's core
(with default features disabled) when HTTP is enabled, and OpenSSL when Web is
enabled. Core-only and HTTP-only builds do not install OpenSSL.

When Ruvia is included with `FetchContent` or `add_subdirectory`, the parent
project owns its toolchain, vcpkg manifest features, triplets, and cache-wide
compiler policy. Select the dependencies needed by the enabled `RUVIA_*`
options in the parent manifest: HTTP needs ngtcp2 with default features disabled,
and Web additionally needs OpenSSL 3.5 or newer. No OpenSSL QUIC feature or
repository overlay is needed; QUIC protocol processing uses the HTTP component,
while Web supplies TLS/EVP callbacks and UDP/runtime I/O. Ruvia does not change a
parent's vcpkg configuration. On MSVC, select the static runtime before
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

JWT signing and verification enforce RFC 7518's minimum raw key sizes:
32 bytes for HS256, 48 for HS384, and 64 for HS512. Supply cryptographically
random key bytes through `JwtSignOptions::secret` / `JwtVerifyOptions::secret`;
short keys (including empty keys) throw `std::invalid_argument`. Keys are not
padded, stretched, or implicitly decoded from base64/hex. Decode encoded secrets
before passing them, and do not use passwords or the public demonstration key
from `examples/web/auth_jwt.cpp` as production keys.

Verification accepts ordinary base64url-encoded compact JWTs, not critical JOSE
extensions. Headers containing `crit` or `b64` are rejected, including malformed
uses of `b64` without `crit`. Omit `b64` to use standard JWT encoding; unencoded
payloads are forbidden for JWTs by RFC 7797. Unrelated noncritical header fields
remain ignorable.

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

`DbConfig::tls` and `RedisConfig::tls` use `ruvia::client_tls_config`.
The default `client_tls_mode::verify_identity` requires TLS and authenticates the
server certificate. Set `ca_file` for a private CA, and configure
`certificate_file` with `private_key_file` together for mutual TLS. Empty CA
configuration uses the driver's trust-store defaults. Redis and PostgreSQL
support `server_name` when the certificate identity differs from the connection
host. Redis completes verification before sending AUTH or any command;
PostgreSQL uses `verify-full` without plaintext fallback.

MariaDB authenticated TLS currently requires a numeric `host` and a certificate
valid for that IP address; DNS hosts and `server_name` overrides are rejected at
configuration time. This keeps the connector's identity check compatible with
nonblocking address resolution. For a deliberately plaintext local service,
explicitly set `.tls = {.mode = ruvia::client_tls_mode::disabled}`. TLS failures
never silently downgrade to plaintext.

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

Transaction and database-stream operations acquire their exclusive connection
lane when awaited, not when the cold operation is created. Multiple cold
operations may be prepared and then awaited sequentially; overlapping started
operations are rejected without disturbing the operation already in flight.
Destroy or consume every pending operation before destroying its transaction
or stream. A cold operation started after that lease has failed or
closed is rejected rather than reusing the connection.

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
return `entity_rows<Output>`. The default output is the repository entity and the
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

Enable `RUVIA_ENABLE_REDIS` together with either database driver. Register a
Redis capability, then bind its alias in the database registration. The cache
uses that worker's existing Redis pools. Cache policy is separate from the
database connection configuration.
Cache keys isolate the registration alias, driver, endpoint, database and login
identity even when registrations share the same configured cache namespace.

```cpp
using namespace std::chrono_literals;
ruvia::DbConfig settings{
    .driver = ruvia::DbDriver::kPostgreSql,
    .username = "app",
    .database = "devices"};
ruvia::app()
    .redis({.alias = "cache", .config = {.host = "127.0.0.1", .port = 6379}})
    .database({.config = settings,
        .query_cache = ruvia::db_query_cache_registration{
            .redis_alias = "cache",
            .policy = {.duration = 1s, .nameSpace = "devices"}}});
```

For a standalone client, first connect a `RedisClient` on the same event loop,
then construct the database client with its capability and the cache policy:

```cpp
co_await redis.connect();
auto cache_store = redis.withOptions({});
ruvia::DbClient db(loop, settings, cache_store,
    ruvia::DbCacheConfig{.duration = 1s, .nameSpace = "devices"});
co_await db.connect();
// Run database operations, destroy their results, then close in this order.
co_await db.shutdown();
co_await redis.shutdown();
```

The Redis owner and its capability scope must remain alive while the database
uses the cache. Closing the database cancels its cache work without closing the
shared Redis pools. Closing the Redis scope safely expires the borrowed cache
capability. A missing App alias or a capability from another worker is rejected
before the database is published.

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
entities declared for the other backend. Both use the same field-access methods
and `entity_rows`, while Redis filters use `Entity::field<"name">()`,
`redis_predicate`, and `redis_find_options`.
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
        {.field = "name", .kind = ruvia::RedisIndexKind::kTag, .sortable = true},
        {.field = "age", .kind = ruvia::RedisIndexKind::kNumeric},
    },
};

auto users = c.redis().getRepository<User>(userRedisConfig);
User user(c.pool());
user.set<"id">("u-42");
user.set<"name">("Alice");
user.set<"age">(25);
const auto inserted = co_await users.insert(user, {.ttl = std::chrono::hours(1)});
if (inserted.affected_entities() == 0) {
    // The primary key already exists; neither fields nor TTL were changed.
}

auto loaded = co_await users.findOne({.where = User::field<"id">() == "u-42"});
User changes(c.pool());
changes.set<"name">("Alice Smith");
changes.setNull<"note">();
co_await users.update(User::field<"id">() == "u-42", changes);
const auto expiration = co_await users.ttl(User::field<"id">() == "u-42");
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
Redis find options contain only filters, field ordering and pagination.

`insert` creates absent entities; `update` changes existing entities; `upsert`
merges supplied fields and inserts when absent. `deleteBy` and `remove` delete an
entity. Mutations return `redis_write_result`: `affected_entities()` is zero or one. `update`, `deleteBy`, `expire` and `ttl` require an exact
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
    .where = (User::field<"name">() == "Alice Smith")
        && (User::field<"age">() >= 18),
    .order = {{.field = "name", .direction = ruvia::redis_order_direction::ascending}},
    .skip = 0,
    .take = 20,
});
```

`find` returns `entity_rows<User>`, `findOne` returns `std::optional<User>`, and
`count` counts matches independently of pagination. `exists` accepts the same
`redis_find_options`. `findAndCount` returns the page and search engine total from one
command. Results may omit documents that expire while Search loads them. Empty
predicates match the whole index; `find` defaults to a page of 100. Search supports
one sortable column per query; equal values do not guarantee stable page order.

`kTag` provides exact case-sensitive string/boolean matching using encoded shadow
fields, preserving punctuation, commas, empty strings and binary values.
`kNumeric` provides comparisons and ranges; indexed values and query operands
must be finite and within `[-(2^53-1), 2^53-1]`. Unindexed integer columns retain
their full native range. `kText` creates a text index for external Search clients;
Filters support scalar comparisons, `between`, `in`, `not_in`, `is_null`,
`is_not_null`, and logical `&&` / `||`. Text-search expressions are not exposed
by the repository filter API. Null predicates use internal presence
fields; inequality comparisons exclude absent fields.

Index creation is explicit; an existing index or missing Search capability
produces a Redis error. `dropIndex()` retains entity hashes. Repositories inherit
the Redis handle's scope, worker affinity, timeout and cancellation. Configuration
and operation inputs are copied into owned worker PMR storage before asynchronous
execution. Results retain their own reclaimable storage independently of later
operations and must be destroyed before that worker resource expires.

[redis_orm.cpp](examples/web/redis_orm.cpp) demonstrates validated JSON input,
insertion with TTL, primary-key lookup, indexed queries and typed models.
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
targets that link them. A consumer linking the installed Web component must
provide OpenSSL 3.5 or newer for TLS and QUIC crypto callbacks. The HTTP
component's ngtcp2 dependency is part of the imported dependency closure; the
installed CMake package does not install external dependencies for its caller.

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
| Models | `RUVIA_MODEL`, `RUVIA_REQUIRED_FIELD`, `RUVIA_OPTIONAL_FIELD`, `RUVIA_NULLABLE` |
| Validation | Field rules on `RUVIA_REQUIRED_FIELD` / `RUVIA_OPTIONAL_FIELD`; route bindings `JsonBody<T>` / `QueryModel<T>` / `PathModel<T>` |

Route tables, middleware chains, and controller instances are finalized before
workers start.

A WebSocket route uses HTTP/1.1 Upgrade with a `101` response, or Extended
CONNECT with a `200` response on HTTP/2 and HTTP/3. HTTP/2 and HTTP/3 do not
reuse the HTTP/1-only `Connection`, `Upgrade`, `Sec-WebSocket-Key`, or
`Sec-WebSocket-Accept` fields. In all three versions, ordinary end-to-end fields
such as Origin, Cookie, Authorization, selected subprotocol, extensions, and
application response fields remain available through the same route. An unsupported
WebSocket version receives `400` and `Sec-WebSocket-Version: 13` on all three
HTTP versions, without HTTP/1 Upgrade fields in HTTP/2 or HTTP/3.

After a WebSocket handshake, the ordinary HTTP/1 or HTTP/2 connection idle
deadline is suspended while the WebSocket is active; HTTP/3 request-body
inactivity timeout stops applying once its CONNECT response is accepted. The
HTTP/3 QUIC transport idle timeout still applies to the shared connection.
Use `WebSocketRouteConfig::lifecycle.heartbeat` to configure idle Ping and
matching-Pong deadlines; the route's `closeHandshakeTimeout` controls close
completion independently. For HTTP/3, `peerTransportFinTimeout` (default 5
seconds, required to be positive) separately bounds the wait for the peer's
QUIC FIN after the server queues its own stream FIN; expiry terminates only
that stream. Without heartbeat, there is no additional per-WebSocket application
idle deadline: the application owns any required liveness policy. Ordinary HTTP
connection timeouts remain unchanged.

### WebSocket 压缩

服务端 HTTP/1 Upgrade 以及 HTTP/2、HTTP/3 Extended CONNECT 均通过路由的 `deflate` 配置协商 RFC 7692 `permessage-deflate`：

```cpp
ruvia::WebSocketRouteConfig options{
    .deflate = {.enabled = true, .compressionLevel = 9, .contextTakeover = true},
};
// 在 Controller 路由声明中使用 RUVIA_GET_WS_OPTIONS("/connect", connect, options)。
// 敏感消息不进入压缩字典：
co_await socket.binary(payload, {.compress = false});
```

- 默认等级为 6、禁用跨消息字典；可选等级为 0–9，9 是 zlib 的最高压缩等级，不保证每种输入都更小。窗口为 32KiB，等级影响本端发送，不强制对端等级。
- 只有客户端提供可接受的扩展时才启用压缩；客户端不支持时保持普通 WS。`enabled = false` 禁止该路由协商压缩。
- `contextTakeover = true` 允许连接独立的双向字典；对端要求任一方向不复用字典时，本实现协商双向 `no_context_takeover`。连接关闭释放字典，新连接重新建立；不同连接、Worker 不共享字典。
- 压缩后没有变小则原样发送，并丢弃本次压缩状态，避免后续引用对端未收到的字节。Ping、Pong、Close 从不压缩。`compress = false` 的消息既不压缩也不进入字典。
- 解压后大小受 `ServerConfig::maxWebSocketMessageBytes` 限制。跨消息字典增加每条连接的内存占用，并可能泄露秘密与攻击者可控内容之间的长度相关性；不要混压此类内容。跳过压缩不能清除先前已经进入字典的秘密，应从首次发送起正确分类，必要时禁用整个路由的压缩。
- sans-I/O 使用者可通过 `WebSocketServerHandshakeOptions::deflate` 协商，随后将握手结果的 `compression()` 和本端 `compressionLevel` 传给 `WebSocketConnectionOptions`；`submitFrame(..., false)` 跳过单条消息压缩。运行时 outbound client 的扩展支持仍以其自身公开能力为准。

### Worker 内部请求

WebSocket 等长连接入口可以使用 `Context::dispatch()` 调用已注册的普通响应路由：

```cpp
const std::array headers{
    ruvia::HttpHeaderView{"Authorization", accessToken},
    ruvia::HttpHeaderView{"Content-Type", "application/json"}};
auto reply = co_await c.dispatch({
    .method = "POST",
    .target = "/devices/42/commands",
    .headers = headers,
    .body = commandJson});
// reply.status()、reply.header()、reply.body() 属于本次调用的独立结果。
```

调用在当前 Worker 的独立请求上下文和 arena 中执行，经过目标路由的中间件、
参数绑定、认证与校验。父请求的业务状态不会自动继承；需要的凭据应显式传入。
连接来源信息保持不变，子请求可通过 `isSubrequest()` 识别。输入在创建操作时复制，
结果使用 Worker 的可回收内存池，后续调用不会覆盖前一次结果。

仅接受 origin-form 路径和缓冲请求/响应，不支持文件响应、SSE 或 WebSocket 升级。
传输层头由框架生成，不接受调用方覆盖。路由请求体上限和超时仍然生效，
`operation.stopToken` 可取消尚未完成的操作；取消不撤销已完成的业务写入。
调用与结果必须留在所属 Worker 的生命周期内，嵌套调用最多 8 层。

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
without a registered callback it is written to stderr rather than dropped with
the connection. `App::httpStats()` sums the same events across every worker as
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

A model is declared once with `RUVIA_MODEL`; the generated type derives from
public `ruvia::Model<DerivedT, ...>` and is usable for both input and output.
There are no request/response role markers or duplicate declarations:

```cpp
RUVIA_MODEL(User,
    RUVIA_REQUIRED_FIELD(id, ruvia::UInt64),
    RUVIA_REQUIRED_FIELD_NAME("user_name", name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32, RUVIA_DEFAULT(18)),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

// One declaration: JSON -> the same model -> JSON.
auto user = ruvia::fromJson<User>(body);       // structure parsing
if (user) {
    auto json = ruvia::toJson(*user);
    auto response = c.json(*user);
}
```

`fromJson<T>()` performs complete structure parsing for a typed Model, a Ruvia
scalar/String/Bytes, or a recursive `Array<T>` / `BoxedArray<T>` root. The
parsed values own their data in the supplied PMR resource; unknown model fields
are skipped. Parsing does not run field rules. `toJson()` supports the same typed roots (but not a
root `JsonValue`/`JsonObject` dynamic writer). `Context::json()` and HTTP
`JsonBody<T>` bindings remain Model-root APIs; they do not make `c.json()` or
`jsonIf()` accept root arrays.

`fromForm()` is the form parser. Field rules run only through the existing
binding middleware (`JsonBody<T>`, `FormBody<T>`, and the other typed bindings);
there is no public `validate()` function. Runtime-sized model collections use
`ruvia::Array<T>` or recursive/address-stable `ruvia::BoxedArray<T>` fields.
Do not directly echo a model containing sensitive input fields without
selecting what is safe to expose deliberately.

A model's allocation resource stays fixed. Public field assignment and collection
insertion own strings and recursively normalize nested values to that resource.
`Array<T>` and `BoxedArray<T>` also use their resource for newly constructed
elements. Move construction transfers the complete value; move assignment keeps
the destination resource and can allocate. Moving never extends the lifetime of
a PMR resource or borrowed input.

JSON string values and wire names must be valid UTF-8. Serialization escapes
JSON syntax and control characters, but does not transcode, validate, or repair
UTF-8 byte sequences. Convert legacy encodings before assigning strings; encode
binary data explicitly (for example, as base64) rather than treating it as text.

Fields use compile-time accessors: `model.get<"name">()`,
`model.set<"name">("Ada")`, `model.ensure<"tags">()`, and
`model.reset<"age">()`. Required, non-nullable `get` returns `const T&`;
optional **or nullable** `get` returns `const std::optional<T>&`. The source field
name (`name`) is used by these accessors; a `*_FIELD_NAME` wire name
(`user_name`) is used in JSON and validation paths.

Presence and nullability are independent:

| Declaration | Missing input | Explicit JSON `null` |
| --- | --- | --- |
| `RUVIA_REQUIRED_FIELD(value, T)` | `required` error | `invalid_type` error |
| `RUVIA_REQUIRED_FIELD(value, T, RUVIA_NULLABLE)` | `required` error | accepted |
| `RUVIA_OPTIONAL_FIELD(value, T)` | accepted | `invalid_type` error |
| `RUVIA_OPTIONAL_FIELD(value, T, RUVIA_NULLABLE)` | accepted | accepted |

`RUVIA_INITIAL(expr)` is for an explicitly constructed business value: it is
evaluated at construction only, at most once per field, and starts with
`presence == false`. Parsing, moving, and resource rebinding do not evaluate it.
`RUVIA_DEFAULT(expr)` is a separate rule: it is evaluated only for a missing
optional input. An explicit null, wrong type, duplicate, or a present empty/
zero/false value never triggers it; `REQUIRED + DEFAULT` still rejects missing
input. The two options may coexist, but each is independently limited to one
expression. Defaulted values are normalized to the model resource and undergo
the same field rules as supplied values during route validation. `fromJson()`,
`fromForm()`, `jsonIf()` and `formIf()` parse structure only.

`model.isPresent<"remark">()` records whether the original input contained the
field, independently of defaults. `model.isNull<"remark">()` reports an accepted
explicit null in the current field state. Missing and null both have an empty
optional value when no default applies. A PATCH handler can distinguish all
three cases without accessing internals or rescanning JSON:

```cpp
RUVIA_MODEL(ProfilePatch,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE));

// const auto& patch = c.req().validated<ProfilePatch>();
// !patch.isPresent<"remark">()  -> leave unchanged
// patch.isNull<"remark">()      -> clear
// otherwise                    -> assign patch.get<"remark">()->view()
```

Input presence is provenance: `set`, `ensure`, and `reset` do not rewrite it;
a manually constructed model has no input presence. Moves transfer this
provenance along with the value. `set<"remark">(nullptr)` explicitly clears a
nullable field; `reset<"remark">()` removes an optional field's current value.
Neither operation reapplies defaults.

Unset model fields are omitted by default. An explicitly set nullable null is
emitted as `null`; `RUVIA_EMIT_NULL` additionally emits unset fields as null,
while `RUVIA_OMIT_EMPTY` omits concrete empty values. These serialization options
do not change input presence/nullability rules.

`ValidationError` owns its message, code, and all issue details independently of
the validator or request arena, including when the exception is copied or moved.

Models bind JSON through schema. Use concrete field types
for ordinary validation. `JsonValue` and `JsonObject` represent dynamic content;
field validation rules on these two types are rejected at compile time. Their
field-level nulls still require `RUVIA_NULLABLE`; nulls **inside** a dynamic token
(or `Array<JsonValue>`) remain ordinary JSON values. `JsonObject` requires an
object token, while `JsonValue` can represent any JSON token.

They expose `isObject()` / `isArray()` / `isNull()`, `view()`, and
`get<T>("field")`, but are not a `c.json()` / `toJson()` writer. `JsonValue`
also provides root-token `get<T>()`; `forEachElement()` and
`forEachField()` visit dynamic arrays/objects. `JsonObject::forEachField()`
visits decoded `string_view` keys. Visitors receive scoped-borrowed values and
return `bool`: `true` continues and `false` stops early. The traversal returns
`true` only when it finishes, and `false` for early stop or a mismatched kind.
Keep the source and its PMR resource alive for every callback borrow.
Their ownership contract is explicit:

- `JsonValue::parse()` / `JsonObject::parse()` borrow the supplied complete token;
  passing a memory resource does not copy it. The input must outlive the value
  and any borrowed views or subvalues.
- Request-bound parsing borrows the request body. Standalone `fromJson<Model>()`
  owns dynamic tokens and strings in the model's PMR resource, so its input may
  be destroyed after parsing. The resource must outlive the model and results.
- Move construction preserves the borrowed/owned mode; it does not turn a borrow
  into ownership or extend an allocator's lifetime. Field assignment owns/rebinds
  to the destination model resource. Move assignment of a dynamic value keeps
  the destination resource, transfers compatible owned storage, and copies
  borrowed or incompatible storage.

URL-encoded form binding stays schema-based. Raw `bytes()` /
`text()` remain available for custom formats. Buffered `multipart()` and
streaming `multipartReader()` expose flat protocol parts, preserving repeated
names and file metadata without interpreting dotted names or array suffixes.
`MultipartParseOptions` defaults to `max_parts = 1024` and
`max_metadata_bytes = 1024 * 1024` (total part-header bytes). Exceeding either
limit rejects the body with 413; streaming parsing preserves scan progress
across input chunks.
`multipartReader()` uses the worker pool for transient parsing state and releases
it on completion, failure, or body-reader teardown. Part views expire on the next
read, parent body-reader teardown, or reader destruction; copy values that must
survive subsequent reads.

`ModelParseOptions` bounds each typed JSON document to 65,536 total array
members and a conservative 16 MiB representation budget by default, including
nested and boxed arrays. `max_array_elements` and `max_representation_bytes`
can explicitly customize standalone parsing; HTTP model binding uses the finite
defaults. Parsing rejects over-budget input before growing its representation.
Validation collects at most 64 issues and caps each diagnostic field/message at
1024 bytes; nested validation stops when the issue limit is reached.

Models declare field rules on `RUVIA_REQUIRED_FIELD` / `RUVIA_OPTIONAL_FIELD`.
`RUVIA_REGEX` does not accept general `std::regex`: for safe request validation it
supports the same anchored, bounded dialect as `RUVIA_PATTERN` (literals, `.`,
character classes, `\d` / `\w` / `\s`, and `*` / `+` / `?`). Patterns over 256
bytes are rejected at compile time, and inputs over 4096 bytes fail validation.
Matching charges recursive states and bytes inspected (including class members)
to a fixed work budget; exhaustion fails validation closed. Use `RUVIA_CUSTOM`
for other matching logic. Routes select the source with
`ruvia::JsonBody<T>`, `FormBody<T>`,
`QueryModel<T>`, `PathModel<T>`, `HeaderModel<T>`, or `CookieModel<T>`; these
bindings perform the explicit validation step. A handler returns
`Task<HttpResponse>` and serializes the same model with `c.json(model)`.
Core and Web use the same `Task<T>` with an explicit result type;
operations without a result use `Task<void>`. Ordinary route
handlers return HTTP responses, while services can return typed model values:

```cpp
ruvia::Task<User> getUser(std::uint64_t id,
    std::pmr::memory_resource* requestResource) {
    User response({.resource = requestResource});
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
complete model parsing, nested, array, default, and validation example.

### Model values and JSON traversal

Use Ruvia value types in one `RUVIA_MODEL` declaration. Narrow integers
(`Int8`/`UInt8`/`Int16`/`UInt16`) check their exact range on construction.
`Bytes` owns PMR bytes, exposes `view()` as `std::span<const std::uint8_t>`, and
accepts spans or vectors through `set()`. Its JSON representation is a padded,
canonical RFC 4648 base64 string, never raw text or hex. Invalid characters,
missing padding and nonzero padding bits are rejected.

```cpp
RUVIA_MODEL(Config,
    RUVIA_OPTIONAL_FIELD(retries, ruvia::UInt8, RUVIA_INITIAL(3)),
    RUVIA_OPTIONAL_FIELD(timeoutMs, ruvia::UInt16, RUVIA_INITIAL(250)),
    RUVIA_OPTIONAL_FIELD(secret, ruvia::Bytes));

Config config({.resource = requestResource});
config.set<"secret">(std::span<const std::uint8_t>(bytes));
if (config.get<"retries">() == ruvia::UInt8{3}) {
    // compare values, not allocator or input provenance
}
```

`String`, `Bytes`, `Array`, `BoxedArray`, and Models compare their values.
Model equality compares each field's current state and value, ignoring allocator
and input presence; missing and null remain different. `JsonValue` and
`JsonObject` instead compare their retained original token bytes, not a
semantically normalized JSON tree.

Typed root JSON is also available outside HTTP bindings:

```cpp
auto values = ruvia::fromJson<ruvia::Array<ruvia::UInt16>>(R"([1, 2, 3])");
if (values) {
    auto body = ruvia::toJson(*values);
}

if (auto value = ruvia::JsonValue::parse(R"({"enabled":true,"n":2})")) {
    const bool complete = value->forEachField([](std::string_view key, const ruvia::JsonValue& item) {
        return !key.empty() && !item.isNull();
    });
    // complete is false if the visitor stopped early.
}
```

Dynamic visitors borrow the source token only for the callback. Keep its input
and PMR resource alive; moving a value does not extend either lifetime.

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
them. Global security middleware also covers successful document-root GET/HEAD
responses.

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
Every non-empty `set()` publishes a new session ID at commit, even when the data
is unchanged. Replacing a loaded session atomically revokes its old ID.
Writing authenticated state therefore revokes the ID used before login.
Repeated writes in one request publish one new ID. Read-only requests keep their
ID. Empty data clears the session. Call `regenerate()` when authentication or
permissions change without writing session data. All mutations must precede
response publication.

After validating credentials, writing the authenticated data is enough:

```cpp
auto session = c.session();
session.set(authenticated_session_data);
```

Logout deletes the old ID. Rotation requires the loaded ID to still exist.
Concurrent mutations cannot recreate a revoked ID. A stale writer receives 409
and publishes no session cookie. Do not retry its old authenticated state under
a new ID.

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

`ruvia::http` is a standalone protocol library. Client and server drivers supply
transport I/O, clocks, cancellation, and TLS/EVP callbacks. The HTTP component
owns HTTP semantics and the QUIC protocol core: packet protection and key
updates, packet and connection-ID processing, streams and flow control,
acknowledgments and loss recovery/retransmission, datagrams, and connection
close. It uses ngtcp2's QUIC core, but has no dependency on `ruvia::core`, Asio,
sockets, or OpenSSL. Web implements the crypto callbacks with OpenSSL EVP and
drives UDP I/O through its runtime.

| Protocol | Client entry points | Server entry points |
| --- | --- | --- |
| HTTP/1.0 and HTTP/1.1 | `Http1ClientRequestWriter`, `Http1RequestContentWriter`, `Http1ClientResponseParser`, `Http1ClientResponseBodyDecoder`, `Http1ClientExchangeState` | `Http1ServerRequestParser`, request body/connection plans, `Http1ResponseHeadPlan`, `Http1ResponseStreamPlan`, response serialization |
| HTTP/2 | `Http2Connection::client()`: request heads, DATA, request trailers, responses, cancellation, flow-control credits, push and GOAWAY | `Http2Connection::server()`: request events, interim/final/streaming responses, trailers, push promises, cancellation and connection control |
| HTTP/3 | `Http3Connection` with client role, `Http3ClientRequestHead`, `Http3RequestWriter`, `Http3ClientResponse`, `Http3DataWritePlan` | `Http3Connection` with server role, `Http3MessageHead`, `Http3ResponseWriter`, `Http3DataWritePlan` |
| WebSocket | `Http1WebSocketClientHandshake` or `WebSocketClientNegotiation`, then `WebSocketConnection` with client role | HTTP/1 Upgrade or HTTP/2/3 Extended CONNECT handshake helpers, then `WebSocketConnection` with server role |

The protocol interfaces implement HTTP semantics/framing from RFC 9110, 9112,
9113, and 9114; QUIC transport, TLS integration, and loss recovery from RFC 9000,
9001, and 9002; HPACK (RFC 7541), QPACK (RFC 9204), WebSocket (RFC 6455), and
these published optional extensions:

- HTTP/2 and HTTP/3 server push, including promise metadata and cancellation.
  HTTP/2 clients opt in with `Http2ConnectionOptions::enablePush`; HTTP/3 clients
  send `prepareMaxPushId()` before accepting pushes. Pushed origins still require
  transport authority validation before use.
- Extensible priorities (RFC 9218): `HttpPriority`, typed priority-update events,
  and connection submission/preparation methods. Scheduling remains a driver choice.
- WebSocket permessage-deflate (RFC 7692), including independent send/receive
  windows of 8–15 bits and context takeover. Client offers and server selections
  are validated before configuring the frame connection.
- Extended CONNECT (RFC 8441/9220), including WebSocket over HTTP/2 and HTTP/3.
  Client helpers require the peer's `SETTINGS_ENABLE_CONNECT_PROTOCOL`;
  HTTP/3 servers configure their advertised receive capability explicitly.
- HTTP Datagrams and Capsule Protocol (RFC 9297), and CONNECT-UDP (RFC 9298):
  `<ruvia/http/HttpDatagram.h>` provides framing and a tunnel session that checks
  negotiation, context IDs, payload limits, and half-close state;
  `<ruvia/http/HttpConnectUdp.h>` validates HTTP/1.1 Upgrade and HTTP/2/3
  handshakes and handles the default UDP URI template. QUIC DATAGRAM use requires
  both HTTP SETTINGS values and QUIC negotiation; capsules provide reliable delivery.
- ORIGIN (RFC 8336/9412) and HTTP/2 ALTSVC (RFC 7838):
  `<ruvia/http/HttpConnectionAdvertisement.h>` provides codecs; the connection
  APIs send advertisements and deliver typed events. Enable ORIGIN reception only
  for an authenticated origin connection, outside an explicit proxy. The runtime
  applies certificate checks, connection coalescing, and alternative-service policy.

`Http3QpackEncoder` and `Http3QpackDecoder` from
`<ruvia/http/Http3QpackConnection.h>` share dynamic tables across streams, handle
all instruction representations, blocked sections, acknowledgments, cancellation,
and bounded critical-stream output. `Http3Connection` owns both contexts and
configures its encoder from peer SETTINGS. Construct local critical-stream
prefixes from `connection.localSettings()` using `Http3LocalCriticalStreams`.
When `feed()` returns `kQpackBlocked`, retain the unconsumed suffix and FIN, feed
encoder-stream bytes, and retry the blocked stream. Drain encoder and decoder
output on their respective critical streams and keep those streams open.
The independent request/response encoders also have dynamic QPACK overloads;
share one encoder across the whole connection when using them separately.

HTTP/3 uses the same QUIC core as the rest of the protocol library. QUIC packet
processing, stream I/O, flow control, acknowledgments, loss recovery, retransmission,
and resets are protocol responsibilities; the Web runtime only supplies UDP I/O
and TLS/EVP callbacks. Message writers produce field sections and scatter-gather
DATA plans; drivers preserve write ordering and commit plans only after successful
transmission.
For trailing HEADERS, complete the body plan, write the trailer section, then
commit the finishing plan and FIN. `cancelRequest()` queues QPACK cancellation
before releasing a parser. Returned PMR results can survive later operations;
the supplied resource must outlive every retained result. Callback views expire
on callback return.

`Http1ClientRequestWriter::prepareStreaming()` accepts either a known body length
or chunked framing. `Http1RequestContentWriter` plans borrowed payload segments,
validates request trailers, enforces the declared length, and gates upload on
`100 Continue` or the driver's timeout. A final response aborts unfinished upload.

The sans-I/O `WebSocketConnection` and `WebSocketServerProtocol` default to a
16 MiB message limit, including assembled and decompressed messages.
`WebSocketConnectionOptions::max_buffered_input_bytes` also limits the owned
input queue (16 MiB + 14 bytes by default). A `backpressured` feed result consumes
no input; drain events before retrying, and split input chunks larger than this
queue limit. An explicit unlimited message policy does not disable the input
queue bound.

`WebSocketConnectionOptions::role` selects server (default) or client masking
and inbound validation. Client connections require `maskKeyGenerator` and an
optional borrowed `maskKeyContext`; the transport supplies a fresh
cryptographically random four-byte key for every frame, including automatic
Pong and Close responses. The context must outlive the connection. Generator
failure throws; abort the connection. `WebSocketClientNegotiation` owns offered
headers, subprotocols, and compression parameters, and prepares HTTP/2/3 requests.
Use the negotiated `WebSocketCompression` value when constructing the connection.

The Web runtime uses these protocol primitives with its own enabled capabilities;
protocol-library extension support does not enable an App transport feature by
itself. HTTP/3 always uses ngtcp2 through `ruvia::http`; Web uses OpenSSL 3.5 or
newer for TLS and EVP callbacks and supplies the UDP/runtime integration. The
HTTP component does not require OpenSSL, and Web does not directly depend on or
call ngtcp2.

HTTP message helpers also cover multipart, ranges, conditional requests, cookies,
content negotiation, redirects, and content coding. Parse `Content-Encoding`
with `parseHttpContentCoding()` from `<ruvia/http/HttpContentCoding.h>` and use
bounded buffer codecs from `<ruvia/http/HttpContentCodec.h>`.
`parseMultipartBoundary()` and multipart parsers are declared by
`<ruvia/http/MultipartParser.h>`. SSE messages are
formatted through `ruvia::formatSseMessage()` from `<ruvia/http/Sse.h>`. URL
component and URL-encoded pair helpers are available from
`<ruvia/http/UrlEncoding.h>`: `UrlDecodeMode::kPercent` leaves `+` literal,
while `kForm` maps it to a space. `decodeUrlComponent()` returns a PMR string
using the selected `UrlDecodeOptions::resource` (or the default resource), which
must outlive the result. `visitUrlEncodedPairs()` supplies raw views borrowing
the input, can stop early when a bool visitor returns false, and does not validate
percent escapes. `findUrlEncodedValue()` compares decoded names, selects the
last duplicate, and returns the still-encoded value as a borrowed view.

`Http1WebSocketClientHandshake` prepares an HTTP/1.1 upgrade request from a
caller-generated random nonce and validates the peer's response against that
request's key, offered subprotocols, and permessage-deflate parameters.
The caller supplies the transport and drives `Http1ClientResponseParser`;
handshake acceptance is required before exchanging WebSocket frames.

`HttpTransferCodingDecoder` from `<ruvia/http/HttpTransferCodingDecoder.h>`
provides incremental transfer decoding with caller-owned input and output storage.
Its typed failures report invalid encoding or a decoded-size limit violation,
not request-specific HTTP statuses. Request drivers use the HTTP request-body
error mapping; response drivers retain their own response error contract.

`Http1ChunkedBodyDecoder` from `<ruvia/http/Http1ChunkedBodyDecoder.h>`
provides zero-copy chunk framing. Its aggregate configuration selects payload
limits and request or response trailer semantics. Failures report neutral
framing or limit categories; request-side status mapping remains HTTP-owned.
`HttpResponseChunkedBodyDecoder` fixes the response role and returns the same
exclusive typed result. Consume body and trailer views before modifying input,
retain unconsumed wire bytes, and use a positive per-step body-output budget.

Ordinary HTTP/1 client responses use `Http1ClientResponseBodyDecoder` from
`<ruvia/http/Http1ClientResponseBodyDecoder.h>`, bound to the final parser plan.
It owns message-length progression, chunk framing, transfer decoding, validated
trailers, EOF completion, and the 205 empty-content constraint. The transport
retains any unconsumed input suffix and consumes returned views before changing
input or reusing output storage. A nonempty scratch span bounds each output;
uncompressed content remains zero-copy. Keep the decoder at a stable address
and its PMR resource alive through destruction. Completion, not merely receiving
the response head or reaching a gzip member boundary, permits connection reuse.

`Http2Connection` delivers owned response heads and owned request/response
trailers through its events, using `HttpHeader` values. Move them out with
`takeHead()` and `takeTrailers()` when retaining them beyond event processing.
DATA events carry linear flow-control credits: retain a credit to apply
backpressure, merge credits from the same stream without allocation, and
acknowledge or destroy them when their bytes have been consumed.
The supplied PMR resource must outlive the connection and all retained events,
credits, response heads, and trailers allocated from it.

HTTP/1 persistence uses `Http1RequestConnectionPlan` from
`<ruvia/http/Http1RequestConnectionPlan.h>`. Parsing establishes its version and
initial reuse disposition; `applyRequestBodyConsumption()` and `requireClose()`
can only tighten it to close. Buffered and streaming response drivers consume
that same plan when finalizing connection semantics.

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

File responses preserve an opaque `HttpResponseFileIdentity` from
`<ruvia/http/HttpResponseFile.h>`. Pass that token directly to
`HttpResponse::fileBody(path, size, offset, length, identity)`; the response owns
its path, while the `HttpResponseFileView` returned by `fileBody()` borrows it.
The runtime supplies and validates checked identities after opening the file;
the protocol library does not perform file I/O.

Borrowed outbound-client models say so in their names: `HttpOriginView`,
`HttpClientRequestView`, `HttpClientRequestContentView`, and
`HttpClientRequestBytesView`. Their referenced storage must remain alive until
the external sans-I/O driver finishes using it; response-head values remain
owned PMR results. `parseSetCookie()` likewise returns views into its input, so
owning string temporaries are rejected at compile time.

Headers below `ruvia/http/detail/` are internal to `ruvia-http` and are not a
supported application API.

## License

Ruvia is released under the [MIT License](LICENSE).
