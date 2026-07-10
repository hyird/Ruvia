# Ruvia

Ruvia is a small C++23 HTTP/Web framework with explicit library boundaries. The repository is a monorepo, but the reusable core and HTTP layers are first-class CMake targets that can be consumed without the full web framework.

## Targets

| Directory | Target | Public alias | Purpose |
| --- | --- | --- | --- |
| `ruvia-core/` | `ruvia-core` | `ruvia::core` | Runtime foundation: coroutine task type, Asio integration glue, PMR memory resources, mimalloc integration, and small runtime helpers. |
| `ruvia-http/` | `ruvia-http` | `ruvia::http` | Pure sans-I/O protocol library (zero core/asio/socket runtime): HTTP message types, header token/value helpers, HTTP/1 parser + connection core, HTTP/2 connection core (server + client roles), WebSocket protocol core, HPACK, request/response body framing and HTTP/2 stream state, multipart/SSE/content-encoding protocol helpers, cookie/cache/range/conditional/negotiation helpers, and the outbound client's protocol core. |
| `ruvia-web/` | `ruvia-web` | `ruvia::web` | The full server-side web framework: App, Context, Controller, Router, middleware, model/validation, server I/O over the HTTP cores (Asio, TLS/ALPN, timeouts, streaming, WebSocket routes, and file read buffers), plus application policies such as session, CSRF/JWT, CORS, security headers, rate limits, static roots with MIME/validator metadata, AutoHTTPS redirect, DB, and Redis. |

Dependency direction:

```text
ruvia-web  -> ruvia-core + ruvia-http
```

## Layer Boundary

The boundary is decided by who owns the behavior, not by whether a file touches HTTP header names.

- `ruvia-core` owns the runtime foundation: coroutine task machinery, Asio awaiter/driver glue, PMR resources, worker memory, connection scanning, and small runtime helpers.
- `ruvia-http` owns HTTP itself and must not depend on `ruvia-core`: wire bytes, message shape, header syntax helpers, parser/framing rules, connection persistence, `Expect: 100-continue`, upgrade handshakes, HTTP/2 frames/settings/flow control, WebSocket frames, multipart/SSE/content-encoding protocol logic, and allocation-free `HttpProtocolError` signals.
- `ruvia-web` owns the application framework built on HTTP: route dispatch, middleware, controllers, `Context`, model/validation JSON serialization, `HttpError`/JSON application error responses, sessions, CSRF, JWT integration, CORS policy, security-header policy, rate limits, static-file indexing, AutoHTTPS redirect, DB/Redis integrations, and the socket/TLS/Asio runtime drivers that drive `ruvia-http`.

If code decides how bytes are parsed, framed, serialized, kept alive, upgraded, or rejected by the HTTP/WebSocket/HTTP2 protocols, it belongs in `ruvia-http`. If code decides what the application product does with those protocol facts, it belongs in `ruvia-web`.

Protocol primitives report status plus a static diagnostic through `HttpProtocolError`.
`ruvia-web` translates that signal into its `HttpErrorInfo`/JSON envelope. Router and
custom error handlers never set HTTP/1 connection persistence; the server runtime applies
`Connection: close` only after it knows the request-body and keep-alive state.
For streaming responses, `ruvia-http` returns one `Http1ResponseStreamPlan` that binds
HTTP version, chunked versus close-delimited framing, persistence, and response signaling;
`ruvia-web` contributes only an external force-close policy bit and drives the returned plan.
Buffered and streaming body decisions are also HTTP-owned: `HttpResponseBodyPlan` combines
the request method with the response status, while `HttpBufferedResponseWritePlan` adds the
representation length and the final send-body verdict. HTTP/1 and HTTP/2 consume these same
plans; `ruvia-web` must not recompute them with loose `skipBody` flags. In particular, a HEAD
response keeps the GET representation metadata (including negotiated content coding and
content length) but never emits payload bytes or HTTP/2 DATA frames. The HTTP/2 core records
local `END_STREAM` for requests and responses and rejects later `submitData()` calls, so an
external runtime cannot accidentally violate the stream lifecycle after the plan is applied.

