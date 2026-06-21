# Design: HTTP Client, HTTP/HTTPS Listening, App Lifecycle Hooks

Date: 2026-06-19

## Scope

Three new features for Ruvia v0.0.7:

1. **HTTP client** - outbound HTTP/HTTPS requests with per-worker connection pools, accessed via `c.fetch(...)` inside handlers.
2. **HTTP/HTTPS listening** - `App` has explicit HTTP and HTTPS listener ports instead of an arbitrary listener list.
3. **App lifecycle hooks** - synchronous `onStart` / `onStop` callbacks that fire on the main thread in `App::run()`.

---

## 1. HTTP Client

### Build gating

Strictly optional. Guarded by `RUVIA_ENABLE_HTTP_CLIENT=ON` (CMake) and vcpkg feature `http-client`. No new external dependencies; it uses ASIO TCP and OpenSSL already in the dependency tree.

When disabled, `include/ruvia/http/HttpClient.h`, `App::useHttpClient(...)`, and `Context::fetch(...)` are not installed or exposed.

### Public API

**`include/ruvia/http/HttpClient.h`**

```cpp
struct HttpClientConfig {
    std::pmr::string host;
    std::uint16_t port{80};
    bool tls{false};
    std::size_t poolSizePerWorker{4};
    std::chrono::milliseconds connectTimeout{0};  // 0 = disabled
    std::chrono::milliseconds requestTimeout{0};  // 0 = disabled
    std::chrono::milliseconds acquireTimeout{0};  // 0 = disabled
};

struct FetchRequestHeader {
    std::string_view name;   // borrowed; must remain valid through co_await
    std::string_view value;  // borrowed; must remain valid through co_await
};

struct FetchResponseHeader {
    std::pmr::string name;   // arena-allocated
    std::pmr::string value;  // arena-allocated
};

struct FetchOptions {
    std::string_view method{"GET"};
    std::initializer_list<FetchRequestHeader> headers{};
    std::string_view body{};  // borrowed; must remain valid through co_await
    std::chrono::milliseconds timeout{0};  // overrides config.requestTimeout when non-zero
};

struct FetchResponse {
    int statusCode{0};
    std::pmr::vector<FetchResponseHeader> headers;  // arena-allocated
    std::pmr::string body;                          // arena-allocated
};
```

**`include/ruvia/app/App.h`** additions under `#ifdef RUVIA_ENABLE_HTTP_CLIENT`:

```cpp
App& useHttpClient(HttpClientConfig config);
App& useHttpClient(std::string_view alias, HttpClientConfig config);
```

**`include/ruvia/http/Context.h`** additions under `#ifdef RUVIA_ENABLE_HTTP_CLIENT`:

```cpp
Task<FetchResponse> fetch(std::string_view path, FetchOptions options = {});
Task<FetchResponse> fetch(std::string_view alias, std::string_view path, FetchOptions options = {});
```

### Internal structure

Mirrors the DB/Redis pattern:

| File | Purpose |
|---|---|
| `src/http/client/HttpClientInternal.h` | `HttpClientDefinition`, `HttpClientRegistry` declarations |
| `src/http/client/HttpClientPool.cpp` | Per-worker pool: connect, acquire slot, send request, release slot |
| `src/http/client/HttpClientPool.h` | Pool class header |

`HttpServer` constructor gains a `std::span<const HttpClientDefinition> clients` parameter, following the same pattern as `databases` and `redis`. Each worker owns its own `HttpClientRegistry` containing one pool per alias.

### Connection pool behaviour

- Pool holds `poolSizePerWorker` persistent TCP connections to the configured `host:port`.
- Connections are established when the worker starts, same timing as DB/Redis `connect()`.
- Acquire blocks up to `acquireTimeout` if all slots are in use; then it times out with an exception.
- TLS uses `asio::ssl::context::tls_client` with OpenSSL.
- `FetchResponse::headers` and `FetchResponse::body` are allocated in the request arena (`c.resource()`).
- Dropped or errored connections are re-established on next acquire.

### Memory

`FetchResponse` is allocated into the request arena via `c.resource()`. `FetchOptions::body` is a borrowed view; the caller must keep it alive through the `co_await`.

---

## 2. HTTP/HTTPS Listening

### Public API

Ruvia exposes two listener roles instead of a general multi-listener registry:

```cpp
App& setListenAddress(std::string_view address);
App& setListenAddress(std::string_view address, std::uint16_t httpPort);
App& setHttpListenPort(std::uint16_t port);
App& setHttpsListenPort(std::uint16_t port);
App& setAutoHttps(bool enabled = true);
```

