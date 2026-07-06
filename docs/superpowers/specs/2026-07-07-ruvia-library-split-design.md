# Ruvia Library Split Final Design

## Goal

Ruvia will have two first-class product surfaces:

- A C++23 web framework.
- A CDN / reverse-proxy runtime.

They must reuse the same protocol and runtime code, but neither product may depend on the other. The shared layers are public libraries, not private implementation buckets, so external users must be able to consume them directly.

## Final Target Graph

Ruvia exposes four physical CMake targets:

```text
ruvia-core
ruvia-http
ruvia-web
ruvia-cdn
```

Namespace aliases:

```text
ruvia::core -> ruvia-core
ruvia::http -> ruvia-http
ruvia::web  -> ruvia-web
ruvia::cdn  -> ruvia-cdn
```

Dependency direction:

```text
ruvia-http -> ruvia-core

ruvia-web  -> ruvia-http
ruvia-cdn  -> ruvia-http
```

Compatibility:

```text
ruvia::ruvia -> ruvia::web
```

`ruvia-web` and `ruvia-cdn` are sibling products. The build must make it impossible for either one to link to the other.

## Design Principles

1. CMake target boundaries are authoritative. Source grouping alone is not a split.
2. Public header ownership is explicit. Do not install whole mixed directories as a target's public API.
3. `ruvia-core` and `ruvia-http` are external-grade libraries.
4. `Context` is a web framework facade and never becomes an HTTP-library abstraction.
5. CDN policy never lives in `ruvia-http`.
6. Web route registration and middleware dispatch never live in `ruvia-http`.
7. Runtime state stays per-worker where it is mutable.
8. Request hot paths must not gain shared locks, shared reference-count contention, or avoidable copies.
9. Compatibility is preserved through wrappers and forwarding headers, not by weakening the new boundaries.

## ruvia-core

### Role

`ruvia-core` is the small runtime and utility base. It is useful outside HTTP and outside the Ruvia web framework.

### Public API

It owns:

- `ruvia::Task<T>` and its non-Asio coroutine machinery.
- PMR helpers, object lifetime helpers, and memory-resource utilities.
- ASCII case helpers.
- Hex, base64, base64url, and constant-time helpers.
- Date/time helpers that are not HTTP-specific.
- Small cross-layer value utilities with no HTTP, Web, DB, Redis, JWT, or CDN semantics.

### Dependency Rule

`ruvia-core` must not publicly depend on Asio, OpenSSL, zlib, Brotli, zstd, MariaDB, hiredis, or web framework headers.

`Task` belongs in core because the current `Task` type is a Ruvia coroutine handle/promise abstraction. The Asio adapter stays outside core, in the HTTP/runtime implementation boundary.

### Process-Global Behavior

Core may provide a mimalloc-backed `std::pmr::memory_resource`, but linking `ruvia-core` must not silently replace the process allocator. Process-global allocator override remains an explicit product/application decision.

## ruvia-http

### Role

`ruvia-http` is the standalone HTTP protocol and transport library. External users can use it for clients, protocol tooling, reverse proxies, tests, and custom servers without linking `ruvia-web`.

### Public API

It owns stable public APIs for:

- HTTP methods, status codes, headers, request/response value types.
- HTTP/1.1 parser, chunk parser, request-target parser.
- HTTP cache helpers: cache-control, validators, HTTP-date parsing and formatting.
- Cookie parsing, serialization, signing primitives, and cookie validation.
- Range, conditional request, content negotiation, CORS, security-header helpers.
- Multipart, form/url encoding, body decoding, and HTTP body stream primitives.
- WebSocket frame, handshake, message assembly, validation, and extension primitives.
- Outbound HTTP client, including HTTP/1.1, HTTP/2, TLS verification, redirect handling, decompression, and streaming fetch.

### Internal / Unstable API

It may own internal or unstable transport primitives needed by product layers:

- Response-head generation.
- Streaming response body plumbing.
- Compression writers.
- Trailer encoding.
- File/range response helpers.
- HTTP/2 frame, HPACK, flow-control, and stream-state internals.
- Connection/session pieces that are pure protocol transport.

These internal pieces can be installed for Ruvia product targets only when required, but they must not be documented as stable external API until promoted.

### Explicit Exclusions

`ruvia-http` must not own:

- `App`.
- `Router`.
- `Controller`.
- Route macros.
- Middleware chains.
- `Next`.
- `Context`.
- Model validation macros.
- DB, Redis, JWT, CSRF, or session application integration.
- CDN origin selection, cache-store policy, purge, invalidation, SWR policy, or edge rules.

