# 流式传输、WebSocket、隧道与出站客户端 API 手册

本文档涵盖 `ruvia::web` 中的流式请求体/响应流、Server-Sent Events（SSE）、HTTP/1.1 与 HTTP/2 高级协议能力、WebSocket 服务端、HTTP `CONNECT` 与 `CONNECT-UDP` 隧道，以及出站 `http_client` 和 `websocket_client` 的使用方法与公开 API。所有公开符号均位于 `namespace ruvia`。

---

## 1. 流式请求体与流式响应 / SSE

头文件：`"ruvia/web/streaming.h"`、`"ruvia/web/multipart_reader.h"`、`"ruvia/http/sse.h"`。

### 1.1 流式消费请求体（`body_reader` 与 `multipart_reader`）

在 `RUVIA_POST`、`RUVIA_PUT` 或 `RUVIA_PATCH` 路由宏中附加 `ruvia::stream_body` 策略即可注册流式请求体路由。此类路由不会预先缓冲整个请求体，而是由处理器按块拉取。

#### `ruvia::body_reader`（通过 `c.req().body_reader()` 获取）

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<std::span<const std::byte>>> read() &` | 读取下一个请求体字节块；返回 `std::nullopt` 表示已到达请求体末尾（EOF） |
| `task<std::optional<std::string_view>> text() &` | 读取下一个请求体字符视图块；返回 `std::nullopt` 表示 EOF |

> **生命周期约束**：每次 `read()` 或 `text()` 返回的视图仅在下一次调用 `read()` / `text()` 启动前有效；同一 `body_reader` 上同一时刻只能有一个运行中的读取操作。

#### `ruvia::multipart_reader`（通过 `c.req().multipart_reader()` 获取）

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<multipart_stream_part>> read() &` | 流式读取 `multipart/form-data` 的下一个分片块，返回 `std::nullopt` 表示全部解析完成 |

`multipart_stream_part` 提供：
- `name()`、`filename()`、`has_filename()`、`content_type()`、`body()`（当前块内容视图）
- `phase()`：返回 `multipart_chunk_phase`（`complete` 表示单块完整字段，`first` / `middle` / `last` 表示跨多块的大文件分片阶段）

```cpp
RUVIA_POST("/upload", upload_stream, ruvia::stream_body);

ruvia::task<ruvia::http_response> upload_stream(ruvia::context& c) {
    auto& reader = c.req().body_reader();
    std::size_t total = 0;
    while (auto chunk = co_await reader.read()) {
        total += chunk->size();
    }
    co_return c.text("done\n");
}
```

---

### 1.2 流式响应写入（`response_stream_writer`）

使用 `RUVIA_GET(path, handler, ...)`（或 `RUVIA_POST` 等）且处理器返回 `ruvia::task<void>` 时，框架自动将其推导为流式响应路由。通过 `c.stream()`（默认二进制流）或 `c.stream_text()`（自动设置 `text/plain; charset=utf-8`）获取 `response_stream_writer&`：

| 方法签名 | 说明 |
| --- | --- |
| `task<void> write(std::span<const std::byte> chunk) &` | 写入一段字节块（HTTP/1.1 自动使用 `Transfer-Encoding: chunked`，HTTP/2 使用 `DATA` 帧） |
| `task<void> write(std::string_view chunk) &` | 写入一段字符串视图 |
| `task<void> write(std::pmr::string&& chunk) &` | 移动写入 PMR 字符串（同内存资源下零拷贝接管） |
| `task<void> writeln(std::string_view chunk) &` | 写入文本并追加 `\n` |
| `task<timer_sleep_result> sleep(std::chrono::milliseconds duration) &` | 等待指定时长；请求取消或停机返回 `stop_requested`。HTTP/1 默认不主动检测断开；HTTP/2 流终止抛入站传输异常 |
| `bool aborted() const noexcept` | 检查客户端连接或流是否已中止 |
| `stop_token cancel_on_peer_eof() &` | 显式启用读侧终止取消，返回供业务等待使用的组合 token，语义见下文 |
| `task<void> end(std::span<const http_header_view> trailers = {}) &` | 正常结束流式响应，可选附带响应 Trailer 字段 |

---

### 1.3 Server-Sent Events（`sse_writer`）

使用 `RUVIA_GET(path, handler, ...)` 或 `RUVIA_POST(path, handler, ...)`（处理器返回 `ruvia::task<void>`）注册 SSE 路由。通过 `c.stream_sse()` 获取 `sse_writer`：

