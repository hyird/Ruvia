# 流式传输、WebSocket 与隧道 API 手册

本文档涵盖 `ruvia::web` 的流式请求体/响应流、SSE、HTTP/1.1、HTTP/2、HTTP/3、WebSocket 与 CONNECT/CONNECT-UDP，以及统一出站客户端。所有公开符号均位于 `namespace ruvia`。

---

## 1. 流式请求体与流式响应 / SSE

头文件：`"ruvia/web/streaming.h"`、`"ruvia/web/multipart_reader.h"`、`"ruvia/http/sse.h"`。

### 1.1 流式消费请求体（`body_reader` 与 `multipart_reader`）

在 `RUVIA_POST`、`RUVIA_PUT` 或 `RUVIA_PATCH` 路由宏中附加 `ruvia::stream_body` 策略即可注册流式请求体路由。此类路由不会预先缓冲整个请求体，而是由处理器按块拉取。

> **内容编码**：`stream_body` 路由只移除传输编码（`Transfer-Encoding`，如 `chunked`、`gzip`），不解码请求的 `Content-Encoding`。无论请求声明何种内容编码（包括 `gzip` 等已知编码与 `xyz` 等未知编码），处理器读到的都是编码后的原始字节，框架既不解码也不回 `415`；需要时由处理器自行检查 `Content-Encoding` 并解码或拒绝。缓冲路由则会解码受支持的内容编码，对不支持的编码回 `415`（附 `Accept-Encoding`）、对损坏的编码数据回 `400`。RFC 9110 §8.4 中以 `415` 拒绝不可接受的内容编码是 MAY，两种行为均符合规范。

> **体积限额与拒绝生命周期**：`ruvia::body_limit<N>` 可与 `stream_body` 叠加，且只能收紧部署级请求体限额。HTTP/2 在声明的 `Content-Length` 或已接收的 DATA 超限时，若响应头尚未提交，仍通过应用错误处理器返回 `413`（`request_body_too_large`）；处理器已启动但尚未读取请求体时也适用。已确定超限后不会再发送 `100 Continue`。响应头提交后再发现超限则重置该流，不能发送第二个响应头。拒绝任务按值持有错误元数据快照，协议诊断文本复制到请求内存域，直到异步错误处理与响应发送结束；不会借用生成拒绝结果时的栈临时对象。

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

使用 `RUVIA_GET(path, handler, ...)`（或 `RUVIA_POST` 等）且处理器返回 `ruvia::task<void>` 时，框架自动将其推导为流式响应路由。通过 `c.stream()`（默认二进制流）或 `c.stream_text()`（自动设置 `text/plain; charset=UTF-8` 与 `X-Content-Type-Options: nosniff`，与缓冲的 `c.text(...)` 相同；应用已设置的 `X-Content-Type-Options` 保留）获取 `response_stream_writer&`：

| 方法签名 | 说明 |
| --- | --- |
| `task<stream_commit_result> commit() &` | 显式提交头和 session；返回 `body` 才可生产正文，返回 `completed` 必须停止生产（HEAD、304 等正常无正文完成） |
| `task<void> write(std::span<const std::byte> chunk) &` | 在正文阶段写入字节块（HTTP/1.1 chunked，HTTP/2 DATA）；要求显式提交，提交前或完成后写入抛 `std::logic_error` |
| `task<void> write(std::string_view chunk) &` | 写入一段字符串视图 |
| `task<void> write(std::pmr::string&& chunk) &` | 移动写入 PMR 字符串（同内存资源下零拷贝接管） |
| `task<void> writeln(std::string_view chunk) &` | 写入文本并追加 `\n` |
| `task<timer_sleep_result> sleep(std::chrono::milliseconds duration) &` | 仅在正文阶段等待；`c.stop_token()` 取消（含客户端断开）或停机返回 `stop_requested`，正常超时返回 `elapsed`；提交前/完成后调用抛 `std::logic_error`。HTTP/2 流终止抛传输异常 |
| `bool aborted() const noexcept` | 只报告已观察到的真实失败/中止（写失败、客户端断开），正常完成（含 HEAD、304）为 `false` |
| `task<void> end(std::span<const http_header_view> trailers = {}) &` | 正常结束流式响应，可选附带响应 Trailer 字段 |

