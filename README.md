# Ruvia

Ruvia is a C++20 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, WebSocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Independent sans-I/O HTTP/1.0, HTTP/1.1, HTTP/2, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access; uses Core and HTTP. |

## Build

Requires CMake 3.28+, a C++20 compiler, and a bootstrapped vcpkg checkout
(`VCPKG_ROOT`). Windows builds require MSVC and use static dependencies and runtime
with the `x64-windows-static` triplet. Linux executables require glibc.

Dependencies are selected from [vcpkg.json](vcpkg.json): Asio for Core; zlib,
Brotli, and Zstandard for HTTP; OpenSSL 3.0+ for Web. A top-level build selects
the vcpkg toolchain from `VCPKG_ROOT`; an embedding project supplies its own
toolchain and manifest.

The libraries default to enabled. `RUVIA_BUILD_WEB` requires Core and HTTP.
`RUVIA_BUILD_TESTS` and `RUVIA_BUILD_EXAMPLES` default to `OFF`; examples require
Web. `RUVIA_ENABLE_MARIADB`, `RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_REDIS`, and
`RUVIA_ENABLE_JWT` also default to `OFF`. Database and Redis options add their
respective client dependencies and public APIs.

## Quick start

Linux, POSIX shell, with `VCPKG_ROOT` set:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
./build/examples/ruvia_example_basic_http
```

Windows, PowerShell, with MSVC installed and `$env:VCPKG_ROOT` set:

```powershell
cmake -S . -B build -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release "-j$([Environment]::ProcessorCount)"
ctest --test-dir build -C Release --output-on-failure
.\build\examples\Release\ruvia_example_basic_http.exe
```

The example serves `http://127.0.0.1:8080/api/hello`; stop it with Ctrl+C.

## Use in your project

An installed package is consumed with
`find_package(ruvia REQUIRED COMPONENTS core http web)` and the corresponding
`ruvia::` targets. Its toolchain must provide the same dependencies. MSVC consumers
select the matching static `CMAKE_MSVC_RUNTIME_LIBRARY` before `project()`.

- Configure the process-wide `ruvia::app()` before calling `run()`.
- Handlers return `ruvia::task<T>`. Tasks are lazy; once started, they must be
  awaited or joined, including after cancellation.
- Request views borrow their owner. Copy data before carrying it beyond the
  request lifetime or into offloaded work.
- The server and outbound client support HTTP/1 and HTTP/2 over TCP and TLS.
  TLS negotiates HTTP/2 through ALPN; cleartext HTTP/2 uses prior knowledge,
  not the HTTP/1 `Upgrade: h2c` mechanism. WebSocket uses HTTP/1.1 Upgrade or
  HTTP/2 Extended CONNECT.
- Server workers bind their own listeners. Linux and other supported POSIX
  systems require `SO_REUSEPORT`; startup fails if it is unavailable. Windows
  workers bind with `SO_REUSEADDR` and accept through all shared listener handles
  using native `AcceptEx`; connections stay with their initiating worker.
  Windows does not guarantee strict balancing or exclusive use of the port.

## Examples

Start with [basic_http.cpp](examples/web/basic_http.cpp). Further examples cover
[models and validation](examples/web/models_validation.cpp),
[tunnels](examples/web/tunnels.cpp), and [ORM with PostgreSQL](examples/web/orm.cpp).
For HTTP/2, run [http_features.cpp](examples/web/http_features.cpp), then
[http_client_advanced.cpp](examples/web/http_client_advanced.cpp) or
[websocket_client.cpp](examples/web/websocket_client.cpp) with argument `2`.
Browse [examples/web](examples/web) for the remaining features;
[examples/CMakeLists.txt](examples/CMakeLists.txt) lists their build targets.

SQL connections are configured by `db_config::pool_size_per_worker_` (default 1)
for each database alias on each worker, or for one standalone `db_client`.
See [database.cpp](examples/web/database.cpp) for configuration and usage;
database migrations use a dedicated connection.

Redis uses `redis_config::pool_size_per_worker_` ordinary connections plus a
lazily connected blocking pool per alias and worker. Pub/Sub does not share
them: every `redis_handle::subscription()` opens one dedicated connection that
lives until it is aborted, cancelled or shut down. See
[redis_pubsub.cpp](examples/web/redis_pubsub.cpp).

## License

[MIT License](LICENSE).
