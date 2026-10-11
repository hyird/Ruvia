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
Closing or destroying a `one_shot_receiver` discards an unconsumed result and
closes pending and future waits.
`blocking_pool::submit()` can return `queue_full` while `stats().queued_` is
below `queue_capacity()`: concurrent submissions also occupy bounded capacity
until their input ownership has been transferred.
Plain TCP response writes yield under socket backpressure instead of blocking
the connection's worker.
Web requires OpenSSL 3.5+ and builds with deprecated OpenSSL interfaces disabled.
TLS certificates, private keys and client CA bundles are read once when the
server configuration is validated; every worker installs those parsed credentials.

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

`ruvia/web/controller.h` brings in `context` and request validation, but only
forward-declares the capabilities a handler reaches through it: include
`streaming.h` (stream/SSE writers, `body_reader`), `multipart_reader.h`,
`websocket.h`, `http_tunnel.h`, `session.h` or `http_client_handle.h` where
those are used.

Dynamic model values default to JSON `null` (`json_value`) and `{}` (`json_object`).
Model serialization preserves these defaults, including `null` array elements.
Fields that permit JSON null require `RUVIA_NULLABLE`; see
[model_values.cpp](examples/web/model_values.cpp).

Model integer wrappers accept integer inputs only; out-of-range construction or
assignment throws `std::out_of_range`, preserving an existing field on failed
assignment. JSON body binding reports malformed syntax as a body error and
well-formed values of the wrong field type as validation errors, regardless of whitespace.
`validator::range` and model `RUVIA_MIN/MAX` compare numeric bounds without
rounding integer limits to floating point or changing integer signedness.

Ordinary buffered `RUVIA_GET` routes also handle HEAD by default. An explicit
`RUVIA_HEAD` match takes precedence; otherwise the GET handler and middleware
receive the original HEAD request, and the response writer suppresses its body.
Streaming, SSE, and websocket endpoints require an explicit HEAD route.
Parameterized routes match with or without a single trailing slash, regardless
of whether the route declaration includes that slash.
Interior repeated slashes remain distinct path segments; parameters cannot match
an empty segment. `url_for` preserves these segments and declared trailing
slashes so generated URLs match their declared routes.
Exact static routes win over dynamic ones; dynamic routes try a literal segment
before a `:param` and a `:param` before a final `*`, backtracking when a branch
fails. Overlapping patterns such as `/a/*` and `/:x/:y` therefore coexist; startup
fails only when two routes of one method differ just in parameter names or a
trailing slash (`/u/:id` and `/u/:name/`).
`use_at` and prefix-scoped `on_error`/`on_not_found` match request paths on whole,
percent-decoded segments: with `use_at<auth>({.prefix_ = "/admin"})`, a route
`/:section/panel` runs `auth` for `/admin/panel` and `/%61dmin/panel` but not for
`/other/panel`.

Incremental HTTP content encoding accepts input borrowed from its output string
and empty flushes without ending the stream; subsequent writes and `finish()`
remain valid. Decoding treats an empty
content- or transfer-coded body (for example `Content-Encoding: gzip` with no
bytes) as empty content.
HTTP/3 frame and QPACK string encoders accept overlapping input and output spans.
Static QPACK field sections accept valid nonzero Base values when they contain
no dynamic-table references.
HPACK field encoders also accept names and values borrowed from their output;
indexed references must be nonzero and are rejected before writing otherwise.
WebSocket `feed()` accepts bytes borrowed from the previous event's payload,
including when consumed input storage must be reclaimed.
HTTP/2 `submit_data()` also accepts bytes borrowed from `pending_output()`.
`submit_buffered_response()` accepts the entire body on success, including any
flow-controlled remainder. An allocation failure leaves the response unsubmitted
so the same response can be retried.
HTTP/1 request trailers may borrow the buffer passed to `plan_finish()`.

HTTP/3 applies the shared TRACE and OPTIONS request-content rules when encoding
and receiving messages. For ordinary requests, its buffered request owner rejects
content beyond the declared Content-Length and requires the full length before
`finish_body()`. CONNECT tunnel bytes use separate events.
HTTP/3 response field count and size limits apply independently to initial
headers, trailers, and promised request headers, including when using a shared
QPACK decoder. Exceeding these limits reports `excessive_load`.