### Current-Code Constraint

Do not move the current `HttpServer` wholesale into `ruvia-http`. It currently binds transport to route tables, app services, DB, Redis, HTTP client registries, and rate limiting. Only pure protocol/transport pieces may move down.

The long-term server split should be:

```text
ruvia-http:
  low-level HTTP transport, codecs, readers, writers, protocol state machines

ruvia-web:
  app/router/context/middleware assembly over that transport

ruvia-cdn:
  CDN request handling and reverse-proxy/cache assembly over that transport
```

## ruvia-web

### Role

`ruvia-web` is the web framework product.

### Public API

It owns:

- `App`.
- `Router`.
- `Controller`.
- Route macros.
- Middleware registration and dispatch.
- `Context` as the handler facade.
- `Next`.
- Model macros, model parsing, model JSON writing, and validation middleware.
- DB, Redis, JWT, CSRF, session, and other application integrations.
- Web-framework examples and compatibility umbrella headers.

### Proxy Convenience

`Context::fetch`, `Context::fetchStream`, `Context::proxy`, and `Context::defer` may remain as web convenience APIs. Their protocol work must delegate to `ruvia-http` primitives. CDN code must not depend on these methods or on `Context`.

## ruvia-cdn

### Role

`ruvia-cdn` is the CDN / reverse-proxy product layer.

### Public API

It owns:

- Origin definitions and origin selection policy.
- Origin health policy.
- Cache-store interfaces.
- Cache key policy.
- Stale-while-revalidate and background refresh policy.
- Purge and invalidation.
- Edge rules.
- CDN-specific request/response policy.
- Reverse-proxy assembly using `ruvia-http` client and transport primitives.

### Exclusions

`ruvia-cdn` must not depend on:

- `ruvia-web`.
- `App`.
- `Router`.
- `Controller`.
- Route macros.
- Middleware chains.
- `Context`.

If the CDN needs a handler abstraction, it gets a CDN-native abstraction over HTTP transport, not the web framework `Context`.

## Public Header Strategy

The current `include/ruvia/http` directory is mixed. It contains both protocol headers and web-framework headers. Therefore the split must not install or assign the entire directory to `ruvia-http`.

### First Pass

Use explicit public header manifests per target.

Examples:

```text
ruvia-core public headers:
  include/ruvia/app/Task.h initially, later moved or forwarded to include/ruvia/core/Task.h
  include/ruvia/memory/*.h
  include/ruvia/detail/AsciiCase.h
  include/ruvia/detail/Base64Url.h
  include/ruvia/detail/ConstantTime.h
  include/ruvia/http/detail/Hex.h only if it is moved or forwarded to a core-owned path

ruvia-http public headers:
  include/ruvia/http/HttpCommon.h
  include/ruvia/http/HttpTypes.h
  include/ruvia/http/HttpStatus.h
  include/ruvia/http/HttpRequest.h
  include/ruvia/http/HttpResponse.h
  include/ruvia/http/HttpParser.h
  include/ruvia/http/HttpCache.h
  include/ruvia/http/HttpClient.h
  include/ruvia/http/HttpBodyStream.h
  include/ruvia/http/MultipartReader.h
  include/ruvia/http/WebSocket.h
  protocol-only helper headers

ruvia-web public headers:
  include/ruvia/app/*.h
  include/ruvia/router/*.h
  include/ruvia/http/Context*.h
  include/ruvia/http/Controller*.h
  include/ruvia/http/Middleware*.h
  include/ruvia/http/Next.h
  include/ruvia/http/Model*.h
  include/ruvia/http/Validation*.h
  include/ruvia/http/Csrf.h
  include/ruvia/http/Session.h
  optional db/redis/auth headers when enabled
```

### Compatibility Pass

Existing include paths should keep working through forwarding headers. For example, a future `include/ruvia/core/Task.h` can be introduced while `include/ruvia/app/Task.h` remains as a compatibility include.

Compatibility headers must not be used as proof that a lower-level target owns a higher-level API.

## CMake and Package Design

### Build Options

Recommended options:

```text
RUVIA_BUILD_CORE    default ON
RUVIA_BUILD_HTTP    default ON
RUVIA_BUILD_WEB     default ON
RUVIA_BUILD_CDN     default OFF until the CDN product is real
RUVIA_ENABLE_MARIADB
RUVIA_ENABLE_REDIS
RUVIA_ENABLE_JWT
```

The old `RUVIA_ENABLE_HTTP_CLIENT` should disappear once the HTTP client is part of `ruvia-http`. If a smaller HTTP build is required later, make client support a sub-option of `ruvia-http`, not a web-framework option.

