# Ruvia

Ruvia is a C++20 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, websocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3/QUIC, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access. |

Requires CMake 3.28+ and a C++20 compiler. All third-party libraries are
downloaded from pinned, SHA-256-verified release archives by CMake FetchContent.
Windows builds use MSVC with static dependencies and runtime.
The three Ruvia libraries are static archives; Linux executables also link their
runtime libraries statically.
Result-returning APIs use C++20 `std::variant` value and error alternatives.
Coroutines use `ruvia::task<T>`. With GCC 13/14, keep over-aligned coroutine locals
in separately allocated, owned storage: the compiler can misalign objects stored
directly in coroutine frames.
Plain TCP response writes yield under socket backpressure instead of blocking
the connection's worker.
Web code targets the OpenSSL 4 API with deprecated interfaces disabled. Crypto
operations use provider-based EVP APIs; no older OpenSSL compatibility path is built.

Web builds also require Perl and Make (Jom on Windows) for OpenSSL.
PostgreSQL support additionally requires Python, Meson, Ninja, Bison, Flex, and pkg-config.
Run Windows configuration and builds in an MSVC developer shell. These are build
tools; no preinstalled third-party libraries or package-manager toolchain is used.

```sh
# Linux, POSIX shell; prerequisites listed above must be on PATH.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
```

Windows, MSVC developer PowerShell (the same prerequisites must be on PATH):

```powershell
cmake -S . -B build -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release "-j$([Environment]::ProcessorCount)"
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

Only dependencies of enabled components are fetched. Third-party headers and
libraries come exclusively from these builds; preinstalled third-party libraries
and package-manager toolchains are not dependency alternatives. MariaDB and
PostgreSQL use the fetched OpenSSL static libraries, not a separate OpenSSL
discovered on the host.
Sources and native build outputs stay under `build/_deps`; CMake's
`FETCHCONTENT_BASE_DIR` and `FETCHCONTENT_SOURCE_DIR_RUVIA_<NAME>` overrides support
shared caches and offline source trees. Installed packages use the same dependency
definitions when consumed with `find_package(ruvia REQUIRED COMPONENTS core http web)`;
consumers enable both C and C++ in their CMake project. MSVC consumers also select
the matching static runtime through `CMAKE_MSVC_RUNTIME_LIBRARY` before `project()`.
Updating a dependency means changing its release URL and SHA-256 together in
`cmake/ruvia_dependencies.cmake`. The aggregate `ruvia_dependencies` CMake target
builds all third-party dependencies ahead of the project targets.

Start with [basic_http.cpp](examples/web/basic_http.cpp). The
[Web examples](examples/web) contain usage, configuration, and lifetime notes
in their source comments; [examples/CMakeLists.txt](examples/CMakeLists.txt)
lists the build targets and optional feature flags.

Ordinary buffered `RUVIA_GET` routes also handle HEAD by default. An explicit
`RUVIA_HEAD` match takes precedence; otherwise the GET handler and middleware
receive the original HEAD request, and the response writer suppresses its body.
Streaming, SSE, and websocket endpoints require an explicit HEAD route.
Parameterized routes match with or without a single trailing slash, regardless
of whether the route declaration includes that slash.
Incremental HTTP content encoding accepts empty flushes without ending the stream;
subsequent writes and `finish()` remain valid.

CONNECT tunnels support independent send and receive half-closes: a peer FIN
ends reads without preventing further writes; `finish()` closes only the local
send direction. HTTP/3 tunnel input uses bounded backpressure while the receiver
consumes buffered data. See [tunnels.cpp](examples/web/tunnels.cpp).

Redirect `Location` values preserve existing `%HH` escapes and the first `#`
fragment separator; subsequent `#` bytes are encoded as `%23`.

File responses require a matching strong ETag to honor `If-Range`; a
`Last-Modified` date cannot authorize a partial file response.

Response cookie updates preserve partitioned and unpartitioned cookies as
distinct storage keys, even when their names and Domain/Path scopes match.

WebSocket `permessage-deflate` accepts the final DEFLATE blocks permitted by
[RFC 7692](https://www.rfc-editor.org/rfc/rfc7692.html#section-7.2.1), preserving
the required dictionary history. Truncated compressed messages close with
protocol error 1002.

QUIC close error codes are limited to `2^62 - 1`; an out-of-range first close
throws `quic_error` with `invalid_configuration` without changing the connection state.

Enable `RUVIA_BUILD_EXAMPLES=ON`, then build with
`cmake --build build --config Release --target ruvia_examples_web "-j$(nproc)"`.
In PowerShell, replace `-j$(nproc)` with `"-j$([Environment]::ProcessorCount)"`.
Enable `RUVIA_ENABLE_MARIADB`, `RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_REDIS`,
or `RUVIA_ENABLE_JWT` for the corresponding examples.

[MIT License](LICENSE).