HTTP/2 服务端写循环直接等待 Worker 信号；没有输出时不创建额外的等待协程。

提交、写入与结束共用线性输出通道；并发启动或重复提交抛 `std::logic_error`。头提交前的失败仍可替换为缓冲错误响应，提交后的失败不能再发送第二个响应头。正常无正文完成不通过异常控制流、不停止取消 token、不伪造成功写入；中间件后置代码仍正常执行。响应完成与请求体消费是两个独立阶段：必要时仍须读取/排空合法请求体，HTTP/1 在提交时未消费完整请求体则按关闭计划处理，不因稍后读取而恢复复用。

---

### 1.3 Server-Sent Events（`sse_writer`）

使用 `RUVIA_GET(path, handler, ...)` 或 `RUVIA_POST(path, handler, ...)`（处理器返回 `ruvia::task<void>`）注册 SSE 路由。通过 `c.stream_sse()` 获取 `sse_writer`：

```cpp
RUVIA_GET("/events", stream_events);

ruvia::task<void> stream_events(ruvia::context& c) {
    auto sse = c.stream_sse();
    if (co_await sse.commit() == ruvia::stream_commit_result::completed) {
        co_return;
    }
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
| `task<stream_commit_result> commit()` | 与普通流相同：先检查 `body` / `completed`，再启动订阅或事件生产 |
| `task<void> write(const sse_message& message)` | 格式化并发送一条 SSE 事件（自动处理多行 `data:` 切分与 UTF-8 校验） |
| `task<timer_sleep_result> sleep(std::chrono::milliseconds duration)` | 挂起等待指定时长 |
| `bool aborted() const noexcept` | 只报告真实失败，正常 HEAD 完成不是取消 |
| `task<void> end(std::span<const http_header_view> trailers = {})` | 结束 SSE 流 |

#### 客户端断开即取消请求

流式响应路由（处理器返回 `ruvia::task<void>`）中，客户端关闭连接会停止 `c.stop_token()`，与 Go `net/http` 的后台读取、Pingora `read_body_or_idle` 及 nginx 默认 `proxy_ignore_client_abort off` 一致。该 token 已由 `db()`、`redis()`（含 Pub/Sub 订阅）、`run_blocking()` 与 `sse.sleep()` / `stream.sleep()` 自动绑定，空闲 SSE 等待订阅消息时也能及时结束，无需显式启用。

- HTTP/1 明文和 TLS：请求体完整消费后，框架在连接所属事件循环上发起一次异步 transport read 观察读侧终止；没有探测写入、心跳依赖或 socket 状态轮询。没有请求体的 GET 在分派时即开始；带请求体时在处理器读完请求体（如 `co_await c.req().text()`）的那次读取返回前开始。未读完的请求体字节仍在 socket 中时不检测，也不自动排空或抢占请求体读取。
- 单独到达的请求（同一批输入中前面没有其他请求）遇 EOF、TLS `close_notify`；任何请求遇 TLS 截断（无 `close_notify` 的 TCP 结束）、TCP RST 或写失败，都停止 `c.stop_token()`，`aborted()` 随之为 `true`，之后的写入抛传输异常；当前流按截断结束并关闭连接。正常结束与异常退出都先取消并等待检测结束，再释放连接资源。
- **TCP 写半关闭**：[RFC 9112 §9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6) 指出客户端半关闭本身不表示放弃响应，但单个 FIN 无法区分“关闭整个客户端”与“仅关闭发送方向”。流式响应把单独到达请求之后的 FIN 视为客户端离开：只发送一个请求即 `shutdown(SHUT_WR)`、仍要读取完整流式响应的客户端会被截断，这类客户端应改为保持发送方向打开或使用缓冲响应路由。缓冲响应路由不受影响。
- **Pipeline**：检测期间读到的后续请求字节按原顺序完整保留，不将“可读”误判为断开。pipeline 内的干净结束（TCP FIN 或 TLS `close_notify`）是请求方向半关闭，与 FIN 到达的时机无关：当前请求排在前一请求之后到达（客户端在前一响应结束前已发出它，无论前一路由是缓冲还是流式），或其后已有后续请求字节时，不取消当前响应；FIN 之前已收到的请求（包括流式响应，其 `c.stop_token()` 不因该 FIN 停止）照常处理，全部响应后关闭连接，不再读取 transport。TCP RST 与 TLS 截断仍中止。预读达到既有请求头大小上限时暂停检测，直到当前响应结束；不为探测无界缓冲。
- HTTP/2 不新增读操作：本流 `RST_STREAM` 或使该流终止的连接关闭停止 `c.stop_token()`，不取消同连接其他 stream。请求方向正常的 `END_STREAM` 不取消响应；`GOAWAY` 遵循连接排空规则。
- 网络黑洞、客户端断电等没有 FIN/RST 到达的情况不能保证即时识别，仍由既有超时策略处理，不新增保活探测。
- 取消是协作式的：未绑定 `c.stop_token()` 的自定义等待须显式传入它，并在取消后返回、释放订阅；不会抢占 CPU 循环或无取消支持的等待。

FlexEdge 的最小接入：把 `c.stop_token()` 传给订阅的异步接收。下面以公开的 `channel_receiver` 表示订阅接收端，不假定 FlexEdge 私有 API 名称：

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
    if (co_await sse.commit() == ruvia::stream_commit_result::completed) {
        co_return;
    }
    const auto stop = c.stop_token();
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

在 handler 中持有订阅的 RAII owner 并 `co_await stream_subscription(...)`；返回后销毁订阅。已有额外取消源时声明 `ruvia::stop_group group{stop, extra_stop};`，把 `group.token()` 传给等待，并让 group 存活到等待及取消后的 join 完成。不要用临时 group 返回 token，也不要停止整个 Worker 来取消单个 SSE。可运行示例见 [streaming.cpp](../examples/web/streaming.cpp) 与 [redis_pubsub.cpp](../examples/web/redis_pubsub.cpp)。

---

## 2. HTTP/1.1 & HTTP/2 服务端高级协议能力

参考示例：[examples/web/http_features.cpp](../examples/web/http_features.cpp)。

| `ruvia::context` / `context_request` 方法 | 适用协议与说明 |
| --- | --- |
| `co_await c.inform(const http_interim_response_head& response)` | 在最终响应头提交前发送 `1xx` 临时响应（如 `103 Early Hints`：`http_interim_response_head(http_status::early_hints, headers)`） |
| `c.draft().priority(http_priority_fields{.urgency_ = 2, .incremental_ = true})` | 设置当前响应优先级；跨替换策略用 `c.policy().priority(...)`，客户端请求优先级用 `c.req().priority()` |
| `bool c.push(http_push_request_view request)` | （HTTP/2）向开启了 Server Push 的客户端推送关联资源，同步返回 `bool` 表示是否被接纳；`method_`、`scheme_`、`authority_`、`path_` 使用 `borrowed_text`，`headers_` 使用 `borrowed_http_headers`，拒绝临时拥有型字符串与临时首部容器；对端禁用推送，或已打开的推送流（不含仅预留的推送）已达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS`（包括其为 0）时返回 `false`，不发送 `PUSH_PROMISE`；已预留的推送在提交响应头时仍无并发额度则以 `RST_STREAM(CANCEL)` 取消，不视为连接失败 |
| `void c.advertise_origins(std::span<const std::string_view> origins)` | （HTTP/2）在连接上排入 RFC 8336 `ORIGIN` 帧，通告当前连接可合并服务的权威源集合 |
| `void c.advertise_alternative_service(std::string_view value)` | （HTTP/2）在连接上排入 RFC 7838 `ALTSVC` 帧通告替代服务 |
| `c.req().trailers()` / `c.req().trailer(name)` | 在流式请求体读取到 EOF 后，读取客户端发送的请求 Trailer 首部；按 RFC 9110 §6.5.1 不允许出现在 Trailer 中的字段（如 `Content-Length`、`Cache-Control`）被丢弃而不导致请求失败，也不会并入请求头 |
| `co_await stream.end(trailers)` | 在正文阶段结束响应并发送 Trailer；须在 commit 前声明 `c.draft().header("Trailer", "...")`，completed 结果不生产 trailers |

