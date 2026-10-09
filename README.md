# Ruvia

Ruvia is a C++20 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, WebSocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3/QUIC, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access. |

Requires CMake 3.28+ and a C++20 compiler. All third-party libraries are
downloaded from pinned, SHA-256-verified release archives by CMake FetchContent.
Windows builds use MSVC with static dependencies and runtime.
Result-returning APIs use C++20 `std::variant` value and error alternatives.
Web code targets the OpenSSL 4 API with deprecated interfaces disabled. Crypto
operations use provider-based EVP APIs; no older OpenSSL compatibility path is built.

Web builds also require Perl and Make (Jom on Windows) for OpenSSL.
PostgreSQL support additionally requires Python, Meson, Ninja, Bison, and Flex.
Run Windows configuration and builds in an MSVC developer shell. These are build
tools; no preinstalled third-party libraries or package-manager toolchain is used.

```sh
cmake -S . -B build -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
```

| Dependency | Pinned stable release (2026-10-09) |
| --- | --- |
| Asio | 1.38.2 |
| ngtcp2 (protocol core only) | 1.25.0 |
| zlib | 1.3.2 |
| Brotli | 1.2.0 |
| Zstandard | 1.5.7 |
| OpenSSL | 4.0.3 |
| MariaDB Connector/C (optional) | 3.4.11 |
| PostgreSQL libpq (optional) | 18.6 |
| hiredis (optional) | 1.4.1 |

Only dependencies of enabled components are fetched. Sources and native build
outputs stay under `build/_deps`; CMake's `FETCHCONTENT_BASE_DIR` and
`FETCHCONTENT_SOURCE_DIR_RUVIA_<NAME>` overrides support shared caches and offline
source trees. Installed packages use the same dependency definitions when
consumed with `find_package(ruvia REQUIRED COMPONENTS core http web)`; consumers
enable both C and C++ in their CMake project. Updating a dependency means changing
its release URL and SHA-256 together in `cmake/RuviaDependencies.cmake`.

Start with [basic_http.cpp](examples/web/basic_http.cpp). The
[Web examples](examples/web) contain usage, configuration, and lifetime notes
in their source comments; [examples/CMakeLists.txt](examples/CMakeLists.txt)
lists the build targets and optional feature flags.

Ordinary buffered `RUVIA_GET` routes also handle HEAD by default. An explicit
`RUVIA_HEAD` match takes precedence; otherwise the GET handler and middleware
receive the original HEAD request, and the response writer suppresses its body.
Streaming, SSE, and WebSocket endpoints require an explicit HEAD route.

Enable `RUVIA_BUILD_EXAMPLES=ON`, then build with
`cmake --build build --config Release --target ruvia_examples_web "-j$(nproc)"`.
In PowerShell, define `function nproc { [Environment]::ProcessorCount }` first.
Enable `RUVIA_ENABLE_MARIADB`, `RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_REDIS`,
or `RUVIA_ENABLE_JWT` for the corresponding examples.

[MIT License](LICENSE).