### Dependency Visibility

Dependencies must be attached to the smallest target that needs them.

- `ruvia-core`: no public Asio/OpenSSL/compression dependency.
- `ruvia-http`: Asio and OpenSSL for transport/client; compression dependencies only as required by the target interface.
- `ruvia-web`: links `ruvia-http`; adds DB/Redis/JWT dependencies only when enabled.
- `ruvia-cdn`: links `ruvia-http`; adds cache-store backend dependencies only when those backends exist.

Use `PRIVATE` dependencies whenever public headers do not require the dependency type.

### Package Components

`find_package(ruvia COMPONENTS core)` must not load HTTP, web, CDN, DB, Redis, JWT, OpenSSL, or compression dependencies unless the exported core target actually exposes them.

`find_package(ruvia COMPONENTS http)` must not load web, DB, Redis, JWT, or CDN dependencies.

`find_package(ruvia COMPONENTS web)` loads core and http transitively and then optional web integrations that were built.

If no component is requested, default to the compatibility web framework surface:

```text
find_package(ruvia)
target_link_libraries(app PRIVATE ruvia::ruvia)
```

## Migration Plan

### Phase 1: Define Target Boundaries

1. Keep file paths mostly stable.
2. Create explicit source lists for `ruvia-core`, `ruvia-http`, and `ruvia-web`.
3. Keep `ruvia::ruvia` compatible with the current web framework target.
4. Create explicit public header lists for each target.
5. Stop using whole-directory install rules for mixed public API directories.

### Phase 2: Make Core Standalone

1. Move or forward `Task` into a core-owned public path.
2. Keep Asio adapters in internal runtime/http code, not in `ruvia-core`.
3. Move generic helpers down only when they have no HTTP or product semantics.
4. Keep mimalloc override explicit.
5. Add a core-only compile/link smoke target.

### Phase 3: Make HTTP Standalone

1. Move protocol value types, parsers, HTTP cache helpers, cookie helpers, WebSocket primitives, body streams, and outbound HTTP client into `ruvia-http`.
2. Keep web-only headers out of the `ruvia-http` public manifest.
3. Extract pure server transport primitives only where they do not depend on `RouteTable`, `Context`, app services, DB, Redis, or route middleware.
4. Leave the current app-bound `HttpServer` assembly in `ruvia-web` until it is split behind a pure transport callback boundary.
5. Add an http-only executable that performs outbound HTTP client work without linking `ruvia-web`.

### Phase 4: Preserve Web Compatibility

1. Build examples through `ruvia::ruvia` and `ruvia::web`.
2. Keep existing route/controller/context source compatibility.
3. Make `Context::proxy` and related helpers call lower-level HTTP primitives.
4. Verify optional DB, Redis, and JWT builds remain web integrations, not HTTP dependencies.

### Phase 5: Add CDN Product Layer

1. Add `ruvia-cdn` after `ruvia-http` builds independently.
2. Implement CDN policy over HTTP primitives, not over `Context`.
3. Add a CDN-only link check that fails if `ruvia-web` appears in the target graph.
4. Add CDN cache/proxy tests around origin selection, cache key policy, purge, and stale-while-revalidate.

## Validation Gates

The design is implemented only when all of these are true:

- `cmake --build build --config Debug` passes.
- The existing HTTP-client and JWT debug builds still pass when those build trees exist.
- `git diff --check` passes.
- A core-only smoke executable links against `ruvia::core`.
- An http-only smoke executable links against `ruvia::http`, uses outbound HTTP client APIs, and does not include or link `Context`, `Router`, or `Controller`.
- Existing web examples build through `ruvia::ruvia` and `ruvia::web`.
- A CDN-only smoke executable links against `ruvia::cdn` and does not link `ruvia::web`.
- The generated CMake package config resolves `core`, `http`, `web`, and `cdn` components independently.
- Target link interfaces prove `ruvia-cdn` does not depend on `ruvia-web`.
- Public header manifests prove `ruvia-http` does not publish web-framework headers.
- No request hot-path change introduces shared mutable runtime state, shared lock contention, or shared reference-count contention.

## Non-Goals

- Do not redesign the web framework public API during the split.
- Do not make CDN use `Context`.
- Do not create separate `ruvia-client` or `ruvia-server` targets unless the four-target layout proves too coarse later.
- Do not move every file into new directories in the first pass.
- Do not treat compatibility forwarding headers as ownership boundaries.
- Do not make a smaller target graph by letting product layers leak into shared libraries.