---

## 3. WebSocket 服务端

头文件：`"ruvia/web/websocket.h"`、`"ruvia/http/websocket_protocol.h"`。
参考示例：[examples/web/websocket.cpp](../examples/web/websocket.cpp)。

Ruvia WebSocket 服务端支持 HTTP/1.1 `Upgrade: websocket`、HTTP/2 Extended CONNECT（RFC 8441）与 HTTP/3 Extended CONNECT（RFC 9220）；后两者使用 `:protocol = websocket`，路由代码统一。

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
        .deflater_ = {.compression_level_ = 6},
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
        co_await ws.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "normal closure"});
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
| `task<void> ping(std::string_view payload = {}) &` / `pong(std::string_view payload = {}) &` | 发送显式 `Ping` 或 `Pong` 控制帧 |
| `task<void> close(websocket_close_options options = {}) &` | 发起或完成 WebSocket 关闭握手（默认 `code_ = websocket_close_status::normal_closure`） |
| `std::string_view subprotocol() const& noexcept` | 获取握手协商选中的 WebSocket 子协议（未选中时为空视图） |
| `std::optional<std::uint16_t> peer_close_code() const noexcept` / `std::string_view peer_close_reason() const& noexcept` | 读取对端发送的 Close 帧状态码与 UTF-8 原因（收到对端 Close 帧后可用） |
| `void abort() noexcept` | 立即中止底层传输而不执行关闭握手 |