`setListenAddress(addr, port)` is the HTTP listener convenience entry. Calling `setHttpListenPort(...)` or `setHttpsListenPort(...)` declares that listener. `useTls(TlsConfig)` only configures the HTTPS certificate and key. `setAutoHttps(true)` makes the HTTP listener return a `308 Permanent Redirect` to the configured HTTPS port and close the connection.

### Internal changes

**`App` private state:**

```cpp
std::pmr::string listenAddress_;
std::optional<std::uint16_t> httpListenPort_{8080};
std::optional<std::uint16_t> httpsListenPort_;
bool autoHttps_{false};
```

**`App::run()` worker creation:**

```text
total workers =
    (httpListenPort_.has_value() ? threadNum_ : 0) +
    (httpsListenPort_.has_value() ? threadNum_ : 0)
```

Each enabled listener still creates `threadNum_` `HttpServer` instances with one acceptor per worker. Ruvia does not use a single acceptor that distributes sockets to workers.

**TLS and auto HTTPS:**

HTTP workers force `HttpServerOptions::tls.enabled = false`. HTTPS workers use the app-level TLS config. HTTP workers receive `HttpServerOptions::autoHttps` only when `setAutoHttps(true)` is enabled.

Startup rejects:

- HTTPS enabled without TLS config.
- Auto HTTPS without both HTTP and HTTPS listeners.
- Auto HTTPS when the HTTPS port is `0`.
- HTTP and HTTPS on the same non-zero port.

### Usage

Plain HTTP:

```cpp
ruvia::app()
    .setListenAddress("0.0.0.0", 8080)
    .run();
```

HTTP plus HTTPS with automatic redirect:

```cpp
ruvia::app()
    .setListenAddress("0.0.0.0")
    .setHttpListenPort(80)
    .setHttpsListenPort(443)
    .useTls(tlsConfig)
    .setAutoHttps(true)
    .setThreadNum(2)
    .run();
// 4 workers total: 2 on :80, 2 on :443
```

---

## 3. App Lifecycle Hooks

### Public API

```cpp
using AppHook = std::function<void()>;

App& onStart(AppHook hook);
App& onStop(AppHook hook);
```

Multiple calls accumulate hooks; they fire in registration order.

### Execution timing inside `App::run()`, main thread

```text
configure routes
start all workers (worker->start() for each)
onStart hooks fire here; all workers are accepting
block until shutdown signal or stop()
onStop hooks fire before workers are stopped
stop all workers
join all workers
```

- Hooks run synchronously on the main thread, the thread that called `run()`.
- An exception thrown by any hook propagates out of `run()` immediately, aborting startup or triggering cleanup.
- Hooks registered after `run()` starts throw `std::logic_error`, following the same guard pattern as other `App` setters.

### Storage

```cpp
std::pmr::vector<AppHook> onStartHooks_{ProcessMemory::instance().upstreamResource()};
std::pmr::vector<AppHook> onStopHooks_{ProcessMemory::instance().upstreamResource()};
```

### Example usage

```cpp
ruvia::app()
    .onStart([] {
        spdlog::info("server ready on :8080");
    })
    .onStop([] {
        spdlog::info("shutting down");
    })
    .run();
```

---

## File changes summary

### New files

| Path | Purpose |
|---|---|
| `include/ruvia/http/HttpClient.h` | Public HTTP client types and config |
| `src/http/client/HttpClientInternal.h` | Internal pool definitions |
| `src/http/client/HttpClientPool.h` | Per-worker pool class |
| `src/http/client/HttpClientPool.cpp` | Pool implementation |

### Modified files

| Path | Change |
|---|---|
| `include/ruvia/app/App.h` | Add HTTP/HTTPS listener config, `AppHook`, `onStart`, `onStop`, `useHttpClient` |
| `include/ruvia/http/Context.h` | Add `fetch(...)` under feature guard |
| `src/app/App.cpp` | Configure HTTP/HTTPS listeners, add hook storage and invocation, add `useHttpClient` |
| `src/net/server/HttpServer.h` | Add `HttpClientRegistry` member; extend constructor |
| `src/net/server/HttpServerLifecycle.cpp` | Wire `HttpClientRegistry` into worker startup/shutdown |
| `CMakeLists.txt` | Add `RUVIA_ENABLE_HTTP_CLIENT` option and `http-client` vcpkg feature |
| `vcpkg.json` | Add `http-client` feature with no new dependencies |

---

## Out of scope for this spec

- HTTP/2 client
- Client-side connection pooling across workers; each worker owns its own pool
- Per-listener thread count; HTTP and HTTPS share `threadNum_`
- Arbitrary per-listener TLS configs
- Async lifecycle hooks (`Task<void>`)
- Client request streaming or response streaming
