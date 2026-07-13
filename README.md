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

Add tests and examples to the same configuration when needed:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
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

Request and response schemas are deliberately separate. Request models support
JSON/form parsing and validation; response models provide typed setters and JSON
serialization. Defaults apply to request fields, while omit-empty and emit-null
apply to response fields. See the compiled
[`models_validation.cpp`](examples/models_validation.cpp) example for the complete
API.

## HTTP Protocol Library

`ruvia::http` can be used without the runtime or Web framework. It provides HTTP
message types and helpers, HTTP/1 request and response parsing/writing, the
server/client-role `Http2Connection` state machine, HPACK, WebSocket protocol state,
multipart parsing, SSE formatting, range and conditional-request helpers, cookies,
content negotiation, redirects, and content decoding.

The library is sans-I/O: callers feed bytes, consume typed results/events, and drive
transport I/O themselves. It contains no App, Context, Router, socket, TLS,
connection pool, runtime timeout, static-root policy, DB, Redis, or JWT integration.