`Content-Type` validation, multipart boundary extraction, and `Accept` media matching allow empty
semicolon-delimited parameter slots as specified by
[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.6).
Nonempty parameters still require `name=value` without whitespace around `=`.
Multipart parsing applies the same preamble limit regardless of input chunk
boundaries and accepts MIME body-part header folding. Content-Type values from
unfolded header blocks are owned by the result; part bodies still borrow the
input. Streaming `multipart_parser::feed()` can also copy a body chunk borrowed
from its latest part. See [streaming.cpp](examples/web/streaming.cpp).

HTTP/1 client responses unfold obsolete folded field lines before validation and
framing, as required by [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html#section-5.2).
HEAD and 304 responses end at the header section even when they declare `Trailer`
fields; those declarations do not require a chunked body.
HTTP/1 receivers ignore empty comma-separated members in `TE`; senders must not
generate them. Request trailers are limited to 64 fields and 65536 bytes in both
their received representation and normalized form (`name.size() + value.size() + 4`
per field), checked during chunk decoding.

For the sans-I/O `http2_connection`, drain `next_event()` before feeding more input.
Request metadata remains valid until its request lease is released, including
after a peer reset. A fatal local connection error rejects further stream output;
request and receive-credit leases can still be released without emitting frames.
Graceful GOAWAY draining lets accepted streams finish.

CONNECT tunnels support independent send and receive half-closes: a peer FIN
ends reads without preventing further writes; `finish()` closes only the local
send direction. HTTP/3 tunnel input uses bounded backpressure while the receiver
consumes buffered data. See [tunnels.cpp](examples/web/tunnels.cpp).

Redirect `Location` values preserve existing `%HH` escapes and the first `#`
fragment separator; subsequent `#` bytes are encoded as `%23`.

File responses require a matching strong ETag to honor `If-Range`; a
`Last-Modified` date cannot authorize a partial file response.
Plain-TCP HTTP/1.1 hands file bodies to the kernel (`sendfile`/`TransmitFile`).
HTTP/2, HTTP/3 and HTTP/1.1 over TLS read them on the server's blocking pool;
with the pool disabled (`get_blocking_pool(nullptr)`) the same reads run on the
worker, as response compression does. A file that changes while it is sent
aborts the response instead of completing it.

Context body builders (`body`, `text`, `html`) copy strings and character
buffers into the response; a character array ends at its first NUL byte. Wrap
static bytes in `ruvia::static_text` to send them without copying; it accepts
only constants with static storage, as in [basic_http.cpp](examples/web/basic_http.cpp).

`context::dispatch` subrequests inherit the parent's HTTP/3 early-data
provenance. Under 0-RTT they receive `425 Too Early` unless they are content-free
GET/HEAD requests to a replay-safe route
([RFC 8470](https://www.rfc-editor.org/rfc/rfc8470.html#section-5.1)).

Response cookie updates preserve partitioned and unpartitioned cookies as
distinct storage keys, even when their names and Domain/Path scopes match.

WebSocket `permessage-deflate` accepts the final DEFLATE blocks permitted by
[RFC 7692](https://www.rfc-editor.org/rfc/rfc7692.html#section-7.2.1), preserving
the required dictionary history. Truncated compressed messages close with
protocol error 1002.
Failed outbound submissions preserve queued bytes and discard unsent compression
history so a retry does not depend on a message the peer never received.

QUIC close error codes are limited to `2^62 - 1`; an out-of-range first close
throws `quic_error` with `invalid_configuration` without changing the connection state.
A storage allocation failure while preparing the first close packet leaves
`write_packet()` retryable.
After its CONNECTION_CLOSE is written, or a peer's is received, a `quic_connection`
keeps the [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000.html#section-10.2)
closing or draining period: `next_expiry()` reports its end, when `handle_expiry()`
retires the connection. The HTTP/3 client sends CONNECTION_CLOSE whenever it ends a
connection, including on shutdown.

Enable `RUVIA_BUILD_EXAMPLES=ON`, then build with
`cmake --build build --config Release --target ruvia_examples_web "-j$(nproc)"`.
In PowerShell, replace `-j$(nproc)` with `"-j$([Environment]::ProcessorCount)"`.
Enable `RUVIA_ENABLE_MARIADB`, `RUVIA_ENABLE_POSTGRESQL`, `RUVIA_ENABLE_REDIS`,
or `RUVIA_ENABLE_JWT` for the corresponding examples.

[MIT License](LICENSE).