`websocket_close_options::code_` 与 Sans-I/O `websocket_connection::submit_close()` 共用受检值类型 `websocket_close_code`，拒绝隐式整数转换。使用 `websocket_close_status::{normal_closure, going_away, internal_error}` 常量，或通过 `websocket_close_code::from_value(input)` 在原始整数宽度校验；非法值抛 `std::invalid_argument`，`try_from_value(input)` 返回 `nullopt`，拒绝时尚未进入关闭操作。不要先转换成 `uint16_t`：`66536`、`-64536` 不能截断成正常关闭 `1000`。可表示当前支持的 `1000..1003`、`1007..1014` 与 `3000..4999`，不允许本地哨兵码 `1005` / `1006` 或保留码上网；`1010` 仍只允许客户端主动发送（RFC 6455 §7.4.1）。接收事件的 `close_code()` 保留整数语义，包括未出示状态时的本地 `1005`，不能未经检查直接复用于发送。

`websocket_message` 方法：
- `opcode()`（`websocket_opcode::text` 或 `binary`）
- `payload()`（返回 `std::string_view`，在下一次 `read()` 启动前有效）
- `text()` / `binary()`（返回 `bool`）

`websocket_message::borrow(opcode, borrowed_text payload)` 用于调用方自有存储的零拷贝消息；字符串字面量、`std::string_view` 和具名 `std::string` / `std::pmr::string` 可直接传入，拥有型字符串右值（包括 `const` 右值）被编译器拒绝。需要发送临时字符串时使用 `text()` / `binary()` 的拥有化入口，不要预先转成 `string_view` 绕过生命周期检查。`borrow()` 不延长存储寿命；消息视图及 `send()` 操作完成前必须保持 owner 和字节有效。

---

## 4. HTTP `CONNECT` 与 `CONNECT-UDP` 隧道

头文件：`"ruvia/web/http_tunnel.h"`。
参考示例：[examples/web/tunnels.cpp](../examples/web/tunnels.cpp)。

### 4.1 服务端 `CONNECT` 字节流隧道

使用 `RUVIA_CONNECT(authority, handler, ...)` 注册普通 `CONNECT` 隧道（支持可选的 `http_tunnel_route_config` 首参，兼容 HTTP/1.1、HTTP/2 与 HTTP/3）：

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

