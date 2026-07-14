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

## Core Runtime

`ruvia::Runtime` creates application-owned workers. `WorkerHandle::post()` is a
bounded, thread-safe queue-in-loop API; it does not expose the underlying Asio
executor:

```cpp
#include <ruvia/core/Runtime.h>

ruvia::Runtime runtime({.workerCount = 4, .mailboxCapacity = 1024});
runtime.start();

auto worker = runtime.workerFor("device-42");
if (worker.post([] { /* runs on the selected worker */ }) !=
    ruvia::PostResult::kAccepted) {
    // Apply application backpressure or shutdown handling.
}

runtime.stop();
runtime.join();
```

Web handlers obtain their current worker with `Context::worker()`. Background
components can obtain the Web worker set from `App::workers()` after startup and
post owning data back to the worker before using worker-affine DB or Redis APIs.
`TaskScope`, worker-bound `sleepFor`, bounded `Channel`, and `OneShot` are also
provided by `ruvia::core`; their deadlines share the worker's single timer queue.

## Requirements

- CMake 3.24 or newer.
- A C++23 compiler.
- vcpkg.
- Component dependencies: core uses Asio and mimalloc; HTTP uses zlib, Brotli,
  and zstd; Web adds OpenSSL.
- Optional vcpkg features: MariaDB, PostgreSQL, Redis, and JWT.

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
| `RUVIA_BUILD_BENCHMARKS` | `OFF` | Build Release-oriented HTTP hot-path benchmarks; requires HTTP. |
| `RUVIA_BUILD_FUZZERS` | `OFF` | Build the HTTP/1, HTTP/2, and HPACK Clang/libFuzzer targets with UBSan; requires HTTP. |
| `RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS` | `OFF` | Add the repository-owned RFC 9113 wire conformance suite against a real Ruvia h2c server; requires tests, Web, and Python 3. |
| `RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS` | `OFF` | Add live PostgreSQL driver tests; requires tests and PostgreSQL support. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples; requires Web. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Enable MariaDB integration in Web. |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | Enable PostgreSQL integration in Web. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Enable Redis integration in Web. |
| `RUVIA_ENABLE_JWT` | `OFF` | Enable JWT integration in Web. |

### Database drivers

MariaDB and PostgreSQL use the same `DbHandle`, result, streaming, transaction,
pool and migration APIs. Select the driver when constructing its configuration:

```cpp
auto config = ruvia::DbConfig::postgreSql(); // port 5432
config.username = "app";
config.password = "secret";
config.database = "app";
app.useDb(std::move(config));
```

Enable its matching CMake feature first. PostgreSQL parameters use `$1`, `$2`,
and so on; MariaDB parameters use `?`. For generated PostgreSQL keys, use
`INSERT ... RETURNING id` and read the returned row.

### Protocol conformance and fuzzing

The regular Linux CI runs a repository-owned wire-level suite against Ruvia's actual
cleartext HTTP/2 server. Its connection-per-case, handcrafted-frame, and wire-error
assertion model follows the proven h2spec approach, but every expectation is maintained
directly against RFC 9113 instead of filtering RFC 7540 results. To reproduce it
locally, install Python 3 and configure:

```powershell
cmake -S . -B build-conformance `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS=ON `
  -DPython3_EXECUTABLE="C:/Python312/python.exe"
cmake --build build-conformance --config Debug --target ruvia_http2_conformance_server
ctest --test-dir build-conformance -C Debug -R ruvia_http2_conformance --output-on-failure
```

Protocol fuzzing requires Clang with libFuzzer. Enabling the option instruments
`ruvia-http` and builds three independent fuzz targets:

```bash
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRUVIA_BUILD_FUZZERS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-fuzz
build-fuzz/tests/fuzz/ruvia_fuzz_http2_connection -max_total_time=60
```

## Performance Baseline

Release benchmark scope, commands, and comparison rules live with the benchmark
sources in [tests/benchmarks/README.md](tests/benchmarks/README.md).

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
Content-Encoding parsing distinguishes identity, one supported coding, and an
unsupported coding stack; Web request decoding reports the latter as HTTP 415.
