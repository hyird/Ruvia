# Ruvia

Ruvia is a C++23 HTTP/Web framework with three independently consumable CMake
targets. The repository is a monorepo, but its runtime foundation and protocol
library do not require the full Web framework.

## Targets

| Directory | CMake target | Purpose |
| --- | --- | --- |
| `ruvia-core/` | `ruvia::core` | Coroutine tasks, Asio integration, PMR/mimalloc memory, connection scanning, and runtime helpers. |
| `ruvia-http/` | `ruvia::http` | Pure sans-I/O HTTP, HTTP/2, WebSocket, multipart, SSE, content-coding, and outbound-client protocol primitives. |
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server I/O, TLS, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web  -> ruvia-core + ruvia-http
```

`ruvia-http` is core-free, Asio-free, and socket-free. Applications that need an
outbound HTTP client provide their own I/O runtime and drive its sans-I/O client
APIs; `ruvia-web` intentionally does not provide `fetch`, proxy, connection-pool,
or client TLS runtime APIs.

## Architecture

The layer boundary follows ownership of behavior:

- `ruvia-core` owns reusable runtime infrastructure with no HTTP/Web semantics.
- `ruvia-http` owns wire parsing, framing, serialization, connection semantics,
  protocol state machines, and reusable protocol helpers.
- `ruvia-web` owns application policy and runtime integration: routing,
  middleware, error envelopes, sessions, CSRF, CORS, rate limits, static-root
  policy, TLS/socket drivers, DB, Redis, and JWT integration.

If code decides how protocol bytes are interpreted or emitted, it belongs in
`ruvia-http`. If it decides what an application or Web product does with those
facts, it belongs in `ruvia-web`. Cross-target reuse happens only through installed
public headers and `target_link_libraries()` include interfaces.

Runtime highlights:

- One standalone Asio `io_context`, acceptor, server, and thread per worker.
- Connections remain owned by one worker for their entire lifetime.
- HTTP/1 request parsing is zero-copy; the header limit is 64KB and the ordinary
  buffered-body limit is 16MB.
- Large request bodies, response streaming, SSE, and WebSocket handlers use
  explicit route modes.
- Response heads use fixed buffers and scatter-gather writes; file responses are
  not loaded into memory and use platform zero-copy paths when available.
- Request-local containers use a PMR arena; worker and process lifetimes use the
  corresponding Ruvia memory resources.

The detailed invariants are executable: unit tests, package-consumer tests, and
`scripts/check_layer_boundaries.cmake` guard protocol and target boundaries. This
README describes the supported product, not the history of internal refactors.

## Requirements

- CMake 3.24 or newer.
- A C++23 compiler.
- vcpkg.
- Component dependencies: core uses Asio and mimalloc; HTTP uses zlib, Brotli,
  and zstd; Web adds OpenSSL.
- Optional vcpkg features: MariaDB, Redis, and JWT.

On Windows, CMake defaults the vcpkg triplet to `x64-windows-static` unless the
caller already selected one.

## Build

Default build:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Debug
```

Build tests and examples:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Build only the standalone core component:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_CORE=ON `
  -DRUVIA_BUILD_HTTP=OFF `
  -DRUVIA_BUILD_WEB=OFF
```

For HTTP-only builds, enable `RUVIA_BUILD_HTTP` and disable core and Web. A Web
build always requires both lower targets.

Optional Web integrations are strict build features:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_ENABLE_MARIADB=ON `
  -DRUVIA_ENABLE_REDIS=ON `
  -DRUVIA_ENABLE_JWT=ON
```

## Build Options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build `ruvia::core`. |
| `RUVIA_BUILD_HTTP` | `ON` | Build standalone `ruvia::http`. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia::web`; requires core and HTTP. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit, guard, and package-consumer tests. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples; requires Web. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Enable MariaDB integration in Web. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Enable Redis integration in Web. |
| `RUVIA_ENABLE_JWT` | `OFF` | Enable JWT integration in Web. |

## Install and Consume

Install all selected targets:

```powershell
cmake --install build --config Debug --prefix build/install
```

Each library has an independent export. Consumers request the component they use:

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
independent, while Web imports core, HTTP, and Web. Component-scoped installation
uses `core`, `http`, `web`, and `Development` install components.

## Minimal Web App

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
        .setHttpListenPort(8080)
        .run();
}
```

