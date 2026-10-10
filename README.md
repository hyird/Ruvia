# Ruvia

Ruvia is a C++20 HTTP/Web framework with coroutine handlers, typed models,
middleware, streaming, websocket, and optional SQL, Redis, and JWT support.

| CMake target | Purpose |
| --- | --- |
| `ruvia::core` | Tasks, event loops, bounded channels, and blocking work. |
| `ruvia::http` | Sans-I/O HTTP/1, HTTP/2, HTTP/3/QUIC, and WebSocket protocols. |
| `ruvia::web` | Servers, routing, TLS, outbound clients, and data access. |

Requires CMake 3.28+, a C++20 compiler, and vcpkg (`VCPKG_ROOT`). Third-party
libraries come from the vcpkg manifest [vcpkg.json](vcpkg.json) without a
version baseline, so they follow the ports of the vcpkg checkout in use; CI
always uses the latest vcpkg commit. A top-level configure selects
`$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` and installs only the manifest
features of enabled components and options.
Windows builds use MSVC with static dependencies and runtime; the default vcpkg
triplet there is `x64-windows-static`.
The three Ruvia libraries are static archives; Linux executables also link their
runtime libraries statically.
Result-returning APIs use C++20 `std::variant` value and error alternatives.
Coroutines use `ruvia::task<T>`. With GCC 13/14, keep over-aligned coroutine locals
in separately allocated, owned storage: the compiler can misalign objects stored
directly in coroutine frames.
Plain TCP response writes yield under socket backpressure instead of blocking
the connection's worker.
Web requires OpenSSL 3.5+ and builds with deprecated OpenSSL interfaces disabled.

```sh
# Linux, POSIX shell; VCPKG_ROOT points to a bootstrapped vcpkg checkout.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
```

Windows, PowerShell with MSVC installed and `$env:VCPKG_ROOT` set:

```powershell
cmake -S . -B build -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release "-j$([Environment]::ProcessorCount)"
ctest --test-dir build -C Release --output-on-failure
```

Installed packages are consumed with
`find_package(ruvia REQUIRED COMPONENTS core http web)`; the consumer's toolchain
must provide the same third-party packages (Asio, zlib, Brotli, Zstandard,
ngtcp2 1.25+, OpenSSL 3.5+, and the enabled MariaDB, libpq, or hiredis clients).
MSVC consumers also select the matching static runtime through
`CMAKE_MSVC_RUNTIME_LIBRARY` before `project()`.

Start with [basic_http.cpp](examples/web/basic_http.cpp). The
[Web examples](examples/web) contain usage, configuration, and lifetime notes
in their source comments; [examples/CMakeLists.txt](examples/CMakeLists.txt)
lists the build targets and optional feature flags.

Dynamic model values default to JSON `null` (`json_value`) and `{}` (`json_object`).
Model serialization preserves these defaults, including `null` array elements.
Fields that permit JSON null require `RUVIA_NULLABLE`; see
[model_values.cpp](examples/web/model_values.cpp).

Ordinary buffered `RUVIA_GET` routes also handle HEAD by default. An explicit
`RUVIA_HEAD` match takes precedence; otherwise the GET handler and middleware
receive the original HEAD request, and the response writer suppresses its body.
Streaming, SSE, and websocket endpoints require an explicit HEAD route.
Parameterized routes match with or without a single trailing slash, regardless
of whether the route declaration includes that slash.
Interior repeated slashes remain distinct path segments; parameters cannot match
an empty segment. `url_for` preserves these segments and declared trailing
slashes so generated URLs match their declared routes.
Incremental HTTP content encoding accepts empty flushes without ending the stream;
subsequent writes and `finish()` remain valid.

`Content-Type` validation, multipart boundary extraction, and `Accept` media matching allow empty
semicolon-delimited parameter slots as specified by
[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.6).
Nonempty parameters still require `name=value` without whitespace around `=`.
Multipart parsing applies the same preamble limit regardless of input chunk
boundaries; see [streaming.cpp](examples/web/streaming.cpp).

For the sans-I/O `http2_connection`, drain `next_event()` before feeding more input.
Request metadata remains valid until its request lease is released, including
after a peer reset.

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
