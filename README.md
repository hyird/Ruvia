# Ruvia

Ruvia is a small, focused C++23 HTTP/1.1 and HTTP/2 web framework for core web services with a compact public API and a low-overhead request path.

## Contents

- [Highlights](#highlights)
- [Project Scope](#project-scope)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Minimal Controller](#minimal-controller)
- [Routing and Middleware](#routing-and-middleware)
- [Context Helpers](#context-helpers)
- [Request and Response Models](#request-and-response-models)
- [Macro Reference](#macro-reference)
- [Configuration](#configuration)
- [Database Access](#database-access)
- [Redis and JWT Helpers](#redis-and-jwt-helpers)
- [HTTPS and Compression](#https-and-compression)
- [Runtime Behavior](#runtime-behavior)
- [Install](#install)
- [Current Status](#current-status)

## Highlights

| Area | What Ruvia Provides |
| --- | --- |
| Macro DSL | A compile-time macro surface that expands to startup descriptors and generated members with no runtime reflection: routing (`RUVIA_GET` / `RUVIA_POST` / �?, grouping (`RUVIA_CONTROLLER_GROUP`, `RUVIA_GROUP_BEGIN`), streaming and realtime routes (`RUVIA_GET_STREAM` / `RUVIA_GET_SSE` / `RUVIA_GET_WS`), runtime-selectable responses (`RUVIA_GET_DYNAMIC` / `RUVIA_POST_DYNAMIC`), schema models (`RUVIA_MODEL` / `RUVIA_FIELD`), and inline validation (`RUVIA_VALIDATE_JSON` with `RUVIA_RULE` / `RUVIA_REQUIRED` / �?. See the consolidated [Macro Reference](#macro-reference). |
| Controller API | Hono-style single-argument handlers with `ruvia::Context& c` and async-only `ruvia::Task<ruvia::HttpResponse>` returns. |
| Routing | Static controller registration through macros, route groups, scoped middleware, `:param` segments, and `*` wildcards. |
| Request handling | Zero-copy HTTP parser, request views into the connection read buffer, explicit streaming body routes, chunked body decoding, multipart form parsing, and helpers for headers, query values, cookies, JSON bodies, and form bodies. |
| Responses | Chainable helpers for status, headers, cookies, redirects, text, JSON, file responses, static files with validators/ranges, configurable error handling, and unified JSON error bodies. |
| Models | `RUVIA_MODEL` schema macros for typed JSON/form bodies and JSON responses, plus inline validator middleware rules without runtime reflection. |
| Runtime | Per-worker standalone Asio `io_context`, HTTP/1.1, HTTP/2 over h2c or TLS ALPN, built-in HTTPS/TLS, gzip compression, optional MariaDB/Redis/JWT feature support, graceful shutdown, centralized timeout scanning, connection limits, security headers, route-level per-IP rate limiting, health/readiness helpers, per-worker PMR allocators, per-request arenas, and `mimalloc` as the production upstream allocator. |
| Distribution | CMake install/export support through one installed library file and one public target: `ruvia::ruvia`. |

## Project Scope

Ruvia is intentionally a small HTTP framework, not a full-stack application platform. The implementation boundary is the high-performance core needed to build HTTP services with explicit ownership and low request-path overhead.

In scope:

- HTTP/1.1 and HTTP/2 server runtime, h2c prior knowledge, h2c upgrade, TLS ALPN `h2`, OpenSSL-backed TLS, keep-alive, pipelining safety, parser limits, timeout handling, and per-worker connection ownership.
- Hono-style controller and route macros, route groups, user-defined middleware, exact routes, `:param` routes, `*` wildcards, startup duplicate/conflict detection, and explicit `HEAD` / `OPTIONS` behavior.
- Request helpers for headers, query values, cookies, route params, lazy buffered body reads, JSON, URL-encoded forms, buffered multipart, and explicit streaming request bodies.
- Response helpers for status, headers, cookies, text, JSON, redirects, JSON errors, gzip compression for buffered responses, file/static-file responses with validators and Range support, explicit response streaming, SSE, HTTP/1.1 WebSocket upgrades, and HTTP/2 RFC 8441 WebSocket streams.
- `RUVIA_MODEL` schema macros backed by Ruvia model types, without runtime reflection or general-purpose maps.
- A performance-oriented runtime: one standalone Asio `io_context` per worker, worker-owned acceptors/sockets, PMR memory resources, request arenas, zero-copy parser views, scatter-gather-friendly response writes, and `mimalloc` as the production upstream allocator.
- Optional MariaDB-compatible DB query, transaction, and migration support through `DbHandle`, `DbTransaction`, and `DbMigrator`.
- Optional Redis command helpers, pipelines, transactions, scans, scripts, and blocking pops through worker-local pools and `Context::redis(...)`.
- Startup conveniences such as dotenv loading, CORS response headers, security headers, health/readiness response helpers, gzip configuration, and optional HMAC JWT helpers.

Out of scope for the current boundary:

- HTTP/3, HTTP/2 priority scheduling policy tuning, template rendering, ORM/entity modeling, session/auth batteries beyond the low-level JWT helper, background job systems, distributed WebSocket fanout, and full-stack frontend integration.
- Public synchronous handlers or public `asio::awaitable` APIs. Public handlers use `ruvia::Task<T>`.
- Direct public route registration through `Router::addRoute(...)` or mutable runtime route rebuilding.
- Request-path shared locks, cross-worker connection migration, shared response-body reference counting, and implicit buffering of streaming responses.

Startup-time registries, factories, and validation are allowed. Request handling is kept to prebuilt read-only route data, direct thunks, worker-local state, request-local arenas, and explicit streaming APIs.

## Requirements

- C++23 compiler
- CMake 3.24+
- vcpkg
- Core dependencies declared in `vcpkg.json`: `asio`, `mimalloc`, `openssl`, and `zlib`. Optional vcpkg features add `libmariadb` for `mariadb` and `hiredis` for `redis`.

## Quick Start

Configure and build the default runtime:

```powershell
cmake -S . -B build `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

The default build is core-only: HTTP/App/TLS/gzip/static files/models/streaming/WebSocket are included, while MariaDB, Redis, and JWT APIs are strictly hidden. Enable optional surfaces explicitly:

```powershell
cmake -S . -B build `
  -DCMAKE_BUILD_TYPE=Debug `
  -DRUVIA_ENABLE_MARIADB=ON `
  -DRUVIA_ENABLE_REDIS=ON `
  -DRUVIA_ENABLE_JWT=ON `
  -DVCPKG_MANIFEST_FEATURES="mariadb;redis;jwt" `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

## Minimal Controller

```cpp
#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

class HelloController final : public ruvia::Controller<HelloController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello from ruvia\n");
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8080)
        .setThreadNum(2)
        .run();
}
```

Controller classes are discovered through static route registration. Application startup only needs to configure and run `ruvia::app()`.

## Routing and Middleware

Routes are declared with macros inside `RUVIA_ROUTES_BEGIN` and `RUVIA_ROUTES_END`:

```cpp
RUVIA_ROUTES_BEGIN
RUVIA_GROUP_BEGIN("/api", AuthMiddleware)
RUVIA_GET("/users/:id", getUser);
RUVIA_GROUP_BEGIN("/admin", AdminMiddleware)
RUVIA_GET("/stats", stats);
RUVIA_GROUP_END
RUVIA_GROUP_END
RUVIA_ROUTES_END
```

This registers `GET /api/users/:id` and `GET /api/admin/stats`. Middleware order is controller group, outer group, inner group, then route-specific middleware.

For controllers split across multiple files, put the shared prefix and controller-level middleware on each controller:

```cpp
class UserController final : public ruvia::Controller<UserController> {
public:
    RUVIA_CONTROLLER_GROUP("/api", AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/users/:id", getUser);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> getUser(ruvia::Context& c);
};
```

Route registration is macro-only:

- Public macros produce startup descriptors.
- Route handler/middleware storage, resolve tables, and dispatch types live behind `src/router/RouterInternal.h`; they are not public API.
- Middleware chains are built before workers start, so request dispatch remains a route lookup plus prebuilt thunk calls.
- Duplicate routes are rejected during startup instead of silently replacing an existing handler.
- Exact same-method paths conflict, and dynamic routes with the same match shape conflict. For example, `GET /users/:id` conflicts with `GET /users/:name`, and `GET /files/*` conflicts with any later same-method route shadowed by that wildcard.

`HEAD` falls back to an existing `GET` route when no explicit `HEAD` route is registered. `Allow` headers include `HEAD` whenever `GET` is allowed, and framework-generated `OPTIONS` remains distinct from user-defined `RUVIA_OPTIONS(...)` routes.

Applications define middleware with `ruvia::Middleware<T>` and attach it to controller groups, nested groups, or individual routes. A `Task<void>` middleware uses Hono-style `await next()` and mutates `c.res()` after dispatch; downstream exceptions do not escape `next()`, and instead populate `c.error()` plus an error response in `c.res()`. A `Task<HttpResponse>` middleware can return a response directly to early-exit:

```cpp
class AuthMiddleware final : public ruvia::Middleware<AuthMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        if (c.req().header("X-Api-Key") != "secret") {
            co_return c.error(401, "unauthorized", "unauthorized");
        }
        co_await next();
        c.header("X-Auth", "ok");
        co_return std::move(c.res());
    }
};

class ApiController final : public ruvia::Controller<ApiController> {
public:
    RUVIA_CONTROLLER_GROUP("/api", AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c);
};
```

Middleware instances and chains are built before workers start, so request dispatch uses prebuilt route metadata and direct thunks. Middleware returns `ruvia::Task<void>`: `co_await next()` advances the chain once, `c.header(...)` mutates response headers, and `c.res(response)` short-circuits with a prepared response. Calling the same `next` continuation more than once records an error on `c.error()`, does not re-enter the downstream handler, and falls back to a JSON `500` response unless middleware replaces `c.res()`.

Route-specific per-IP limits can be declared as middleware and attached only to the routes that need them:

```cpp
#include "ruvia/http/RateLimit.h"

class ApiController final : public ruvia::Controller<ApiController> {
public:
    RUVIA_ROUTE_RATE_LIMIT(LoginRateLimit, 5, 60'000);

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/login", login, LoginRateLimit);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& c);
};
```

`RUVIA_ROUTE_RATE_LIMIT(name, maxRequests, windowMs)` keys by the request remote address, returns JSON `429` with `Retry-After` and `X-RateLimit-*` headers, and keeps worker-local/thread-local buckets so request dispatch stays lock-free. Use the server-level Redis/global limiter when a cross-worker or distributed limit is required.

## Context Helpers

Use `ruvia::Context` to read request data and construct responses:

| Helper | Purpose |
| --- | --- |
| `c.req()` / `c.req().raw()` | Access the request facade, or the raw `ruvia::HttpRequest` when framework-level code needs the parser view. |
| `c.req().method()` / `c.req().methodEnum()` | Read the request method as a Hono-style method string, or as the Ruvia `HttpMethod` enum when C++ control flow needs it. |
| `c.req().url()` | Return the absolute request URL, synthesizing it from scheme, `Host`, and target when the request uses origin-form. |
| `ruvia::routePath(c)` / `c.req().routePath()` | Read the matched route pattern, such as `/users/:id`, without copying. |
| `c.req().decodedPath()` | Read the request path through the same lazy decoding helpers as params; call `.toString()` only when a decoded string is needed. |
| `c.req().header(name)` / `c.req().header()` | Read one request header, or all request headers with lowercase names. Use `c.req().headers()` for the raw parser header view. |
| `c.req().query(name)` / `c.req().query()` / `c.req().queries(name)` / `c.req().queries()` | Read one decoded query value, all decoded single-value query params with last-wins duplicate semantics, all values for one key, or all decoded query params grouped by key. |
| `c.req().cookie(name)` / `c.req().cookie()` | Read one cookie value or parse the current request cookie list. |
| `c.req().param(name)` / `c.req().param()` | Read one dynamic route parameter through typed helpers, or all decoded route parameters as a request-arena collection. |
| `co_await c.req().text()` | Lazily read the full buffered request body into the request arena. |
| `co_await c.req().arrayBuffer()` | Lazily read the full buffered request body as a zero-copy byte span. |
| `co_await c.req().blob()` | Lazily read the full buffered request body as a zero-copy byte span plus its `Content-Type`. |
| `co_await c.req().json<T>()` | Lazily read and parse a `RUVIA_MODEL` JSON body. |
| `co_await c.req().form<T>()` | Lazily read and parse a `RUVIA_MODEL` URL-encoded form body. |
| `co_await c.req().multipart()` | Lazily read and parse a buffered multipart/form-data body into part views. |
| `co_await c.req().parseBody()` | Parse URL-encoded or multipart form data into a Hono-style object facade: use `body["title"]` or `body.get("title")` for the last field, `body.getAll("tag[]")` for arrays/files, and `body.isArray("tag[]")` to inspect `[]` fields. `value()` / `values()` remain available as string-view convenience helpers. With `{.dot = true}`, use `getAt("obj.key")` / `getAllAt("obj.key")` for dot notation access. |
| `co_await c.req().formData()` | Web FormData-style form parsing with `get()` / `getAll()` access that preserves duplicate field names by default. |
| `co_await c.req().discardBody()` | Explicitly drain the request body when a route wants to keep the connection alive without using the body. |
| `c.req().bodyReader()` | Read an explicitly streaming request body chunk by chunk. |
| `c.req().multipartReader()` | Stream multipart/form-data parts from an explicitly streaming route. |
| `c.stream()` | Write an explicitly streaming chunked response from a `RUVIA_GET_STREAM(...)` route. |
| `c.streamText()` | Hono-like text streaming helper; sets `Content-Type: text/plain; charset=utf-8` and returns the stream writer. |
| `c.streamSSE()` | Hono-like Server-Sent Events helper from a `RUVIA_GET_SSE(...)` route. |
| `c.webSocket()` | Access the upgraded WebSocket connection from a `RUVIA_GET_WS(...)` route. |
| `c.status(code)` | Set the response status used by subsequent response helpers. |
| `c.header(name, value)` | Add or replace a response header. |
| `c.setCookie(name, value, options)` | Append a `Set-Cookie` response header. |
| `c.deleteCookie(name, options)` | Expire a response cookie with `Max-Age=0`. |
| `c.res()` / `c.res(response)` | Access the final response object or replace it to short-circuit middleware. Use `c.res().responseHeaders().append(...)` to append a response header after `co_await next()`, mirroring Hono's `c.res.headers.append(...)` shape. |
| `c.finalized()` | Check whether downstream middleware or the handler has already set the final response. |
| `c.env()` | Read application environment values from the current context. |
| `c.set(key, value)` / `c.get<T>(key)` / `c.var<T>(key)` | Store and read request-local values across middleware and handlers. Use `c.getIf<T>(key)` / `c.varIf<T>(key)` when Hono-like missing-value semantics are needed instead of throwing. |
| `c.body(...)` | Return a raw response body without setting a content type. |
| `c.text(...)` | Return a `text/plain` response. |
| `c.json(value)` | Serialize a response model as JSON. |
| `c.html(...)` / `co_await c.render(body, {.head = ...})` | Return HTML directly or through a middleware-installed renderer with explicit head metadata. |
| `c.text(body, 201, {{"X-Trace", "..."}})` / `c.text(body, {.status = ..., .headers = ...})` | Use Hono-style response init options with `body`, `text`, `json`, or `html`. |
| `c.file(path)` | Return a file response without loading the whole file into memory. |
| `c.staticFile(staticRoot, relative)` | Return a static file under a startup-built `ruvia::StaticRoot` with traversal checks. |
| `c.redirect(location)` | Return a redirect response. |
| `c.error()` | Read a downstream exception after `co_await next()` in middleware. |
| `c.error(status, code, message)` | Return a unified JSON error response. |
| `c.jsonError(status, code, message)` | Compatibility alias for `c.error(status, code, message)`. |
| `c.notFound()` | Return the framework JSON 404 response. |
| `c.db()` / `c.db(alias)` | Access a startup-registered database handle when `RUVIA_ENABLE_MARIADB` is enabled. |
| `c.redis()` / `c.redis(alias)` | Access a startup-registered Redis handle when `RUVIA_ENABLE_REDIS` is enabled. |

A few lifetime and ownership rules are worth keeping close:

- Request headers are read through `c.req().header(...)`; `c.header(...)` follows Hono-style response header semantics and mutates `c.res()` once a downstream response exists.
- Middleware must finalize the context by setting `c.res(...)`, returning through downstream `next()`, or completing a stream route; otherwise Ruvia treats it as an error instead of silently returning an empty response.
- `ruvia::HttpRequest` is a read-only request metadata view for application code. It is populated by the parser/server, and its string views point at the current connection/request buffers.
- Raw and model request body I/O lives on `c.req()`. Use `co_await c.req().text()` for raw text, `co_await c.req().arrayBuffer()` for raw bytes, `co_await c.req().blob()` when raw bytes also need the request `Content-Type`, `co_await c.req().cloneRawRequest()` for a request-arena metadata/body snapshot after validation or body reads, `c.req().matchedRoutes()` / `c.req().routeIndex()` for Hono-style route debugging metadata, `co_await c.req().json<T>()` for JSON models, `co_await c.req().form<T>()` for URL-encoded form models, `co_await c.req().parseBody()` for Hono-style form object access including dot notation through `getAt(...)`, and `co_await c.req().formData()` for Web FormData-style duplicate-preserving access.
- `co_await c.req().multipart()` returns a request-arena vector whose `name`, `filename`, `contentType`, and `body` fields are `std::string_view`s into the current request body.
- Response status codes, reason phrases, header names, header values, cookie names, and cookie values are validated when set. Invalid output metadata throws `std::invalid_argument` before it reaches the writer.
- File bodies are constructed through `c.file(...)` and `c.staticFile(...)`; application code should not build raw file-body responses directly.
- Create `ruvia::StaticRoot` during startup and pass that object to `c.staticFile(...)`; the root path is canonicalized once before workers run.

`HttpResponse` separates borrowed memory bodies, owned arena bodies, and file-token bodies:

- Use `HttpResponse::setBodyCopy(...)` when manually building a response from temporary data.
- Use `setBodyView(...)` only for data that remains valid through response write-out.
- Use `setBodyOwned(std::pmr::string&&)` for request-arena owned output.
- Build dynamic text in a non-const `std::pmr::string` with `c.allocator<char>()`, then return it as `c.text(body)` so the response consumes the arena string.
- Use `c.text(std::string_view)` only as a borrowed-view shortcut for stable data such as string literals.
- `c.text(std::string)` and `c.text(char*)` are intentionally unavailable.

Static file policy is also startup-owned. `ruvia::StaticRootOptions` supports a shared `Cache-Control` value, directory index file, custom MIME types, fallback content type, a Drogon-like file type allowlist, and per-root switches for Range handling and validators. By default, Ruvia only indexes common web static extensions such as `html`, `js`, `css`, `json`, `png`, `jpg`, `svg`, `webp`, `ico`, `txt`, `wasm`, `woff`, and `woff2`; set `allowAll = true` only for roots that should expose every regular file. Custom MIME mappings do not automatically allow a type, so add custom extensions to `fileTypes` too:

```cpp
ruvia::StaticRootOptions staticOptions;
staticOptions.cacheControl = "public, max-age=3600";
staticOptions.indexFile = "index.html";
staticOptions.mimeTypes.push_back({".ruvia", "application/x-ruvia"});
staticOptions.fileTypes.push_back("ruvia");
staticOptions.enableRanges = true;
staticOptions.enableValidators = true;

ruvia::StaticRoot assets("public", std::move(staticOptions));
```

For Drogon-like static hosting, configure a document root on the app. Controller routes still win first; only unmatched `GET` requests fall through to the document root. Ruvia serves existing files and directory index pages; missing files keep the normal 404 path, so applications can still define `RUVIA_GET("/*", ...)` for redirects or a custom home-page catch-all:

```cpp
ruvia::DocumentRootConfig documentRoot;
documentRoot.root = "dist";
documentRoot.staticOptions.indexFile = "index.html";
documentRoot.staticOptions.cacheControl = "public, max-age=3600";

ruvia::app()
    .setDocumentRoot(std::move(documentRoot))
    .run();
```

Ordinary request bodies are lazy: Ruvia dispatches middleware and handlers after headers, and reads the body only when code explicitly awaits `c.req().text()`, `c.req().arrayBuffer()`, `c.req().blob()`, `c.req().json<T>()`, `c.req().form<T>()`, `c.req().multipart()`, `c.req().parseBody()`, `c.req().formData()`, or `c.req().discardBody()`. If a request declares a body and the route returns without consuming or discarding it, Ruvia closes the connection instead of draining bytes just to preserve keep-alive. `Expect: 100-continue` is answered only when body reading actually starts, so middleware can reject large uploads without encouraging the client to send the body.

Streaming request bodies are opt-in per route and keep large uploads out of buffered memory:

```cpp
RUVIA_POST_STREAM("/upload", upload);

ruvia::Task<ruvia::HttpResponse> upload(ruvia::Context& c) {
    std::pmr::string body(c.allocator<char>());
    auto& reader = c.req().bodyReader();
    while (auto chunk = co_await reader.read()) {
        body.append(chunk->data(), chunk->size());
    }
    co_return c.text(body);
}
```

Chunks returned by `BodyReader::read()` are `std::string_view`s valid until the next `read()` call. Use the request arena if a chunk needs to outlive that read step.

Streaming responses are also explicit and bypass normal response-body buffering:

- `c.stream()`, `c.streamText()`, and `c.streamSSE()` mirror Hono streaming helper names.
- `RUVIA_GET_STREAM(...)` sends HTTP/1.1 chunked data or HTTP/2 DATA frames depending on the connection protocol.
- `RUVIA_GET_SSE(...)` sets `Content-Type: text/event-stream` and formats SSE frames with `writeSSE(...)`.
- Streaming route macros accept the same middleware arguments as ordinary routes.
- Middleware can set status/headers before `next()`, mutate headers after `next()`, or short-circuit by assigning `c.res(response)`.
- Post-`next()` response mutations do not change an already committed stream.
- Set status and headers before the first `write()` because the response head is committed on the first chunk.
- `HEAD` does not implicitly run a streaming `GET` handler.

```cpp
RUVIA_GET_STREAM("/chunks", chunks, AuthMiddleware);
RUVIA_GET_SSE("/events", events, AuthMiddleware);

ruvia::Task<void> chunks(ruvia::Context& c) {
    auto& stream = c.streamText();
    co_await stream.write("part 1\n");
    co_await stream.write("part 2\n");
}

ruvia::Task<void> events(ruvia::Context& c) {
    auto stream = c.streamSSE();
    co_await stream.writeSSE({.data = "hello", .event = "message"});
    co_await stream.writeSSE({.data = "heartbeat", .event = "heartbeat"});
}
```

Dynamic-response routes let a single handler decide buffered-vs-streamed at runtime. `RUVIA_GET_DYNAMIC(...)` and `RUVIA_POST_DYNAMIC(...)` register a handler that returns `ruvia::Task<ruvia::HttpResponse>` like a buffered route, but the handler may instead stream through `c.stream()` or `c.streamSSE()`. Whichever it does is honored: if it commits a stream, the returned `HttpResponse` is ignored; otherwise the returned response is sent buffered. This is the basis for content negotiation (the same route serving buffered JSON or an SSE stream) and the MCP Streamable HTTP response shape. `RUVIA_POST_DYNAMIC` additionally exposes the buffered request body, so one handler can read a JSON-RPC body and answer either `application/json` or `text/event-stream`:

```cpp
RUVIA_POST_DYNAMIC("/mcp", mcp);

ruvia::Task<ruvia::HttpResponse> mcp(ruvia::Context& c) {
    const auto request = co_await c.req().json<RpcRequest>();
    if (c.req().header("Accept") == "text/event-stream") {
        auto stream = c.streamSSE();
        co_await stream.writeSSE({.data = "{\"echo\":true}", .event = "message"});
        co_return c.text("");  // ignored: the stream is the response
    }
    RpcResponse response(c);
    response.echo(ruvia::Bool{true});
    co_return c.json(response);
}
```

Because the dynamic dispatch reuses the shared response-stream path, HTTP/1.1 and HTTP/2 behave identically.

WebSocket routes use an explicit upgrade macro and a connection-local handle. The implementation supports RFC 6455 HTTP/1.1 upgrade and RFC 8441 HTTP/2 extended CONNECT, text/binary messages, ping/pong, close frames, and a WebSocket-specific timeout phase that uses idle timeout instead of request-body timeout.

Do not store the request-local `WebSocket` object outside the handler lifetime.

```cpp
RUVIA_GET_WS("/ws", websocket);
RUVIA_GET_WS("/chat", chat);

ruvia::Task<void> websocket(ruvia::Context& c) {
    auto& ws = c.webSocket();
    while (auto message = co_await ws.read()) {
        if (message->text()) {
            co_await ws.text(message->payload);
        }
    }
}
```

Use `RUVIA_GET_WS_OPTIONS(path, handler, options, Middleware...)` to attach a `ruvia::WebSocketRouteOptions` value declaring negotiated subprotocols and a heartbeat ping/pong policy. The options value is evaluated where the macro is expanded, so it can be a local declared inside `RUVIA_ROUTES_BEGIN`:

```cpp
RUVIA_ROUTES_BEGIN
const auto chatOptions = ruvia::WebSocketRouteOptions{
    .subprotocols = "chat.v1",
    .heartbeat = {
        .pingInterval = std::chrono::seconds(30),
        .pongTimeout = std::chrono::seconds(10),
    },
};
RUVIA_GET_WS("/echo", echo);
RUVIA_GET_WS_OPTIONS("/chat", chat, chatOptions);
RUVIA_ROUTES_END
```

`ruvia::Task<T>` is Ruvia's coroutine type, not an alias for `asio::awaitable<T>`. It is a single-shot task, preserves exceptions through `co_await`, and resumes the awaiting coroutine from `final_suspend`. Use `co_await reader.read()` and `co_await next()` for temporary tasks in public code; if a task is stored in a local variable, await it with `co_await std::move(task)`. Public API does not expose `.asAwaitable()` and there is no conversion to `asio::awaitable<T>`; Asio bridging is an internal server/test boundary through `src/runtime/AsioAwait.h`.

Streaming multipart uploads are also explicit:

```cpp
RUVIA_POST_STREAM("/upload", uploadMultipart);

ruvia::Task<ruvia::HttpResponse> uploadMultipart(ruvia::Context& c) {
    auto reader = c.req().multipartReader();
    while (auto part = co_await reader.read()) {
        // part->name / filename / contentType / body are valid until the next reader.read().
    }
    co_return c.text("ok\n");
}
```

File and static responses do not read the full file into memory. Plain TCP responses use the platform zero-copy path when available (`sendfile` on Linux and `TransmitFile` on Windows); fallback paths write the response head plus 64KB file chunks. Plain `c.file(...)` responses add validators and range support by default. `c.staticFile(...)` uses the startup-built `StaticRootOptions`, so a static root can opt out of `ETag` / `Last-Modified` validators or Range handling while still using the same file writer semantics.

## Request and Response Models

`RUVIA_MODEL` is Ruvia's schema entry point for request bodies and JSON responses. Field declarations carry only the model type and model options such as defaults or JSON emission behavior; request validation is declared directly inside the route middleware:

```cpp
RUVIA_MODEL(LoginRequest,
    RUVIA_FIELD(username, ruvia::String),
    RUVIA_FIELD(password, ruvia::String)
);

RUVIA_MODEL(LoginResponse,
    RUVIA_FIELD(username, ruvia::String),
    RUVIA_FIELD(ok, ruvia::Bool)
);

ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& c) {
    const auto request = co_await c.req().json<LoginRequest>();
    const auto& username = request.username();
    const auto& password = request.password();
    if (!username || !password) {
        co_return c.error(400, "invalid_login_body", "username and password are required");
    }

    LoginResponse response(c);
    response
        .username(username->view())
        .ok(ruvia::Bool{true});
    co_return c.json(response);
}
```

Response JSON should be built through response DTOs and `c.json(response)`, not by manually concatenating JSON strings in handlers. Ruvia serializes `RUVIA_MODEL` fields with its internal schema-driven writer, including field names, string escaping, arrays/lists, nested models, null emission, and empty-field omission.

Model fields are intentionally restricted to Ruvia model types: `ruvia::String`, `ruvia::Array<T>`, `ruvia::List<T>`, nested `RUVIA_MODEL` types, and Ruvia scalar wrappers such as `ruvia::Bool`, `ruvia::Int32`, `ruvia::UInt64`, `ruvia::Float`, and `ruvia::Double`. Raw `bool`, integer, floating-point, `std::string`, `std::string_view`, `std::vector`, and `std::pmr::string` are not supported as model field types. This keeps request/response ownership inside Ruvia's arena-backed memory model.

`ruvia::String` keeps a zero-copy view when possible and decodes or copies into the request arena only when needed. Raw model reads keep missing fields and type mismatches as `std::nullopt`; route validator middleware can enforce field rules before the handler runs and reports precise type messages such as `must be a string`, `must be an array`, or `must be an object`. Fields are read with dot syntax such as `request.username()`, and response models are populated with the same field name as a setter, such as `response.username(...)`.
Model macros can be declared in an application namespace; Ruvia discovers their generated metadata through the model type rather than a global namespace specialization.

Use Hono-style validator middleware when a route needs typed body validation. The validator middleware is part of the route chain, invalid input throws `ruvia::ValidationError`, and the server layer converts it to JSON `400` with a `details` array:

```cpp
class LoginValidator final : public ruvia::Middleware<LoginValidator> {
    RUVIA_VALIDATE_JSON(LoginRequest,
        RUVIA_RULE(username,
            RUVIA_REQUIRED("username is required"),
            RUVIA_MIN(3, "username is too short")),
        RUVIA_RULE(password,
            RUVIA_REQUIRED("password is required"),
            RUVIA_MIN(8, "password is too short")))
};

class AuthController final : public ruvia::Controller<AuthController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/login", login, LoginValidator);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& c) {
        const auto& request = c.req().valid<LoginRequest>();
        std::pmr::string body(c.allocator<char>());
        body.append(request.username()->view());
        co_return c.text(body);
    }
};
```

`RUVIA_VALIDATE_FORM(Body, rules...)` validates `application/x-www-form-urlencoded` bodies and exposes the result through `c.req().valid<Body>("form")`. `RUVIA_VALIDATE_QUERY(Body, rules...)` applies the same flat model parser to the URL query string and exposes the result through `c.req().valid<Body>("query")`. `RUVIA_VALIDATE_PARAM(Body, rules...)`, `RUVIA_VALIDATE_HEADER(Body, rules...)`, and `RUVIA_VALIDATE_COOKIE(Body, rules...)` validate route parameters, request headers, and cookies through `"param"`, `"header"`, and `"cookie"` targets. The `ruvia::Form` / `ruvia::Query` / `ruvia::Param` / `ruvia::Header` / `ruvia::Cookie` enum constants remain available for C++ code that prefers compile-time target names.

Model field options support `RUVIA_DEFAULT(value)`, `RUVIA_OMIT_EMPTY`, and `RUVIA_EMIT_NULL`. `RUVIA_FIELD_NAME("wire_name", field, type, options...)` maps a JSON/form wire name to a C++ field getter/setter, while `RUVIA_RULE_NAME("wire_name", field, rules...)` maps validation errors to that wire name. Field parse state is tracked explicitly, so type mismatches and duplicate known fields are reported by validator middleware as field issues instead of requiring a second body scan. Recursive JSON parsing is depth-limited to protect the request path from excessive nesting.

Validator rules are declared inside validation middleware macros such as `RUVIA_VALIDATE_JSON`, `RUVIA_VALIDATE_FORM`, `RUVIA_VALIDATE_QUERY`, `RUVIA_VALIDATE_PARAM`, `RUVIA_VALIDATE_HEADER`, and `RUVIA_VALIDATE_COOKIE`:

| Rule | Applies to | Issue code | Notes |
| --- | --- | --- | --- |
| `RUVIA_REQUIRED(message)` | Any model field | `required` | Fails when the field is missing. Invalid types are reported separately as `invalid_type`. |
| `RUVIA_MIN(value, message)` | `ruvia::String`, `ruvia::Array<T>`, `ruvia::List<T>`, numeric Ruvia scalars | `too_small` | Uses length for strings/arrays/lists and numeric comparison for numbers. |
| `RUVIA_MAX(value, message)` | `ruvia::String`, `ruvia::Array<T>`, `ruvia::List<T>`, numeric Ruvia scalars | `too_big` | Uses length for strings/arrays/lists and numeric comparison for numbers. |
| `RUVIA_ONE_OF(message, "a", "b", ...)` | String-like fields | `one_of` | Compile-time allowed string set. |
| `RUVIA_EMAIL(message)` | String-like fields | `email` | Lightweight email-shape check for hot paths. |
| `RUVIA_PATTERN(message, "^[a-z]+$")` | String-like fields | `pattern` | Compile-time checked lightweight full-match pattern plan. |
| `RUVIA_REGEX(message, "^(foo|bar)$")` | String-like fields | `regex` | Full `std::regex` validation. |
| `RUVIA_MATCH(message, predicate)` | String-like fields | `match` | Predicate receives `std::string_view`. |
| `RUVIA_CUSTOM(message, predicate)` | Any parsed field type | `custom` | Predicate receives the typed Ruvia field value. |
| `RUVIA_NESTED(Validator)` | Nested `RUVIA_MODEL` field | Validator-defined codes | Validates a present nested object with another validator middleware. |
| `RUVIA_EACH(Validator)` | `ruvia::Array<T>` or `ruvia::List<T>` | Validator-defined codes | Validates each present item with indexed paths such as `roles[0].name`. |

`RUVIA_PATTERN` is the preferred hot-path string format rule; use `RUVIA_REGEX` only when you explicitly need full `std::regex` syntax, or `RUVIA_MATCH` for custom `std::string_view` matchers.

```cpp
bool validCode(const ruvia::String& code) {
    return code.view().starts_with("CY-");
}

RUVIA_MODEL(Account,
    RUVIA_FIELD_NAME("user_name", username, ruvia::String,
        RUVIA_DEFAULT("guest")),
    RUVIA_FIELD(email, ruvia::String),
    RUVIA_FIELD(code, ruvia::String),
    RUVIA_FIELD(slug, ruvia::String),
    RUVIA_FIELD(nickname, ruvia::String,
        RUVIA_OMIT_EMPTY),
    RUVIA_FIELD(optionalText, ruvia::String,
        RUVIA_EMIT_NULL)
);

class AccountValidator final : public ruvia::Middleware<AccountValidator> {
    RUVIA_VALIDATE_JSON(Account,
        RUVIA_RULE_NAME("user_name", username,
            RUVIA_PATTERN("username format is invalid", "^[a-z][a-z0-9_]*$")),
        RUVIA_RULE(email,
            RUVIA_EMAIL("email format is invalid")),
        RUVIA_RULE(code,
            RUVIA_CUSTOM("code must use CY- prefix", validCode)),
        RUVIA_RULE(slug,
            RUVIA_MATCH("slug must not contain spaces", [](std::string_view value) {
                return value.find(' ') == std::string_view::npos;
            })))
};
```

```cpp
RUVIA_MODEL(ProfileRequest,
    RUVIA_FIELD(displayName, ruvia::String),
    RUVIA_FIELD(age, ruvia::Int64)
);

class ProfileValidator final : public ruvia::Middleware<ProfileValidator> {
    RUVIA_VALIDATE_JSON(ProfileRequest,
        RUVIA_RULE(displayName,
            RUVIA_REQUIRED("display name is required"),
            RUVIA_MIN(2, "display name is too short")),
        RUVIA_RULE(age,
            RUVIA_MIN(0, "age is too small"),
            RUVIA_MAX(130, "age is too big")))
};

RUVIA_MODEL(RoleRequest,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(level, ruvia::Int64)
);

class RoleValidator final : public ruvia::Middleware<RoleValidator> {
    RUVIA_VALIDATE_JSON(RoleRequest,
        RUVIA_RULE(name,
            RUVIA_REQUIRED("role is required"),
            RUVIA_ONE_OF("role is not allowed", "admin", "user", "editor")),
        RUVIA_RULE(level,
            RUVIA_MIN(1, "level is too small"),
            RUVIA_MAX(10, "level is too big")))
};

RUVIA_MODEL(RegisterRequest,
    RUVIA_FIELD(username, ruvia::String),
    RUVIA_FIELD(profile, ProfileRequest),
    RUVIA_FIELD(roles, ruvia::Array<RoleRequest>),
    RUVIA_FIELD(tags, ruvia::Array<ruvia::String>)
);

class RegisterValidator final : public ruvia::Middleware<RegisterValidator> {
    RUVIA_VALIDATE_JSON(RegisterRequest,
        RUVIA_RULE(username,
            RUVIA_REQUIRED("username is required"),
            RUVIA_MIN(3, "username is too short"),
            RUVIA_MAX(32, "username is too long")),
        RUVIA_RULE(profile,
            RUVIA_REQUIRED("profile is required"),
            RUVIA_NESTED(ProfileValidator)),
        RUVIA_RULE(roles,
            RUVIA_REQUIRED("at least one role is required"),
            RUVIA_MIN(1, "too few roles"),
            RUVIA_MAX(5, "too many roles"),
            RUVIA_EACH(RoleValidator)),
        RUVIA_RULE(tags,
            RUVIA_MIN(1, "too few tags"),
            RUVIA_MAX(8, "too many tags")))
};
```

Recursive JSON trees use `ruvia::List<T>`, which stores child objects in the request arena:

```cpp
RUVIA_MODEL(Category,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(children, ruvia::List<Category>)
);

class CategoryValidator final : public ruvia::Middleware<CategoryValidator> {
    RUVIA_VALIDATE_JSON(Category,
        RUVIA_RULE(name, RUVIA_REQUIRED("name is required")),
        RUVIA_RULE(children, RUVIA_EACH(CategoryValidator)))
};

Category root(c);
root.name("root");
root.children().emplace().name("leaf");
```

Forms intentionally remain flat and are best paired with string, number, and boolean field rules; nested object and array rules are for JSON request models.

Configure a custom error handler when an application wants to shape framework errors, thrown `ruvia::HttpError`, validation failures, or parser errors:

```cpp
ruvia::Task<ruvia::HttpResponse> errors(ruvia::Context& c, ruvia::HttpErrorInfo error) {
    co_return c.status(error.statusCode)
        .setHeader("X-Error-Code", error.code)
        .json(ErrorResponse{.code = error.code, .message = error.message});
}

ruvia::app()
    .setErrorHandler(&errors)
    .run();
```

## Macro Reference

Ruvia's public API is a compile-time macro DSL. Every macro expands to startup-time descriptors or generated class members �?there is no runtime reflection and no per-request macro cost. This section is a single catalog of the public macros; the sections above show each one in context.

### Controller structure

| Macro | Purpose |
| --- | --- |
| `RUVIA_ROUTES_BEGIN` / `RUVIA_ROUTES_END` | Open and close the route-registration block inside a `ruvia::Controller<T>`. `RUVIA_ROUTES_END` also performs static controller registration. |
| `RUVIA_CONTROLLER_GROUP(prefix, Middleware...)` | Declare the controller-wide path prefix and controller-level middleware, so a controller can be split across files. |
| `RUVIA_GROUP_BEGIN(prefix, Middleware...)` / `RUVIA_GROUP_END` | Open and close a nested route group with its own prefix and scoped middleware. Groups nest. |

### HTTP method routes

Each macro takes `(path, handler, Middleware...)`, registers a buffered route whose handler returns `ruvia::Task<ruvia::HttpResponse>`, and accepts `:param` and `*` wildcard path segments.

| Macro | Method |
| --- | --- |
| `RUVIA_GET` | `GET` |
| `RUVIA_POST` | `POST` |
| `RUVIA_PUT` | `PUT` |
| `RUVIA_PATCH` | `PATCH` |
| `RUVIA_DELETE` | `DELETE` |
| `RUVIA_HEAD` | `HEAD` (explicit; otherwise `GET` is reused for `HEAD`) |
| `RUVIA_OPTIONS` | `OPTIONS` (explicit; distinct from framework-generated `OPTIONS`) |

### Streaming, SSE, and WebSocket routes

| Macro | Purpose |
| --- | --- |
| `RUVIA_GET_STREAM(path, handler, Middleware...)` | Chunked / HTTP/2 DATA response stream; handler returns `ruvia::Task<void>` and writes through `c.stream()` / `c.streamText()`. |
| `RUVIA_GET_SSE(path, handler, Middleware...)` | Server-Sent Events stream via `c.streamSSE()`; sets `Content-Type: text/event-stream`. |
| `RUVIA_POST_STREAM` / `RUVIA_PUT_STREAM` / `RUVIA_PATCH_STREAM` | Streaming **request body** routes; handler reads chunk by chunk through `c.req().bodyReader()` / `c.req().multipartReader()`. |
| `RUVIA_GET_WS(path, handler, Middleware...)` | WebSocket upgrade route (RFC 6455 over HTTP/1.1, RFC 8441 over HTTP/2); handler uses `c.webSocket()`. |
| `RUVIA_GET_WS_OPTIONS(path, handler, options, Middleware...)` | WebSocket route with a `ruvia::WebSocketRouteOptions` value (subprotocols, heartbeat ping/pong). |

### Dynamic-response routes

| Macro | Purpose |
| --- | --- |
| `RUVIA_GET_DYNAMIC(path, handler, Middleware...)` | Handler returns `ruvia::Task<ruvia::HttpResponse>` but may instead stream via `c.stream()` / `c.streamSSE()`; whichever it does at runtime is honored. |
| `RUVIA_POST_DYNAMIC(path, handler, Middleware...)` | As above for `POST`, with the buffered request body available (content negotiation, MCP Streamable HTTP). |

### Model schema

| Macro | Purpose |
| --- | --- |
| `RUVIA_MODEL(Type, fields...)` | Generate a request/response model class from `RUVIA_FIELD` declarations: typed getters/setters, JSON/form parsing, and schema-driven JSON serialization. |
| `RUVIA_FIELD(field, ModelType, options...)` | Declare a field; the wire name defaults to the field name. |
| `RUVIA_FIELD_NAME("wire_name", field, ModelType, options...)` | Declare a field whose JSON/form wire name differs from the C++ getter/setter. |

Field model types are Ruvia model types only: `ruvia::String`, `ruvia::Array<T>`, `ruvia::List<T>`, scalar wrappers (`ruvia::Bool`, `ruvia::Int32`, `ruvia::Int64`, `ruvia::UInt64`, `ruvia::Float`, `ruvia::Double`), and nested `RUVIA_MODEL` types.

### Field options (inside `RUVIA_FIELD` / `RUVIA_FIELD_NAME`)

| Macro | Purpose |
| --- | --- |
| `RUVIA_DEFAULT(value)` | Default applied when the field is absent from the parsed body. |
| `RUVIA_OMIT_EMPTY` | Omit the field from serialized JSON when it is empty. |
| `RUVIA_EMIT_NULL` | Emit `null` for the field instead of omitting it. |

### Validation entry points (inside a `ruvia::Middleware<T>`)

| Macro | Purpose |
| --- | --- |
| `RUVIA_VALIDATE_JSON(Body, rules...)` | Validate a JSON body; the result is exposed through `c.req().valid<Body>()`. |
| `RUVIA_VALIDATE_FORM(Body, rules...)` | Validate an `application/x-www-form-urlencoded` body; result through `c.req().valid<Body>("form")`. |
| `RUVIA_VALIDATE_QUERY(Body, rules...)` | Validate URL query parameters with the flat form-model parser; result through `c.req().valid<Body>("query")`. |
| `RUVIA_VALIDATE_PARAM(Body, rules...)` | Validate matched route parameters; result through `c.req().valid<Body>("param")`. |
| `RUVIA_VALIDATE_HEADER(Body, rules...)` | Validate request headers using lowercase header names; result through `c.req().valid<Body>("header")`. |
| `RUVIA_VALIDATE_COOKIE(Body, rules...)` | Validate request cookies; result through `c.req().valid<Body>("cookie")`. |
| `RUVIA_VALIDATE_BODY(target, Body, rules...)` | Lower-level form of the two above with an explicit `ruvia::ValidationTarget`. |
| `RUVIA_RULE(field, rules...)` | Bind a rule set to a model field; issues are reported under the field's wire name. |
| `RUVIA_RULE_NAME("wire_name", field, rules...)` | Bind rules and report issues under a custom wire name. |

### Validation rules (inside `RUVIA_RULE` / `RUVIA_RULE_NAME`)

| Macro | Applies to | Issue code |
| --- | --- | --- |
| `RUVIA_REQUIRED(message)` | Any field | `required` |
| `RUVIA_MIN(value, message)` | strings, arrays, lists, numeric scalars | `too_small` |
| `RUVIA_MAX(value, message)` | strings, arrays, lists, numeric scalars | `too_big` |
| `RUVIA_ONE_OF(message, "a", "b", ...)` | string-like | `one_of` |
| `RUVIA_EMAIL(message)` | string-like | `email` |
| `RUVIA_PATTERN(message, "^[a-z]+$")` | string-like | `pattern` (compile-time, hot-path) |
| `RUVIA_REGEX(message, "^(foo\|bar)$")` | string-like | `regex` (full `std::regex`) |
| `RUVIA_MATCH(message, predicate)` | string-like | `match` (`std::string_view` predicate) |
| `RUVIA_CUSTOM(message, predicate)` | any parsed field | `custom` (typed value predicate) |
| `RUVIA_NESTED(Validator)` | nested `RUVIA_MODEL` field | validator-defined |
| `RUVIA_EACH(Validator)` | `ruvia::Array<T>` / `ruvia::List<T>` | validator-defined |

The version header `ruvia/version.h` also exposes `RUVIA_VERSION_MAJOR`, `RUVIA_VERSION_MINOR`, `RUVIA_VERSION_PATCH`, and `RUVIA_VERSION_STRING`.

## Configuration

Load `.env` during startup with the chainable `App` helper. Without an explicit path, Ruvia reads `.env` from the executable directory:

```cpp
ruvia::app()
    .loadDotenv()
    .setListenAddress("0.0.0.0", 8080)
    .setThreadNum(2)
    .setIdleTimeout(std::chrono::seconds(60))
    .setHeaderTimeout(std::chrono::seconds(15))
    .setBodyTimeout(std::chrono::seconds(30))
    .setWriteTimeout(std::chrono::seconds(30))
    .setMaxConnectionsPerWorker(10000)
    .setMaxRequestsPerConnection(1000)
    .run();
```

`setListenAddress(address, port)` configures the HTTP listener; app-managed listener ports must be non-zero. HTTPS has its own startup-only port:
calling `setHttpsListenPort(port)` declares the HTTPS listener, and `useTls(...)` supplies its certificate and key.

Each timeout governs exactly one phase: `headerTimeout` is the request-header read window (TLS handshake included), `bodyTimeout` is the request-body read window, `writeTimeout` is the response write window. `idleTimeout` covers both keep-alive idle time **and the dispatch (handler) phase** �?once the body has been read, the connection scanner classifies the connection as idle until writing starts, so `idleTimeout` serves as the deadman switch for hung handlers. To bound business-logic runtime, use `idleTimeout` or in-handler cancellation �?`headerTimeout` / `bodyTimeout` no longer leak into dispatch. Any timeout set to `0ms` is disabled; `setConnectionScanInterval(...)` is a scanner cadence and must be greater than zero.

Memory pool configuration is also startup-only. Set it before `run()` and before creating any `ruvia::WorkerMemory`; the process memory layer freezes as workers are created:

```cpp
ruvia::MemoryPoolConfig memory;
memory.requestInitialBufferBytes = 4096;

ruvia::app()
    .setMemoryPoolConfig(memory)
    .run();
```

Read values from the app-owned environment store:

```cpp
const auto name = ruvia::app().env().get("RUVIA_EXAMPLE_NAME");
if (!name) {
    // std::nullopt when the key is missing
}

const auto port = ruvia::app().env().get<std::uint16_t>("RUVIA_EXAMPLE_PORT").value_or(8080);
const auto debug = ruvia::app().env().get<bool>("RUVIA_EXAMPLE_DEBUG").value_or(false);
```

Typed reads support `std::string_view`, `bool`, integral types, and floating-point types. Conversion failures return `std::nullopt`, the same as a missing key.

Missing `.env` files are ignored by default. Existing loaded values are preserved unless `overrideExisting` is enabled.

CORS is configured at startup and applied by the server after route handling:

```cpp
ruvia::app()
    .setCors(ruvia::CorsConfig{
        .enabled = true,
        .allowOrigin = "https://example.com",
        .allowHeaders = "content-type, authorization",
        .maxAge = std::chrono::seconds(600)})
    .run();
```

## Database Access

This section is available only when Ruvia is built with `RUVIA_ENABLE_MARIADB=ON` or the vcpkg `mariadb` feature. In core-only builds, `include/ruvia/db/Db.h`, `App::useDb(...)`, and `Context::db(...)` are not installed or exposed.

Database clients are registered during startup and read through `c.db()` inside handlers. Ruvia's database layer is fast-only: every worker owns its own database pool on the same `io_context` as HTTP I/O, and only asynchronous `ruvia::Task` APIs are exposed.

```cpp
ruvia::DbConfig config;
config.host = "127.0.0.1";
config.port = 3306;
config.username = "ruvia";
config.password = "secret";
config.database = "ruvia";
config.acquireTimeout = std::chrono::seconds(2);
config.connectTimeout = std::chrono::seconds(5);
config.queryTimeout = std::chrono::seconds(30);

ruvia::app()
    .useDb(std::move(config))
    .run();
```

```cpp
static constexpr std::string_view kFindUserQuery =
    "SELECT id, name FROM users WHERE id = ?";

ruvia::Task<ruvia::HttpResponse> findUser(ruvia::Context& c) {
    auto result = co_await c.db().query(kFindUserQuery, {c.req().param("id").toStringView().value_or("")});
    (void)result;
    co_return c.text("user query completed\n");
}
```

`DbConfig::poolSize` is the number of connections per worker for one database alias. For example, `setThreadNum(4)` and `poolSize = 2` creates up to eight backend connections for that alias. Use `ruvia::app().useDb("analytics", config)` and `c.db("analytics")` only when an application needs multiple named database pools.
Parameterized `query(std::string_view, params)` and `execute(std::string_view, params)` calls use `?` placeholders and run through per-connection prepared statement caches. Use `query(...)` for statements where row data matters, and `execute(...)` for writes, DDL, and other statements where callers primarily need `affectedRows()` or `lastInsertId()`. SQL text and parameter values are copied into the request arena before the `ruvia::Task` is returned, so temporary inputs remain safe across async suspension. `DbConfig::statementCacheSize` controls the number of prepared statements kept by each connection slot; set it to `0` to disable caching.

Schema migrations are an explicit startup/operations path, separate from request-time worker pools:

```cpp
ruvia::DbConfig config;
config.host = "127.0.0.1";
config.username = "ruvia";
config.password = "secret";
config.database = "ruvia";

const ruvia::DbMigration migrations[] = {
    {"001_create_users",
        "CREATE TABLE users (id BIGINT PRIMARY KEY, name VARCHAR(120) NOT NULL)"}
};

auto report = ruvia::DbMigrator::migrate(config, migrations);

ruvia::app()
    .useDb(std::move(config))
    .run();
```

`DbMigrator` uses a temporary single-connection registry, creates `ruvia_schema_migrations` by default, skips already applied migration ids, and takes a MariaDB `GET_LOCK` while it runs. The migration table has an auto-increment `id`, a unique `migration_id`, and `applied_at`. Each migration entry is one SQL statement; keep larger changes as ordered entries with stable ids. Migration failures are rethrown after cleanup; Ruvia does not mark a failed migration as applied.

Large result sets can be consumed one row at a time with `queryStream(...)`:

```cpp
ruvia::Task<ruvia::HttpResponse> listNames(ruvia::Context& c) {
    auto stream = co_await c.db().queryStream("SELECT name FROM users");
    std::pmr::string body(c.allocator<char>());
    while (auto row = co_await stream.read()) {
        body.append((*row)[0].text());
        body.push_back('\n');
    }
    co_return c.text(body);
}
```

Dropping an active stream closes its connection slot before returning it to the pool, so unread result bytes cannot leak into the next request.

`DbHandle::beginTransaction()` pins one database connection slot to the returned move-only `ruvia::DbTransaction` until `commit()` or `rollback()` completes. Dropping an active transaction closes that slot's connection before returning it to the worker-local pool, so an uncommitted transaction cannot leak into the next request:

```cpp
ruvia::Task<ruvia::HttpResponse> transfer(ruvia::Context& c) {
    auto tx = co_await c.db().beginTransaction();
    co_await tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", {100, 1});
    co_await tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", {100, 2});
    co_await tx.commit();
    co_return c.text("ok\n");
}
```

The supported backend is MariaDB-compatible access through MariaDB Connector/C.
Database errors are not swallowed: MariaDB query, prepare, execute, fetch, transaction, stream, and migration failures throw exceptions with the operation name plus MariaDB errno/sqlstate/message. Cleanup errors during migration lock release are only suppressed when an earlier migration failure already exists, so the original failure remains the reported error.
Database `connectTimeout`, `readTimeout`, `writeTimeout`, `queryTimeout`, and `acquireTimeout` are startup configuration fields; `0ms` keeps that timeout disabled.

## Redis and JWT Helpers

Redis support is available only when Ruvia is built with `RUVIA_ENABLE_REDIS=ON` or the vcpkg `redis` feature. JWT helpers are available only with `RUVIA_ENABLE_JWT=ON` or the vcpkg `jwt` feature. In core-only builds, `include/ruvia/redis/Redis.h`, `include/ruvia/auth/Jwt.h`, `App::useRedis(...)`, and `Context::redis(...)` are not installed or exposed.

Redis clients are registered at startup and are accessed through `c.redis()` or `c.redis("alias")`. Like DB pools, Redis pools are worker-local and run on the worker's own `io_context`.

```cpp
ruvia::RedisConfig redis;
redis.host = "127.0.0.1";
redis.port = 6379;
redis.commandTimeout = std::chrono::seconds(2);

ruvia::app()
    .useRedis(std::move(redis))
    .run();
```

```cpp
ruvia::Task<ruvia::HttpResponse> cacheValue(ruvia::Context& c) {
    co_await c.redis().set("ruvia:key", "value");
    auto value = co_await c.redis().get("ruvia:key");
    co_return c.text(value.value_or(""));
}
```

`RedisHandle` exposes typed helpers for common string, hash, list, set, sorted-set, scan, script, pipeline, transaction, and blocking-pop operations. `RedisConfig::poolSizePerWorker` is per worker; `connectTimeout`, `commandTimeout`, and `acquireTimeout` are startup configuration fields, with `0ms` meaning disabled.

JWT helpers are low-level HMAC signing and verification utilities. They do not add session state or implicit auth middleware; applications decide where to store secrets and how to attach middleware:

```cpp
ruvia::JwtSignOptions sign;
sign.secret = "secret";
sign.subject = "user-1";
auto token = ruvia::jwtSign(sign, std::pmr::get_default_resource());

ruvia::JwtVerifyOptions verify;
verify.secret = "secret";
auto payload = ruvia::jwtVerify(token, verify, std::pmr::get_default_resource());
```

## HTTPS and Compression

HTTPS/TLS is built into the normal Ruvia runtime and does not use a separate build toggle. Configure certificate and key files before `run()`:

```cpp
ruvia::app()
    .setListenAddress("0.0.0.0")
    .setHttpListenPort(8080)
    .setHttpsListenPort(8443)
    .useTls(ruvia::TlsConfig{
        .certificateChainFile = "server.crt",
        .privateKeyFile = "server.key"})
    .setAutoHttps(true)
    .run();
```

With `setAutoHttps(true)`, the HTTP listener returns a `308 Permanent Redirect` to the configured HTTPS port and closes the connection. TLS is backed by OpenSSL, disables legacy SSL/TLS protocol versions and TLS compression, and keeps each accepted connection on its owning worker. Buffered responses can be gzip-compressed when clients send `Accept-Encoding: gzip`; file responses, response streams, SSE, and WebSocket traffic are not compressed.

## Runtime Behavior

- `App::run()` creates one internal server/acceptor/thread per worker.
- Each worker owns exactly one standalone Asio `io_context`.
- SIGINT and SIGTERM trigger graceful shutdown by closing each worker acceptor and active worker-owned sockets on that worker's `io_context`.
- Idle/header/body/write timeouts are enforced by one scanner per worker, not by per-connection timers; a `0ms` timeout disables that category.
- `setMaxConnectionsPerWorker(...)` returns `429 Too Many Requests` for excess accepted connections; `setMaxRequestsPerConnection(...)` closes keep-alive after the configured request count; `0` means unlimited.
- Buffered request body and WebSocket message limits are memory bounds and must be greater than zero. Stream body routes are explicit; `setMaxStreamBodyBytes(0)` disables only the stream body limit.
- HTTP parsing uses Ruvia's zero-copy parser; request method, path, version, headers, and common values are views into the connection read buffer by default. Chunked request bodies are decoded in place.
- Stream routes (`RUVIA_POST_STREAM`, `RUVIA_PUT_STREAM`, `RUVIA_PATCH_STREAM`) dispatch after headers and let handlers consume Content-Length or chunked bodies through `c.req().bodyReader()`.
- Buffered multipart parsing returns field views into the current request body; streaming multipart returns chunk views valid until the next `read()`.
- `Expect: 100-continue` is answered before body reads, and comma-separated `Connection` tokens are honored.
- Response construction uses request arenas and scatter-gather-friendly response data instead of building a full response string for every request. Memory bodies are either explicit borrowed views or owned arena strings; file bodies are internal path references created only by `Context`.
- Gzip compression is applied only immediately before writing ordinary buffered responses, after routing and middleware have completed.
- Plain TCP file responses use the platform zero-copy path when available; fallback file responses write headers plus 64KB file chunks.
- File response conditionals support `If-None-Match`, `If-Modified-Since`, `If-Match`, `If-Unmodified-Since`, and `If-Range` for the built-in single-range path.

## Install

Install the library and CMake package files:

```powershell
cmake --install build --config Debug --prefix install-ruvia
```

Downstream CMake projects can consume the installed package with:

```cmake
find_package(ruvia CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ruvia::ruvia)
```

The installed package intentionally exposes only `ruvia::ruvia`; enabled surfaces are compiled into that single library target instead of separate component targets.

TLS, App, and the HTTP server runtime are part of the normal build. MariaDB, Redis, and JWT are strict feature cuts: when disabled, their headers and public APIs are not installed or exposed.

Feature options:

- `RUVIA_ENABLE_MARIADB=ON` enables MariaDB-compatible DB APIs and links `libmariadb`.
- `RUVIA_ENABLE_REDIS=ON` enables Redis APIs and links `hiredis`.
- `RUVIA_ENABLE_JWT=ON` enables JWT helper APIs.

With vcpkg manifest features, request the same optional surfaces through `mariadb`, `redis`, and `jwt`.

## Current Status

The current formal release is `v0.0.6`.

This release is focused on evaluating:

- The core HTTP/1.1 and HTTP/2 server and controller API.
- Request model validation and response helpers.
- WebSocket, SSE, request streaming, and response streaming support.
- Optional MariaDB-compatible database query, transaction, and migration integration.
- HTTPS/TLS, gzip compression, optional Redis/JWT helpers, and performance-oriented memory layout.

The project is pre-1.0, so API changes are still possible when they improve hot-path safety or remove ambiguity. The implementation boundary is summarized in [Project Scope](#project-scope).
