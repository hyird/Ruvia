# Ruvia Layer Boundaries (authoritative)

Four physical CMake targets, strict single-direction dependencies, no cycles:

```
ruvia-core  ←  ruvia-http  ←  ruvia-web
                     ↑
                 ruvia-edge          (web and edge are siblings; must NOT link each other)
```
`http → core`, `web → http`, `edge → http`.

## The one rule that decides everything

**A thing belongs to a layer by what concepts it *knows*, not by what it is named.**
- Knows nothing of HTTP or the app → `core`.
- Knows the HTTP protocol but nothing of `Context`/`Router`/`App` **and nothing of sockets/asio** → `http`.
- Knows `Context`/`Router`/`App` (web-framework semantics) or performs I/O for the framework → `web`.
- CDN/reverse-proxy product policy, never uses `Context` → `edge`.

## Per-target responsibility

### ruvia-core — runtime
`Task`/coroutine primitives, PMR memory helpers, ascii/hex/base64/const-time/date utils, the asio glue (`AsioAwait`), the generic sans-I/O pump (`runtime/SansIoDriver.h`), and the connection timeout scanner + socket utils (`ruvia-core/src/net/server/ConnectionScanner.{h,cpp}`, `SocketUtils.h` — include path unchanged: `net/server/...`). Core links asio; it knows nothing of HTTP.

### ruvia-http — pure sans-I/O protocol (ZERO asio, grep-enforced)
The most basic HTTP/1.1, HTTP/2, WebSocket protocol library, usable from any runtime (nghttp2 / picohttpparser class). You feed it bytes, it emits events and produces bytes:
- h1 parser (`src/parser/`, sans-I/O).
- h2: `Http2Connection` — **one connection state machine for BOTH roles**: server (accepts peer streams, `submit*` responses, RFC 8441 WebSocket handshake, GOAWAY drain, h2c upgrade seeding) and client (`Http2Role::kClient`: odd streams, request heads, RESPONSE decode with 1xx handling, consume-paced receive windows). Frame codec, HPACK, flow control, stream state underneath.
- ws: `WsConnection` sans-I/O core + the pure frame codec/assembler/validation/permessage-deflate shared by every driver.
- Message model (`HttpRequest`/`HttpResponse`, headers, methods, status, cookies-parsing, body streams, multipart), content coding, protocol value helpers (Cache-Control, HTTP-date, Range, Accept).
- **Client note**: `ruvia::http` contains the client's *protocol engine* (the h2 core's client role + the h1 parser + message types), NOT a ready-to-use client runtime. The runtime is web (below); the public surface `ruvia/http/HttpClient.h` is installed by **ruvia-web**.
- **MUST NOT own**: `Context`, `Router`, `Controller`, route macros, middleware, `App`, DB/Redis/JWT integration, CDN policy, sockets, TLS, timers, or any `#include <asio...>`.
- Deliberate boundary artifacts hosted here (documented in-file, not drift): `router/RouteResolution.h` (the http-side dispatcher-contract POD; `RouteEntry` only as an opaque pointer) and the `HttpErrorHandler`/`HttpNotFoundHandler` aliases in `ruvia/http/Error.h` (web hook types next to `HttpErrorInfo`; never invoked by http).

### ruvia-web — the web framework product (all framework I/O drivers live here)
`App`, `Router`, `Controller`, route macros, middleware, `Context`, model validation, DB/Redis/JWT/CSRF/sessions. **All framework policy**: rate limiting, access-log policy, CORS, static-file serving, cookie signing, server timeouts, `HttpServerOptions`. **All I/O drivers over the http cores**:
- `src/net/server/Http2SansIoSession.h` — the production h2 server session (reader + single writer over `Http2Connection`; the accept loop's TLS-ALPN / cleartext-preface / h2c-upgrade entries all run it).
- `src/net/server/HttpServerStreamSession.inl` — the h1 server session (an I/O driver over the single pure parser; not a duplicate protocol implementation).
- `src/net/ws/` — `WebSocketConnection<Transport>` + socket/h2 transports + the h1 101 handshake writer.
- `src/net/body/` — asio streaming request-body readers.
- `src/client/` — the outbound HTTP client **runtime**: `HttpClientPool` (h1), `Http2ClientSession` (a thin driver over the shared `Http2Connection` client role), `HttpClientRegistry`, TLS verification, redirects, deadlines, streaming sources. Surfaced via `Context::fetch` / `fetchStream` / `proxy`.

### ruvia-edge — the CDN / reverse-proxy product
Origin selection, cache store/key policy, stale-while-revalidate, purge, edge rules, reverse-proxy assembly over `ruvia-http` primitives (and its own drivers when built). **MUST NOT** depend on `ruvia-web`, `App`, `Router`, middleware, or `Context`. Currently an empty skeleton by decision.

## Verification (mechanised)

`scripts/check_layer_boundaries.sh` runs the checks below; the `ruvia_check_boundaries` CMake target (built with tests) executes it, so a violation fails the build. Manual anytime: `bash scripts/check_layer_boundaries.sh`.

1. **http is asio-free**: no `#include <asio` / `#include "asio` / `asio::` anywhere under `ruvia-http/` (covers `Http2Connection.h` / `WsConnection.h` transitively, since the whole target is clean).
2. **http does not reach the framework**: no include of `ruvia/router/...`, `ruvia/http/Context.h`, `ruvia/app/App.h`, or other web-hosted `ruvia/app/...` headers (core's `ruvia/app/Task.h` + `ruvia/app/detail/...` are allowed).
3. **edge stays web-free**: `ruvia-edge/CMakeLists.txt` does not link `ruvia-web`/`ruvia::web`; edge sources include no `Context`/router/app headers.
4. **docs carry no stale ownership**: `README.md` / `AGENTS.md` / this spec must not attribute the client runtime to the http target nor reference the deleted coroutine h2 server session class by name.
5. **No bugs**: `cmake --build build-client` green; `ruvia_unit_tests` all pass (985 at the time of writing, incl. the core-vs-core loopback `sansio_h2_client_to_sansio_h2_server_round_trip`); `ruvia_smoke_http_target` / `ruvia_smoke_edge_target` exit 0.
6. **Dependency direction** enforced by CMake: each target exposes only its own roots; an upward `#include` fails to compile.

## History / remaining refinements

- The sans-I/O unification is COMPLETE (commits `88eb8ac..17bdb47`, 2026-07-08): the coroutine h2 server session stack and the client's standalone h2 implementation are deleted; `Http2Connection` is the single h2 state machine for both sides; `ruvia-http` preprocesses with zero asio.
- **h1 sans-I/O core**: the parser is already the single h1 protocol implementation; an embeddable `Http1Connection` (feed → head/body/end events, h2-core-shaped) is the remaining refinement so edge can reuse h1 connection handling without the web session. The web h1 session keeps driving the parser directly until edge needs it.

Progress log: memory `ruvia-sansio-migration`.