Handlers use `ruvia::Task<T>`, read HTTP input through `c.req()`, and build
responses through `Context`. Connection metadata is deliberately separate from the
HTTP request model:

```cpp
const auto info = ruvia::getConnInfo(c);
const auto peerAddress = info.remote().address();
if (const auto* tls = info.tls()) {
    const auto clientSubject = tls->clientCertificateSubject();
}
```

## Web API Shape

Routes and schemas are registered at startup through macros:

- `RUVIA_CONTROLLER_GROUP(...)`
- `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END`
- `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE`
- `RUVIA_GET_STREAM`, `RUVIA_GET_SSE`
- `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS`
- `RUVIA_REQUEST_MODEL`, `RUVIA_RESPONSE_MODEL`, `RUVIA_FIELD`, `RUVIA_FIELD_NAME`
- `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_RULE`

Route tables, middleware chains, and controller factories are finalized before
workers start. The request path does not rebuild them or use a per-request virtual
dispatcher.

## Request and Response Models

Request and response schemas are deliberately separate. A request model can be
parsed from JSON/form/fields and used with `RUVIA_VALIDATE_*`; it is not a JSON
response type:

```cpp
RUVIA_REQUEST_MODEL(CreateUserRequest,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(age, ruvia::UInt32)
);

class CreateUserValidator final
    : public ruvia::Middleware<CreateUserValidator> {
public:
    RUVIA_VALIDATE_JSON(CreateUserRequest,
        RUVIA_RULE(name, RUVIA_REQUIRED("name is required")),
        RUVIA_RULE(age, RUVIA_MIN(1, "age must be positive")))
};
```

A response model provides typed setters and JSON serialization only. It cannot be
parsed as a request or passed to `RUVIA_VALIDATE_*`:

```cpp
RUVIA_RESPONSE_MODEL(CreateUserResponse,
    RUVIA_FIELD(id, ruvia::UInt32),
    RUVIA_FIELD(name, ruvia::String)
);

ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
    CreateUserResponse response(c);
    response.id(ruvia::UInt32{1}).name("Ruvia");
    co_return c.json(response, 201);
}
```

`RUVIA_DEFAULT` belongs to request fields. `RUVIA_OMIT_EMPTY` and
`RUVIA_EMIT_NULL` belong to response fields. Request models may nest only request
models; response models may nest only response models.

## HTTP Protocol Library

`ruvia::http` can be used without the runtime or Web framework. It provides HTTP
message types and helpers, HTTP/1 request and response parsing/writing, the
server/client-role `Http2Connection` state machine, HPACK, WebSocket protocol state,
multipart parsing, SSE formatting, range and conditional-request helpers, cookies,
content negotiation, redirects, and content decoding.

The library is sans-I/O: callers feed bytes, consume typed results/events, and drive
transport I/O themselves. It contains no App, Context, Router, socket, TLS,
connection pool, runtime timeout, static-root policy, DB, Redis, or JWT integration.

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- ruvia-core/
|   |-- CMakeLists.txt
|   |-- include/ruvia/core/
|   `-- src/
|-- ruvia-http/
|   |-- CMakeLists.txt
|   |-- include/ruvia/http/
|   `-- src/{body,client,http2,parser,server,websocket}/
|-- ruvia-web/
|   |-- CMakeLists.txt
|   |-- include/ruvia/web/
|   `-- src/{app,auth,db,http,redis,router,server}/
|-- examples/
|-- tests/
`-- vcpkg.json
```

Each target compiles only its own sources and installs headers only under its
matching namespace. Build directories, `vcpkg_installed`, and local tool indexes are
ignored and must not be committed.

## Verification

The normal local verification loop is:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build/install
```

Repository checks also include:

```powershell
git diff --check
cmake -P scripts/check_layer_boundaries.cmake
```