HTTP/2 客户端在收到 2xx 前乐观发送的 `DATA`（RFC 9113 §8.5；RFC 9298 §5，经 RFC 9931 更新）属于隧道数据：协议层在流控窗口内保留这些字节（不归还接收窗口），隧道接受后由 `tunnel.read()` 按序读到；若路由返回非 2xx，已缓冲及后续字节被丢弃并归还流控额度；非 2xx 响应发送完毕后服务端以 `RST_STREAM(NO_ERROR)` 结束该流（RFC 9113 §8.1），而不是等待客户端结束请求半边。RFC 8441 WebSocket（`:protocol = websocket`）同样处理：RFC 6455 §4.1 要求客户端等待 2xx，但这是对发送方的约束，提前到达的 `DATA` 在流控窗口内保留，握手接受后按序交给 WebSocket 会话。已建立隧道上的未知扩展帧类型按 RFC 9113 §5.5 丢弃；`ALTSVC`、`ORIGIN` 帧也一样忽略（RFC 7838 §4、RFC 8336 §2.1），不复位隧道流。

HTTP/3 使用相同路由及隧道门面；WebSocket 经 RFC 9220 Extended CONNECT，普通 CONNECT 经 RFC 9114 §4.4。`finish()` 仅关闭发送方向，接收方向继续读到对端结束；隧道流与同连接的其他请求独立。

TLS 隧道把对端未发送 `close_notify` 就关闭 TCP 视为输入结束，`tunnel.read()` 返回 `std::nullopt`（与 Go `crypto/tls` 在记录边界接受截断的策略一致）；HTTP/1.1 响应体等有消息定界的读取路径仍把该情形作为传输错误上报。

### 4.2 服务端 `CONNECT-UDP`（RFC 9298）与 Capsule 数据报隧道

使用 `RUVIA_CONNECT_PROTOCOL("connect-udp", path, handler, options, ...)` 注册 RFC 9298 UDP 代理隧道（`connect-udp` 路由自动设置 `http_tunnel_route_config{.datagrams_ = true}`，下例显式写出仅为说明）：

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

