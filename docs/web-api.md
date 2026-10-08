# Web application API

[Documentation index](../README.md#contents)

## Web API Shape

Use `ruvia::app()` to configure the single App before `run()`. Configuration
uses designated initializers; pass `nullptr` to disable an optional feature.
Controllers register through CRTP and route macros, as shown in the
[quick start](../README.md#quick-start). For controller static/object libraries,
use `ruvia_link_controllers(application controllers)`.

### Listeners and TLS

```cpp
ruvia::app().listen({
    .address = "0.0.0.0",
    .http = 80,
    .https = 443,
    .tls = {
        .certificateChainFile = "certs/server.crt",
        .privateKeyFile = "certs/server.key",
    },
    .autoHttpsRedirect = true,
});
```

Use a numeric IPv4/IPv6 bind address. Omitted ports are disabled. HTTPS also
starts HTTP/3 on the same UDP port and advertises it with `Alt-Svc`; set
`.http3.mode = ruvia::Http3Mode::kDisabled` to opt out. Configure
`ListenConfig::altSvc` when advertising an external port or disabling the field.

### Routes and responses

| Use case | Route declaration / handler result |
| --- | --- |
| Ordinary request | `RUVIA_GET`, `RUVIA_POST`, `RUVIA_PUT`, `RUVIA_PATCH`, `RUVIA_DELETE`; `Task<HttpResponse>` |
| Streaming response | `RUVIA_GET_STREAM`; `Task<void>` using `c.stream()` |
| Server-sent events | `RUVIA_GET_SSE`; `Task<void>` using `c.streamSse()` |
| WebSocket | `RUVIA_GET_WS`, `RUVIA_GET_WS_OPTIONS`; `Task<void>` using `c.webSocket()` |
| CONNECT tunnel | `RUVIA_CONNECT`, `RUVIA_CONNECT_PROTOCOL`; `Task<void>` using `c.tunnel()` |

Read input through `c.req()`. Set metadata with `c.status()`, `c.header()`, and
`c.setCookie()` before returning `c.text()`, `c.json()`, or another body builder.
Use named statuses such as `ruvia::http_status::kCreated`. `c.conn()` exposes
peer and TLS information. `c.inform()` sends interim responses, and `c.push()`
dispatches a GET/HEAD push when supported by the peer.

Add middleware types after the handler; configured middleware uses template
arguments in the same list:

```cpp
RUVIA_POST("/upload", upload, AuthMiddleware,
    ruvia::BodyLimit<64 * 1024>, ruvia::RateLimit<10, 1000>);
```

Controller and route limits may only tighten app-wide limits. Rate limits count
independently on each worker. An app-wide rule is configured as:

```cpp
ruvia::app().rateLimit({
    .rule = {.maxRequests = 100, .window = std::chrono::seconds(60)},
    .capacityPerWorker = 8192,
});
```

`capacityPerWorker` must be a power of two. Use `trustedProxies({.cidrs = {...}})`
only for proxies that sanitize forwarded headers. `X-Forwarded-Proto` additionally
requires `trust_x_forwarded_proto = true` and matching XFF/XFP chains.

### Models and validation

```cpp
RUVIA_MODEL(User,
    RUVIA_REQUIRED_FIELD(id, ruvia::UInt64),
    RUVIA_REQUIRED_FIELD_NAME("user_name", name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32, RUVIA_DEFAULT(18)),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE));

// In a controller route block:
RUVIA_POST("/users", createUser, ruvia::JsonBody<User>);

// In createUser(Context& c):
const auto& user = c.req().validated<User>();
co_return c.json(user);
```

Declare field rules in the model. `JsonBody`, `FormBody`, `QueryModel`,
`PathModel`, `HeaderModel`, and `CookieModel` parse and validate their input.
Standalone `fromJson<T>()` / `fromForm<T>()` and request `jsonIf()` / `formIf()`
parse only. `validatedJson<T>().raw()` exposes validated original JSON bytes.

Use `get<"name">()`, `set<"name">(...)`, `ensure<"field">()`, and
`reset<"field">()` with C++ field names, even when JSON uses a different wire name.

| Declaration | Missing input | JSON `null` |
| --- | --- | --- |
| Required | Error | Error |
| Required + `RUVIA_NULLABLE` | Error | Accepted |
| Optional | Accepted | Error |
| Optional + `RUVIA_NULLABLE` | Accepted | Accepted |

`RUVIA_DEFAULT(value)` applies to missing optional input.
`RUVIA_INITIAL(value)` initializes explicitly constructed values.
For PATCH semantics, inspect `isPresent<"field">()` and `isNull<"field">()`;
missing means unchanged, while explicit null can mean clear. Setters do not
rewrite original input presence. Unset fields are omitted from JSON by default;
`RUVIA_EMIT_NULL` and `RUVIA_OMIT_EMPTY` customize output.

### Model values and JSON traversal

Use `String`, numeric wrappers, `Bytes`, `Array<T>`, and `BoxedArray<T>` in models.
`Bytes` serializes as canonical padded base64. Strings and field names must
already contain valid UTF-8. Select output fields deliberately when an input
model contains secrets.

`fromJson` and `toJson` also support typed scalar and array roots; `c.json()`
and `JsonBody<T>` require a Model root. `JsonValue` / `JsonObject` support dynamic
reading, not root serialization through `c.json()`:

```cpp
if (auto value = ruvia::JsonValue::parse(R"({"enabled":true})")) {
    value->forEachField([](std::string_view key, const ruvia::JsonValue& item) {
        return true; // false stops traversal
    });
}
```

Visitors borrow their input for the callback. Keep model PMR resources alive
through all values using them; see [lifetime rules](runtime.md#lifetime-rules).
Typed JSON parsing defaults to 65,536 array members and 16 MiB per document;
standalone parsing can override these through `ModelParseOptions`.
`RUVIA_PATTERN` / `RUVIA_REGEX` use a bounded pattern dialect; use `RUVIA_CUSTOM`
for other validation logic.

### WebSocket

The same route supports HTTP/1.1, HTTP/2, and HTTP/3. Configure compression and
liveness with `WebSocketRouteConfig`:

```cpp
ruvia::WebSocketRouteConfig options{
    .deflate = {.enabled = true, .compressionLevel = 6, .contextTakeover = false},
};
// In the route block: RUVIA_GET_WS_OPTIONS("/connect", connect, options);
// In the handler, omit sensitive messages from compression:
co_await c.webSocket().binary(payload, {.compress = false});
```

Compression is negotiated only when offered by the peer. Levels range from
0 to 9; context takeover retains a per-connection dictionary. Avoid mixing
secrets and attacker-controlled input in that dictionary.
Use `options.lifecycle.heartbeat` for Ping/Pong deadlines and
`closeHandshakeTimeout` for the Close exchange. HTTP/3 also has
`peerTransportFinTimeout`; its connection-level QUIC idle timeout still applies.
Payload views expire on the next read. See [websocket.cpp](../examples/web/websocket.cpp).

### Worker-local requests

`Context::dispatch()` invokes a registered buffered route on the current worker,
including its middleware and validation:

```cpp
auto reply = co_await c.dispatch({
    .method = "POST",
    .target = "/devices/42/commands",
    .headers = headers,
    .body = commandJson,
});
```

Pass credentials explicitly; parent business state is not inherited. Use an
origin-form path. Files, streams, SSE, and upgrades are unsupported; nesting is
limited to eight levels. Keep the result within its worker lifetime.

### Errors, sessions, and JWT

Default errors use `application/problem+json`; validation errors include field
issues. Customize them with `onError`. Use `onConnectionFailure` for failures
after response publication and `httpStats()` for operational counters.

With Redis enabled, `SessionMiddleware` exposes `c.session()` / `c.trySession()`.
Call `set()`, `clear()`, or `regenerate()` before response publication, including
before the first stream write or WebSocket handshake. Non-empty writes rotate
and revoke the previous session ID; read-only requests keep it. A stale writer
receives 409 and must not retry old authenticated state under a new ID.

Enable `RUVIA_ENABLE_JWT` for `<ruvia/web/auth/Jwt.h>`. HMAC keys must be random
raw bytes: at least 32/48/64 bytes for HS256/384/512. Decode encoded secrets
before use. Verification accepts standard encoded JWTs; `crit` and `b64`
extensions are rejected. See [auth_jwt.cpp](../examples/web/auth_jwt.cpp).

### Timeouts and limits

Configure request body size, connection request count, and timeouts through
`server_config`. HTTP/1 absolute header/body completion defaults are 30/120
seconds; successful reads do not extend them. `std::nullopt` disables those
absolute deadlines. Inbound buffer defaults are 256 MiB per worker and 64 MiB
per connection. WebSocket message size uses `max_web_socket_message_bytes`.

HTTP/3 early data is opt-in through `ListenConfig::tls.http3_early_data` and is
unavailable with client-certificate authentication. Only bodyless GET/HEAD on
buffered routes qualify. Include at least one middleware declaring
`static constexpr bool ruvia_replay_safe = true;`; every middleware in the chain
must declare it. Ineligible early requests receive 425. Use `early_data_info()`
for transport-confirmed state, never the request's `early-data` header alone.

### Strict integer conversion

`parseInteger<T>(text)` from `<ruvia/core/Integer.h>` returns `std::expected`.
It rejects whitespace, `+`, trailing bytes, unsigned negatives, and overflow.
Use `QueryModel<T>` for structured request validation.

Complete examples: [models](../examples/web/models_validation.cpp),
[middleware](../examples/web/middleware_next.cpp), [streams](../examples/web/streaming.cpp),
[API usage](../examples/web/api_surface.cpp), and [TestApp](../examples/web/testing.cpp).
