# Ruvia

[![Build](https://github.com/hyird/Ruvia/actions/workflows/build.yml/badge.svg)](https://github.com/hyird/Ruvia/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hyird/Ruvia)](https://github.com/hyird/Ruvia/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)

Ruvia is a C++23 HTTP/Web framework built as three independently consumable
CMake targets. The repository is a monorepo, but its runtime foundation and
protocol library do not require the full Web framework.

## Highlights

- **Layered by design** — `ruvia::core` (runtime), `ruvia::http` (protocol), and
  `ruvia::web` (framework) install and import as independent package components.
- **Sans-I/O protocol library** — one HTTP/1, HTTP/2, WebSocket, and HPACK
  implementation shared by the server and the outbound client; callers feed
  bytes and consume typed events. No sockets, no Asio, no TLS inside.
- **Coroutine-first Web framework** — controllers, typed request/response
  models, validation, middleware, streaming, SSE, and WebSocket routes, all
  finalized at startup with no per-request rebuilding.
- **Bounded, application-owned runtime** — explicit workers, bounded mailboxes,
  backpressure at every producer, and deterministic shutdown draining.
- **TLS out of the box** — server TLS with optional mandatory mutual TLS
  (`requireClientCertificate`), and connection metadata kept separate from the
  HTTP request model.
- **Optional integrations** — MariaDB, PostgreSQL, Redis, and JWT behind vcpkg
  features; both database drivers share one `DbHandle` API surface.
- **Verified** — RFC 9113 wire conformance suite against a real h2c server,
  libFuzzer targets for the parsing surface, and guard tests that pin the
  public API contracts.

## Contents

- [Quick Start](#quick-start)
- [Targets](#targets)
- [Core Runtime](#core-runtime)
- [Requirements](#requirements)
- [Build](#build)
- [Database Drivers](#database-drivers)
- [Conformance and Fuzzing](#conformance-and-fuzzing)
- [Performance Baseline](#performance-baseline)
- [Install and Consume](#install-and-consume)
- [Web API Shape](#web-api-shape)
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
        .setHttpListenPort(8080)
        .run();
}
```

The same route is part of the compiled
[`basic_http.cpp`](examples/basic_http.cpp) example. Configure once with
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

Connection metadata is deliberately separate from the HTTP request model:

```cpp
const auto info = ruvia::getConnInfo(c);
const auto peerAddress = info.remote().address();
if (const auto* tls = info.tls()) {
    const auto clientSubject = tls->clientCertificateSubject();
}
```

## Targets

| Directory | CMake target | Purpose |
| --- | --- | --- |
| `ruvia-core/` | `ruvia::core` | Coroutine tasks, Asio integration, PMR/mimalloc memory, connection scanning, and runtime helpers. |
| `ruvia-http/` | `ruvia::http` | Pure sans-I/O HTTP, HTTP/2, WebSocket, multipart, SSE, content-coding, and outbound-client protocol primitives. |
| `ruvia-web/` | `ruvia::web` | App, Context, Router, middleware, server I/O, TLS, streaming, WebSocket routes, validation, static files, and optional integrations. |

Dependency direction is fixed:

```text
ruvia-web  ->  ruvia-core + ruvia-http
```

`ruvia-http` is core-free, Asio-free, and socket-free. Applications that need
an outbound HTTP client provide their own I/O runtime and drive its sans-I/O
client APIs; `ruvia-web` intentionally does not provide `fetch`, proxy,
connection-pool, or client TLS runtime APIs.

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

Web handlers obtain their current core worker with `Context::worker()`.
Background components select a stable Web worker with `App::workerFor()` and
submit an asynchronous Web job. The callback receives worker-local DB and Redis
access without exposing the underlying executor:

```cpp
auto worker = ruvia::app().workerFor("device-42");
auto result = worker.post(
    [event = std::move(event)](
        ruvia::WebWorkerContext& workerContext) mutable -> ruvia::Task<void> {
        co_await persistEvent(workerContext.db(), event);
    });
```

Web job contract:

- **Backpressure** — configure the bounded queue before startup with
  `App::setWorkerMailboxCapacity()` and handle `kQueueFull` at every producer.
- **Metrics** — `WebWorkerHandle::stats()` exposes accepted, rejected,
  completed, failed, and outstanding counts.
- **Shutdown draining** — a job accepted before shutdown is drained before that
  worker closes DB/Redis; new jobs are rejected once shutdown starts.
- **Lifetimes** — captured data must own its lifetime, and `WebWorkerContext`
  must not be stored beyond the callback.
- **Producers** — `App::workers()` returns all Web worker handles; external
  producers must keep using `WebWorkerHandle::post()` so accepted jobs remain
  covered by Web shutdown, failure propagation, and resource draining.
- **Failures** — an unhandled job exception stops every App worker and is
  rethrown by `App::run()`; applications should still catch expected DB or
  business failures inside the job.
- **Cancellation** — jobs may use `WebWorkerContext::worker()` for worker-bound
  core primitives, and must use cancellable waits or observe
  `WebWorkerContext::stopToken()` so shutdown can finish.

`TaskScope`, worker-bound `sleepFor`, bounded `Channel`, and `OneShot` are also
provided by `ruvia::core`; their deadlines share the worker's single timer
queue. `App::onStop()` hooks run once for signal shutdown, direct
`App::stop()`, and worker failure before the worker-local Web resources are
closed.

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

Linux / macOS:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Windows:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Debug
```

When needed, add `-DRUVIA_BUILD_TESTS=ON` and `-DRUVIA_BUILD_EXAMPLES=ON` to
the same configuration, rebuild, and run the tests:

```bash
ctest --test-dir build --output-on-failure   # add -C Debug on Windows
```

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Build `ruvia::core`. |
| `RUVIA_BUILD_HTTP` | `ON` | Build standalone `ruvia::http`. |
| `RUVIA_BUILD_WEB` | `ON` | Build `ruvia::web`; requires core and HTTP. |
| `RUVIA_BUILD_TESTS` | `OFF` | Build unit, guard, and package-consumer tests. |
| `RUVIA_BUILD_BENCHMARKS` | `OFF` | Build Release-oriented HTTP hot-path benchmarks; requires HTTP. |
| `RUVIA_BUILD_FUZZERS` | `OFF` | Build the Clang/libFuzzer protocol fuzz targets with UBSan; requires HTTP. |
| `RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS` | `OFF` | Add the repository-owned RFC 9113 wire conformance suite against a real Ruvia h2c server; requires tests, Web, and Python 3. |
| `RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS` | `OFF` | Add live PostgreSQL driver tests; requires tests and PostgreSQL support. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Build examples; requires Web. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | Enable MariaDB integration in Web. |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | Enable PostgreSQL integration in Web. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Enable Redis integration in Web. |
| `RUVIA_ENABLE_JWT` | `OFF` | Enable JWT integration in Web. |

## Database Drivers

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

## Conformance and Fuzzing

The regular Linux CI runs a repository-owned wire-level suite against Ruvia's
actual cleartext HTTP/2 server. Its connection-per-case, handcrafted-frame, and
wire-error assertion model follows the proven h2spec approach, but every
expectation is maintained directly against RFC 9113 instead of filtering
RFC 7540 results. To reproduce it locally, install Python 3 and configure:

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
`ruvia-http` and builds independent fuzz targets covering HTTP/1, HTTP/2, HPACK,
chunked transfer coding, multipart, and WebSocket frame decoding:

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

Each library has an independent export. Consumers request the component they
use:

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
independent, while Web imports core, HTTP, and Web. Component-scoped
installation uses `core`, `http`, `web`, and `Development` install components.

## Web API Shape

Routes and schemas are registered at startup through macros:

| Concern | Macros |
| --- | --- |
| Controller grouping | `RUVIA_CONTROLLER_GROUP(...)` |
| Route table | `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END` |
| HTTP methods | `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE` |
| Streaming / SSE | `RUVIA_GET_STREAM`, `RUVIA_GET_SSE` |
| WebSocket | `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS` |
| Models | `RUVIA_REQUEST_MODEL`, `RUVIA_RESPONSE_MODEL`, `RUVIA_FIELD`, `RUVIA_FIELD_NAME` |
| Validation | `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_RULE` |

Route tables, middleware chains, and controller factories are finalized before
workers start. The request path does not rebuild them or use a per-request
virtual dispatcher.

Request and response schemas are deliberately separate. Request models support
JSON/form parsing and validation; response models provide typed setters and
JSON serialization. Defaults apply to request fields, while omit-empty and
emit-null apply to response fields. See the compiled
[`models_validation.cpp`](examples/models_validation.cpp) example for the
complete API.

## HTTP Protocol Library

`ruvia::http` can be used without the runtime or Web framework. It provides
HTTP message types and helpers, HTTP/1 request and response parsing/writing,
the server/client-role `Http2Connection` state machine, HPACK, WebSocket
protocol state, multipart parsing, SSE formatting, range and
conditional-request helpers, cookies, content negotiation, redirects, and
content decoding.

The library is sans-I/O: callers feed bytes, consume typed results/events, and
drive transport I/O themselves. It contains no App, Context, Router, socket,
TLS, connection pool, runtime timeout, static-root policy, DB, Redis, or JWT
integration. Content-Encoding parsing distinguishes identity, one supported
coding, and an unsupported coding stack; Web request decoding reports the
latter as HTTP 415.

## License

Ruvia is released under the [MIT License](LICENSE).