The root `CMakeLists.txt` only coordinates global options, dependency discovery, installation, package export, tests, and examples. Each library owns its own `CMakeLists.txt`, `include/`, and `src/` directory. There is no root-level source `include/`, `src/`, or `fuzz/` tree.

## Requirements

- CMake 3.24 or newer.
- C++23 compiler.
- vcpkg.
- Component manifest dependencies: `core` uses `asio`/`mimalloc`, `http` uses
  `zlib`/`brotli`/`zstd`, and `web` adds `openssl`.
- Optional vcpkg features: `mariadb`, `redis`, `jwt`.

On Windows, the root CMake file defaults `VCPKG_TARGET_TRIPLET` to `x64-windows-static` when it is not already set.

## Build

Default build:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Debug
```

Build only one standalone component (vcpkg installs only that component's
dependency feature set):

```powershell
# core only
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_CORE=ON `
  -DRUVIA_BUILD_HTTP=OFF `
  -DRUVIA_BUILD_WEB=OFF

# http only
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_CORE=OFF `
  -DRUVIA_BUILD_HTTP=ON `
  -DRUVIA_BUILD_WEB=OFF
```

Build tests and examples:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Enable optional web integrations:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_ENABLE_MARIADB=ON `
  -DRUVIA_ENABLE_REDIS=ON `
  -DRUVIA_ENABLE_JWT=ON
```

## Install And Consume

Install:

```powershell
cmake --install build --config Debug --prefix build/install
```

Consume the full web framework:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(my_app PRIVATE ruvia::web)
```

Consume narrower libraries:

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS core)
target_link_libraries(tool PRIVATE ruvia::core)

find_package(ruvia CONFIG REQUIRED COMPONENTS http)
target_link_libraries(protocol_tool PRIVATE ruvia::http)
```

When no component is requested, the package loads the built direct targets. New projects should still request the component they use explicitly.

## Minimal Web App

```cpp
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class HelloController final : public ruvia::Controller<HelloController> {
public:
    RUVIA_ROUTES_BEGIN
        RUVIA_GET("/hello", hello)
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello");
    }
};

int main() {
    ruvia::app()
        .setHttpListenPort(8080)
        .run();
}
```

Handlers are async-only and use `ruvia::Task<T>`. HTTP request data is read through
`c.req()`, and response helpers live on `Context`. Adapter-owned connection metadata is
kept out of the request model and queried with the Hono-like `ruvia::getConnInfo(c)`:

```cpp
const auto info = ruvia::getConnInfo(c);
const auto peerAddress = info.remote().address();
const bool tls = info.secure();
const auto clientSubject = info.clientCertificateSubject();
```

The certificate value is the verified mutual-TLS peer subject DN, or empty when no
client certificate was presented.

## Core API Shape

Ruvia's public web API is macro-based and startup-built:

- `RUVIA_CONTROLLER_GROUP(...)`
- `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END`
- `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE`
- `RUVIA_GET_STREAM`, `RUVIA_GET_SSE`
- `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS`
- `RUVIA_MODEL`, `RUVIA_FIELD`, `RUVIA_FIELD_NAME`
- `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_RULE`

The request hot path uses prebuilt route tables and middleware chains. Public APIs expose Ruvia types, not Web runtime objects.

## Runtime Model

- Per-worker standalone Asio `io_context`.
- One acceptor/server/thread per worker.
- Connections stay owned by their worker.
- Request parsing is zero-copy by default.
- Header limit is 64KB; ordinary body limit is 16MB.
- Explicit stream routes handle large request bodies.
- Responses use fixed header buffers and scatter-gather writes.
- File responses avoid full-file buffering and use zero-copy paths where available.
- The HTTP/1 request path is allocation- and lock-free; HTTP/2 multiplexing adds one
  recycled coroutine frame and one virtual dispatch per stream.
- The optional rate limiter is the one shared-atomic structure on the request path; it is
  off by default, so per-request atomic cost is opt-in.
