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
- Knows the HTTP protocol but nothing of `Context`/`Router`/`App` → `http`.
- Knows `Context`/`Router`/`App` (web-framework semantics) → `web`.
- CDN/reverse-proxy product policy, never uses `Context` → `edge`.

## Per-target responsibility

### ruvia-core — runtime
`Task`/coroutine primitives, PMR memory helpers, ascii/hex/base64/const-time/date utils, the asio glue (`AsioAwait`). Zero HTTP, zero framework. (Future: the generic sans-I/O driver template + `ConnectionScanner` timeout live here — a concept-driven driver that does not name HTTP, so `core` never depends on `http`.)

### ruvia-http — pure protocol
The most basic HTTP/1.1, HTTP/2, WebSocket protocol library, usable by anyone (nghttp2 / picohttpparser class):
- h1 parser (already sans-I/O), h2 frame codec + HPACK + flow control + stream state, ws frame codec + handshake + permessage-deflate.
- Message model (`HttpRequest`/`HttpResponse`, headers, methods, status, cookies-parsing, body streams, multipart), transfer/content coding (chunked, gzip/br/zstd), protocol value helpers (Cache-Control, HTTP-date, Range, Accept).
- Outbound HTTP client transport (protocol client, shares the h2 core).
- **MUST NOT own**: `Context`, `Router`, `Controller`, route macros, middleware, `App`, DB/Redis/JWT integration, and CDN policy.

### ruvia-web — the web framework product
`App`, `Router`, `Controller`, route macros, middleware, `Context`, model validation, DB/Redis/JWT/CSRF/sessions. **All framework policy**: rate limiting, access-log policy, CORS, static-file serving, cookie signing, server timeouts, and `HttpServerOptions`.

### ruvia-edge — the CDN / reverse-proxy product
Origin selection, cache store/key policy, stale-while-revalidate, purge, edge rules, reverse-proxy assembly over `ruvia-http` primitives. **MUST NOT** depend on `ruvia-web`, `App`, `Router`, middleware, or `Context`.

## Verification (how "clear responsibilities + no bugs" is checked)

Run from repo root (client build variant, `-DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EDGE=ON`):
- **No bugs**: `cmake --build build-client` green; `ruvia_unit_tests` 950 pass; `ruvia_smoke_http_target` / `ruvia_smoke_edge_target` exit 0.
- **Dependency direction** enforced by CMake: each target exposes only its own `src` as an include root; an upward `#include` fails to compile.
- **h2/ws server sessions are http-clean** (compile with only http+core include dirs, no `RUVIA_ENABLE_WEB`, no web path):
  `g++ -std=c++23 -fsyntax-only -DASIO_STANDALONE -I ruvia-http/src -I ruvia-http/src/net -I ruvia-http/include -I ruvia-core/src -I ruvia-core/include -I <vcpkg>/include <tu including net/http2/Http2ServerSession.h>` → 0 errors. Same for `net/ws/HttpWebSocketSession.h`.
- **sans-I/O protocol core is asio-free**: `Http2Connection.h` has no `#include <asio…>`; compiles standalone with http+core includes only.
- **edge does not link web**: `ruvia-edge/CMakeLists.txt` links only `ruvia::http`; `smoke_edge_target` runs without web.

## Tracked scoped refinements (not violations — future work, plan-tracked)

1. **Pure-protocol purity of `ruvia-http`**: a few framework-policy files (RateLimiter, AccessLogRecord, HttpCors, HttpServerOptions) were pulled into `ruvia-http` while making the h2/ws sessions compile there (commit `dd65b1f`). Per this boundary spec they belong in `ruvia-web`. Moving them back is entangled with the session's direct use of them; it is done cleanly once the **sans-I/O refactor** removes those uses (session emits events; web applies policy). Sequenced last in the sans-I/O plan.
2. **sans-I/O rewrite** (in progress): `Http2Connection` sans-I/O core skeleton landed (commit `f1e3e85`), asio-free and verified; the `feed`/`submit` logic port (1:1 from the current coroutine `Http2ServerSession*.inl`, backpressure via defer/resume) + a dedicated `Http2Connection` unit test are the remaining work. Turns `ruvia-http` into a runtime-agnostic protocol library.
3. **h1 server session** currently lives in `ruvia-web` (names `RouteTable`); it is web's own session and not a boundary violation, but gets the same sans-I/O treatment for edge reuse later.

Full design + phasing: the approved sans-I/O plan. Progress log: memory `ruvia-library-split`.
