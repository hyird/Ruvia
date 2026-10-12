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
Linux executables link libstdc++ and libgcc statically, but use shared glibc and
its dynamic loader; deployment requires a compatible system glibc.

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
cancellation. Stream and WebSocket operations occupy their read/write lanes when
started; unstarted operations may coexist and be awaited in sequence. These write
entry points own their input before returning.
A task scope accepts children until its join task starts; joining is single-shot
and prevents further spawning.
Offloaded work carries owned data and returns through a bounded
blocking pool. With GCC 13/14, keep over-aligned coroutine locals in separately
allocated owned storage because those compilers can misalign coroutine frames.

`ruvia::worker_group` stops every member when one fails and wakes pending startup
or serving waits with the original exception. Call `join()` explicitly to retire
its borrowed workers in tier order.

`ruvia::event_loop_pool` owns its worker threads; `ruvia::attach_event_loop`
binds one worker to a caller-owned `asio::io_context`. Both retire through the
same stop sequence: `stop()` closes admission immediately, and stop callbacks,
root tasks, cleanup, and timer cancellation then finish on the loop's thread.
An attachment holds work on its context until it has retired, so keep running
the context after `stop()` until `run()` returns; destroying the context while
Ruvia work is still outstanding is a contract violation that terminates.
Closing a channel sender permits buffered input to drain; closing or destroying
its receiver destroys the buffered input on that thread and prevents later
receives from consuming any.
`ruvia::stop_token` values are copyable views over at most four distinct stop
sources; combining more, for example by merging `ruvia::operation_options`,
throws `std::length_error`. A `ruvia::stop_source` keeps its shared stop state
in Ruvia's process memory pool rather than the global heap.

Each worker's `ruvia::worker_memory` pools objects up to 16 KiB for the
worker's life and keeps freed blocks between 16 KiB and 1 MiB (sizes rounded
to 4 KiB) for exact-size reuse, up to `memory_pool_config::retained_block_bytes_`
(8 MiB by default; 0 disables the cache). Blocks freed beyond that budget go
back to the C allocator at once, and `worker_memory::trim(owner)` returns every
cached block and the thread's cached coroutine frames; it must run on the
owning worker and throws `std::logic_error` elsewhere. Live allocations are
never released. Whether returned memory reaches the OS depends on the C
allocator: on glibc hosts that care about resident memory after load peaks,
set `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=65536` so returned large
blocks are unmapped; calling `malloc_trim(0)` from the host, or linking
jemalloc or mimalloc, are other host-side options. An application configures
its workers' memory through `server_config::memory_pool_`; each server worker
trims its own memory at the first connection scan
(`server_config::connection_scan_interval_`) that follows a whole interval with
no request exchange in progress and no connection activity.

The server and outbound client support HTTP/1 over TCP and TLS. Server TLS
certificates, private keys, and client CA bundles are loaded once by
`application::listen()`, which throws for unreadable files or a wrong key
password; paths are opened as Unicode on every platform, and the server keeps
no copy of a private-key password.
Outbound origins and connection pools are configured before serving.
Clients configured to retain response cookies save them when the final response
headers arrive, even while the body is still being received.
WebSocket uses HTTP/1.1 Upgrade. CONNECT tunnels support independent send and receive
half-closes; CONNECT-UDP uses reliable DATAGRAM capsules. See
[tunnels.cpp](examples/web/tunnels.cpp).

`application::stop()` (also the installed SIGINT/SIGTERM handlers) stops a
serving app gracefully. The listeners close; connections with no admitted
request (idle, or still receiving a request head) close at once; each
WebSocket session starts its close handshake with 1001 Going Away, bounded by
the route's `close_handshake_timeout_`; requests whose head arrived finish
and then close their connection (`Connection: close`), and tunnels keep
running. An upgrade still in its route chain completes its handshake and then
ends without running its session handler: a WebSocket with the 1001 close
handshake, a tunnel by closing. Once they have all finished, or when
`server_config::drain_timeout_` (default 10 s; zero disables the drain) passes,
every remaining connection closes, handler stop tokens fire, and `on_stop`
hooks run before the workers retire. Streaming responses and tunnels that end
only on their stop token hold the drain until the deadline. A repeated signal
during the drain gets the default disposition. A stop during startup, or a
worker failure, closes everything at once. `run()` rethrows the primary
failure (the first preparation, startup, or worker failure) without reporting
it, so `main()` must catch it, log it (`ruvia::exception_text` gives UTF-8
text), and exit non-zero; the library reports to stderr only the failures it
supersedes.

Controllers and middleware are worker-local. Request views borrow their owner;
copy data before carrying it beyond that lifetime or into offloaded work.
JSON parsing applies cumulative array-element and storage limits through
`model_parse_options`, including arrays in unknown fields, decoded field names,
and borrowed `json_value`/`json_object` parses.
Model moves into a destination owned by the source, such as a root into one of
its children, throw `std::invalid_argument` and leave both sides unchanged.
`header_model` matches names ASCII case-insensitively, so its schema must not
declare fields whose names differ only by ASCII case.
JWT payload assignment retains the destination PMR resource, including nested
audiences and custom claims.
Validator copies retain the source PMR resource, which must outlive them;
assignment retains the destination resource. Thrown validation errors own their
diagnostics independently.
`RUVIA_PATTERN` and `RUVIA_REGEX` use the same bounded, anchored byte-pattern
dialect; use `RUVIA_CUSTOM` for matching outside its supported syntax. See
[models_validation.cpp](examples/web/models_validation.cpp) for model rules.
ORM array columns retain PostgreSQL one-dimensional element order, including
nondefault lower bounds, without keeping the subscripts; multidimensional arrays
are rejected. See [orm.cpp](examples/web/orm.cpp).
`ruvia/web/controller.h` includes context and request validation; include
`streaming.h`, `multipart_reader.h`, `websocket.h`, `http_tunnel.h`, `session.h`,
or `http_client_handle.h` where their capabilities are used.

Ordinary buffered GET routes also handle HEAD unless an explicit HEAD route
matches. Streaming, SSE, and WebSocket routes require an explicit HEAD route.
Context body builders copy strings; `ruvia::static_text` explicitly borrows
constant static storage. Moving an `http_response` transfers its headers and body;
the source keeps its PMR resource and remains reusable with empty headers and body.
Moving an `http_client_response_head` leaves source metadata readable and fields
empty; repeated `take_headers()` calls return empty vectors in its original PMR.
HTTP/1 client request preparation permits metadata to borrow its output buffer;
content must remain outside the emitted head.
File responses use native file transfer for plain TCP,
and the blocking pool for TLS reads; disabling the pool makes those reads run
on the worker. A file changed during transfer aborts the response. A
`context::file` body up to 16 KiB is read when the response is framed and sent
as those framed bytes. `file_response_options::path_` borrows its path for the
call: UTF-8 text, or a `std::filesystem::path` lvalue, never an owning
temporary. A standalone `static_root` fails closed on changed files
unless `static_root_options::resident_max_bytes_` and `resident_budget_bytes_`
opt in to serving small files' indexed bytes from memory; document roots use
`document_root_config::resident_max_bytes_` instead.

[MIT License](LICENSE).