```cpp
RUVIA_GET("/events", stream_events);

ruvia::task<void> stream_events(ruvia::context& c) {
    auto sse = c.stream_sse();
    co_await sse.write({
        .data_ = "hello",
        .event_ = "greeting",
        .id_ = "1",
        .retry_ = std::chrono::milliseconds(3000),
    });
    co_await sse.end();
}
```

| `ruvia::sse_writer` 方法 | 说明 |
| --- | --- |
| `task<void> write(const sse_message& message)` | 格式化并发送一条 SSE 事件（自动处理多行 `data:` 切分与 UTF-8 校验） |
| `task<timer_sleep_result> sleep(std::chrono::milliseconds duration)` | 挂起等待指定时长 |
| `bool aborted() const noexcept` | 检查客户端是否已断开 |
| `stop_token cancel_on_peer_eof()` | 与 `response_stream_writer` 同名方法相同 |
| `task<void> end(std::span<const http_header_view> trailers = {})` | 结束 SSE 流 |

#### 空闲 SSE 的断连取消

普通 HTTP/1 响应和 SSE **默认不改变 TCP 半关闭语义**。[RFC 9112 §9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6) 明确指出：客户端的 TCP 半关闭既不界定请求消息的结束，也不表示客户端不再关心响应。业务选择 `cancel_on_peer_eof()` 后，明确接受将读侧 EOF 视为放弃当前响应；这适用于不使用写半关闭的 SSE 客户端，不适用于发送完请求后 `shutdown(SHUT_WR)`、仍要完整读取响应的客户端。

- HTTP/1 明文和 TLS 均使用连接所属事件循环上的异步 transport read。没有探测写入、心跳依赖或 socket 状态轮询。
- 请求体必须已完整消费。没有请求体的 GET 可立即启用；带请求体时先 `co_await c.req().text()` 等完整读取，再启用。提前返回响应而未读完请求体时，调用抛出 `std::logic_error`；不自动排空、不抢占请求体读取。响应头提交后才读完请求体的情况仍遵守原先确定的关闭计划，不因此恢复连接复用。
- HTTP/1 流已结束或中止后不能首次启用；有效启用后重复调用幂等。
- 读到后续请求字节时保留原字节与顺序，不将“可读”误判为断开。预读达到既有请求头大小上限时暂停检测，直到当前响应结束；不支持在任意长度的待处理 pipeline 后仍保证即时发现 EOF，也不为此无限缓冲。
- EOF、TLS `close_notify`、异常 TCP 断开及超时关闭触发当前流的取消。正常结束与异常退出都先取消并等待检测结束，再释放连接资源。HTTP/2 不新增读操作，返回的 token 观察对应 stream 的终止，不取消同连接其他 stream。
- HTTP/2 请求方向正常的 `END_STREAM` 不取消响应；`RST_STREAM` 或使该流终止的连接关闭才取消。`GOAWAY` 仍遵循原有排空规则。
- 网络黑洞、客户端断电等没有 FIN/RST 到达的情况不能保证即时识别，仍由既有超时策略处理。本 API 不新增保活探测。
- 返回值合并当前请求的原取消信号，`sse.sleep()` / `stream.sleep()` 也使用它；`c.stop_token()` 和此前取得的 token 不变。数据库、Redis、HTTP client 及自定义订阅继续使用各自已有的显式 token 参数，无第二套取消协议。
- 取消是协作式的。必须将返回 token 传给正在等待的操作，并在取消后返回、释放订阅；不会抢占 CPU 循环或无取消支持的等待。

FlexEdge 的最小接入是：取得 SSE writer 后启用选项，将返回 token 传给订阅的异步接收。下面以公开的 `channel_receiver` 表示订阅接收端，不假定 FlexEdge 私有 API 名称：

```cpp
#include <string>
#include "ruvia/core/channel.h"
#include "ruvia/web/context.h"
#include "ruvia/web/streaming.h"

ruvia::task<void> stream_subscription(
    ruvia::context& c,
    ruvia::channel_receiver<std::string>& subscription,
    std::string_view initial_snapshot) {
    auto sse = c.stream_sse();
    const auto stop = sse.cancel_on_peer_eof();
    co_await sse.write({.data_ = initial_snapshot, .event_ = "snapshot"});
    while (!stop.stop_requested()) {
        auto message = co_await subscription.receive(stop);
        if (!message.has_value()) {
            co_return;  // cancelled、closed 或 worker_stopping
        }
        co_await sse.write({.data_ = message.value(), .event_ = "update"});
    }
}
```

