# Ruvia

Ruvia is a C++20 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, WebSocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Independent sans-I/O HTTP/1.0, HTTP/1.1, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access; uses Core and HTTP. |

Requires CMake 3.28+, a C++20 compiler, and a bootstrapped vcpkg checkout
(`VCPKG_ROOT`). Windows builds require MSVC and use static dependencies and runtime
with the `x64-windows-static` triplet. The three libraries are static archives;
Linux executables also link their runtime libraries statically.

Dependencies are selected from [vcpkg.json](vcpkg.json): Asio for Core; zlib,
Brotli, and Zstandard for HTTP; OpenSSL 3.0+ for Web. The manifest has no version
baseline, so package versions follow the vcpkg checkout. A top-level configure
uses `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` and installs the features
required by enabled components. An embedding project supplies its own toolchain
and manifest.

The libraries default to enabled. `RUVIA_BUILD_WEB` requires Core and HTTP.
`RUVIA_BUILD_TESTS` and `RUVIA_BUILD_EXAMPLES` default to `OFF`; examples require
Web. `RUVIA_ENABLE_MARIADB`, `RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_REDIS`, and
`RUVIA_ENABLE_JWT` also default to `OFF`. Database and Redis options add their
respective client dependencies and public APIs.

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
See [basic_http.cpp](examples/web/basic_http.cpp) for controller registration,
routes, middleware, and the process-wide `ruvia::app()` lifecycle.
[Web examples](examples/web) cover configuration and feature usage;
[examples/CMakeLists.txt](examples/CMakeLists.txt) lists their targets and options.

An installed package is consumed with
`find_package(ruvia REQUIRED COMPONENTS core http web)` and the corresponding
`ruvia::` targets. Its toolchain must provide the same dependencies. MSVC consumers
select the matching static `CMAKE_MSVC_RUNTIME_LIBRARY` before `project()`.

Coroutines return `ruvia::task<T>`. Tasks are lazy: an unstarted task may be
discarded, but a started task must be awaited or joined, including after
cancellation. Offloaded work carries owned data and returns through a bounded
blocking pool. With GCC 13/14, keep over-aligned coroutine locals in separately
allocated owned storage because those compilers can misalign coroutine frames.

`ruvia::event_loop_pool` owns its worker threads; `ruvia::attach_event_loop`
binds one worker to a caller-owned `asio::io_context`. Both retire through the
same stop sequence: `stop()` closes admission immediately, and stop callbacks,
root tasks, cleanup, and timer cancellation then finish on the loop's thread.
An attachment holds work on its context until it has retired, so keep running
the context after `stop()` until `run()` returns; destroying the context while
Ruvia work is still outstanding is a contract violation that terminates.
`ruvia::stop_token` values are copyable views over at most four distinct stop
sources; combining more, for example by merging `ruvia::operation_options`,
throws `std::length_error`.

The server and outbound client support HTTP/1 over TCP and TLS. TLS certificates,
private keys, and client CA bundles are loaded during configuration validation.
Outbound origins and connection pools are configured before serving. WebSocket
uses HTTP/1.1 Upgrade. CONNECT tunnels support independent send and receive
half-closes; CONNECT-UDP uses reliable DATAGRAM capsules. See
[tunnels.cpp](examples/web/tunnels.cpp).

Controllers and middleware are worker-local. Request views borrow their owner;
copy data before carrying it beyond that lifetime or into offloaded work.
`ruvia/web/controller.h` includes context and request validation; include
`streaming.h`, `multipart_reader.h`, `websocket.h`, `http_tunnel.h`, `session.h`,
or `http_client_handle.h` where their capabilities are used.

Ordinary buffered GET routes also handle HEAD unless an explicit HEAD route
matches. Streaming, SSE, and WebSocket routes require an explicit HEAD route.
Context body builders copy strings; `ruvia::static_text` explicitly borrows
constant static storage. File responses use native file transfer for plain TCP,
and the blocking pool for TLS reads; disabling the pool makes those reads run
on the worker. A file changed during transfer aborts the response.

[MIT License](LICENSE).
