# Ruvia Examples

These examples are built when `RUVIA_BUILD_EXAMPLES` is enabled. They double as compile-time coverage for Ruvia's public API; there is no separate `tests/` or CTest target.

| Target | Source | Covers |
| --- | --- | --- |
| `ruvia_example_basic_http` | `basic_http.cpp` | Controller/group macros, middleware, params, wildcard routes, query/header/cookie helpers, body reads, text/JSON/redirect/error responses, HEAD and OPTIONS. |
| `ruvia_example_api_surface` | `api_surface.cpp` | Remaining route macros, request metadata, decoded paths, Accept checks, buffered multipart, explicit body discard, response cookies, manual `HttpResponse` body ownership and PUT/PATCH streaming. |
| `ruvia_example_models_validation` | `models_validation.cpp` | `RUVIA_MODEL`, JSON/form bodies, response models, nested models, arrays, recursive lists, defaults, validation middleware and rules. |
| `ruvia_example_streaming` | `streaming.cpp` | Streaming request bodies, streaming multipart, chunked response streaming and SSE. |
| `ruvia_example_files_static` | `files_static.cpp` | `c.file(...)`, `c.staticFile(...)`, `StaticRoot`, document root, validators/ranges and gzip configuration. |
| `ruvia_example_websocket` | `websocket.cpp` | WebSocket upgrade routes, subprotocol options, heartbeat options, text/binary echo and close. |
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
