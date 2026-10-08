# Outbound clients

[Documentation index](../README.md#contents)

## Outbound HTTP Client

Register each origin before `App::run()`, then select it by alias in a handler:

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

// In a handler:
auto response = co_await c.httpClient()
    .withOptions({.timeout = std::chrono::seconds(2)})
    .send({.target = "/v1/data"});
c.status(response.status());
auto body = co_await response.body().readAll();
co_return c.body(body.bytes());
```

Use `c.httpClient("billing")` for another alias. Standalone applications can
construct `HttpClient(loop, config)`; it connects lazily. Run operations on that
loop and await `shutdown()` before releasing the client. See
[http_client.cpp](../examples/web/http_client.cpp).

| Configuration | Behavior |
| --- | --- |
| Default HTTPS | ALPN HTTP/2, falling back to HTTP/1.1. |
| Default cleartext | HTTP/1.1. |
| `HttpClientProtocol::kHttp1Only` | HTTP/1.1 only. |
| `kHttp2Only` | HTTP/2; cleartext requires prior knowledge, not h2c Upgrade. |
| `kHttp3Only` | HTTPS over QUIC/UDP; no TCP fallback. |

HTTPS verifies certificates and hostnames by default. Set the CA and client
certificate fields for private PKI/mTLS. `host` accepts ASCII DNS/IDNA names,
IPv4, or unbracketed IPv6; configure the port separately.
Set `receivedCookies = HttpClientReceivedCookiePolicy::kRetainAndSend` to retain
response cookies; `maxCookies` and `maxCookieBytes` bound the jar.

`withOptions()` combines its timeout/stop token with the handle's scope.
Connection and acquisition limits are configured per origin; a full waiting
queue fails with `kQueueFull`, and acquisition expiry with `kTimeout`.
Request cancellation retires the HTTP/1 socket or resets only the HTTP/2/3
stream. Later requests reconnect as needed. Requests proven unprocessed by
GOAWAY may be retried once; ambiguous requests are not retried automatically.

### Response bodies

`send()` completes at the final response head. Each response has one body reader:

| Operation | Result |
| --- | --- |
| `read()` / `text()` | Next borrowed chunk; valid until the next body operation or response destruction. |
| `readAll()` | Remaining body in owned `HttpClientResponseBytes`. |
| `pipeTo(c.stream())` | Forward remaining bytes with backpressure. |

Read trailers after body completion. `maxResponseBytes` limits `readAll()` and
HTTP/1 buffering, not total streamed bytes. Compressed responses are currently
decoded as a whole before `send()` returns.

Responses may outlive their client on the same worker, but must be destroyed
before that event loop retires. Finish body consumers before destroying their
response or downstream writer. Cancellation alone does not invalidate a borrowed
chunk. Discarding a partial HTTP/1 response closes that connection; a later HTTPS
request performs a fresh verified handshake.

`HttpClientResponseBytes` may outlive the client and worker and be moved across
threads. Keep it alive while using `bytes()`. The retained-result and in-flight
response budgets each default to 64 MiB. Configure them with
`HttpClientResultBudgetConfig` (standalone constructor) or
`server_config::http_client_result_budget` (shared by App aliases per worker).
`kResultBudgetExceeded` from `readAll()` can be retried after releasing retained
results; a receive-budget failure fails the operation.

### Streaming uploads and optional features

`openRequest()` returns an `HttpClientExchange`. Write through `body().write()`,
finish with `body().end(trailers)`, and await `response()`. Response reading may
run concurrently with upload. `HttpClientUploadConfig` selects chunk limits,
content length, and `100-continue` behavior.

- `informationalResponses()` exposes interim heads; `reprioritize()` updates
  HTTP/2/3 priority.
- Enable `.push = {.enabled = true}` for `nextPush()`; configure
  `.advertisements` for `nextAdvertisement()`.
- HTTP/3 `.qpack` controls receive limits; `initial_quic_version` selects QUIC.
- Enable `http3_early_data` and mark each eligible request `replay_safe = true`
  for bodyless GET/HEAD 0-RTT. TLS rejection replays eligible requests in 1-RTT;
  HTTP 425 is returned to the caller without an automatic retry.
- `start_quic_path_migration()` starts local endpoint migration on the owner loop;
  inspect `path_migration()`. Cancelling a submitted migration closes that
  connection and its active requests.

### CONNECT and Capsule channels

```cpp
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
    // An empty packet is data; nullopt is EOF.
}
```

Use `openTunnel({.authority = "target.example:443"})` for ordinary CONNECT;
extended tunnels also set `.protocol` and `.target`. `encodeHttpConnectUdpPath()`
expands the default CONNECT-UDP URI template from a host and port.

`finish()` closes only the send direction. Use `.capsules()`, `.datagrams()`, or
`.udp()` exclusively after selecting an adapter. Automatic datagram sending uses
native QUIC when negotiated and the packet fits, otherwise reliable capsules.
Native sends are best-effort and may be dropped when their queue is full.
`HttpDatagramConfig::sendPolicy` can force `kCapsule` or `kQuic`.

One read and one write/finish may coexist. Raw tunnel reads borrow until the
next read; capsule and UDP results own their data. Keep results on the worker
until destruction, and await/join operations after `abort()`.

Server handlers use `RUVIA_CONNECT` / `RUVIA_CONNECT_PROTOCOL` and `c.tunnel()`.
Authorize the target before accepting the tunnel; CONNECT-UDP handlers also
open and drive their application's UDP socket.

## Outbound WebSocket Client

```cpp
#include <ruvia/web/WebSocketClient.h>

ruvia::WebSocketClient client(loop, {
    .scheme = ruvia::WebSocketScheme::kWss,
    .host = "events.example.com",
    .target = "/v1/stream",
    .subprotocols = {"events.v1"},
    .heartbeat = {
        .pingInterval = std::chrono::seconds(30),
        .pongTimeout = std::chrono::seconds(10),
    },
});

// In a coroutine running on loop:
co_await client.connect();
co_await client.text("ready");
while (auto message = co_await client.read()) {
    // payload() borrows until the next read; copy it if needed later.
}
co_await client.shutdown();
```

Select `WebSocketClientConfig::protocol` (`kHttp1`, `kHttp2`, or `kHttp3`).
`.deflate` negotiates compression; `wss` verifies certificates and hostnames.
Configuration strings are copied at construction.

Keep a `read()` active to process control frames and heartbeat responses. One
read and one write may run concurrently. Use `withOptions()` for operation
policy; timeout or cancellation closes the transport.

Await `close(WebSocketCloseOptions)` for the Close handshake. Success requires
an actual peer Close frame: EOF or an error reset is a failure. `abort()` requests
immediate termination; `shutdown()` joins connection, heartbeat, and outstanding
operations. The client stays on its bound loop for its entire lifetime.