在 handler 中持有订阅的 RAII owner 并 `co_await stream_subscription(...)`；返回后销毁订阅。若已有额外取消源，使用 `combine_stop_tokens(stop, extra_stop)` 后传给同一个等待。不要调用 Worker 的停止入口来取消单个 SSE。可运行的基本用法见 [streaming.cpp](../examples/web/streaming.cpp)。

---

## 2. HTTP/1.1 & HTTP/2 服务端高级协议能力

参考示例：[examples/web/http_features.cpp](../examples/web/http_features.cpp)。

| `ruvia::context` / `context_request` 方法 | 适用协议与说明 |
| --- | --- |
| `co_await c.inform(const http_interim_response_head& response)` | 在最终响应头提交前发送 `1xx` 临时响应（如 `103 Early Hints`：`http_interim_response_head(http_status::early_hints, headers)`） |
| `c.priority(http_priority_fields{.urgency_ = 2, .incremental_ = true})` | 设置 RFC 9218 `Priority` 响应优先级；通过 `c.req().priority()` 读取客户端请求优先级 |
| `bool c.push(http_push_request_view request)` | （HTTP/2）向开启了 Server Push 的客户端推送关联资源，同步返回 `bool` 表示是否被接纳 |
| `void c.advertise_origins(std::span<const std::string_view> origins)` | （HTTP/2）在连接上排入 RFC 8336 `ORIGIN` 帧，通告当前连接可合并服务的权威源集合 |
| `void c.advertise_alternative_service(std::string_view value)` | （HTTP/2）在连接上排入 RFC 7838 `ALTSVC` 帧通告替代服务 |
| `c.req().trailers()` / `c.req().trailer(name)` | 在流式请求体读取到 EOF 后，读取客户端发送的请求 Trailer 首部；按 RFC 9110 §6.5.1 不允许出现在 Trailer 中的字段（如 `Content-Length`、`Cache-Control`）被丢弃而不导致请求失败，也不会并入请求头 |
| `co_await stream.end(trailers)` | 在流式响应结束时发送响应 Trailer 首部（需在写出首块前声明 `c.header("Trailer", "...")`） |

---

## 3. WebSocket 服务端

头文件：`"ruvia/web/websocket.h"`、`"ruvia/http/websocket_protocol.h"`。
参考示例：[examples/web/websocket.cpp](../examples/web/websocket.cpp)。

Ruvia WebSocket 服务端同时支持 **HTTP/1.1 `Upgrade: websocket`** 与 **HTTP/2 Extended `CONNECT`（RFC 8441，`:protocol = websocket`）**，路由代码完全统一。

