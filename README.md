# Ruvia

[![Build](https://github.com/hyird/Ruvia/actions/workflows/build.yml/badge.svg)](https://github.com/hyird/Ruvia/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hyird/Ruvia)](https://github.com/hyird/Ruvia/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)

Ruvia is a C++23 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, WebSocket, and optional database, Redis, and JWT support.

## Targets

| CMake target | Use it for |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3/QUIC, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access. |

Core and HTTP can be used independently. Web includes both.

## Contents

- [Quick Start](#quick-start)
- [Build](#build) and [installation](#install-and-consume)
- [Web API: configuration, routes, models, and sessions](docs/web-api.md)
- [Runtime: tasks, workers, and blocking work](docs/runtime.md)
- [Outbound HTTP and WebSocket clients](docs/outbound-clients.md)
- [Static files and compression](docs/static-files.md)
- [Database and ORM](docs/database.md)
- [Redis and Redis ORM](docs/redis.md)
- [Standalone HTTP protocol APIs](docs/http-protocol.md)

## Quick Start

```cpp
#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>

class HelloController final : public ruvia::Controller<HelloController> {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN
        RUVIA_GET("/hello", hello);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello");
    }
};

int main() {
    ruvia::app()
        .server({
            .process_signal_handlers = ruvia::process_signal_handler_policy::install,
        })
        .listen({.address = "0.0.0.0", .http = 8080})
        .run();
}
```

Configure the process-wide App before `run()`. A listener is required.
Signal handlers are opt-in; embedded applications can call `App::stop()`.

With examples enabled, run
[`ruvia_example_basic_http`](examples/web/basic_http.cpp) and request
`http://127.0.0.1:8080/api/hello`. On Windows the executable is under
`build/examples/Release/`; on Linux it is under `build/examples/`.

## Build

Requires CMake 3.24+, vcpkg, and a C++23 compiler. Supported platforms are
Linux and Windows 10+; Windows requires MSVC with static dependencies/runtime.
Set `VCPKG_ROOT` to your vcpkg checkout. A standalone build selects its toolchain
automatically unless `CMAKE_TOOLCHAIN_FILE` is already set.

Linux:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRUVIA_BUILD_EXAMPLES=ON -DRUVIA_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Windows PowerShell:

```powershell
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows-static"
$env:VCPKG_DEFAULT_HOST_TRIPLET = "x64-windows-static"
function nproc { [Environment]::ProcessorCount }

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DRUVIA_BUILD_EXAMPLES=ON -DRUVIA_BUILD_TESTS=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
```

### Build options

| Option | Default | Purpose |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | Runtime library. |
| `RUVIA_BUILD_HTTP` | `ON` | Protocol library. |
| `RUVIA_BUILD_WEB` | `ON` | Web framework; requires core and HTTP. |
| `RUVIA_BUILD_TESTS` | `OFF` | Unit tests for selected components. |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | Web examples. |
| `RUVIA_ENABLE_MARIADB` | `OFF` | MariaDB driver. |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | PostgreSQL driver. |
| `RUVIA_ENABLE_REDIS` | `OFF` | Redis client and ORM. |
| `RUVIA_ENABLE_JWT` | `OFF` | JWT signing and verification. |

For `FetchContent` or `add_subdirectory`, configure dependencies in the parent
project: core needs Asio; HTTP needs zlib, Brotli, zstd, and ngtcp2 1.25+ with
default features disabled; Web also needs OpenSSL 3.5+. The parent controls its
toolchain, manifest features, triplets, and compiler settings.

## Install and Consume

```bash
cmake --install build --prefix build/install
```

Add `--config Release` for a Visual Studio build. Point `CMAKE_PREFIX_PATH` at
the install prefix and request the component you need:

```cmake
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE ruvia::web)
```

Use `COMPONENTS core` / `ruvia::core` or `COMPONENTS http` / `ruvia::http`
for a smaller dependency set. External dependencies must also be available to
the consumer. MSVC consumers must match `/MT` (`/MTd` in Debug) and
`_ITERATOR_DEBUG_LEVEL=0`; the latter is exported by Ruvia's targets.

## License

[MIT](LICENSE).
