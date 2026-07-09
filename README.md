# Ruvia

Ruvia is a small C++23 HTTP/Web framework with explicit library boundaries. The repository is a monorepo, but the reusable core and HTTP layers are first-class CMake targets that can be consumed without the full web framework.

## Targets

| Directory | Target | Public alias | Purpose |
| --- | --- | --- | --- |
| `ruvia-core/` | `ruvia-core` | `ruvia::core` | Runtime foundation: coroutine task type, Asio integration glue, PMR memory resources, mimalloc integration, and small runtime helpers. |
| `ruvia-http/` | `ruvia-http` | `ruvia::http` | Pure sans-I/O protocol library (zero core/asio/socket runtime): HTTP message types, header token/value helpers, HTTP/1 parser + connection core, HTTP/2 connection core (server + client roles), WebSocket protocol core, HPACK, body framing/streams, multipart/SSE/content-encoding protocol helpers, cookie/cache/range/conditional/negotiation helpers, and the outbound client's protocol core. |
| `ruvia-web/` | `ruvia-web` | `ruvia::web` | The full web framework: App, Context, Controller, Router, middleware, model/validation, server and outbound-client I/O drivers over the HTTP cores (Asio, TLS/ALPN, timeouts, streaming, WebSocket routes, `Context::fetch`/`proxy`), plus application policies such as session, CSRF/JWT, CORS, security headers, rate limits, static roots, AutoHTTPS redirect, DB, and Redis. |
| `ruvia-edge/` | `ruvia-edge` | `ruvia::edge` | Edge product target built directly on `ruvia::http`, without linking `ruvia::web` or using `Context`, `Router`, controllers, route macros, or middleware. |

Dependency direction:

```text
ruvia-web  -> ruvia-core + ruvia-http
ruvia-edge -> ruvia-core + ruvia-http
```

## Layer Boundary

The boundary is decided by who owns the behavior, not by whether a file touches HTTP header names.

- `ruvia-core` owns the runtime foundation: coroutine task machinery, Asio awaiter/driver glue, PMR resources, worker memory, connection scanning, and small runtime helpers.
- `ruvia-http` owns HTTP itself and must not depend on `ruvia-core`: wire bytes, message shape, header syntax helpers, parser/framing rules, connection persistence, `Expect: 100-continue`, upgrade handshakes, HTTP/2 frames/settings/flow control, WebSocket frames, multipart/SSE/content-encoding protocol logic, and protocol errors.
- `ruvia-web` owns the application framework built on HTTP: route dispatch, middleware, controllers, `Context`, validation, sessions, CSRF, JWT integration, CORS policy, security-header policy, rate limits, static-file indexing, AutoHTTPS redirect, DB/Redis integrations, and the socket/TLS/Asio runtime drivers that drive `ruvia-http`.
- `ruvia-edge` owns edge product behavior. It may call `ruvia::http` directly, but it must not depend on `ruvia::web` abstractions.

If code decides how bytes are parsed, framed, serialized, kept alive, upgraded, or rejected by the HTTP/WebSocket/HTTP2 protocols, it belongs in `ruvia-http`. If code decides what the application product does with those protocol facts, it belongs in `ruvia-web` or `ruvia-edge`.

The root `CMakeLists.txt` only coordinates global options, dependency discovery, installation, package export, tests, and examples. Each library owns its own `CMakeLists.txt`, `include/`, and `src/` directory. There is no root-level source `include/`, `src/`, or `fuzz/` tree.

## Requirements

- CMake 3.24 or newer.
- C++23 compiler.
- vcpkg.
- Core manifest dependencies: `asio`, `mimalloc`, `openssl`, `zlib`, `brotli`, and `zstd`.
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

Build tests, examples, and the edge target:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON `
  -DRUVIA_BUILD_EDGE=ON
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
  -DRUVIA_ENABLE_JWT=ON `
  -DVCPKG_MANIFEST_FEATURES="mariadb;redis;jwt"
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
target_link_libraries(proxy_tool PRIVATE ruvia::http)