- Two PMR pooling tiers back memory: a process-level mimalloc resource and a per-request
  monotonic arena. Per-worker isolation comes from an object pool plus mimalloc's
  thread-local heaps rather than a distinct PMR tier.

## HTTP Library

`ruvia::http` is intended to be useful without the web framework or the Ruvia runtime foundation, in the nghttp2 class: a pure, core-free, asio-free, sans-I/O protocol library. It owns HTTP wire/message/framing/connection semantics and reusable helpers -- the h1 parser and connection semantics, the HTTP/2 connection state machine (`Http2Connection`, one implementation driven in both server and client role), the WebSocket protocol core (`WebSocketProtocol.h`), HPACK, response-head serialization helpers, cookie/cache/range/conditional request/content negotiation helpers, multipart/form/url encoding (`MultipartParser.h`), SSE formatting (`Sse.h`), content decoding, and opaque protocol body handles. You feed it bytes and drive its events from any runtime.

It does not own `App`, `Context`, `Controller`, `Router`, middleware, model validation, DB, Redis, JWT, CORS policy, security-header middleware, static-file product policy, or any socket/TLS I/O. Its `HttpRequest` represents the HTTP message only and therefore never stores peer addresses, TLS state, or client-certificate identity. Reading or writing HTTP headers is not by itself a reason to live in `ruvia::http`: protocol decisions such as framing, keep-alive, upgrade handshakes, and response-head serialization belong here; product decisions such as CORS, sessions, CSRF, rate limits, redirects, and static-root indexing live in `ruvia::web`.

The outbound HTTP client surface is intentionally limited to the low-level, sans-I/O protocol API in `ruvia::http`: `HttpOrigin`, request/response models, response parsing, transfer/content decoding, redirect replay checks, and the HTTP/2 client-role protocol state. It contains no TLS file settings, pools, or runtime timeouts. `ruvia::web` does not provide a socket/TLS client runtime, `fetch`, or reverse-proxy integration; applications that need outbound HTTP drive the protocol API from their own I/O runtime.

## Build Options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build the standalone `ruvia-core` runtime target. |
| `RUVIA_BUILD_HTTP` | `ON` | Build the standalone, core-free `ruvia-http` protocol target. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia-web`; requires both core and HTTP targets. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit and smoke tests. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Build MariaDB-compatible DB integration into `ruvia-web`. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Build Redis integration into `ruvia-web`. |
| `RUVIA_ENABLE_JWT` | `OFF` | Build JWT helpers into `ruvia-web`. |

The outbound HTTP client protocol core is a `ruvia-http` capability with no separate build switch. Applications supply the transport implementation for outbound requests.

CMake derives vcpkg manifest features from these switches before dependency
installation. Optional MariaDB, Redis, and JWT switches require `RUVIA_BUILD_WEB=ON`.

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- ruvia-core/
|   |-- CMakeLists.txt
|   |-- include/ruvia/core/         # core-only public/install namespace
|   `-- src/                        # flat runtime implementations
|-- ruvia-http/
|   |-- CMakeLists.txt
|   |-- include/ruvia/http/         # HTTP-only public/install namespace
|   `-- src/{client,http2,parser,server,websocket}/
|-- ruvia-web/
|   |-- CMakeLists.txt
|   |-- include/ruvia/web/          # Web-only public/install namespace
|   `-- src/{app,http2,server,websocket,db,redis,router}/
|-- examples/
|-- tests/
`-- vcpkg.json
```

Each target compiles files only from its own source directory and installs headers only
under its matching `ruvia/core`, `ruvia/http`, or `ruvia/web` namespace. Targets share
contracts only through the dependency target's installed include interface; physical
cross-target source/private-header paths and mixed install roots are rejected at configure
time and by the boundary checker.

The only local build directory is `build/`. If CMake cache or generated files become suspicious, delete `build/` and configure again. Vcpkg installation trees, CodeGraph indexes, and local agent directories are ignored.

## Verification

Common development verification:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build/install
```

Quick cleanup checks:

```powershell
git diff --check
rg -n '<stale split terms>' README.md AGENTS.md CMakeLists.txt ruvia-core ruvia-http ruvia-web tests examples
```
