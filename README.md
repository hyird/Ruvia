# Ruvia

Ruvia is a small, focused C++23 HTTP/1.1 web framework for core web services with a compact public API and a low-overhead request path.

## Contents

- [Highlights](#highlights)
- [Project Scope](#project-scope)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Minimal Controller](#minimal-controller)
- [Routing and Middleware](#routing-and-middleware)
- [Context Helpers](#context-helpers)
- [Request and Response Models](#request-and-response-models)
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
| Controller API | Hono-style single-argument handlers with `ruvia::Context& c` and async-only `ruvia::Task<ruvia::HttpResponse>` returns. |
| Routing | Static controller registration through macros, route groups, scoped middleware, `:param` segments, and `*` wildcards. |
| Request handling | Zero-copy HTTP parser, request views into the connection read buffer, explicit streaming body routes, chunked body decoding, multipart form parsing, and helpers for headers, query values, cookies, JSON bodies, and form bodies. |
| Responses | Chainable helpers for status, headers, cookies, redirects, text, JSON, file responses, static files with validators/ranges, configurable error handling, and unified JSON error bodies. |
| Models | `RUVIA_MODEL` schema macros for typed JSON/form bodies and JSON responses, plus inline validator middleware rules without runtime reflection. |
| Runtime | Per-worker standalone Asio `io_context`, built-in HTTPS/TLS, gzip compression, optional MariaDB/Redis/JWT feature support, graceful shutdown, centralized timeout scanning, connection limits, per-worker PMR allocators, per-request arenas, and `mimalloc` as the production upstream allocator. |
| Distribution | CMake install/export support through one installed library file and one public target: `ruvia::ruvia`. |

## Project Scope

Ruvia is intentionally a small HTTP/1.1 framework, not a full-stack application platform. The implementation boundary is the high-performance core needed to build HTTP services with explicit ownership and low request-path overhead.

In scope:

- HTTP/1.1 and built-in HTTPS server runtime, OpenSSL-backed TLS, keep-alive, pipelining safety, parser limits, timeout handling, and per-worker connection ownership.
- Hono-style controller and route macros, route groups, user-defined middleware, exact routes, `:param` routes, `*` wildcards, startup duplicate/conflict detection, and explicit `HEAD` / `OPTIONS` behavior.
- Request helpers for headers, query values, cookies, route params, lazy buffered body reads, JSON, URL-encoded forms, buffered multipart, and explicit streaming request bodies.
- Response helpers for status, headers, cookies, text, JSON, redirects, JSON errors, gzip compression for buffered responses, file/static-file responses with validators and Range support, explicit chunked response streaming, SSE, and WebSocket upgrades.
- `RUVIA_MODEL` schema macros backed by Ruvia model types, without runtime reflection or general-purpose maps.
- A performance-oriented runtime: one standalone Asio `io_context` per worker, worker-owned acceptors/sockets, PMR memory resources, request arenas, zero-copy parser views, scatter-gather-friendly response writes, and `mimalloc` as the production upstream allocator.
- Optional MariaDB-compatible DB query, transaction, and migration support through `DbHandle`, `DbTransaction`, and `DbMigrator`.
- Optional Redis command helpers, pipelines, transactions, scans, scripts, and blocking pops through worker-local pools and `Context::redis(...)`.
- Startup conveniences such as dotenv loading, CORS response headers, gzip configuration, and optional HMAC JWT helpers.

Out of scope for the current boundary:

- HTTP/2, HTTP/3, template rendering, ORM/entity modeling, session/auth batteries beyond the low-level JWT helper, background job systems, distributed WebSocket fanout, and full-stack frontend integration.
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
  -DCMAKE_TOOLCHAIN_FILE=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake
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
  -DCMAKE_TOOLCHAIN_FILE=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake
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

Applications define middleware with `ruvia::Middleware<T>` and attach it to controller groups, nested groups, or individual routes:

```cpp
class AuthMiddleware final : public ruvia::Middleware<AuthMiddleware> {
public:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        if (c.header("X-Api-Key") != "secret") {
            co_return c.error(401, "unauthorized", "unauthorized");
        }
        co_return co_await next(c);
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

Middleware instances and chains are built before workers start, so request dispatch uses prebuilt route metadata and direct thunks.

## Context Helpers

Use `ruvia::Context` to read request data and construct responses:

| Helper | Purpose |
| --- | --- |
| `c.req()` | Access the current `ruvia::HttpRequest`. |
| `c.header(name)` | Read a request header. |
| `c.query(name)` | Read a query value through `toStringView()`, `toString()`, `toInt()`, `toBool()`, and related typed helpers. |
| `c.cookie(name)` | Read a cookie value as `std::optional<std::string_view>`. |
| `c.param(name)` | Read a dynamic route parameter through the same typed helpers, including `c.param("*")` for wildcard routes. |
| `co_await c.body()` | Lazily read the full buffered request body into the request arena. |
| `co_await c.json<T>()` | Lazily read and parse a `RUVIA_MODEL` JSON body. |
| `co_await c.form<T>()` | Lazily read and parse a `RUVIA_MODEL` URL-encoded form body. |
| `co_await c.multipart()` | Lazily read and parse a buffered multipart/form-data body. |
| `co_await c.discardBody()` | Explicitly drain the request body when a route wants to keep the connection alive without using the body. |
| `c.bodyReader()` | Read an explicitly streaming request body chunk by chunk. |
| `c.multipartReader()` | Stream multipart/form-data parts from an explicitly streaming route. |
| `c.stream()` | Write an explicitly streaming chunked response from a `RUVIA_GET_STREAM(...)` route. |
| `c.streamText()` | Hono-like text streaming helper; sets `Content-Type: text/plain; charset=utf-8` and returns the stream writer. |
| `c.streamSSE()` | Hono-like Server-Sent Events helper from a `RUVIA_GET_SSE(...)` route. |
| `c.webSocket()` | Access the upgraded WebSocket connection from a `RUVIA_GET_WS(...)` route. |
| `c.status(code)` | Set the response status used by subsequent response helpers. |
| `c.setHeader(name, value)` | Add or replace a response header. |
| `c.setCookie(name, value, options)` | Append a `Set-Cookie` response header. |
| `c.text(...)` | Return a `text/plain` response. |
| `c.json(value)` | Serialize a response model as JSON. |
| `c.file(path)` | Return a file response without loading the whole file into memory. |
| `c.staticFile(staticRoot, relative)` | Return a static file under a startup-built `ruvia::StaticRoot` with traversal checks. |
| `c.redirect(location)` | Return a redirect response. |
| `c.error(status, code, message)` | Return a unified JSON error response. |
| `c.db()` / `c.db(alias)` | Access a startup-registered database handle when `RUVIA_ENABLE_MARIADB` is enabled. |
| `c.redis()` / `c.redis(alias)` | Access a startup-registered Redis handle when `RUVIA_ENABLE_REDIS` is enabled. |

A few lifetime and ownership rules are worth keeping close:

- `c.header(...)` is for reading request headers. Use `c.setHeader(...)` for response headers.
- `ruvia::HttpRequest` is a read-only request metadata view for application code. It is populated by the parser/server, and its string views point at the current connection/request buffers.
- Request body I/O lives on `Context`. Use `co_await c.body()`, `co_await c.json<T>()`, `co_await c.form<T>()`, or `co_await c.multipart()` rather than reading body data from `c.req()`.
- Response status codes, reason phrases, header names, header values, cookie names, and cookie values are validated when set. Invalid output metadata throws `std::invalid_argument` before it reaches the writer.
- File bodies are constructed through `c.file(...)` and `c.staticFile(...)`; application code should not build raw file-body responses directly.
- Create `ruvia::StaticRoot` during startup and pass that object to `c.staticFile(...)`; the root path is canonicalized once before workers run.

`HttpResponse` separates borrowed memory bodies, owned arena bodies, and file-token bodies:

- Use `HttpResponse::setBodyCopy(...)` when manually building a response from temporary data.
- Use `setBodyView(...)` only for data that remains valid through response write-out.
- Use `setBody(std::pmr::string&&)` for request-arena owned output.
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

Ordinary request bodies are lazy: Ruvia dispatches middleware and handlers after headers, and reads the body only when code explicitly awaits `c.body()`, `c.json<T>()`, `c.form<T>()`, `c.multipart()`, or `c.discardBody()`. If a request declares a body and the route returns without consuming or discarding it, Ruvia closes the connection instead of draining bytes just to preserve keep-alive. `Expect: 100-continue` is answered only when body reading actually starts, so middleware can reject large uploads without encouraging the client to send the body.

Streaming request bodies are opt-in per route and keep large uploads out of buffered memory:

```cpp
RUVIA_POST_STREAM("/upload", upload);

ruvia::Task<ruvia::HttpResponse> upload(ruvia::Context& c) {
    std::pmr::string body(c.allocator<char>());
    auto& reader = c.bodyReader();
    while (auto chunk = co_await reader.read()) {
        body.append(chunk->data(), chunk->size());
    }
    co_return c.text(body);
}
```

Chunks returned by `BodyReader::read()` are `std::string_view`s valid until the next `read()` call. Use the request arena if a chunk needs to outlive that read step.

Streaming responses are also explicit and bypass normal response-body buffering:

- `c.stream()`, `c.streamText()`, and `c.streamSSE()` mirror Hono streaming helper names.
- `RUVIA_GET_STREAM(...)` sends HTTP/1.1 chunked data.
- `RUVIA_GET_SSE(...)` sets `Content-Type: text/event-stream` and formats SSE frames with `writeSSE(...)`.
- Streaming route macros accept the same middleware arguments as ordinary routes.
- Middleware can set status/headers before `next(c)` or short-circuit with a normal `HttpResponse`.
- Post-`next(c)` response mutations do not change an already committed stream.
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

WebSocket routes use an explicit upgrade macro and a connection-local handle. The implementation supports RFC 6455 HTTP/1.1 upgrade, text/binary messages, ping/pong, close frames, and a WebSocket-specific timeout phase that uses idle timeout instead of request-body timeout.

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

`ruvia::Task<T>` is Ruvia's coroutine type, not an alias for `asio::awaitable<T>`. It is a single-shot task, preserves exceptions through `co_await`, and resumes the awaiting coroutine from `final_suspend`. Use `co_await reader.read()` and `co_await next(c)` for temporary tasks in public code; if a task is stored in a local variable, await it with `co_await std::move(task)`. Public API does not expose `.asAwaitable()` and there is no conversion to `asio::awaitable<T>`; Asio bridging is an internal server/test boundary through `src/AsioAwait.h`.

Streaming multipart uploads are also explicit:

```cpp
RUVIA_POST_STREAM("/upload", uploadMultipart);

ruvia::Task<ruvia::HttpResponse> uploadMultipart(ruvia::Context& c) {
    auto reader = c.multipartReader();
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
    const auto request = co_await c.json<LoginRequest>();
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
        const auto& request = c.valid<LoginRequest>();
        std::pmr::string body(c.allocator<char>());
        body.append(request.username()->view());
        co_return c.text(body);
    }
};
```

`RUVIA_VALIDATE_FORM(Body, rules...)` validates `application/x-www-form-urlencoded` bodies and exposes the result through `c.valid<Body>(ruvia::Form)`.

Model field options support `RUVIA_DEFAULT(value)`, `RUVIA_OMIT_EMPTY`, and `RUVIA_EMIT_NULL`. `RUVIA_FIELD_NAME("wire_name", field, type, options...)` maps a JSON/form wire name to a C++ field getter/setter, while `RUVIA_RULE_NAME("wire_name", field, rules...)` maps validation errors to that wire name. Field parse state is tracked explicitly, so type mismatches and duplicate known fields are reported by validator middleware as field issues instead of requiring a second body scan. Recursive JSON parsing is depth-limited to protect the request path from excessive nesting.

Validator rules are declared only inside `RUVIA_VALIDATE_JSON` or `RUVIA_VALIDATE_FORM`:

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

Each timeout governs exactly one phase: `headerTimeout` is the request-header read window (TLS handshake included), `bodyTimeout` is the request-body read window, `writeTimeout` is the response write window. `idleTimeout` covers both keep-alive idle time **and the dispatch (handler) phase** — once the body has been read, the connection scanner classifies the connection as idle until writing starts, so `idleTimeout` serves as the deadman switch for hung handlers. To bound business-logic runtime, use `idleTimeout` or in-handler cancellation — `headerTimeout` / `bodyTimeout` no longer leak into dispatch. Any timeout set to `0ms` is disabled.

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
    auto result = co_await c.db().query(kFindUserQuery, {c.param("id").toStringView().value_or("")});
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
    .setListenAddress("0.0.0.0", 8443)
    .useTls(ruvia::TlsConfig{
        .certificateChainFile = "server.crt",
        .privateKeyFile = "server.key"})
    .run();
```

TLS is backed by OpenSSL, disables legacy SSL/TLS protocol versions and TLS compression, and keeps each accepted connection on its owning worker. Buffered responses can be gzip-compressed when clients send `Accept-Encoding: gzip`; file responses, response streams, SSE, and WebSocket traffic are not compressed.

## Runtime Behavior

- `App::run()` creates one internal server/acceptor/thread per worker.
- Each worker owns exactly one standalone Asio `io_context`.
- SIGINT and SIGTERM trigger graceful shutdown by closing each worker acceptor and active worker-owned sockets on that worker's `io_context`.
- Idle/header/body/write timeouts are enforced by one scanner per worker, not by per-connection timers; a `0ms` timeout disables that category.
- `setMaxConnectionsPerWorker(...)` returns `429 Too Many Requests` for excess accepted connections; `setMaxRequestsPerConnection(...)` closes keep-alive after the configured request count; `0` means unlimited.
- HTTP parsing uses Ruvia's zero-copy parser; request method, path, version, headers, and common values are views into the connection read buffer by default. Chunked request bodies are decoded in place.
- Stream routes (`RUVIA_POST_STREAM`, `RUVIA_PUT_STREAM`, `RUVIA_PATCH_STREAM`) dispatch after headers and let handlers consume Content-Length or chunked bodies through `c.bodyReader()`.
- Buffered multipart parsing returns field views into the current request body; streaming multipart returns chunk views valid until the next `read()`.
- `Expect: 100-continue` is answered before body reads, and comma-separated `Connection` tokens are honored.
- Response construction uses request arenas and scatter-gather-friendly response data instead of building a full response string for every request. Memory bodies are either explicit borrowed views or owned arena strings; file bodies are internal `FileToken` values created only by `Context`.
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

The current formal release is `v0.0.1`.

This release is focused on evaluating:

- The core HTTP/1.1 server and controller API.
- Request model validation and response helpers.
- WebSocket, SSE, request streaming, and response streaming support.
- Optional MariaDB-compatible database query, transaction, and migration integration.
- HTTPS/TLS, gzip compression, optional Redis/JWT helpers, and performance-oriented memory layout.

The project is pre-1.0, so API changes are still possible when they improve hot-path safety or remove ambiguity. The implementation boundary is summarized in [Project Scope](#project-scope).