find_package(ruvia CONFIG REQUIRED COMPONENTS edge)
target_link_libraries(edge_app PRIVATE ruvia::edge)
```

When no component is requested, the package loads the built direct targets. New projects should still request the component they use explicitly.

## Minimal Web App

```cpp
#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

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

Handlers are async-only and use `ruvia::Task<T>`. Request data is read through `c.req()`, and response helpers live on `Context`.

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
- PMR memory is layered by process, worker, and request lifetime.

## HTTP Library

`ruvia::http` is intended to be useful without the web framework or the Ruvia runtime foundation, in the nghttp2 class: a pure, core-free, asio-free, sans-I/O protocol library. It owns HTTP wire/message/framing/connection semantics and reusable helpers -- the h1 parser and connection semantics, the HTTP/2 connection state machine (`Http2Connection`, one implementation driven in both server and client role), the WebSocket protocol core, HPACK, response-head serialization helpers, cookie/cache/range/conditional request/content negotiation helpers, multipart/form/url encoding, SSE formatting, content decoding, and body-stream protocol primitives. You feed it bytes and drive its events from any runtime.

It does not own `App`, `Context`, `Controller`, `Router`, middleware, model validation, DB, Redis, JWT, CORS policy, security-header middleware, static-file product policy, edge policy, or any socket/TLS I/O. Reading or writing HTTP headers is not by itself a reason to live in `ruvia::http`: protocol decisions such as framing, keep-alive, upgrade handshakes, and response-head serialization belong here; product decisions such as CORS, sessions, CSRF, rate limits, redirects, and static-root indexing live in `ruvia::web` or `ruvia::edge`.

The outbound HTTP client follows the same split as the server: the protocol model and sans-I/O HTTP client core (response parsing, transfer/content decoding, generic redirect replay checks, and HTTP/2 client protocol state) live in `ruvia::http`, while the Asio/TLS runtime driver over them ships in `ruvia::web` and is used through `Context::fetch` / `Context::fetchStream` / `Context::proxy`.

## Edge Target

`ruvia::edge` is a sibling product target, not a web-framework plugin. It may use `ruvia::core` and `ruvia::http`, but it must not link to `ruvia::web` or depend on web routing abstractions.

## Build Options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit and smoke tests. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples. |
| `RUVIA_BUILD_EDGE` | `OFF` | Build `ruvia-edge` / `ruvia::edge`. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Build MariaDB-compatible DB integration into `ruvia-web`. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Build Redis integration into `ruvia-web`. |
| `RUVIA_ENABLE_JWT` | `OFF` | Build JWT helpers into `ruvia-web`. |

The outbound HTTP client protocol core is a `ruvia-http` capability (no build switch); its Asio/TLS runtime driver ships in `ruvia-web`, mirroring the server-side split.

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- ruvia-core/
|   |-- CMakeLists.txt
|   |-- include/
|   `-- src/
|-- ruvia-http/
|   |-- CMakeLists.txt
|   |-- include/
|   `-- src/
|-- ruvia-web/
|   |-- CMakeLists.txt
|   |-- include/
|   `-- src/
|-- ruvia-edge/
|   |-- CMakeLists.txt
|   `-- src/
|-- examples/
|-- tests/
`-- vcpkg.json
```

The only local build directory is `build/`. If CMake cache or generated files become suspicious, delete `build/` and configure again. Vcpkg installation trees, CodeGraph indexes, and local agent directories are ignored.

## Verification

Common development verification:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON `
  -DRUVIA_BUILD_EDGE=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build/install
```

Quick cleanup checks:

```powershell
git diff --check
rg -n '<stale split terms>' README.md AGENTS.md CMakeLists.txt ruvia-core ruvia-http ruvia-web ruvia-edge tests examples
```
