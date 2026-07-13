# Ruvia Examples

These examples are built when `RUVIA_BUILD_EXAMPLES` is enabled and double as compile-time coverage for Ruvia's public API. Unit, package-consumer, boundary, and regression-guard coverage lives separately under `tests/` and is registered with CTest when `RUVIA_BUILD_TESTS` is enabled.

| Target | Source | Covers |
| --- | --- | --- |
| `ruvia_example_basic_http` | `basic_http.cpp` | Controller/group macros, middleware, params, wildcard routes, query/header/cookie helpers, body reads, text/JSON/redirect/error responses, HEAD and OPTIONS. |
| `ruvia_example_api_surface` | `api_surface.cpp` | Hono-like C++ context/request/response facades, discriminated HTTP/1 parse outcomes, route metadata, decoded paths, Accept checks, buffered multipart, explicit body discard, response cookies, manual `HttpResponse` body ownership and PUT/PATCH streaming. |
| `ruvia_example_models_validation` | `models_validation.cpp` | Separate request/response models, JSON/form bodies, nested models, arrays, recursive lists, defaults, validation middleware and rules. |
| `ruvia_example_streaming` | `streaming.cpp` | Streaming request bodies, typed multipart chunk phases, chunked response streaming and SSE. |
| `ruvia_example_files_static` | `files_static.cpp` | `c.file(...)`, `c.staticFile(...)`, `StaticRoot`, document root, validators/ranges and gzip configuration. |
| `ruvia_example_websocket` | `websocket.cpp` | WebSocket upgrade routes, subprotocol options, lifecycle timeouts, text/binary echo and RFC close handshake. |
| `ruvia_example_ops` | `ops.cpp` | Security headers middleware, route-level per-IP rate limiting, and health/readiness response helpers wired through controller macros. |
| `ruvia_example_middleware_next` | `middleware_next.cpp` | Middleware value `Next` and one-shot `co_await next()` signature coverage. |
| `ruvia_example_auth_jwt` | `auth_jwt.cpp` | JWT signing, verification, bearer-token middleware and protected routes. Built only with `RUVIA_ENABLE_JWT=ON`. |
| `ruvia_example_database` | `database.cpp` | DB configuration, query, execute, streaming query, transaction and optional migration. Built only with `RUVIA_ENABLE_MARIADB=ON`. |
| `ruvia_example_redis` | `redis.cpp` | Redis configuration, aliases, strings, hashes, lists, sets, sorted sets, scans, scripts, blocking pops, pipelines and transactions. Built only with `RUVIA_ENABLE_REDIS=ON`. |
| `ruvia_example_runtime_config` | `runtime_config.cpp` | Dotenv, global middleware, memory pool, timeouts, limits, compression and optional TLS. |

Build all examples by enabling the examples option:

```powershell
cmake -S . -B build -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
```

Build feature examples by enabling the matching feature flags, for example:

```powershell
cmake -S . -B build `
  -DRUVIA_BUILD_EXAMPLES=ON `
  -DRUVIA_ENABLE_MARIADB=ON `
  -DRUVIA_ENABLE_REDIS=ON `
  -DRUVIA_ENABLE_JWT=ON
cmake --build build --config Debug
```

The default project build keeps examples disabled:

```powershell
cmake -S . -B build
```
