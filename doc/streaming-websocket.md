# 流式传输、WebSocket 与隧道 API 手册

本文档涵盖 `ruvia::web` 的流式请求体/响应流、SSE、HTTP/1.1、HTTP/2、HTTP/3、WebSocket 与 CONNECT/CONNECT-UDP，以及统一出站客户端。所有公开符号均位于 `namespace ruvia`。

---

## 1. 流式请求体与流式响应 / SSE

头文件：`"ruvia/web/streaming.h"`、`"ruvia/web/multipart_reader.h"`、`"ruvia/http/sse.h"`。
不经路由、以同一组 `body_reader` / `response_stream_writer` 直接处理监听器请求的方式见 [web-guide 1.8 请求流分发](web-guide.md#18-请求流分发ruviawebrequest_streamh)；绑定调用方事件循环、接管外部已接受连接见 [1.9 独立入站宿主](web-guide.md#19-独立入站宿主ruviawebhttp_serverh)。

### 1.1 流式消费请求体（`body_reader` 与 `multipart_reader`）

在 `RUVIA_POST`、`RUVIA_PUT` 或 `RUVIA_PATCH` 路由宏中附加 `ruvia::stream_body` 策略即可注册流式请求体路由。此类路由不会预先缓冲整个请求体，而是由处理器按块拉取。

> **内容编码**：`stream_body` 路由只移除传输编码（`Transfer-Encoding`，如 `chunked`、`gzip`），不解码请求的 `Content-Encoding`。无论请求声明何种内容编码（包括 `gzip` 等已知编码与 `xyz` 等未知编码），处理器读到的都是编码后的原始字节，框架既不解码也不回 `415`；需要时由处理器自行检查 `Content-Encoding` 并解码或拒绝。缓冲路由则会解码受支持的内容编码，对不支持的编码回 `415`（附 `Accept-Encoding`）、对损坏的编码数据回 `400`。RFC 9110 §8.4 中以 `415` 拒绝不可接受的内容编码是 MAY，两种行为均符合规范。

> **体积限额与拒绝生命周期**：`ruvia::body_limit<N>` 可与 `stream_body` 叠加，且只能收紧部署级请求体限额。HTTP/2 在声明的 `Content-Length` 或已接收的 DATA 超限时，若响应头尚未提交，仍通过应用错误处理器返回 `413`（`request_body_too_large`）；处理器已启动但尚未读取请求体时也适用。已确定超限后不会再发送 `100 Continue`。响应头提交后再发现超限则重置该流，不能发送第二个响应头。拒绝任务按值持有错误元数据快照，协议诊断文本复制到请求内存域，直到异步错误处理与响应发送结束；不会借用生成拒绝结果时的栈临时对象。

#### `ruvia::body_reader`（通过 `c.req().body_reader()` 获取）

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &` | 读取下一个请求体字节块；返回 `std::nullopt` 表示已到达请求体末尾（EOF） |
| `task<std::optional<std::string_view>> text(operation_options operation = {}) &` | 读取下一个请求体字符视图块；返回 `std::nullopt` 表示 EOF |

> **生命周期约束**：每次 `read()` 或 `text()` 返回的视图仅在下一次调用 `read()` / `text()` 启动前有效；同一 `body_reader` 上同一时刻只能有一个运行中的读取操作。

> **HTTP/1 读取终态**：主动读到 EOF 表示分帧与传输解码均已完成，重复读取仍返回 EOF。读取失败后再读抛 `std::logic_error`；自动消费已缓冲字节也不能使失败的正文恢复完整状态或连接复用。

- **逐次读取中断**：`operation` 的超时或停止只结束本次等待，抛 `std::system_error`（`timed_out` / `operation_canceled`），不返回 EOF；不替代宿主取消，也不自动提交响应或重置流。
- **继续读取**：HTTP/1、HTTP/2、HTTP/3 均保留已缓冲正文、解码状态与读通道，下次读取从中断处继续；其他流和响应方向不受影响。HTTP/1 提交响应时正文仍未读完则响应后不复用连接。
- **时限叠加**：服务器正文超时继续生效，逐次超时只收紧；非正超时抛 `std::invalid_argument`。

#### `ruvia::multipart_reader`（通过 `c.req().multipart_reader()` 获取）

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<multipart_stream_part>> read(operation_options operation = {}) &` | 流式读取 `multipart/form-data` 的下一个分片块，返回 `std::nullopt` 表示全部解析完成；`operation` 约束本次调用内的全部正文读取，中断语义同 `body_reader::read()`，下次调用继续解析 |

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
| `task<stream_commit_result> commit(operation_options operation = {}) &` | 显式提交头和 session；返回 `body` 才可生产正文，返回 `completed` 必须停止生产（HEAD、304 等正常无正文完成） |
| `task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &` | 在正文阶段写入字节块（HTTP/1.1 chunked，HTTP/2 DATA）；要求显式提交，提交前或完成后写入抛 `std::logic_error` |
| `task<void> write(std::string_view chunk, operation_options operation = {}) &` | 写入一段字符串视图 |
| `task<void> write(std::pmr::string&& chunk, operation_options operation = {}) &` | 移动写入 PMR 字符串（同内存资源下零拷贝接管） |
| `task<void> writeln(std::string_view chunk, operation_options operation = {}) &` | 写入文本并追加 `\n` |
| `task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {}) &` | 仅在正文阶段等待；`c.stop_token()` 或 `stop` 取消（含客户端断开）及停机返回 `stop_requested`，正常超时返回 `elapsed`；提交前/完成后调用抛 `std::logic_error`。HTTP/2 流终止抛传输异常。与 `sleep_for` 相同，时长是睡眠目标而非操作时限，故只接受停止令牌 |
| `bool aborted() const noexcept` | 只报告已观察到的真实失败/中止（写失败、客户端断开），正常完成（含 HEAD、304）为 `false` |
| `task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &` | 正常结束流式响应，可选附带响应 Trailer 字段 |

- **输出中断**：`commit()`、`write()`、`writeln()`、`end()` 的 `operation` 超时或停止中止本请求（HTTP/2、HTTP/3 以 `CANCEL` / `H3_REQUEST_CANCELLED` 重置流，HTTP/1 关闭连接；已写出部分无法撤回），抛 `std::system_error`（`timed_out` / `operation_canceled`）。
- **启动前停止**：令牌在操作启动前已停止时直接抛 `operation_canceled`，不发出任何字节、不中止请求。
- **时限叠加**：服务器 `write_timeout_` 继续生效，逐次超时只收紧；非正超时抛 `std::invalid_argument`。

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
| `task<stream_commit_result> commit(operation_options operation = {})` | 与普通流相同：先检查 `body` / `completed`，再启动订阅或事件生产 |
| `task<void> write(const sse_message& message, operation_options operation = {})` | 格式化并发送一条 SSE 事件（自动处理多行 `data:` 切分与 UTF-8 校验） |
| `task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {})` | 挂起等待指定时长；输入契约见 [Core 超时文档](core-and-http.md) |
| `bool aborted() const noexcept` | 只报告真实失败，正常 HEAD 完成不是取消 |
| `task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {})` | 结束 SSE 流 |

`sse_writer` 是可复制句柄，操作借用其响应流；`operation` 语义同 `response_stream_writer`。

#### 客户端断开即取消请求

流式响应路由（处理器返回 `ruvia::task<void>`）中，客户端关闭连接会停止 `c.stop_token()`，与 Go `net/http` 的后台读取、Pingora `read_body_or_idle` 及 nginx 默认 `proxy_ignore_client_abort off` 一致。该 token 已由 `db()`、`redis()`（含 Pub/Sub 订阅）、`run_blocking()` 与 `sse.sleep()` / `stream.sleep()` 自动绑定，空闲 SSE 等待订阅消息时也能及时结束，无需显式启用。

- HTTP/1 明文和 TLS：请求体完整消费后，框架在连接所属事件循环上发起一次异步 transport read 观察读侧终止；没有探测写入、心跳依赖或 socket 状态轮询。没有请求体的 GET 在分派时即开始；带请求体时在处理器读完请求体（如 `co_await c.req().text()`）的那次读取返回前开始。未读完的请求体字节仍在 socket 中时不检测，也不自动排空或抢占请求体读取。
- 单独到达的请求（同一批输入中前面没有其他请求）遇 EOF、TLS `close_notify`；任何请求遇 TLS 截断（无 `close_notify` 的 TCP 结束）、TCP RST 或写失败，都停止 `c.stop_token()`，`aborted()` 随之为 `true`，之后的写入抛传输异常；当前流按截断结束并关闭连接。正常结束与异常退出都先取消并等待检测结束，再释放连接资源。
- **TCP 写半关闭**：[RFC 9112 §9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6) 指出客户端半关闭本身不表示放弃响应，但单个 FIN 无法区分“关闭整个客户端”与“仅关闭发送方向”。流式响应把单独到达请求之后的 FIN 视为客户端离开：只发送一个请求即 `shutdown(SHUT_WR)`、仍要读取完整流式响应的客户端会被截断，这类客户端应改为保持发送方向打开或使用缓冲响应路由。缓冲响应路由不受影响。
- **Pipeline**：检测期间读到的后续请求字节按原顺序完整保留，不将“可读”误判为断开。pipeline 内的干净结束（TCP FIN 或 TLS `close_notify`）是请求方向半关闭，与 FIN 到达的时机无关：当前请求排在前一请求之后到达（客户端在前一响应结束前已发出它，无论前一路由是缓冲还是流式），或其后已有后续请求字节时，不取消当前响应；FIN 之前已收到的请求（包括流式响应，其 `c.stop_token()` 不因该 FIN 停止）照常处理，全部响应后关闭连接，不再读取 transport。TCP RST 与 TLS 截断仍中止。预读达到既有请求头大小上限时暂停检测，直到当前响应结束；不为探测无界缓冲。
- HTTP/2 不新增读操作：本流 `RST_STREAM` 或使该流终止的连接关闭停止 `c.stop_token()`，不取消同连接其他 stream。请求方向正常的 `END_STREAM` 不取消响应；`GOAWAY` 遵循连接排空规则。
- 网络黑洞、客户端断电等没有 FIN/RST 到达的情况不能保证即时识别，仍由既有超时策略处理，不新增保活探测。
- 取消是协作式的：未绑定 `c.stop_token()` 的自定义等待须显式传入它，并在取消后返回、释放订阅；不会抢占 CPU 循环或无取消支持的等待。

最小接入：把 `c.stop_token()` 传给订阅的异步接收。下面以公开的 `channel_receiver` 表示订阅接收端：

```cpp
#include <memory_resource>
#include <string>
#include "ruvia/core/channel.h"
#include "ruvia/web/context.h"
#include "ruvia/web/streaming.h"

ruvia::task<void> stream_subscription(
    ruvia::context& c,
    ruvia::channel_receiver<std::pmr::string>& subscription,
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

## 2. HTTP/1.1、HTTP/2 & HTTP/3 服务端高级协议能力

参考示例：[examples/web/http_features.cpp](../examples/web/http_features.cpp)。

| `ruvia::context` / `context_request` 方法 | 适用协议与说明 |
| --- | --- |
| `co_await c.inform(const http_interim_response_head& response, operation_options operation = {})` | 在最终响应头提交前发送 `1xx` 临时响应（如 `103 Early Hints`：`http_interim_response_head(http_status::early_hints, headers)`）；提交后启动（包括先前创建的冷操作）抛 `std::logic_error`；`operation` 中断按输出操作处理（重置请求，启动前停止不发出字节） |
| `c.draft().priority(http_priority_fields{.urgency_ = 2, .incremental_ = true})` | 设置当前响应优先级（HTTP/2 与 HTTP/3 同时更新流调度器优先级）；跨替换策略用 `c.policy().priority(...)`，客户端请求优先级（含 `PRIORITY_UPDATE` 帧动态更新）用 `c.req().priority()` |
| `bool c.push(http_push_request_view request)` | （HTTP/2）向开启了 Server Push 的客户端推送关联资源，同步返回 `bool` 表示是否被接纳；`method_`、`scheme_`、`authority_`、`path_` 使用 `borrowed_text`，`headers_` 使用 `borrowed_http_headers`，拒绝临时拥有型字符串与临时首部容器；对端禁用推送，或已打开的推送流（不含仅预留的推送）已达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS`（包括其为 0）时返回 `false`，不发送 `PUSH_PROMISE`；已预留的推送在提交响应头时仍无并发额度则以 `RST_STREAM(CANCEL)` 取消，不视为连接失败 |
| `void c.advertise_origins(std::span<const std::string_view> origins)` | （HTTP/2、HTTP/3，需 TLS）在连接或控制流上排入 RFC 8336 / RFC 9412 `ORIGIN` 帧，通告当前连接可合并服务的权威源集合 |
| `void c.advertise_alternative_service(std::string_view value)` | （HTTP/2）在连接上排入 RFC 7838 `ALTSVC` 帧通告替代服务 |
| `c.req().trailers()` / `c.req().trailer(name)` | 在流式请求体读取到 EOF 后，读取客户端发送的请求 Trailer 首部；按 RFC 9110 §6.5.1 不允许出现在 Trailer 中的字段（如 `Content-Length`、`Cache-Control`）被丢弃而不导致请求失败，也不会并入请求头 |
| `co_await stream.end(trailers)` | 在正文阶段结束响应并发送 Trailer；须在 commit 前声明 `c.draft().header("Trailer", "...")`，completed 结果不生产 trailers |

---

## 3. WebSocket 服务端

头文件：`"ruvia/web/websocket.h"`、`"ruvia/http/websocket_protocol.h"`。
参考示例：[examples/web/websocket.cpp](../examples/web/websocket.cpp)。

Ruvia WebSocket 服务端支持 HTTP/1.1 `Upgrade: websocket`、HTTP/2 Extended CONNECT（RFC 8441）与 HTTP/3 Extended CONNECT（RFC 9220）；后两者使用 `:protocol = websocket`，路由代码统一。
不注册路由时，通过 [`request_stream::accept_websocket()`](web-guide.md#18-请求流分发ruviawebrequest_streamh) 显式协商并取得同一个 `websocket` 会话接口。

- **Extended CONNECT 字段**：HTTP/2 中的 `Sec-WebSocket-Key` / `Sec-WebSocket-Accept` 已被 `:protocol` 取代，按 [RFC 8441 §5](https://www.rfc-editor.org/rfc/rfc8441.html#section-5) 不做处理，既不校验也不拒绝。
- **子协议提议**：接收方跳过 `Sec-WebSocket-Protocol` 提议中的重复子协议；唯一性是 RFC 6455 §4.1 对发送方客户端的要求。
- **版本字段**：`Sec-WebSocket-Version` 必须恰好出现一次，缺失或重复按无效握手拒绝。

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
    .max_message_bytes_ = ruvia::protocol_byte_limit::limited(256 * 1024),
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

- **心跳时序**：服务端与客户端复用心跳判定；Ping 完整写出后才开始 Pong 等待。
- **单会话接收限额**：`max_message_bytes_` 默认为继承服务器上限，只能收紧 `server_config::max_websocket_message_bytes_`；帧声明、分片累计及解压后增长均受限。它不降低发送上限；挑战页可在 `accept_websocket(config)` 中单独配置，无须降低整个 ingress 的限额。

### 3.2 `ruvia::websocket` 与 `ruvia::websocket_message` API

| 方法签名 | 说明 |
| --- | --- |
| `task<std::optional<websocket_message>> read(operation_options operation = {}) &` | 读取下一条完整 WebSocket 数据消息（底层自动处理分片重组与 Ping/Pong 心跳）；会话结束（对端或本端关闭握手完成、EOF、拒收帧、`abort()`、心跳或关闭超时）时返回 `std::nullopt`，原因由 `termination()` 区分 |
| `task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &` | 发送 UTF-8 文本消息（另有 `std::pmr::string&&` 重载） |
| `task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &` | 发送二进制消息（另有 `std::span<const std::byte>` 与 `std::pmr::string&&` 重载） |
| `task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &` | 零拷贝发送 `read()` 返回的 `websocket_message`（发送完成前占用读通道） |
| `task<void> ping(std::string_view payload = {}, operation_options operation = {}) &` / `pong(…)` | 发送显式 `Ping` 或 `Pong` 控制帧（另有 `std::pmr::string&&` 重载） |
| `task<void> close(websocket_close_options options = {}, operation_options operation = {}) &` | 发起或完成关闭握手，只占写通道（默认 `code_ = websocket_close_status::normal_closure`） |
| `void abort() & noexcept` | 立即中止底层传输而不执行关闭握手 |
| `websocket_message_writer begin_text(websocket_send_options options = {}) &` / `begin_binary(…)` | 开始一条分片发送的数据消息，见下文“分片发送” |
| `std::string_view subprotocol() const& noexcept` | 获取握手协商选中的 WebSocket 子协议（未选中时为空视图） |
| `std::optional<std::uint16_t> peer_close_code() const noexcept` / `std::string_view peer_close_reason() const& noexcept` | 读取对端发送的 Close 帧状态码与 UTF-8 原因（收到对端 Close 帧后可用） |
| `websocket_termination termination() const noexcept` | 会话结束原因；活动中为 `none`，首个原因胜出，`read()` 返回 `std::nullopt` 后稳定可读 |
| `std::uint64_t bytes_received() const noexcept` / `bytes_sent() const noexcept` | 双向线上 WebSocket 帧字节，会话结束后仍可读 |

- **通道**：同一时刻最多运行一个读取与一个输出操作，同方向重叠抛 `std::logic_error`；未启动的冷操作可共存。
- **读取中断**：`read()` 的 `operation` 超时或停止请求只结束本次读取，抛 `std::system_error`（`timed_out` / `operation_canceled`）；已缓冲输入、读通道与会话保持可用，可继续读取。
- **输出中断**：输出操作或 `close()` 的 `operation` 超时、停止请求中止会话（已部分写出的帧无法撤回），记为 `timeout` / `aborted` 并抛同类 `std::system_error`；令牌在启动前已停止时不产生任何输出。
- **优雅关闭**：`close()` 可与挂起的 `read()` 并发，Close 发出后由该读取继续解析输入；对端应答后读取返回 `std::nullopt`、记为 `local_close`，关闭握手超时则读取返回 `std::nullopt`、记为 `timeout`，`close()` 抛 `timed_out`。
- **自行排空**：没有挂起读取时 `close()` 自己读到对端 Close，期间启动的 `read()` 抛 `std::logic_error`；未完成的 `send(websocket_message&&)` 借用读缓冲，此时 `close()` 抛 `std::logic_error`。

分片发送（RFC 6455 §5.4）让调用方按站点级带宽预算等外部节奏发送大消息：

- **写入器**：`begin_text()` / `begin_binary()` 返回只移动的 `websocket_message_writer`；`write(chunk, operation = {}) &` 每次发一个非 FIN 帧，传输层接收后完成；`finish(last_chunk = {}, operation = {}) &` 发 FIN 帧。
- **数据通道**：`finish()` 完成前消息独占数据通道，`text()`、`binary()`、`send()` 与再次 `begin_*()` 抛 `std::logic_error`；`ping()`、`pong()`、`close()` 及自动控制帧可插在分片之间。
- **帧规则**：首帧携带 opcode，后续为续帧（opcode 0）；`permessage-deflate` 下 RSV1 只在首帧，整条消息是一条跨分片的 DEFLATE 流，每块以 sync flush 结尾，末块去掉 `00 00 FF FF`；文本跨分片校验 UTF-8，消息上限按整条消息累计；`bytes_sent()` 随每个分片累计。
- **未完成中止**：写入器析构或 `abort()` 时消息未完成即中止会话（对端无法得知消息边界），记为 `aborted`；写入器借用 `websocket`，随其失效。

`websocket_termination`（`"ruvia/http/websocket_protocol.h"`）由协议核心 `websocket_connection` 单点记录；后续 Close、EOF 或 `abort()` 不覆盖已记录原因：

| 取值 | 触发 |
| --- | --- |
| `none` | 会话仍活动 |
| `peer_close` | 对端发起关闭握手；状态码与原因由 `peer_close_code()` / `peer_close_reason()` 给出 |
| `local_close` | 本端 `close()` 发出 Close 后收到对端 Close |
| `eof` | 对端未发 Close 即结束传输（含截断帧；HTTP/2、HTTP/3 下为流结束） |
| `protocol_error` | 对端帧或消息违反 RFC 6455（如未掩码、保留 opcode、非法分片或控制帧），本端已发 `1002` |
| `invalid_payload_data` | 文本消息或 Close 原因不是 UTF-8，本端已发 `1007` |
| `message_too_large` | 帧声明、分片累计或解压后超过接收上限，本端已发 `1009` |
| `timeout` | 心跳 Pong、关闭握手或输出操作超时；客户端另含读取无活动超时（`read_timeout_`）。`read()` 的逐次超时不结束会话 |
| `aborted` | 本端 `abort()`、输出操作被取消、处理器结束拆除会话或本地故障 |
| `io_error` | 传输读写失败，含连接或 HTTP/2、HTTP/3 流被重置；服务端 `write_timeout_` 由连接扫描器直接关闭套接字，同样记为 `io_error` |

- **计数口径**：RFC 6455 帧线上字节，即帧头、掩码键与负载，含 Ping/Pong/Close 控制帧；`permessage-deflate` 下为压缩后负载。
- **计数边界**：不含 HTTP 升级握手；HTTP/2、HTTP/3 下不含 DATA 帧头、QUIC 与 TLS 开销。
- **计数时点**：接收方向计完整解析的帧，被拒绝的帧不计；发送方向计已成功交付传输的字节；两者单调饱和于 `std::uint64_t`。
- **访问期限**：`websocket` 失效（请求处理结束）后访问器返回 `none` 与 `0`；需要会话最终值时在处理器内 `close()` 或读到 `std::nullopt` 后读取。

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
不经 HTTP、直接接管整条 TCP/TLS 连接字节流的监听器见 [web-guide 1.7 原始连接分发](web-guide.md#17-原始连接分发ruviawebserver_connectionh)；不经路由在回调中决定是否接受 CONNECT / HTTP/1.1 Upgrade 隧道见 [web-guide 1.8 请求流分发](web-guide.md#18-请求流分发ruviawebrequest_streamh)。

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

- **操作回收**：与[原始连接分发](web-guide.md#17-原始连接分发ruviawebserver_connectionh)采用相同的在途操作与冷任务回收规则。

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
| `ruvia::http_tunnel` | `read(operation = {})`（返回 `std::optional<std::pmr::string>`）、`write(bytes, operation = {})`、`finish(operation = {})`、`abort()`、`udp(config = {})`、`datagrams(config = {})`、`capsules(config = {})` |
| `ruvia::http_capsule_stream` | `read(operation = {})`（返回 `std::optional<http_capsule>`）、`write(type, payload, operation = {})`、`finish(operation = {})`、`abort()` |
| `ruvia::http_datagram_stream` | `read(operation = {})`（返回 `std::optional<http_datagram>`）、`send(payload, operation = {})`、`finish(operation = {})`、`abort()` |
| `ruvia::http_udp_tunnel` | `read(operation = {})`（返回 `std::optional<http_datagram>`，自动剥离/封装 `context_id = 0`）、`send(payload, operation = {})`、`finish(operation = {})`、`abort()` |

- **逐次读取中断**：`read()` 的 `operation`（`operation_options`）超时或停止只结束本次读取，抛 `std::system_error`（`timed_out` / `operation_canceled`）；未消费输入与半个 Capsule 保留，可继续读取。
- **逐次输出中断**：`write()`、`send()`、`finish()` 的超时或停止中止隧道（部分写出无法撤回）并抛同样的异常；令牌在启动前已停止则不发出任何字节、不中止隧道。
- **时限叠加**：一次 Capsule / Datagram 调用内的多次传输读写共用该调用的截止期；服务器写超时与隧道排空时限继续生效；非正超时抛 `std::invalid_argument`。

- **操作生命周期**：Capsule、Datagram 与 UDP 门面共享同一对读写通道租约，同方向拒绝重叠。销毁门面前必须等待已启动操作结束；未启动冷任务随门面销毁失效。
- **输入与结果所有权**：切换输入或关闭时，先清除借用视图，再释放缓冲并重置输入游标。已交付的 `http_capsule` / `http_datagram` 独立拥有负载，不受后续读取、门面析构或客户端停机影响，但必须在所属 worker 上释放。

Capsule 解析遵循 [RFC 9297 §3.2](https://www.rfc-editor.org/rfc/rfc9297.html#section-3.2)：读取方不使用的 Capsule 被静默丢弃并跳过，不缓冲、不触发回调，也不受 `http_capsule_config::max_capsule_length_` 约束——`datagrams()` 丢弃 `DATAGRAM` 以外的类型，`udp()` 还会丢弃 Context ID 非 0 的 `DATAGRAM`（RFC 9298 §4，长度上限在读出 Context ID 之后才判断）；`capsules()` 保留全部类型，因此上限作用于每个 Capsule。保留的 Capsule 超过上限、流在 Capsule 中途结束或 `DATAGRAM` 格式错误时按畸形消息处理（RFC 9297 §3.3）：读取抛出异常并中止隧道，HTTP/2 隧道以 `RST_STREAM(PROTOCOL_ERROR)` 复位该流（RFC 9113 §8.1.1），HTTP/1.1 关闭连接。

HTTP/3 的畸形 Capsule 中止对应流并使用 `H3_MESSAGE_ERROR`；原生 HTTP Datagram 的关联前缀与 SETTINGS 错误由协议层按 RFC 9297 处理，不混同于 Capsule 流解析错误。

## 5. 出站 HTTP 客户端

头文件：`"ruvia/web/http_client.h"`。具名 origin 各有独立连接池；`http_client(event_loop, ...)` 可直接用于其他项目，不要求 `application`、路由或 `context`。框架处理器通过 `c.client(alias)` 借用注册能力。

### 5.1 请求、响应与会话

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
| `response.framing()` / `content_length()` | 查询协议计划的正文分帧与已解析长度元数据；HEAD/304 可携带长度，HTTP/2、HTTP/3 的流分帧也可同时有长度，不等于解码后的字节数 |
| `response.connection_info()` | 借用物理连接 ID、对端 IP/端口、复用标志及连接/TLS 阶段耗时；隧道结果、WebSocket 同样提供连接信息 |
| `response.text(max_bytes)`、`bytes(max_bytes)` | 收集尚未消费的正文；返回拥有型 `http_client_response_bytes`，以 `.text()` / `.bytes()` 借用内容 |
| `response.json<T>(max_bytes)` | 收集正文并按公开 Model JSON 契约解析 |
| `response.stream().read()` / `.text()` | 拉取下一块正文；`nullopt` 表示 EOF，视图在下一次正文操作启动前有效 |
| `response.pipe_to(c)` | 将上游响应转发给当前服务端响应；提交后的传输失败中止流，不伪造完整响应 |
| `response.stream().pipe_to(writer)` | 只转发正文；调用方先设置响应元数据并 `commit()`，仅 `body` 结果允许转发。入站 Upgrade / CONNECT 拒绝同样支持流式输出，见[请求流分发](web-guide.md#18-请求流分发ruviawebrequest_streamh) |
| `response.reprioritize(http_priority)` | 对尚在传输中的 HTTP/2 或 HTTP/3 响应流发送 RFC 9218 `PRIORITY_UPDATE` 帧 |
| `client.connect(authority).open()`、`client.udp(path).open()` | 打开字节隧道或 CONNECT-UDP；结果区分已建立的隧道与非 2xx HTTP 响应 |
| `request.upgrade(config = {})` | 将带 `Connection: Upgrade`、`Upgrade` 字段的 HTTP/1.1 请求升级为原始字节隧道；非 101 保留为普通流式 HTTP 响应 |
| `client.migrate(local_address, port)` | 对已有 HTTP/3 连接发起本地地址迁移，保留连接与流状态 |
| `client.websocket(path).connect()` | 返回拥有型 WebSocket 连接；读写、关闭、逐次中断与访问器的名称和限定同服务端 `websocket`（见 [3.2](#32-ruviawebsocket-与-ruviawebsocket_message-api)） |

`text()`、`bytes()`、`json<T>()` 与 `stream()` 消费同一条线性正文流，不缓存可重复读取的副本。结果收集同时受调用方 `max_bytes`、客户端响应上限和 worker 共享结果预算约束。Header/Trailer 视图借用响应对象；拥有型收集结果可独立保留。

- **双向异常归属**：上传失败不追溯撤销已验证完整的响应；响应尚未完成时，导致交换失败的传输或解码异常仍由响应消费入口报告。

WebSocket 构造器通过 `.header()` / `.headers()`、`.subprotocol()`、`.deflate()`、`.lifecycle()` 和 `.max_message_bytes()` 配置握手与消息生命周期；origin、TLS 与协议选择沿用所属 HTTP 客户端，不另建一套注册入口。返回消息借用连接的接收存储，视图在下一次 `read()` 或 `close()` 前有效。连接不能跨 worker 使用。

- **结束与失败**：`read()` 在任一方完成关闭握手、或本端 `close()` 已发出 Close 后会话以任何方式结束时返回 `nullopt`；其余失败中止会话并抛 `http_client_error`，读取中断为 `cancelled` / `timeout`。
- **选项继承**：逐次 `operation_options` 继承所属句柄的选项，超时只能收紧，提供的令牌替换继承的用户取消。
- **消息转发**：`send(websocket_message&&)` 在返回冷操作前复制负载，不占读通道。
- **分片发送**：`begin_text()` / `begin_binary()` 返回 `http_client_websocket_message_writer`，契约同服务端写入器；它持有会话状态，须在所属 worker 使用与销毁。
- **访问器**：会话状态访问器与服务端同为 `noexcept`，跨 worker 使用终止进程，已移出的连接返回空值、`none`、`0` 与 `false`；传输元数据 `protocol_version()`、`connection_info()`、`worker()` 对已移出或跨 worker 的调用抛 `std::logic_error`。

出站传输及 WebSocket 握手错误统一抛出 `http_client_error`；`handshake_rejected` 可通过 `status()` 读取拒绝响应的 HTTP 状态，消息越限使用 `message_too_large`。配置非法抛出 `std::invalid_argument`，已消费构造器或并发占用同一读写通道等调用错误抛出 `std::logic_error`。

- **失败连接观测**：`http_client_error::connection_info()` 在已有实际连接时提供自有快照，拨号尚未建立连接时为空；错误中的动态文本不借用响应资源。
- **通用 Upgrade**：调用方提供并转发子协议、扩展及握手字段；此入口只校验 HTTP 切换协定，不替调用方做 WebSocket 消息协商。成功后交付 101 之后已收到的原始字节，不解释消息帧；普通 `.send()` 仍不接受意外 101。
- **Upgrade 协议选择**：`upgrade()` 总在 HTTP/1.1 上进行，复用已协商 HTTP/1.1 的空闲连接或新建仅报价 `http/1.1` 的连接，普通请求继续使用 h2 / h3；版本集合不含 HTTP/1.1 时返回 `protocol_unavailable`。WebSocket 客户端仍按 RFC 8441 / 9220 走所协商协议。
- **放弃上传**：`exchange.body().abandon()` 只停止发送，请求内容保持不完整，`response()` 照常可读。HTTP/1 读完响应后关闭连接；HTTP/2 在完整响应后以 `RST_STREAM(NO_ERROR)` 只结束发送侧，HTTP/3 以 `RESET_STREAM(H3_NO_ERROR)` 只重置发送半流（RFC 9113 §8.1、RFC 9114 §4.1）。

句柄与响应操作留在所属 worker；请求中的句柄受请求作用域约束，不能跨请求保存。拥有者的 `client()` / `client(alias)` 与 `c.client()` 一样只返回句柄（取句柄规则见 [数据手册 1.2](data-and-auth.md#12-直接查询编译期-sql-校验与流式游标db_handle)）；句柄的 `with_options(operation_options)` 可附加用户取消与更紧的截止时间，但不能替换请求宿主的取消。独立拥有者结束前须 `co_await owner.shutdown()`，并在事件循环退役前收束所有已启动任务。

### 5.2 源站、转发与连接控制

| 独立拥有者入口 | origin 选择 |
| --- | --- |
| `http_client(loop, config, worker = {})` | `.client()` 选择唯一配置，`.client(alias)` 可按别名选择 |
| `http_client(loop, span<const http_client_config>, worker = {})` | `.client(alias)` 选择已配置 origin；无参 `.client()` 要求存在 `"default"` |
| `co_await owner.upsert(config, options = {})` | 增加或原子替换一个 alias；入口即拥有输入，TLS 凭据在绑定 `blocking_pool` 中准备，worker 上发布 |
| `owner.remove(alias)` | 撤销一个 alias，返回是否存在；旧池取消并异步 join，其他 alias 的连接与 Cookie 不变 |

配置集合在构造返回前拥有化；所有别名共用拥有者生命周期与 `http_client_worker_config`（应用内为 `server_config::http_client_worker_`，每 worker 一份），各自保留 TLS、连接、Cookie 及凭据策略。

| `http_client_worker_config` | 跨 alias 共享 |
| --- | --- |
| `result_budget_` | 收集结果的保留字节与在途响应字节预算 |
| `max_idle_http1_` / `max_idle_multiplexed_` | 所有 alias 合计保留的空闲 HTTP/1 与 HTTP/2/3 连接（默认各 256），与 alias 级 `connection_limits_` 嵌套，取更紧者；超出时关闭任一 alias 中最久未用的空闲连接 |
| `max_concurrent_dns_refreshes_` | 同时进行的后台 DNS 刷新查询（默认 8）；达到上限的刷新周期顺延到下一周期 |
运行期只对变化的 alias 调用 `upsert()`；它取消被替换池的在途操作，未更新的池不重建。准备失败或取消不发布候选；不可中断的凭据解析完成并收束后才结束取消，`shutdown()` 等待全部准备与退休任务。

自动重定向由 `.redirects(maximum)` 控制（默认最多 5 次）；未设置策略时，只跟随同一 origin 或当前拥有者已注册的其他 origin，未知 origin 或 HTTPS → HTTP 降级返回原响应。`.redirect_policy(task<bool>(http_origin_view, stop_token))` 在跨源跳转前等待调用方策略，策略可先 `co_await owner.upsert(...)` 注册目标；返回 `false` 保留原响应。回调须响应令牌，目标视图只借用到回调完成。跨源始终剥离 `Authorization`、`Proxy-Authorization` 与原源 `Cookie`，目标使用自身 TLS、连接池及凭据策略。

| 回源能力 | 入口与契约 |
| --- | --- |
| 拨号与身份分离 | `base_url_` 决定 Host / `:authority` 及默认 SNI、证书验证名；`dial_host_` / `dial_port_` 只覆盖物理目标。`tls_.server_name_` 可进一步覆盖 TLS 名称，不改 HTTP authority |
| 请求级路由覆盖 | `http_client_request_options` 的 `authority_`（`host[:port]`，拒绝 userinfo）、`dial_host_` / `dial_port_`、`server_name_` 逐请求覆盖 alias 默认值；未设字段继承 alias，scheme 不可改。Host / `:authority` 取有效 authority，SNI 与证书名默认取其 host（IP 字面量不发 SNI、按 IP SAN 校验）；请求级拨号域名只在新建连接时解析并经 `address_validator_` |
| 连接复用键 | 连接按 `(拨号 host, 拨号端口, TLS server name, ALPN 报价)` 复用，Host / `:authority` 不入键：HTTP/1 keep-alive 可跨 Host 复用；HTTP/2、HTTP/3 同一连接上发送其他 authority 时，由调用方保证所选证书覆盖它（RFC 9113 §9.1.1、RFC 9114 §3.3）。每个 alias 只有一个 TLS 上下文，QUIC 会话票据按路由隔离 |
| 覆盖与 origin 策略 | 有效 authority 不同于 alias 时不使用 Cookie jar；Alt-Svc 按实际路由记录与使用（见 5.3），Server Push 只在 alias 路由的连接上接受。重定向按有效 origin 解析，跨 origin 跳转丢弃全部覆盖，同 origin 跳转保留 |
| 无固定源站的出口 alias | `base_url_` 只写 scheme（`"https://"` / `"http://"`）时 alias 不绑定 origin：每个请求必须给 `authority_`（否则 `std::invalid_argument`），拨号默认取其 host 与端口；构造期要求 `address_validator_`，并拒绝 Cookie 配置与 `tls_.server_name_`；无 Cookie jar、无 Alt-Svc、无 DNS 快照（`update_dns` / `refresh_dns` 抛 `std::logic_error`）。`redirect_policy` 接受的同 scheme 跨源跳转仍经此 alias 并改用新 authority |
| 按需连接与上限 | `connection_limits_` 中 `max_open_` 限定全部物理连接（含拨号中与空闲，`nullopt` 不限）；达上限时关闭最久未用的空闲连接，无空闲则在 `acquire_timeout_` 内等待。`max_idle_http1_` / `max_idle_multiplexed_`（默认各 64）限定跨路由保留的空闲 HTTP/1 与 HTTP/2/3 连接，超出立即关闭最久空闲者 |
| 连接观测 | handle 的 `stats()` 中 `open_connections_` 为尚未回收的连接对象数，`idle_connections_` 为空闲连接数；启动不预建连接，关闭且无借用者的连接对象随即回收 |
| SSRF 策略 | `address_validator_(numeric_ip, port)` 校验实际数值候选，再交给 TCP/QUIC 拨号；与重定向 origin 策略分开配置，也覆盖替代服务端点 |
| 原样编码流 | origin 的 `decode_content_ = false`，或请求 `.decode_content(false)`：保留 `Content-Encoding` / `Content-Length`，不等待完整正文便交付首块；默认仍自动解码 |
| 转发字段 | `.field_source(http_request_field_source::forwarded)` 使用协议层接收/转发语义，不套用自生成字段的发送方限制；分帧与歧义校验不放松 |
| 即时 1xx | `.on_informational(task<void>(http_status_code, span<const http_header>, stop_token))` 按线序等待每个中间响应；字段借用到回调完成，不必等待最终响应头 |
| 每请求阶段超时 | `.timeouts(http_client_phase_timeouts)` 分别覆盖 `connect_`、`read_`、`write_`、`acquire_`；默认继承，`http_client_timeout_override::after(duration)` 替换，`disabled()` 只禁用该阶段 |
| 独立探测连接 | `.fresh_connection()` 使用不借用、也不归还业务复用池的物理连接，支持 HTTP/1、HTTP/2、HTTP/3 |
| 统一请求配置 | `.configure(http_client_request_options)` 设置上述请求策略；CONNECT 构造器也提供该入口 |
| DNS 观察与更新 | handle 的 `dns_snapshot()` 返回自有数值地址；`update_dns(addresses)` 同步校验并发布外部快照，`co_await refresh_dns()` 主动解析 alias 拨号名。`dns_refresh_interval_` 每周期只重新解析仍有存活连接的拨号域名（含请求级 `dial_host_`，IP 字面量不查询，无连接不发查询），受 worker 级并发上限约束；答案不再包含连接所用地址时退役该连接。`stats().dns_refreshes_` 计数后台查询 |
| 定向断连 | handle 的 `close_connection(id)` 按实际物理连接 ID 关闭，旧 ID 不会命中新连接；共享多路复用连接上的流一并终止 |

- **总期限独立**：阶段覆盖不改变 `request_timeout_` 或宿主取消。总期限仍计入慢下游与 1xx 回调等待；代理若不需要整体时限，须在 origin 配置中显式设为 `nullopt`。读无活动预算在等待调用方消费正文时暂停。
- **慢速上传**：最终响应头到达前，请求内容的写出进展同样重启读无活动预算：等待响应头的无活动期从最后一次双向进展起算，源站读完全部请求体后再应答不会被误判超时。
- **隧道空闲**：CONNECT / Upgrade 隧道建立后不再受 `read_timeout_` 约束，只受 `http_client_tunnel_config::idle_timeout_`（默认 `nullopt` 不限）与总期限约束；任一方向的字节进展都重启它。
- **1xx 背压边界**：HTTP/2、HTTP/3 只背压当前响应，不阻塞其他流；回调异常终止该交换。回调不得反过来等待本交换，或依赖它释放池容量的操作。
- **地址退役**：刷新失败保留旧快照；地址集合改变时关闭空闲连接和隧道，活动 HTTP/1 完成本次后关闭，HTTP/2、HTTP/3 停止接新流并排空。等待旧连接退役仍计入 acquire 预算。
- **独立连接预算**：fresh 请求的独立连接同样计入 `max_open_`，用后关闭并回收。
- **协商同路由**：同一路由的连接仍在协商协议且可能为 HTTP/2 时，新请求等待其结果再选择复用或新建，计入 acquire 预算。

### 5.3 协议选择与扩展

`http_client_versions`（`http1_` / `http2_` / `http3_`）是请求可用的版本集合：`http_client_config::versions_` 给 alias 默认值（{1, 2}），`http_client_request_options::versions_` 可逐请求覆盖，一个 alias 即可服务不同档位的站点。空集合、未编译进库的版本或明文下的 HTTP/3 抛 `std::invalid_argument`。

| 集合 | https | http |
| --- | --- | --- |
| 含 1 或 2 | TCP 上 TLS ALPN 报价集合 ∩ {1, 2}（`http/1.1` / `h2`），ALPN 报价入连接复用键 | 含 1 走 HTTP/1.1；仅 {2} 为 prior-knowledge h2c |
| 含 3 且含 1 或 2 | 先走 TCP，同一路由的认证响应通告 `Alt-Svc` `h3` 后升级 QUIC | 不允许 |
| 仅 {3} | 直连路由的 QUIC | 不允许 |

TCP TLS 的版本与旧版兼容例外见[协议策略](web-guide.md#ruvialisten_config-与-ruviatls_config)；含 1 的协商可在旧服务端使用 HTTP/1.1，仅 h2 的报价不降到 TLS 1.1，非法的低版本 `h2` 在发送应用数据前拒绝。HTTP/3 使用 QUIC/TLS 1.3；连接、读写、准入与整体请求期限仍由客户端配置约束。`http3_` 分别限制并发流、Alt-Svc 缓存、数据报及待确认输出。

- **Alt-Svc 按路由**：替代服务按实际路由（拨号 host、拨号端口、TLS server name）记录与查找，包括 alias 原路由与请求级覆盖；只采信 TLS 认证的响应，alt-authority 省略 host 时取该路由的拨号 host，QUIC 用同一 server name 校验证书。不含 3 的请求不记录、不升级。
- **容量**：条目数受 `http3_.max_alt_svc_entries_` 跨路由约束，最久未用者先淘汰；`Alt-Svc: clear` 只清除该路由。
- **失败回落**：替代端点在请求发送前连接或握手失败时，请求回落该路由的 TCP，不因此失败；该替代条目 5 分钟内标记不可用、不再尝试，重新通告也不解除（RFC 7838 §2.4）。仅 {3} 不回落。升级与回落共享原请求期限，不降级为明文 HTTP。

客户端 0-RTT 默认关闭，需显式设置 `http3_.enable_early_data_ = true`；仅在持有同一路由（拨号目标与 server name）已认证连接保存的 TLS 票据、QUIC 传输参数与对端 SETTINGS 时尝试；票据按最久未用淘汰，条目数上限同 `max_idle_multiplexed_`（至少 1）。只有无正文、非流式上传、非隧道的 `GET` / `HEAD` / `OPTIONS` / `TRACE` 请求可提前发送。TLS 拒绝早期数据或服务器返回 `425` 时，等待握手完成后最多重发一次 1-RTT 请求，并继续受原请求期限约束；其他请求不自动进行早期重放。

- **路径迁移**：沿用 ngtcp2 路径验证并保留原连接，不自动重放失败请求。CID 容量见[监听器文档](web-guide.md#ruvialisten_config-与-ruviatls_config)。
- **迁移期限**：握手确认、CID 补充等待与路径验证共用 `connect_timeout_` 和 `with_options()` 中更紧的整体期限。
- **迁移终止**：用户取消返回 `cancelled`，期限到期返回 `timeout`；客户端关闭或事件循环停止导致连接退役时，优先返回 `closing`。

`client.next_advertisement()` 可观察 HTTP/2 与 HTTP/3 连接收到的 `ORIGIN` 帧（`advertisements_.receive_origins_ = true`）以及 HTTP/2 连接收到的 `ALTSVC` 帧（`advertisements_.receive_alternative_services_ = true`）。Server Push 仅适用于 HTTP/2；HTTP/3 的 Alt-Svc 发现从已验证源站的响应头获取。

示例：[http_client.cpp](../examples/web/http_client.cpp)、[http_client_advanced.cpp](../examples/web/http_client_advanced.cpp)。
