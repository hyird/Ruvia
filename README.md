# Ruvia

Ruvia is a C++23 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, WebSocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3/QUIC, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access. |

Requires CMake 3.24+, vcpkg (`VCPKG_ROOT`), and a C++23 compiler.
Windows builds use MSVC with static dependencies and runtime.

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
