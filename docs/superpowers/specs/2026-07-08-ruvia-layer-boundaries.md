# Ruvia Layer Boundaries (authoritative)

Four physical CMake targets, strict single-direction dependencies, no cycles:

```text
ruvia-web   -> ruvia-core + ruvia-http
ruvia-edge  -> ruvia-core + ruvia-http
ruvia-http  -> no ruvia-core / no asio / no socket runtime
```

`web -> core + http`, `edge -> core + http`, `http -> external protocol deps only`.
`web` and `edge` are sibling products and must not link each other.

## The One Rule

**A thing belongs to a layer by what concepts it knows, not by what it is named.**

- Knows runtime primitives, Asio, Task, memory, worker/socket helpers, but nothing of HTTP or the app -> `core`.
- Knows HTTP protocol behavior, but nothing of `Context`, `Router`, `App`, `ruvia-core`, sockets, or Asio -> `http`.
- Knows `Context`, `Router`, `App`, web-framework semantics, HTTP-based application policy, or performs I/O for the web framework -> `web`.
- Knows CDN/reverse-proxy product policy and never uses `Context` -> `edge`.

## Per-Target Responsibility

### ruvia-core - Runtime

`Task`/coroutine primitives, PMR memory helpers, mimalloc integration, Asio awaiter/driver glue, the generic sans-I/O pump (`runtime/SansIoDriver.h`), connection timeout scanning, and socket/runtime helpers. Core links Asio; it knows nothing of HTTP.

### ruvia-http - Pure Sans-I/O Protocol

The basic HTTP/1.1, HTTP/2, WebSocket, multipart, SSE, content-coding, and HTTP client protocol library, usable from any runtime. You feed it bytes or protocol values, it emits events and produces bytes:

- h1: parser (`src/parser/`) plus `Http1Connection` (`src/net/http1/`) for embeddable connection semantics.
- h2: `Http2Connection`, one connection state machine for both server and client roles: frame codec, HPACK, settings, flow control, stream state, h2c upgrade, and RFC 8441 WebSocket handshake support.
- ws: `WsConnection` sans-I/O core plus frame codec, assembler, validation, close-code handling, and permessage-deflate.
- Message model: `HttpRequest`, `HttpResponse`, headers, methods, status, body-stream protocol primitives, cookies, cache, range, conditional request, content negotiation, multipart, and form/url helpers.
- Protocol semantics: request/response parsing, transfer framing, keep-alive and `Connection`, `Expect: 100-continue`, Upgrade/h2c/WebSocket handshake bytes, HTTP/1.0 close-delimited responses, response-head serialization, SSE formatting, and content-coding decoders.
- Outbound client protocol half: response parsing, redirect replay rules, content decoding, streaming decoder, config validation, and the HTTP/2 client role. The Asio/TLS driver lives one layer up in web or edge.

`ruvia-http` must not own `Context`, `Router`, `Controller`, route macros, middleware, `App`, DB/Redis/JWT integration, CDN policy, sockets, TLS, timers, Asio, or `ruvia-core` dependencies.

### ruvia-web - Web Framework Product

`App`, `Router`, `Controller`, route macros, middleware, `Context`, model validation, DB/Redis/JWT/CSRF/sessions. All HTTP-based application policy: rate limiting, access-log policy, CORS middleware, security-header middleware, static-file root scanning/indexing/product configuration, cookie signing, server timeouts, and `HttpServerOptions`.

All web I/O drivers over the http protocol cores live here:

- `src/net/server/Http2SansIoSession.h` drives `Http2Connection`.
- `src/net/server/HttpServerStreamSession.inl` drives HTTP/1 parsing/connection behavior.
- `src/net/ws/` binds WebSocket protocol cores to socket/h2 transports and web routes.
- `src/net/body/` contains Asio streaming request-body readers.
- `src/client/` contains the outbound HTTP client runtime driver: `HttpClientPool`, `Http2ClientSession`, `HttpClientRegistry`, TLS verification, deadlines, and socket streaming sources, surfaced through `Context::fetch`, `fetchStream`, and `proxy`.

Web code may read and write HTTP headers, but that does not make application policy protocol ownership.

### ruvia-edge - Edge Product

Origin selection, cache store/key policy, stale-while-revalidate, purge, edge rules, and reverse-proxy assembly over `ruvia-core` runtime primitives and `ruvia-http` protocol primitives. Edge may build its own drivers. It must not depend on `ruvia-web`, `App`, `Router`, middleware, or `Context`.

## Verification

`scripts/check_layer_boundaries.sh` runs the checks below; the `ruvia_check_boundaries` CMake target runs it when tests are enabled.

1. `ruvia-http` is core/asio-free: no `#include <asio`, `#include "asio`, `asio::`, no include of `ruvia/app`, `ruvia/memory`, `ruvia/detail`, `ruvia/router`, or `ruvia/http/Context`, and no `ruvia::core` link from `ruvia-http`.
2. `ruvia-edge` stays web-free: it does not link `ruvia-web`/`ruvia::web`, and edge sources include no `Context`, router, or app headers.
3. Docs carry no stale ownership: `README.md`, `AGENTS.md`, and this spec must describe `web/edge -> core + http`, `http -> no core/asio`, and runtime drivers as web/edge-owned.
4. Build and tests stay green: configure with tests/examples/edge enabled, build Debug, run `ctest`, and install.

## History / Remaining Refinements

- The sans-I/O unification is complete: `Http2Connection` is the single HTTP/2 state machine for both sides; `ruvia-http` builds with zero core/asio.
- `Http1Connection` exists as the HTTP/1 sans-I/O core so edge can reuse h1 connection handling without the web session. The web h1 session may continue driving the parser directly while sharing the same protocol implementation.