其他升级协议隧道（HTTP/2 Extended CONNECT）通过 `http_tunnel_route_config{.datagrams_ = true}` 声明使用 Capsule 协议：该路由的 2xx 响应携带唯一的 `Capsule-Protocol: ?1`（[RFC 9297 §3.4](https://www.rfc-editor.org/rfc/rfc9297.html#section-3.4)；`connect-udp` 由 RFC 9298 握手自行生成，不重复添加），处理器才可调用 `tunnel.datagrams()`、`udp()` 或 `capsules()`；未声明的路由调用这三者抛出 `std::logic_error`（协商由路由协议负责）。普通 `CONNECT` 路由声明 `datagrams_` 时注册即抛出 `std::invalid_argument`。

HTTP/3 CONNECT-UDP 在双方协商 `SETTINGS_H3_DATAGRAM` 及 QUIC DATAGRAM 后优先发送原生不可靠数据报；否则使用请求流中的 DATAGRAM Capsule。`udp()` 在两种传输上统一处理 Context ID 0，空载荷也是有效数据报。不要将成功 `send()` 视为对端已收到：原生 QUIC DATAGRAM 不重传。

| 类名 | 核心方法 |
| --- | --- |
| `ruvia::http_tunnel` | `read()`（返回 `std::optional<std::pmr::string>`）、`write(bytes)`、`finish()`、`abort()`、`udp(config = {})`、`datagrams(config = {})`、`capsules(config = {})` |
| `ruvia::http_capsule_stream` | `read()`（返回 `std::optional<http_capsule>`）、`write(type, payload)`、`finish()`、`abort()` |
| `ruvia::http_datagram_stream` | `read()`（返回 `std::optional<http_datagram>`）、`send(payload)`、`finish()`、`abort()` |
| `ruvia::http_udp_tunnel` | `read()`（返回 `std::optional<http_datagram>`，自动剥离/封装 `context_id = 0`）、`send(payload)`、`finish()`、`abort()` |

Capsule 解析遵循 [RFC 9297 §3.2](https://www.rfc-editor.org/rfc/rfc9297.html#section-3.2)：读取方不使用的 Capsule 被静默丢弃并跳过，不缓冲、不触发回调，也不受 `http_capsule_config::max_capsule_length_` 约束——`datagrams()` 丢弃 `DATAGRAM` 以外的类型，`udp()` 还会丢弃 Context ID 非 0 的 `DATAGRAM`（RFC 9298 §4，长度上限在读出 Context ID 之后才判断）；`capsules()` 保留全部类型，因此上限作用于每个 Capsule。保留的 Capsule 超过上限、流在 Capsule 中途结束或 `DATAGRAM` 格式错误时按畸形消息处理（RFC 9297 §3.3）：读取抛出异常并中止隧道，HTTP/2 隧道以 `RST_STREAM(PROTOCOL_ERROR)` 复位该流（RFC 9113 §8.1.1），HTTP/1.1 关闭连接。

HTTP/3 的畸形 Capsule 中止对应流并使用 `H3_MESSAGE_ERROR`；原生 HTTP Datagram 的关联前缀与 SETTINGS 错误由协议层按 RFC 9297 处理，不混同于 Capsule 流解析错误。

## 5. 出站 HTTP 客户端

头文件：`"ruvia/web/http_client.h"`。启动时按 origin 注册具名客户端，每个业务 worker 独占其连接池；请求处理器通过 `c.client(alias)` 获取能力句柄。

```cpp
ruvia::app().client({
    .alias_ = "api",
    .base_url_ = "https://api.example.com",
});
```

请求使用链式构造器，响应保留明确的所有权与流式消费边界：

```cpp
auto client = c.client("api");
auto response = co_await client.get("/users")
    .query("name", "Ada Lovelace")
    .header("Accept", "application/json")
    .send();
if (!response.ok()) {
    c.draft().status(ruvia::http_status::bad_gateway);
    co_return c.text("upstream failed");
}
auto body = co_await response.text();
co_return c.text(body.text());
```

| 入口 | 契约 |
| --- | --- |
| `get/post/put/patch/del/head/options(path)`、`request(method, path)` | 返回拥有输入的请求构造器；配置的 `base_url_` 决定 origin |
| `.query(name, value)`、`.header(name, value)`、`.headers(fields)` | 添加查询参数与请求字段 |
| `.body(bytes)`、`.json(value)` | 设置拥有型请求体；JSON 使用同一套 Model 序列化契约 |
| `.send()` | 消费构造器，返回惰性 `task<http_client_response>`；同一构造器不能重复发送 |
| `.upload(config)` | 返回流式交换；通过 `exchange.body().write()` / `end(trailers)` 上传，通过 `exchange.response()` 获取响应 |
| `response.ok()`、`status()`、`protocol_version()` | 查询 2xx 成功状态、HTTP 状态和实际协议版本 |
| `response.text(max_bytes)`、`bytes(max_bytes)` | 收集尚未消费的正文；返回拥有型 `http_client_response_bytes`，以 `.text()` / `.bytes()` 借用内容 |
| `response.json<T>(max_bytes)` | 收集正文并按公开 Model JSON 契约解析 |
| `response.stream().read()` / `.text()` | 拉取下一块正文；`nullopt` 表示 EOF，视图在下一次正文操作启动前有效 |
| `response.pipe_to(c)` | 将上游响应转发给当前服务端响应；提交后的传输失败中止流，不伪造完整响应 |
| `client.connect(authority).open()`、`client.udp(path).open()` | 打开字节隧道或 CONNECT-UDP；结果区分已建立的隧道与非 2xx HTTP 响应 |
| `client.migrate(local_address, port)` | 对已有 HTTP/3 连接发起本地地址迁移，保留连接与流状态 |
| `client.websocket(path).connect()` | 返回拥有型 WebSocket 连接；`send()` / `receive()` 收发消息，`close()` 执行关闭握手 |

`text()`、`bytes()`、`json<T>()` 与 `stream()` 消费同一条线性正文流，不缓存可重复读取的副本。结果收集同时受调用方 `max_bytes`、客户端响应上限和 worker 共享结果预算约束。Header/Trailer 视图借用响应对象；拥有型收集结果可独立保留。

WebSocket 构造器通过 `.header()` / `.headers()`、`.subprotocol()`、`.deflate()`、`.lifecycle()` 和 `.max_message_bytes()` 配置握手与消息生命周期；origin、TLS 与协议选择沿用所属 HTTP 客户端，不另建一套注册入口。`receive()` 返回 `nullopt` 表示结束；返回消息借用连接的接收存储，视图在下一次 `receive()` 或 `close()` 前有效。连接不能跨 worker 使用。

出站传输及 WebSocket 握手错误统一抛出 `http_client_error`；`handshake_rejected` 可通过 `status()` 读取拒绝响应的 HTTP 状态，消息越限使用 `message_too_large`。配置非法抛出 `std::invalid_argument`，已消费构造器或并发占用同一读写通道等调用错误抛出 `std::logic_error`。WebSocket 的 `receive`、`send`、`ping`、`pong`、`close` 接受逐次操作选项，收紧超时或附加取消。

句柄与响应操作留在所属 worker；请求中的句柄受请求作用域约束，不能跨请求保存。`with_options(operation_options)` 可附加取消与更紧的截止时间。独立事件循环使用 `http_client(loop, config)`，通过 `.client()` 取得句柄；结束前 `co_await owner.shutdown()`，并在事件循环退役前收束所有已启动任务。

自动重定向由 `.redirects(maximum)` 控制（默认最多 5 次）；只跟随同一 origin 或启动时已注册的其他 origin，未知 origin 或 HTTPS → HTTP 降级直接返回重定向响应。跨 origin 时移除 `Authorization`、`Proxy-Authorization` 与 `Cookie`，目标客户端按自己的 TLS、连接池和凭据配置执行。独立客户端未注册其他 origin，因此只自动跟随同源重定向。

`http_client_config::protocol_` 可限定为 `http1_only`、`http2_only` 或 `http3_only`。HTTP/3 使用 QUIC/TLS 1.3；连接、读写、准入与整体请求期限仍由客户端配置约束。`http3_` 分别限制并发流、Alt-Svc 缓存、数据报及待确认输出。

`negotiate` 模式从 HTTPS 响应的 `Alt-Svc` 缓存 `h3` 替代地址，后续请求尝试 QUIC，证书身份仍按原始 origin 校验。替代端点在请求发送前连接失败时淘汰该缓存项并回落至原站 TCP/TLS；显式 `http3_only` 不回落。升级与回落共享原请求期限，不降级为明文 HTTP。

客户端 0-RTT 默认关闭，需显式设置 `http3_.enable_early_data_ = true`；仅在持有已认证连接保存的 TLS 票据、QUIC 传输参数与对端 SETTINGS 时尝试。只有无正文、非流式上传、非隧道的 `GET` / `HEAD` / `OPTIONS` / `TRACE` 请求可提前发送。TLS 拒绝早期数据或服务器返回 `425` 时，等待握手完成后最多重发一次 1-RTT 请求，并继续受原请求期限约束；其他请求不自动进行早期重放。

迁移沿用 ngtcp2 的路径验证机制；连续迁移的已知限制见[监听器文档](web-guide.md#ruvialisten_config-与-ruviatls_config)，不能把单次成功当作无限次连续迁移保证。

Server Push 与 ORIGIN/ALTSVC 帧观察仅适用于 HTTP/2。HTTP/3 不授权 Server Push；Alt-Svc 从已验证源站的响应字段获取，不把 HTTP/2 扩展帧接口当作 HTTP/3 能力。

示例：[http_client.cpp](../examples/web/http_client.cpp)、[http_client_advanced.cpp](../examples/web/http_client_advanced.cpp)。
