# Ruvia Examples

These examples are built when `RUVIA_BUILD_EXAMPLES` is enabled and double as compile-time coverage for Ruvia's public API. Unit, boundary, and regression-guard coverage lives separately under `tests/` and is registered with CTest when `RUVIA_BUILD_TESTS` is enabled.

| Target | Source | Covers |
| --- | --- | --- |
| `ruvia_example_basic_http` | `web/basic_http.cpp` | Controller/group macros, middleware, params, wildcard routes, query/header/cookie helpers, body reads, `urlFor` links, text/JSON/redirect/error responses, HEAD and OPTIONS, prefix-scoped `notFound`/`onError` fallbacks. |
| `ruvia_example_bench_server` | `web/bench_server.cpp` | Benchmark server mirroring the hical bench endpoints for throughput comparisons. |
| `ruvia_example_api_surface` | `web/api_surface.cpp` | Hono-like C++ context/request/response facades, discriminated HTTP/1 parse outcomes, route metadata, decoded paths, Accept checks, buffered multipart, explicit body discard, response cookies, manual `HttpResponse` body ownership, PUT/PATCH streaming, and the outbound client surface including same- and cross-origin redirect resolution. |
| `ruvia_example_models_validation` | `web/models_validation.cpp` | Unified JSON models, form bodies, nested models, arrays, recursive lists, defaults, validation middleware and rules, non-throwing `jsonIf`/`formIf` fallbacks. |
| `ruvia_example_streaming` | `web/streaming.cpp` | Streaming request bodies, typed multipart chunk phases, chunked response streaming and SSE. |
| `ruvia_example_files_static` | `web/files_static.cpp` | `c.file(...)`, `c.staticFile(...)`, `StaticRoot`, document root, validators/ranges and gzip configuration. |
| `ruvia_example_websocket` | `web/websocket.cpp` | WebSocket upgrade routes, subprotocol options, lifecycle timeouts, text/binary echo and RFC close handshake. |
| `ruvia_example_ops` | `web/ops.cpp` | Security headers middleware, route-level per-IP rate limiting, and health/readiness response helpers wired through controller macros. |
| `ruvia_example_middleware_next` | `web/middleware_next.cpp` | Middleware value `Next` and one-shot `co_await next()` signature coverage. |
| `ruvia_example_workers_blocking` | `web/workers_blocking.cpp` | Worker-local state (`useWorkerState<T>`/`workerState<T>`), cross-worker task posting (`App::workers`, `WebWorkerHandle::post`), and the `OneShot` pattern for offloading blocking calls to an application-owned thread. |
| `ruvia_example_testing` | `web/testing.cpp` | In-memory application testing with `TestApp`/`TestRequest`/`TestResponse`: production routing, middleware, model bodies, fallbacks, urlFor and worker state without a socket. |
| `ruvia_example_auth_jwt` | `web/auth_jwt.cpp` | JWT signing, verification, bearer-token middleware and protected routes. Built only with `RUVIA_ENABLE_JWT=ON`. |
| `ruvia_example_database` | `web/database.cpp` | Unified MariaDB/PostgreSQL configuration, query, execute, streaming query, transaction and optional migration. Built with either database feature. |
| `ruvia_example_redis` | `web/redis.cpp` | Redis configuration, aliases, strings, hashes, lists, sets, sorted sets, scans, scripts, blocking pops, pipelines and transactions. Built only with `RUVIA_ENABLE_REDIS=ON`. |
| `ruvia_example_runtime_config` | `web/runtime_config.cpp` | Dotenv, app-wide middleware via `App::use`, memory pool, timeouts, limits, compression and optional TLS. |

Build all examples by enabling the examples option:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build
```

Build feature examples by enabling the matching feature flags, for example:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRUVIA_BUILD_EXAMPLES=ON \
  -DRUVIA_ENABLE_MARIADB=ON \
  -DRUVIA_ENABLE_POSTGRESQL=ON \
  -DRUVIA_ENABLE_REDIS=ON \
  -DRUVIA_ENABLE_JWT=ON
cmake --build build
```

The database example defaults to MariaDB. Set `RUVIA_DB_DRIVER=postgresql` to
select PostgreSQL; the default port then changes from `3306` to `5432`.

The default project build keeps examples disabled:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```