握手校验遵循接收方规则：HTTP/2 Extended CONNECT 中出现的 `Sec-WebSocket-Key` / `Sec-WebSocket-Accept` 已被 `:protocol` 取代，按 [RFC 8441 §5](https://www.rfc-editor.org/rfc/rfc8441.html#section-5) 不做处理（既不校验也不拒绝）；`Sec-WebSocket-Protocol` 提议中的重复子协议会被跳过（唯一性是 RFC 6455 §4.1 对发送方客户端的要求）。

### 3.1 路由注册与配置

```cpp
#include "ruvia/web/websocket.h"

inline const ruvia::websocket_route_config chat_ws_options{
    .subprotocols_ = {"chat.v1", "json.v1"},
    .require_subprotocol_ = true,
    .lifecycle_ = {
        .heartbeat_ = {
            .ping_interval_ = std::chrono::seconds(15),
            .pong_timeout_ = std::chrono::seconds(5),
        },
        .close_handshake_timeout_ = std::chrono::seconds(5),
    },
    .deflate_ = {
        .enabled_ = true,
        .compression_level_ = 6,
    },
};

class ws_controller final {
public:
    RUVIA_ROUTES_BEGIN(ws_controller)
    RUVIA_GET_WS("/ws/echo", echo);
    RUVIA_GET_WS("/ws/chat", chat, chat_ws_options);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> echo(ruvia::context& c) {
        auto& ws = c.websocket();
        while (auto message = co_await ws.read()) {
            // 零拷贝原样回显当前消息（保持 opcode）：
            co_await ws.send(std::move(*message));
        }
        co_await ws.close({.code_ = 1000, .reason_ = "normal closure"});
    }

    ruvia::task<void> chat(ruvia::context& c) {
        auto& ws = c.websocket();
        co_await ws.text("welcome");
        co_await ws.close();
    }
};
```

### 3.2 `ruvia::websocket` 与 `ruvia::websocket_message` API

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<websocket_message>> read() &` | 读取下一条完整 WebSocket 数据消息（底层自动处理分片重组与 Ping/Pong 心跳）；收到对端 Close 帧或连接结束时返回 `std::nullopt` |
| `task<void> text(std::string_view payload, websocket_send_options options = {}) &` | 发送 UTF-8 文本消息（另有 `std::pmr::string&&` 重载） |
| `task<void> binary(std::string_view payload, websocket_send_options options = {}) &` | 发送二进制消息（另有 `std::pmr::string&&` 重载） |
| `task<void> send(websocket_message&& message, websocket_send_options options = {}) &` | 零拷贝发送 `read()` 返回的 `websocket_message`（发送完成前会占用读缓冲） |
| `task<void> ping(std::string_view payload = {}) &` / `pong(std::string_view payload) &` | 发送显式 `Ping` 或 `Pong` 控制帧 |
| `task<void> close(websocket_close_options options = {}) &` | 发起或完成 WebSocket 关闭握手（默认 `code_ = 1000`） |
| `void abort() noexcept` | 立即中止底层传输而不执行关闭握手 |

`websocket_message` 方法：
- `opcode()`（`websocket_opcode::text` 或 `binary`）
- `payload()`（返回 `std::string_view`，在下一次 `read()` 启动前有效）
- `text()` / `binary()`（返回 `bool`）

---

## 4. HTTP `CONNECT` 与 `CONNECT-UDP` 隧道

头文件：`"ruvia/web/http_tunnel.h"`。
参考示例：[examples/web/tunnels.cpp](../examples/web/tunnels.cpp)。

### 4.1 服务端 `CONNECT` 字节流隧道

使用 `RUVIA_CONNECT(authority, handler, ...)` 注册普通 `CONNECT` 隧道（支持可选的 `http_tunnel_route_config` 首参，兼容 HTTP/1.1 `CONNECT` 与 HTTP/2 `CONNECT`）：

```cpp
RUVIA_CONNECT("example.com:443", handle_connect);

ruvia::task<void> handle_connect(ruvia::context& c) {
    auto& tunnel = c.tunnel();
    while (auto chunk = co_await tunnel.read()) {
        co_await tunnel.write(*chunk);
    }
    co_await tunnel.finish();
}
```

HTTP/2 客户端在收到 2xx 前乐观发送的 `DATA`（RFC 9113 §8.5；RFC 9298 §5，经 RFC 9931 更新）属于隧道数据：协议层在流控窗口内保留这些字节（不归还接收窗口），隧道接受后由 `tunnel.read()` 按序读到；若路由返回非 2xx，已缓冲及后续字节被丢弃并归还流控额度，不复位流。RFC 8441 WebSocket（`:protocol = websocket`）仍要求客户端等待 2xx（RFC 6455 §4.1），提前到达的 `DATA` 以 `RST_STREAM(PROTOCOL_ERROR)` 复位该流。已建立隧道上的未知扩展帧类型按 RFC 9113 §5.5 丢弃。

### 4.2 服务端 `CONNECT-UDP`（RFC 9298）与 Capsule 数据报隧道

使用 `RUVIA_CONNECT_PROTOCOL("connect-udp", path, handler, options, ...)`（设置 `http_tunnel_route_config{.datagrams_ = true}`）注册 RFC 9298 UDP 代理隧道：

```cpp
inline constexpr ruvia::http_tunnel_route_config udp_tunnel_options{
    .datagrams_ = true,
};

RUVIA_CONNECT_PROTOCOL("connect-udp", "/proxy/udp", handle_udp, udp_tunnel_options);

ruvia::task<void> handle_udp(ruvia::context& c) {
    auto udp = c.tunnel().udp();
    while (auto datagram = co_await udp.read()) {
        co_await udp.send(datagram->payload());
    }
    co_await udp.finish();
}
```

| 类名 | 核心方法 |
| --- | --- |
| `ruvia::http_tunnel` | `read()`（返回 `std::optional<std::pmr::string>`）、`write(bytes)`、`finish()`、`abort()`、`udp(config = {})`、`datagrams(config = {})`、`capsules(config = {})` |
| `ruvia::http_capsule_stream` | `read()`（返回 `std::optional<http_capsule>`）、`write(type, payload)`、`finish()`、`abort()` |
| `ruvia::http_datagram_stream` | `read()`（返回 `std::optional<http_datagram>`）、`send(payload)`、`finish()`、`abort()` |
| `ruvia::http_udp_tunnel` | `read()`（返回 `std::optional<http_datagram>`，自动剥离/封装 `context_id = 0`）、`send(payload)`、`finish()`、`abort()` |

---

## 5. 出站 HTTP/1.1 & HTTP/2 客户端（`http_client`）

头文件：`"ruvia/web/http_client.h"`、`"ruvia/web/http_client_types.h"`。
参考示例：[examples/web/http_client.cpp](../examples/web/http_client.cpp)、[examples/web/http_client_advanced.cpp](../examples/web/http_client_advanced.cpp)。

### 5.1 两种使用模式

1. **绑定到 `ruvia::application` Worker（在路由与 Worker 任务中使用）**：
   启动前通过 `app().http_client({.alias_ = "upstream", .config_ = config})` 注册，每个业务 worker 自动维护独立的到该 origin 的连接池；在路由中通过 `c.http_client("upstream")`（默认别名为 `c.http_client()`）获取 `http_client_handle`。
2. **独立绑定到 `ruvia::event_loop`（脱离 Web Server 独立运行）**：
   构造 `ruvia::http_client client(loop, config)`，在使用完毕后于所属事件循环协程中调用 `co_await client.shutdown()`。

### 5.2 `ruvia::http_client_config` 关键配置项

| 字段名 | 类型与默认值 | 说明 |
| --- | --- | --- |
| `scheme_` | `http_scheme`（默认 `https`） | `http_scheme::http` 或 `https` |
| `host_` | `borrowed_text` | 目标主机名或 IP（必填，outbound origin 在创建时固定） |
| `port_` | `std::optional<std::uint16_t>` | 目标端口（缺省按 scheme 使用 80 或 443） |
| `protocol_` | `http_client_protocol`（默认 `negotiate`） | `negotiate`（TLS 下通过 ALPN 协商 H2/H1.1，明文使用 H1.1）、`http1_only`、`http2_only`（明文下使用 H2 prior knowledge） |
| `connection_count_` | `std::size_t`（默认 `1`） | 每个客户端实例维护的最大连接数 |
| `max_concurrent_http2_streams_per_connection_` | `std::size_t`（默认 `100`） | 单条 HTTP/2 连接并发流上限 |
| `connect_timeout_` / `write_timeout_` / `request_timeout_` / `acquire_timeout_` | 超时配置 | 连接建立（默认 `5s`）、写入（`30s`）、完整请求（`30s`）与连接池借用等待超时（`5s`） |
| `max_response_bytes_` | `std::size_t`（默认 `default_max_buffered_body_bytes`） | `read_all()` 默认最大响应体字节数 |
| `tls_` / `tcp_` | `client_tls_config` / `client_tcp_config` | 客户端 TLS 证书校验（`verify_identity` / `skip_verification`）、CA 文件、mTLS 客户端证书与 TCP `no_delay_` / `keep_alive_`。主机名只与证书 DNS 类型 subjectAltName 比对（RFC 9525）：不回退到主题 CN，通配符只能是完整的最左标签；仅 TLS 1.2 / 1.3，TLS 1.2 只提供 ECDHE + AEAD 密码套件，拒绝重协商（WebSocket 与 Redis 客户端相同） |
| `received_cookies_` | `http_client_received_cookie_policy`（默认 `ignore`） | 设为 `retain_and_send` 时启用客户端 Cookie Jar 自动管理 `Set-Cookie` |
| `push_` / `advertisements_` | `http_client_push_config` / `http_client_advertisement_config` | 控制是否接收 HTTP/2 Server Push 与 `ORIGIN` / `ALTSVC` 通告 |

### 5.3 发送请求、读取响应与流式代理

```cpp
auto client = c.http_client().with_options({
    .timeout_ = std::chrono::seconds(5),
});

auto response = co_await client.send({
    .method_ = "POST",
    .target_ = "/v1/items",
    .headers_ = headers,
    .content_ = ruvia::http_client_request_content_view::bytes(payload),
});

// 方式 A：一次性读取完整响应体字节
auto body_bytes = co_await response.body().read_all();
std::string_view text = body_bytes.text();

// 方式 B：在返回 task<void> 的流式路由中零拷贝转发给下游客户端
// co_await response.body().pipe_to(c.stream());
```

### 5.4 流式请求体上传、`100-continue` 与客户端隧道

- **流式上传 (`open_request`)**：
  ```cpp
  auto exchange = co_await client.open_request(
      {.method_ = "POST", .target_ = "/upload"},
      {.expectation_ = ruvia::http_client_request_expectation::continue_value}
  );
  co_await exchange.body().write("chunk-1");
  co_await exchange.body().end(trailers);
  auto response = co_await exchange.response();
  ```
- **出站 `CONNECT` 与 `CONNECT-UDP` 隧道**：
  - `co_await client.open_tunnel({.authority_ = "example.com:443"})`：返回 `http_client_tunnel_result`。成功时 `result.tunnel()` 非空，支持 `read()`、`write(...)`、`finish()`；非 2xx 时 `result.response()` 提供错误响应。
  - `co_await client.open_udp_tunnel({.target_ = "/.well-known/masque/udp/192.0.2.1/53/"})`：通过 `std::move(*opened.tunnel()).udp()` 获取 `http_udp_tunnel` 收发 UDP 数据报。

---

## 6. 出站 WebSocket 客户端（`websocket_client`）

头文件：`"ruvia/web/websocket_client.h"`。
参考示例：[examples/web/websocket_client.cpp](../examples/web/websocket_client.cpp)。

`websocket_client_config::headers_` 与 `subprotocols_` 使用 `std::span<const T>`，借用数组以及元素中的文本；独立保存配置时，这些 owner 必须存活到构造入口消费完成。`websocket_client` 在构造返回前复制配置，之后不再依赖这些借用。`std::span` 本身不延长 owner 生命周期，也不防止所有悬垂。

`ruvia::websocket_client` 绑定到指定的 `ruvia::event_loop`，支持通过 `websocket_client_protocol::http1`（HTTP/1.1 Upgrade）或 `websocket_client_protocol::http2`（HTTP/2 Extended CONNECT）发起连接：

```cpp
#include "ruvia/web/websocket_client.h"
#include <array>

inline constexpr std::array<ruvia::borrowed_text, 1> subprotocols{"chat.v1"};
ruvia::websocket_client_config config{
    .scheme_ = ruvia::websocket_scheme::ws,
    .protocol_ = ruvia::websocket_client_protocol::http1,
    .host_ = "127.0.0.1",
    .port_ = 8080,
    .target_ = "/ws/echo",
    .subprotocols_ = subprotocols,
};

ruvia::websocket_client client(loop, config);
co_await client.connect();

auto peer = client.with_options({.timeout_ = std::chrono::seconds(5)});
co_await peer.text("hello");
if (auto msg = co_await peer.read()) {
    // msg->payload()
}
co_await peer.close({.code_ = 1000, .reason_ = "done"});
co_await client.shutdown();
```

| `websocket_client` / `websocket_client_handle` 方法 | 说明 |
| --- | --- |
| `task<void> connect() &` | 建立底层 TCP/TLS 连接并完成 WebSocket 握手（失败抛 `websocket_client_error`） |
| `websocket_client_handle with_options(operation_options options) const` | 绑定单次操作的 `timeout_` 与 `stop_token_` |
| `task<std::optional<websocket_message>> read()` | 读取下一条消息；收到对端 Close 时回显 Close 并等待服务端先关闭传输后返回 `std::nullopt` |
| `task<void> text(payload, options = {})` / `binary(payload, options = {})` | 发送文本或二进制消息 |
| `task<void> ping(payload = {})` / `pong(payload)` | 发送 `Ping` 或 `Pong` 控制帧 |
| `task<void> close(websocket_close_options options = {})` | 句柄方法：发起优雅关闭握手，收到对端 Close 并等待服务端先关闭传输后完成 |
| `void close() noexcept` / `void abort() noexcept` | 客户端 `client.close()`（或句柄 `peer.abort()`）请求立即中止连接 |
| `task<void> shutdown() &` | 关闭并等待底层全部挂起 I/O 安全退役 |

关闭握手完成后，客户端按 [RFC 6455 §7.1.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.1.1) 由服务端先关闭底层 TCP 连接（HTTP/2 下为服务端先发送 `END_STREAM`）：客户端继续读取并丢弃 Close 之后的字节，直到对端 EOF，再结束自己的传输。等待时长受 `close_handshake_timeout_` 约束（`nullopt` 表示不设上限）；超时或期间传输出错时客户端自行关闭连接，由于关闭握手已经完成，`read()` / `close()` 仍正常返回，不抛错。
