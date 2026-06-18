# Design: HTTP Client, Multi-Port Listening, App Lifecycle Hooks

Date: 2026-06-19

## Scope

Three new features for Ruvia v0.0.7:

1. **HTTP client** — outbound HTTP/HTTPS requests with per-worker connection pools, accessed via `c.fetch(...)` inside handlers.
2. **Multi-port listening** — `App` can listen on multiple address:port pairs simultaneously, each with optional per-listener TLS.
3. **App lifecycle hooks** — synchronous `onStart` / `onStop` callbacks that fire on the main thread in `App::run()`.

---

## 1. HTTP Client

### Build gating

Strictly optional. Guarded by `RUVIA_ENABLE_HTTP_CLIENT=ON` (CMake) and vcpkg feature `http-client`. No new external dependencies — uses ASIO TCP + OpenSSL already in the dependency tree.

When disabled: `include/ruvia/http/HttpClient.h`, `App::useHttpClient(...)`, and `Context::fetch(...)` are not installed or exposed.

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
    std::string_view name;   // borrowed — must remain valid through co_await
    std::string_view value;  // borrowed — must remain valid through co_await
};

struct FetchResponseHeader {
    std::pmr::string name;   // arena-allocated
    std::pmr::string value;  // arena-allocated
};

struct FetchOptions {
    std::string_view method{"GET"};
    std::initializer_list<FetchRequestHeader> headers{};
    std::string_view body{};  // borrowed — must remain valid through co_await
    std::chrono::milliseconds timeout{0};  // overrides config.requestTimeout when non-zero
};

struct FetchResponse {
    int statusCode{0};
    std::pmr::vector<FetchResponseHeader> headers;  // arena-allocated
    std::pmr::string body;                          // arena-allocated
};
```

**`include/ruvia/app/App.h`** additions (under `#ifdef RUVIA_ENABLE_HTTP_CLIENT`):

```cpp
App& useHttpClient(HttpClientConfig config);
App& useHttpClient(std::string_view alias, HttpClientConfig config);
```

**`include/ruvia/http/Context.h`** additions (under `#ifdef RUVIA_ENABLE_HTTP_CLIENT`):

```cpp
Task<FetchResponse> fetch(std::string_view path, FetchOptions options = {});
Task<FetchResponse> fetch(std::string_view alias, std::string_view path, FetchOptions options = {});
```

### Internal structure

Mirrors the DB/Redis pattern exactly:

| File | Purpose |
|---|---|
| `src/http/client/HttpClientInternal.h` | `HttpClientDefinition`, `HttpClientRegistry` declarations |
| `src/http/client/HttpClientPool.cpp` | Per-worker pool: connect, acquire slot, send request, release slot |
| `src/http/client/HttpClientPool.h` | Pool class header |

`HttpServer` constructor gains `std::span<const HttpClientDefinition> clients` parameter (same pattern as `databases` and `redis`). Each worker owns its own `HttpClientRegistry` containing one pool per alias.

### Connection pool behaviour

- Pool holds `poolSizePerWorker` persistent TCP connections to the configured `host:port`.
- Connections are established when the worker starts (same timing as DB/Redis `connect()`).
- Acquire blocks (up to `acquireTimeout`) if all slots are in use; times out with an exception.
- TLS uses `asio::ssl::context::tls_client` with OpenSSL, same library already used for the server side.
- `FetchResponse::headers` and `FetchResponse::body` are allocated in the request arena (`c.resource()`).
- Dropped/errored connections are re-established on next acquire.

### Memory

`FetchResponse` is allocated into the request arena via `c.resource()`. `FetchOptions::body` is a borrowed view — the caller must keep it alive through the `co_await`.

---

## 2. Multi-Port Listening

### Public API

**New type in `include/ruvia/app/App.h`:**

```cpp
struct ListenerConfig {
    std::pmr::string address{"0.0.0.0"};
    std::uint16_t port{8080};
    std::optional<TlsConfig> tls;  // per-listener TLS; overrides App-level TLS for this listener
};

App& addListener(ListenerConfig config);
```

`setListenAddress(addr, port)` is kept for backward compatibility. It sets the first listener (index 0). If no `addListener` call is made, `setListenAddress` + `useTls` behave exactly as before.

`useTls(TlsConfig)` now applies as the default TLS config for all listeners that do not supply their own `ListenerConfig::tls`.

### Internal changes

**`App` private state:**

Replace `listenAddress_` + `listenPort_` with:
```cpp
std::pmr::vector<ListenerConfig> listeners_;
```

`App` constructor seeds `listeners_` with one default entry `{"0.0.0.0", 8080, std::nullopt}`.

**`App::run()` worker creation:**

```
total workers = listeners_.size() × threadNum_
```

For each listener, `threadNum_` `HttpServer` instances are created on that listener's endpoint and TLS config. All workers across all listeners share the same read-only `RouteTable`.

**`AppRuntimeGraph`:**

`workers` remains a flat `pmr::vector<unique_ptr<HttpServer>>`. Workers are appended listener by listener; the grouping is only needed at start/stop, which iterates the whole vector anyway.

**TLS resolution per listener:**

```
effective TLS = listener.tls.has_value() ? listener.tls : app-level options_.tls
```

`HttpServerOptions::tls` is set per-worker from the resolved effective TLS before constructing each `HttpServer`.

### Backward compatibility

All existing single-listener code compiles and runs unchanged:

```cpp
ruvia::app()
    .setListenAddress("0.0.0.0", 8080)
    .useTls(tlsConfig)
    .run();
```

Multi-listener usage:

```cpp
ruvia::app()
    .addListener({"0.0.0.0", 80})
    .addListener({"0.0.0.0", 443, tlsConfig})
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

### Execution timing (inside `App::run()`, main thread)

```
configure routes
↓
start all workers (worker->start() for each)
↓
[onStart hooks fire here — all workers are accepting]
↓
block on signal (SIGINT / SIGTERM)
↓
[onStop hooks fire here — before workers are stopped]
↓
stop all workers
↓
join all workers
```

- Hooks run synchronously on the main thread (the thread that called `run()`).
- An exception thrown by any hook propagates out of `run()` immediately, aborting the startup or triggering cleanup.
- Hooks registered after `run()` starts throw `std::logic_error` (same guard pattern as other `App` setters).

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
| `include/ruvia/app/App.h` | Add `ListenerConfig`, `AppHook`, `addListener`, `onStart`, `onStop`, `useHttpClient` |
| `include/ruvia/http/Context.h` | Add `fetch(...)` under feature guard |
| `src/app/App.cpp` | Replace single address/port with `listeners_` vector; add hook storage and invocation; add `useHttpClient` |
| `src/net/server/HttpServer.h` | Add `HttpClientRegistry` member; extend constructor |
| `src/net/server/HttpServerLifecycle.cpp` | Wire `HttpClientRegistry` into worker startup/shutdown |
| `CMakeLists.txt` | Add `RUVIA_ENABLE_HTTP_CLIENT` option and `http-client` vcpkg feature |
| `vcpkg.json` | Add `http-client` feature (no new dependencies) |

---

## Out of scope for this spec

- HTTP/2 client
- Client-side connection pooling across workers (each worker owns its own pool)
- Per-listener thread count (all listeners share `threadNum_`)
- Async lifecycle hooks (`Task<void>`)
- Client request streaming or response streaming
