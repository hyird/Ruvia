# `ruvia-http` 协议层 API 手册

`ruvia-http`（CMake target `ruvia::http`）是 Ruvia 的纯 Sans-I/O HTTP 协议引擎：HTTP/1.0、HTTP/1.1、HTTP/2（HPACK）、HTTP/3（QPACK、QUIC）、内容与传输编码、Multipart、SSE、Range、Cookie、缓存与条件请求、CORS、WebSocket、RFC 9297 Capsule / HTTP Datagram、RFC 9298 `CONNECT-UDP`、PROXY protocol 与 TLS ClientHello 解析。它只在调用期间借用输入视图，产出纯值状态、事件与写出计划；不执行 socket I/O，不持有定时器或线程，也不依赖 `ruvia::core`、Asio、OpenSSL 或 mimalloc。异步驱动、TLS 与应用框架由 [`ruvia-web`](ruvia-web.md) 提供，运行时原语见 [`ruvia-core`](ruvia-core.md)，构建总入口见 [README](README.md)。

本手册以 `ruvia-http/CMakeLists.txt` 中 `ruvia_install_public_headers(http ...)` 安装的头文件为唯一公开范围：每个安装头在[第 4–13 章](#3-公开头索引)有一个带稳定锚点 `api-<文件名>-h` 的小节，列出其全部非 `detail` 公开声明与契约。`namespace detail` 符号、`RUVIA_DETAIL_*` 宏、`src/` 内部头、private/protected 成员与编译器隐式生成成员不属于公开契约。

## 目录

- [1. 定位与接入](#1-定位与接入)
  - [1.1 依赖与构建开关](#11-依赖与构建开关)
  - [1.2 最短接入](#12-最短接入)
- [2. 通用契约](#2-通用契约)
  - [2.1 借用与生命周期](#21-借用与生命周期)
  - [2.2 增量解析与消费字节](#22-增量解析与消费字节)
  - [2.3 错误模型](#23-错误模型)
  - [2.4 失败原子性](#24-失败原子性)
- [3. 公开头索引](#3-公开头索引)
- [4. 基础值与文本工具](#4-基础值与文本工具)
  - [4.1 `"borrowed_text.h"`](#api-borrowed-text-h)
  - [4.2 `"checked_integer.h"`](#api-checked-integer-h)
  - [4.3 `"decimal_text.h"`](#api-decimal-text-h)
  - [4.4 `"utf8.h"`](#api-utf8-h)
  - [4.5 `"binary_text_encoding.h"`](#api-binary-text-encoding-h)
  - [4.6 `"url_encoding.h"`](#api-url-encoding-h)
  - [4.7 `"ip_address.h"`](#api-ip-address-h)
  - [4.8 `"decode_step.h"`](#api-decode-step-h)
  - [4.9 `"http_io_buffer.h"`](#api-http-io-buffer-h)
  - [4.10 `"http_protocol.h"`](#api-http-protocol-h)
  - [4.11 `"http_status.h"`](#api-http-status-h)
  - [4.12 `"http_message.h"`](#api-http-message-h)
  - [4.13 `"http_content_length.h"`](#api-http-content-length-h)
  - [4.14 `"http_field_section_budget.h"`](#api-http-field-section-budget-h)
  - [4.15 `"http_date.h"`](#api-http-date-h)
- [5. 首部与请求模型](#5-首部与请求模型)
  - [5.1 `"http_header.h"`](#api-http-header-h)
  - [5.2 `"http_request_head_view.h"`](#api-http-request-head-view-h)
  - [5.3 `"http_request.h"`](#api-http-request-h)
  - [5.4 `"http_request_expectation.h"`](#api-http-request-expectation-h)
  - [5.5 `"http_request_content_plan.h"`](#api-http-request-content-plan-h)
  - [5.6 `"http_request_body_failure.h"`](#api-http-request-body-failure-h)
  - [5.7 `"http_client_response_head.h"`](#api-http-client-response-head-h)
- [6. 响应模型](#6-响应模型)
  - [6.1 `"http_response.h"`](#api-http-response-h)
  - [6.2 `"http_response_headers.h"`](#api-http-response-headers-h)
  - [6.3 `"http_response_body.h"`](#api-http-response-body-h)
  - [6.4 `"http_response_stream.h"`](#api-http-response-stream-h)
- [7. 内容编码、Multipart 与 SSE](#7-内容编码multipart-与-sse)
  - [7.1 `"http_content_coding.h"`](#api-http-content-coding-h)
  - [7.2 `"http_content_codec.h"`](#api-http-content-codec-h)
  - [7.3 `"http_transfer_coding.h"`](#api-http-transfer-coding-h)
  - [7.4 `"http_multipart.h"`](#api-http-multipart-h)
  - [7.5 `"http_server_sent_events.h"`](#api-http-server-sent-events-h)
  - [7.6 `"http_codec_memory.h"`](#api-http-codec-memory-h)
- [8. 目标、源站、转发与连接元数据](#8-目标源站转发与连接元数据)
  - [8.1 `"http_authority.h"`](#api-http-authority-h)
  - [8.2 `"http_target_origin.h"`](#api-http-target-origin-h)
  - [8.3 `"http_proxy_forwarding.h"`](#api-http-proxy-forwarding-h)
  - [8.4 `"http_client_ip.h"`](#api-http-client-ip-h)
  - [8.5 `"proxy_protocol.h"`](#api-proxy-protocol-h)
  - [8.6 `"tls_client_hello.h"`](#api-tls-client-hello-h)
  - [8.7 `"http_client_redirect.h"`](#api-http-client-redirect-h)
  - [8.8 `"http_connection_advertisement.h"`](#api-http-connection-advertisement-h)
  - [8.9 `"http_priority.h"`](#api-http-priority-h)
  - [8.10 `"http_uri.h"`](#api-http-uri-h)
- [9. 缓存、条件请求、范围、Cookie、CORS 与内容协商](#9-缓存条件请求范围cookiecors-与内容协商)
  - [9.1 `"http_cache.h"`](#api-http-cache-h)
  - [9.2 `"http_representation.h"`](#api-http-representation-h)
  - [9.3 `"http_range.h"`](#api-http-range-h)
  - [9.4 `"http_cookie.h"`](#api-http-cookie-h)
  - [9.5 `"http_cookie_jar.h"`](#api-http-cookie-jar-h)
  - [9.6 `"http_cors.h"`](#api-http-cors-h)
  - [9.7 `"http_content_negotiation.h"`](#api-http-content-negotiation-h)
  - [9.8 `"http_structured_field.h"`](#api-http-structured-field-h)
- [10. HTTP/1.x](#10-http1x)
  - [10.1 `"http1_connection_plan.h"`](#api-http1-connection-plan-h)
  - [10.2 `"http1_server_request.h"`](#api-http1-server-request-h)
  - [10.3 `"http1_message_body.h"`](#api-http1-message-body-h)
  - [10.4 `"http1_chunked_body.h"`](#api-http1-chunked-body-h)
  - [10.5 `"http1_server_response.h"`](#api-http1-server-response-h)
  - [10.6 `"http1_client_request.h"`](#api-http1-client-request-h)
  - [10.7 `"http1_client_response.h"`](#api-http1-client-response-h)
- [11. HTTP/2 与多路复用共享词汇](#11-http2-与多路复用共享词汇)
  - [11.1 `"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)
  - [11.2 `"http2_frame.h"`](#api-http2-frame-h)
  - [11.3 `"http2_push.h"`](#api-http2-push-h)
  - [11.4 `"http2_connection.h"`](#api-http2-connection-h)
  - [11.5 `"http2_hpack.h"`](#api-http2-hpack-h)
- [12. HTTP/3 与 QUIC](#12-http3-与-quic)
  - [12.1 `"http3_connection.h"`](#api-http3-connection-h)
  - [12.2 `"http3_request_head.h"`](#api-http3-request-head-h)
  - [12.3 `"http3_quic_connection.h"`](#api-http3-quic-connection-h)
- [13. WebSocket、Capsule 与 CONNECT-UDP](#13-websocketcapsule-与-connect-udp)
  - [13.1 `"websocket_handshake.h"`](#api-websocket-handshake-h)
  - [13.2 `"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)
  - [13.3 `"websocket_connection.h"`](#api-websocket-connection-h)
  - [13.4 `"http_capsule.h"`](#api-http-capsule-h)
  - [13.5 `"http_connect_udp.h"`](#api-http-connect-udp-h)

## 1. 定位与接入

### 1.1 依赖与构建开关

| 项 | 契约 |
| --- | --- |
| CMake target | `ruvia::http`（静态库 `ruvia-http`，导出名 `http`，`find_package(ruvia REQUIRED COMPONENTS http)`），公开要求 `cxx_std_20` |
| 构建开关 | `RUVIA_BUILD_HTTP`（默认 `ON`）。`RUVIA_BUILD_WEB` 要求它开启 |
| 私有依赖 | zlib（gzip / deflate）、Brotli 编码与解码库、Zstandard；启用 HTTP/2 时另有 nghttp2，启用 HTTP/3 时另有 nghttp3 1.17+ 与 ngtcp2 core。私有依赖不进入公开头（`http3_quic_connection.h` 除外，见下） |
| `RUVIA_ENABLE_HTTP2`（默认 `ON`） | 编译 nghttp2 会话实现，安装 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http2_hpack.h"`](#api-http2-hpack-h)，并给 `ruvia::http` 加 PUBLIC 编译定义 `RUVIA_ENABLE_HTTP2` |
| `RUVIA_ENABLE_HTTP3`（默认 `ON`） | 编译 nghttp3 / ngtcp2 实现，安装 [`"http3_connection.h"`](#api-http3-connection-h)、[`"http3_request_head.h"`](#api-http3-request-head-h)、[`"http3_quic_connection.h"`](#api-http3-quic-connection-h)，加 PUBLIC 编译定义 `RUVIA_ENABLE_HTTP3`，并 PUBLIC 链接 `ngtcp2::core`（`http3_quic_connection.h` 直接公开 ngtcp2 类型）。开启 HTTP/3 时根 CMake 强制开启 `RUVIA_ENABLE_HTTP2`；仅当 HTTP/3 关闭时 HTTP/2 才可关闭 |
| 无条件安装 | 其余公开头，包括名称带 `http2` 但与连接实现无关的 [`"http2_frame.h"`](#api-http2-frame-h)、[`"http2_push.h"`](#api-http2-push-h)，以及 HTTP/2 与 HTTP/3 共用的 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h) |

关闭某个协议开关时，其源码不参与编译、其连接头不安装、对应第三方库不进入依赖闭包。下游代码可用 `#if defined(RUVIA_ENABLE_HTTP2)` / `#if defined(RUVIA_ENABLE_HTTP3)` 判断安装内容；`ruviaConfig.cmake` 同时导出同名 CMake 变量。完整安装与 vcpkg 特性选择见 [README](README.md)。

### 1.2 最短接入

```cmake
find_package(ruvia REQUIRED COMPONENTS http)
target_link_libraries(my_proxy PRIVATE ruvia::http)
```

公开头平铺在安装前缀的 `include/` 下，按头文件名直接包含（如 `#include "http1_server_request.h"`）；所有公开符号位于 `namespace ruvia`，HTTP 状态码常量位于 `namespace ruvia::http_status`，WebSocket 关闭码常量位于 `namespace ruvia::websocket_close_status`。

下列最小 HTTP/1.1 服务端循环展示 Sans-I/O 驱动形状：驱动自行读写 socket，协议对象只消费字节并返回计划（完整契约见 [10.2](#api-http1-server-request-h)、[10.3](#api-http1-message-body-h)、[10.5](#api-http1-server-response-h)）。

```cpp
#include "http1_server_request.h"
#include "http_io_buffer.h"

ruvia::http_io_buffer input;
ruvia::http1_server_request_parser parser;

// 驱动把 socket 读到的字节写入 input.prepare(n) 并 commit(n) 后：
auto step = parser.parse(input.view());  // 借用 input 当前可读字节
if (step.holds<ruvia::decode_need_input>()) {
    // 继续读；parser 记住已扫描偏移，下次以同一前缀加新字节调用
} else if (const auto* failure = step.get_if<ruvia::http1_server_request_parse_failure>()) {
    auto error = ruvia::make_http_protocol_error(failure->error());  // 映射为状态码与稳定机器码
} else {
    const ruvia::http_request& request = parser.request();          // 借用 input 中的字节
    // 依据 parser.body_plan() 构造正文解码器；头部字节为 step.consumed_bytes()，处理完成后 input.consume(...) 并 parser.reset()
}
```

## 2. 通用契约

本章只总述跨头共享的规则；每个 API 的具体前置条件、借用区间与错误值写在其定义头的小节中。

### 2.1 借用与生命周期

- **调用期借用**：协议对象在调用期间读取 `std::string_view`、`std::span`、[`http_io_buffer`](#api-http-io-buffer-h) 等输入；结果中的视图借用的对象（输入、调用方提供的 scratch / `storage`、解析器内部存储或事件自身存储）由各 API 明确说明，视图在该对象被修改、移动、重置或销毁后失效。显式构造 `std::string_view` / `std::span` 不会延长 owner 寿命。
- **编译期借用防线**：跨表达式留存视图的入口使用 [`borrowed_text`](#api-borrowed-text-h) 或 `borrowed_span<T>`，以 `= delete` 重载拒绝绑定右值临时 `std::string`、临时拥有型连续容器与 `std::initializer_list`；返回借用的访问器标注 [`RUVIA_LIFETIMEBOUND`](#api-borrowed-text-h)。借用宿主内部存储的访问器及 HTTP/1 正文解码入口限定为左值（`&` / `const&`），对应右值重载为 `= delete`；拥有型内容只经右值 `take_*() &&` 提取，提取后源对象只可 `reset()` 或析构。
- **借用范围不拥有存储**：`std::span`、`borrowed_span<T>` 与 `http_header_range_view` 满足 `std::ranges::borrowed_range`，其迭代器不依赖视图对象本身存活，但底层字段、文本与容器仍须存活且不发生使视图失效的修改。返回借用的首部查找接受拥有型容器 / 消息左值、临时借用范围，以及 `headers()` 返回借用范围的临时消息；拒绝临时拥有型容器 / 消息。同步只读参数 `const http_header_range_view&` 可以在一次调用内读取临时 owner，不代表从该调用返回的视图可继续借用它。
- **线性凭证**：流控信用（[`http_received_data_credit`](#api-http-multiplexed-stream-h)）与请求租约只可移动；析构或显式交还时把精确字节数或存储归还签发连接。
- **回调重入**：观察器与回调只在对应的 `feed()` / `read_stream()` / `next_event()` 等调用内同步执行，记录仅在回调期间有效；回调返回前不得修改、重入、移动或销毁宿主对象。

### 2.2 增量解析与消费字节

使用单步解析接口的解析器与解码器返回 [`decode_step<...>`](#api-decode-step-h)：`consumed_bytes()` 是本次已消费的输入前缀，调用方丢弃该前缀、保留其余字节用于下一次调用；结果恰为本族备选项之一（共享的 `decode_need_input` / `decode_output` / `decode_complete` / `decode_failure<E>`，加本族负载），类别在构造时固定且不可赋值。有状态处理器暴露 `decoder_state state()`（`active` / `finished` / `failed`）与显式 `reset()`：终态结果供提取的解析器再次调用抛 `std::logic_error`，流式解码器重放终态结果且消费 0 字节；两类成员清单见 [4.8](#api-decode-step-h)。处于 `need_input` 的解析器保存扫描偏移，避免对同一前缀重复扫描。

连接驱动输入统一按下表解释。状态和信用的具体定义见对应协议章节。

| 输入入口 | 交付与消费 | 何时可重试 | 输入寿命 |
| --- | --- | --- | --- |
| HTTP/2 `feed()` | `events_pending` 消费零字节，其余状态消费整个输入 | 取尽待处理事件后重试未消费的输入 | 保持至该次的全部事件取出且派生视图失效 |
| HTTP/3 `read_stream()` | `events_pending` 不消费，`credit_bytes_` 只报告可立即归还的流控信用 | 取尽上次的事件再重试 | 借用模式下保持至该次事件取尽 |
| WebSocket `feed()` | 打开时接纳并复制输入。等待对端 EOF 时接纳并丢弃。背压时不追加 | 解析已缓冲输入再重试 | 输入可在调用返回后释放，事件视图另借用连接缓冲区 |

### 2.3 错误模型

| 结果类别 | 形状 | 典型 API |
| --- | --- | --- |
| 正常协议推进（需要输入、已排队、背压、完成） | 状态枚举或进度结果 | `decode_need_input`、`http_data_submit_result::status_ == backpressured`、`websocket_feed_status` |
| 不可信输入的数据、格式或配额错误 | 返回值：`std::optional<T>`、状态枚举、`decode_failure<E>` 或 `std::variant<值, 错误>`（值在前） | `parse_http_date()`、`http_parse_error`、`multipart_parse_error`、`proxy_protocol_parse_error` |
| 本地参数、配置或调用顺序错误 | 异常：`std::invalid_argument`（含其派生类 `http1_response_head_error`）、`std::logic_error` | `checked_integer` 构造、`protocol_byte_limit::limited()`、对终态解析器再次 `parse()` |
| 编解码库自身无法运行 | `http_content_codec_error` | 见 [7.2](#api-http-content-codec-h) |
| 分配失败 | `std::bad_alloc` 原样传出 | 全部拥有存储的 API |

- **受检整数双入口**：信任边界上的整数强类型（`port_number`、`http_status_code`、`http_content_length`、`hpack_table_size`、`http_priority_urgency`、`websocket_close_code` 等）均基于 [`checked_integer`](#api-checked-integer-h)：构造函数在原始整数域校验、失败抛 `std::invalid_argument`；`noexcept` 的 `try_from_value()` 返回 `std::optional`。不接受浮点与枚举隐式转换，不提供隐式转回整数，调用前不要预先窄化。
- **协议错误映射**：解析与解码错误枚举经 [`make_http_protocol_error()`](#api-http-protocol-h) 重载集映射为携带状态码与静态稳定机器码的 `http_protocol_error`；映射本身不抛异常，是否抛出由调用方（如 `ruvia-web`）决定。

### 2.4 失败原子性

- **两阶段提交**：多步变更先在候选副本或预留容量上完成校验与分配，全部成功后再以不抛异常的步骤提交；失败（含 `std::bad_alloc`）保留原值，恢复资源后可用同一输入重试。各头的“失败保留原内容 / 原窗口”说明即此契约的具体范围。
- **不可回滚的例外**：基于 nghttp2 / nghttp3 / HPACK 动态表的状态无法事务回滚；分配失败或回调异常使对应连接或解码器进入致命状态，具体后果在 [11.4](#api-http2-connection-h)、[11.5](#api-http2-hpack-h)、[12.1](#api-http3-connection-h) 写明。
- **退役顺序**：拥有资源的对象在赋值、清空或重分配时先提交新状态，再释放旧存储，释放过程（含运行时提供的 `release` 回调）观察到的总是已提交的新状态；适用的类型在其头小节注明。

## 3. 公开头索引

共 70 个安装头：无条件 65 个，`RUVIA_ENABLE_HTTP2` 2 个，`RUVIA_ENABLE_HTTP3` 3 个。

| 章节 | 头文件 | 安装条件 | 领域 |
| --- | --- | --- | --- |
| [4.1](#api-borrowed-text-h) | `borrowed_text.h` | 无条件 | 防悬垂借用文本 / 连续序列视图、`RUVIA_LIFETIMEBOUND` |
| [4.2](#api-checked-integer-h) | `checked_integer.h` | 无条件 | 受检整数强类型机制 |
| [4.3](#api-decimal-text-h) | `decimal_text.h` | 无条件 | 十进制格式化、严格解析与饱和算术 |
| [4.4](#api-utf8-h) | `utf8.h` | 无条件 | UTF-8 序列扫描与校验 |
| [4.5](#api-binary-text-encoding-h) | `binary_text_encoding.h` | 无条件 | 十六进制与 Base64 编解码 |
| [4.6](#api-url-encoding-h) | `url_encoding.h` | 无条件 | 百分号 / 表单编解码与 URI 路径比较 |
| [4.7](#api-ip-address-h) | `ip_address.h` | 无条件 | IP 地址、端点、CIDR 与 `port_number` |
| [4.8](#api-decode-step-h) | `decode_step.h` | 无条件 | 增量解析单步结果与处理器三态 |
| [4.9](#api-http-io-buffer-h) | `http_io_buffer.h` | 无条件 | 流式读写缓冲 |
| [4.10](#api-http-protocol-h) | `http_protocol.h` | 无条件 | 字节预算、协议错误与解析错误枚举 |
| [4.11](#api-http-status-h) | `http_status.h` | 无条件 | 状态码强类型、标准状态码常量与原因短语 |
| [4.12](#api-http-message-h) | `http_message.h` | 无条件 | 协议版本、scheme 与方法分类 |
| [4.13](#api-http-content-length-h) | `http_content_length.h` | 无条件 | 出站长度受检类型 |
| [4.14](#api-http-field-section-budget-h) | `http_field_section_budget.h` | 无条件 | 字段区段字节与行数预算 |
| [4.15](#api-http-date-h) | `http_date.h` | 无条件 | IMF-fixdate 格式化与解析 |
| [5.1](#api-http-header-h) | `http_header.h` | 无条件 | 首部值类型、视图、查找与字段校验 |
| [5.2](#api-http-request-head-view-h) | `http_request_head_view.h` | 无条件 | 跨协议出站请求头借用视图 |
| [5.3](#api-http-request-h) | `http_request.h` | 无条件 | 服务端请求描述符与请求 Trailer |
| [5.4](#api-http-request-expectation-h) | `http_request_expectation.h` | 无条件 | `Expect` / `100-continue` 状态 |
| [5.5](#api-http-request-content-plan-h) | `http_request_content_plan.h` | 无条件 | 客户端请求内容计划 |
| [5.6](#api-http-request-body-failure-h) | `http_request_body_failure.h` | 无条件 | 请求体失败分类与错误映射 |
| [5.7](#api-http-client-response-head-h) | `http_client_response_head.h` | 无条件 | 客户端收到的响应头 |
| [6.1](#api-http-response-h) | `http_response.h` | 无条件 | 最终响应模型 |
| [6.2](#api-http-response-headers-h) | `http_response_headers.h` | 无条件 | 响应首部集合与临时响应头 |
| [6.3](#api-http-response-body-h) | `http_response_body.h` | 无条件 | 响应正文表示与写出计划 |
| [6.4](#api-http-response-stream-h) | `http_response_stream.h` | 无条件 | 流式响应提交计划 |
| [7.1](#api-http-content-coding-h) | `http_content_coding.h` | 无条件 | coding 模型、编码栈与可压缩判定 |
| [7.2](#api-http-content-codec-h) | `http_content_codec.h` | 无条件 | 内容编码器、解码与编码器租约 |
| [7.3](#api-http-transfer-coding-h) | `http_transfer_coding.h` | 无条件 | 传输编码栈增量解码 |
| [7.4](#api-http-multipart-h) | `http_multipart.h` | 无条件 | `multipart/form-data` 解析 |
| [7.5](#api-http-server-sent-events-h) | `http_server_sent_events.h` | 无条件 | SSE 格式化与增量解析 |
| [7.6](#api-http-codec-memory-h) | `http_codec_memory.h` | 无条件 | 原生 codec 内存预算与分配回调 |
| [8.1](#api-http-authority-h) | `http_authority.h` | 无条件 | authority 与源站视图 |
| [8.2](#api-http-target-origin-h) | `http_target_origin.h` | 无条件 | RFC 9110 §7.4 目标源判定 |
| [8.3](#api-http-proxy-forwarding-h) | `http_proxy_forwarding.h` | 无条件 | 代理目标规范化与转发字段 |
| [8.4](#api-http-client-ip-h) | `http_client_ip.h` | 无条件 | 可信代理下的客户端 IP 提取 |
| [8.5](#api-proxy-protocol-h) | `proxy_protocol.h` | 无条件 | HAProxy PROXY protocol v1 / v2 |
| [8.6](#api-tls-client-hello-h) | `tls_client_hello.h` | 无条件 | TLS ClientHello 解析与指纹 |
| [8.7](#api-http-client-redirect-h) | `http_client_redirect.h` | 无条件 | 客户端重定向规划 |
| [8.8](#api-http-connection-advertisement-h) | `http_connection_advertisement.h` | 无条件 | `ORIGIN` / `ALTSVC` 通告值 |
| [8.9](#api-http-priority-h) | `http_priority.h` | 无条件 | RFC 9218 优先级 |
| [8.10](#api-http-uri-h) | `http_uri.h` | 无条件 | DNS 原语与零拷贝 HTTP(S)/WS(S) URL 拆解 |
| [9.1](#api-http-cache-h) | `http_cache.h` | 无条件 | Cache-Control、Vary 与 304 首部 |
| [9.2](#api-http-representation-h) | `http_representation.h` | 无条件 | 条件请求、ETag 与表示响应计划 |
| [9.3](#api-http-range-h) | `http_range.h` | 无条件 | Range / Content-Range 与字节范围切片 |
| [9.4](#api-http-cookie-h) | `http_cookie.h` | 无条件 | Cookie 属性、`Set-Cookie` 解析与写出 |
| [9.5](#api-http-cookie-jar-h) | `http_cookie_jar.h` | 无条件 | 客户端 Cookie 容器 |
| [9.6](#api-http-cors-h) | `http_cors.h` | 无条件 | CORS 策略原语 |
| [9.7](#api-http-content-negotiation-h) | `http_content_negotiation.h` | 无条件 | Accept 匹配与响应编码协商 |
| [9.8](#api-http-structured-field-h) | `http_structured_field.h` | 无条件 | 结构化字段扫描 |
| [10.1](#api-http1-connection-plan-h) | `http1_connection_plan.h` | 无条件 | HTTP/1 分帧值与连接复用计划 |
| [10.2](#api-http1-server-request-h) | `http1_server_request.h` | 无条件 | 服务端请求头解析 |
| [10.3](#api-http1-message-body-h) | `http1_message_body.h` | 无条件 | HTTP/1 消息正文解码 |
| [10.4](#api-http1-chunked-body-h) | `http1_chunked_body.h` | 无条件 | chunked 分帧解码 |
| [10.5](#api-http1-server-response-h) | `http1_server_response.h` | 无条件 | 服务端响应头计划与写出 |
| [10.6](#api-http1-client-request-h) | `http1_client_request.h` | 无条件 | 客户端请求写出 |
| [10.7](#api-http1-client-response-h) | `http1_client_response.h` | 无条件 | 客户端响应头解析 |
| [11.1](#api-http-multiplexed-stream-h) | `http_multiplexed_stream.h` | 无条件 | HTTP/2 与 HTTP/3 共享事件、状态与流控凭证 |
| [11.2](#api-http2-frame-h) | `http2_frame.h` | 无条件 | HTTP/2 帧与字段观察记录 |
| [11.3](#api-http2-push-h) | `http2_push.h` | 无条件 | Server Push 承诺请求与事件 |
| [11.4](#api-http2-connection-h) | `http2_connection.h` | `RUVIA_ENABLE_HTTP2` | HTTP/2 连接状态机 |
| [11.5](#api-http2-hpack-h) | `http2_hpack.h` | `RUVIA_ENABLE_HTTP2` | 独立 HPACK 编解码 |
| [12.1](#api-http3-connection-h) | `http3_connection.h` | `RUVIA_ENABLE_HTTP3` | HTTP/3 与 QPACK 状态机 |
| [12.2](#api-http3-request-head-h) | `http3_request_head.h` | `RUVIA_ENABLE_HTTP3` | HTTP/3 服务端请求投影 |
| [12.3](#api-http3-quic-connection-h) | `http3_quic_connection.h` | `RUVIA_ENABLE_HTTP3` | 基于 ngtcp2 的 QUIC 连接包装 |
| [13.1](#api-websocket-handshake-h) | `websocket_handshake.h` | 无条件 | WebSocket 服务端握手 |
| [13.2](#api-websocket-client-handshake-h) | `websocket_client_handshake.h` | 无条件 | WebSocket 客户端握手 |
| [13.3](#api-websocket-connection-h) | `websocket_connection.h` | 无条件 | WebSocket 帧状态机与关闭码 |
| [13.4](#api-http-capsule-h) | `http_capsule.h` | 无条件 | Capsule 编解码与 HTTP Datagram 会话 |
| [13.5](#api-http-connect-udp-h) | `http_connect_udp.h` | 无条件 | `CONNECT-UDP` 路径与请求校验 |

## 4. 基础值与文本工具

本章各头不执行 I/O，均可在任何线程独立使用。除非另有说明，同一对象的并发修改需要调用方同步；`const` 成员可并发调用。借用、`decode_step` 三态与异常/值错误的跨头总述见[通用契约](#2-通用契约)。

### 4.1 `"borrowed_text.h"`
<a id="api-borrowed-text-h"></a>

零拷贝借用视图的公共基础：定义 `RUVIA_LIFETIMEBOUND` 宏、`borrowed_text`、`borrowed_span<T>`、`argument_list<T>` 与 `static_text`。保存调用方文本或元素序列的 API 使用这些类型，在编译期拒绝借用即将销毁的临时拥有型容器。源码：[`ruvia-http/include/borrowed_text.h`](../ruvia-http/include/borrowed_text.h)

#### 4.1.1 宏 `RUVIA_LIFETIMEBOUND`

```cpp
#ifndef RUVIA_LIFETIMEBOUND
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(gnu::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[gnu::lifetimebound]]
#else
#define RUVIA_LIFETIMEBOUND
#endif
#else
#define RUVIA_LIFETIMEBOUND
#endif
#endif
```

- 用途：标注在返回借用（指针、视图、span、引用）的成员函数上，表示返回值的生命周期受 `*this`（或被标注参数）约束；支持该属性的编译器可对“从临时对象取得借用后继续使用”发出诊断。它只是诊断辅助，不改变语义，也不延长任何对象寿命。
- 定义分支：若用户已预先定义 `RUVIA_LIFETIMEBOUND`，保持用户定义；否则按 `clang::lifetimebound` → `msvc::lifetimebound` → `gnu::lifetimebound` 顺序选择第一个可用属性；都不可用或编译器不支持 `__has_cpp_attribute` 时展开为空。
- 本库在借用型访问器上通常同时提供左值限定版本（带 `RUVIA_LIFETIMEBOUND`）与 `= delete` 的右值限定版本（如 [`ip_address::bytes() const&&`](#api-ip-address-h)、[`decode_step::get_if() const&&`](#api-decode-step-h)、[`http_io_buffer::view() const&&`](#api-http-io-buffer-h)），后者在所有编译器上都强制拒绝从右值宿主取借用。

#### 4.1.2 `class borrowed_text`

```cpp
namespace ruvia {
class borrowed_text final {
public:
    constexpr borrowed_text() noexcept = default;
    constexpr borrowed_text(std::string_view value) noexcept;
    constexpr borrowed_text(const char* value) noexcept;
    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text(const std::basic_string<char, traits_type, allocator_type>& value) noexcept;
    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ string>
    borrowed_text(string&&) = delete;

    constexpr borrowed_text& operator=(std::string_view value) noexcept;
    constexpr borrowed_text& operator=(const char* value) noexcept;
    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text& operator=(const std::basic_string<char, traits_type, allocator_type>& value) noexcept;
    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ string>
    borrowed_text& operator=(string&&) = delete;

    [[nodiscard]] constexpr std::string_view view() const noexcept;
    [[nodiscard]] constexpr operator std::string_view() const noexcept;
    [[nodiscard]] constexpr const char* data() const noexcept;
    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr bool empty() const noexcept;

    friend constexpr bool operator==(borrowed_text left, borrowed_text right) noexcept;
    friend constexpr bool operator==(borrowed_text left, std::string_view right) noexcept;
    friend constexpr bool operator==(borrowed_text left, const char* right) noexcept;
    template <typename traits_type, typename allocator_type>
    friend constexpr bool operator==(borrowed_text left, const std::basic_string<char, traits_type, allocator_type>& right) noexcept;
};
static_assert(sizeof(borrowed_text) == sizeof(std::string_view));
}
```

`borrowed_text` 是一个拒绝从拥有型临时字符串构造的 `std::string_view`，大小与 `std::string_view` 相同，可平凡按值传递。它只借用，不拥有、不复制、不延长 owner 寿命：调用方必须保持源存储有效，直到消费该值的入口完成读取或把文本复制为自有存储。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 空视图（`data()` 为空指针、`size() == 0`）。 |
| `borrowed_text(std::string_view)` / `operator=(std::string_view)` | 原样借用该视图。显式构造的 `string_view` 不会延长其 owner 寿命，调用方自行负责。 |
| `borrowed_text(const char*)` / `operator=(const char*)` | 借用以 NUL 结尾的 C 字符串（通常是字符串字面量）；`nullptr` 规范化为与默认构造相同的空视图，而不是未定义行为。空文本是否合法由消费入口决定。 |
| `borrowed_text(const std::basic_string<char, ...>&)` / 对应 `operator=` | 借用命名的（左值）拥有型字符串；任意 traits/allocator 均可。该字符串被修改或销毁后视图失效。 |
| `borrowed_text(string&&) = delete` / `operator=(string&&) = delete` | 拒绝任何 `std::basic_string<char, ...>` 右值（含 `const` 右值，例如函数返回的 `std::string` 或 `std::move(s)`）。这样的绑定会让被保存的视图在使用点悬垂，因此在编译期报错。需要传入生成的字符串时先保存为命名 owner。 |
| `view()` / `operator std::string_view()` | 返回被借用的视图，借用区间同源存储。 |
| `data()` / `size()` / `empty()` | 同 `std::string_view` 对应成员。 |
| `operator==`（4 个 friend 重载） | 按字节比较文本内容，可与 `borrowed_text`、`std::string_view`、`const char*`（`nullptr` 视为空）及任意 `std::basic_string<char, ...>` 比较。 |

#### 4.1.3 `template <typename element_type> class borrowed_span`

```cpp
namespace ruvia {
template <typename element_type>
class borrowed_span final {
public:
    constexpr borrowed_span() noexcept = default;
    constexpr borrowed_span(std::span<const element_type> values) noexcept;
    constexpr borrowed_span(std::span<element_type> values) noexcept;
    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) && std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span(range_type&& values) noexcept;
    template </* 元素类型为 element_type 的临时、非 borrowed_range 连续容器 */ range_type>
    borrowed_span(range_type&&) = delete;
    constexpr borrowed_span(std::initializer_list<element_type>) = delete;

    constexpr borrowed_span& operator=(std::span<const element_type> values) noexcept;
    constexpr borrowed_span& operator=(std::span<element_type> values) noexcept;
    template <typename range_type>
        requires /* 与上述左值范围构造函数相同的约束 */
    constexpr borrowed_span& operator=(range_type&& values) noexcept;
    template </* 同上 */ range_type>
    borrowed_span& operator=(range_type&&) = delete;
    constexpr borrowed_span& operator=(std::initializer_list<element_type>) = delete;

    [[nodiscard]] constexpr std::span<const element_type> view() const noexcept;
    [[nodiscard]] constexpr operator std::span<const element_type>() const noexcept;
    [[nodiscard]] constexpr const element_type* data() const noexcept;
    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr bool empty() const noexcept;
    [[nodiscard]] constexpr auto begin() const noexcept;
    [[nodiscard]] constexpr auto end() const noexcept;
    [[nodiscard]] constexpr const element_type& front() const noexcept;
    [[nodiscard]] constexpr const element_type& back() const noexcept;
    [[nodiscard]] constexpr const element_type& operator[](std::size_t index) const noexcept;
};
}

namespace std::ranges {
template <typename element_type>
inline constexpr bool enable_borrowed_range<ruvia::borrowed_span<element_type>> = true;
}
```

保存在选项结构或消息视图结构中的只读连续元素借用。规则：

| 输入 | 结果 |
| --- | --- |
| `std::span<const T>` / `std::span<T>`（含右值 span） | 接受；span 本身是借用范围。 |
| 元素类型为 `T` 的连续容器左值（命名的 `std::vector<T>`、`std::array<T, N>`、C 数组、`std::string`（`T` 为 `char` 时）等） | 接受，借用其 `data()`/`size()`。 |
| 临时拥有型连续容器（`std::vector<T>{...}`、函数返回的 `std::array`、`std::move(v)` 等，即非 `borrowed_range` 的右值） | `= delete`，编译期拒绝：其存储在完整表达式结束时释放，而结构体通常在之后才被消费。 |
| `std::initializer_list<T>`（花括号列表） | `= delete`，编译期拒绝：其底层数组同样只活到完整表达式结束。 |

访问器均为 `const`、`noexcept`，语义同 `std::span<const T>`；`front()`、`back()`、`operator[]` 的前置条件与 `std::span` 相同（非空 / 下标在界内），越界为未定义行为。`borrowed_span<T>` 满足 `std::ranges::borrowed_range`，迭代器和元素视图不借用 span 包装对象本身，因而可使用临时 `borrowed_span`；这不会延长源存储的寿命，源容器被重新分配、移动或销毁后借用可能失效。

示例（数据库选项使用 `borrowed_span` 时的典型写法）：

```cpp
static constexpr std::array<std::string_view, 2> columns{"id", "name"};
options.columns_ = columns;                    // 具名 owner：接受
// options.columns_ = {"id", "name"};           // initializer_list：编译期拒绝
// options.columns_ = std::vector<...>{...};    // 临时容器：编译期拒绝
```

#### 4.1.4 `template <typename element_type> class argument_list`

```cpp
namespace ruvia {
template <typename element_type>
class argument_list final {
public:
    constexpr argument_list() noexcept = default;
    constexpr argument_list(std::span<const element_type> values) noexcept;
    constexpr argument_list(std::initializer_list<element_type> values) noexcept;
    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, argument_list>) && std::ranges::contiguous_range<range_type> && std::ranges::sized_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr argument_list(range_type&& values) noexcept;
    argument_list(const argument_list&) = delete;
    argument_list& operator=(const argument_list&) = delete;

    [[nodiscard]] constexpr std::span<const element_type> span() const noexcept;
};
}
```

按值接收、并在函数返回前同步消费完毕的只读参数列表。与 `borrowed_span` 不同，它接受花括号列表和临时容器：这些存储至少存活到整个调用表达式结束，足够同步消费者使用。为防止越出该区间，`argument_list` 既不可复制也不可移动（复制构造/复制赋值 `= delete`，因此也没有隐式移动），不能被返回、捕获或保存为成员。`span()` 返回元素视图，只在当前调用期间有效。

#### 4.1.5 `class static_text`

`static_text` 在编译期检查文本是否引用静态存储。默认构造为空。显式 `consteval` 构造接受静态存储的字符数组或 `std::string_view`，拒绝帧局部或运行期存储。数组构造仅移除末尾 NUL，保留内嵌 NUL。`view()` 返回对应的 `std::string_view`。

---

### 4.2 `"checked_integer.h"`
<a id="api-checked-integer-h"></a>

本库所有强类型受检整数的唯一机制：在原始整数域内先校验再窄化，因此负数或更宽的输入无法回绕成被接受的值，且不提供回到整数的隐式转换。源码：[`ruvia-http/include/checked_integer.h`](../ruvia-http/include/checked_integer.h)

#### 4.2.1 `template <...> class checked_integer`

```cpp
namespace ruvia {
template <typename derived_type, std::unsigned_integral representation_type, typename policy_type>
class checked_integer {
public:
    constexpr checked_integer() noexcept
        requires requires { policy_type::default_value; };

    template <std::integral integer_type>
    explicit(!policy_type::implicit_conversion) constexpr checked_integer(integer_type value);

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept;

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<derived_type> try_from_value(integer_type value) noexcept;

    [[nodiscard]] constexpr representation_type value() const noexcept;

    [[nodiscard]] friend constexpr bool operator==(const derived_type& left, const derived_type& right) noexcept;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const derived_type& left, const derived_type& right) noexcept;
};
}
```

策略类型 `policy_type` 的要求（以静态成员给出）：

| 成员 | 要求与作用 |
| --- | --- |
| `template <std::integral integer_type> static constexpr bool is_valid_value(integer_type value) noexcept` | 在原始整数类型上判断值是否合法；必须在窄化之前就能判定负数与超宽值。 |
| `static constexpr const char* invalid_value_message` | 构造函数拒绝值时抛出的 `std::invalid_argument` 消息。 |
| `static constexpr bool implicit_conversion` | 为 `true` 时整数到强类型的构造是隐式的（便于数值配置），否则为 `explicit`。 |
| `static constexpr representation_type default_value`（可选） | 存在时启用默认构造并以此为初值；缺失时该类型不可默认构造。 |

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 仅当策略提供 `default_value` 时存在，值为 `default_value`。 |
| `checked_integer(integer_type)` | 本地严格入口：值不合法时抛 `std::invalid_argument(policy_type::invalid_value_message)`；在常量求值中不合法的值导致编译错误。只接受 `std::integral`：浮点、枚举及其他依赖隐式转换的类型不参与，调用方须先自行转换并校验，且不要预先窄化（否则丢失原始边界信息）。 |
| `is_valid_value(integer_type)` | 不抛异常的合法性谓词，转发给策略。 |
| `try_from_value(integer_type)` | 线上容错入口：合法时返回值，不合法时返回 `std::nullopt`，不抛异常。 |
| `value()` | 解包为 `representation_type`；只在原生 API 或格式化边界使用。 |
| `operator==` / `operator<=>` | 同一派生类型之间按数值比较，`std::strong_ordering`。不同派生类型之间互不兼容，不能比较或赋值。 |

派生方式：强类型公开继承自身实例并继承构造函数（`using checked_integer::checked_integer;`），各自以未公开的策略类型声明值域。本 target 中的全部派生类型如下，各自的值域、隐式性与默认值在其头小节完整说明：

| 类型 | 表示类型 | 所在头 |
| --- | --- | --- |
| `port_number` | `std::uint16_t` | [`"ip_address.h"`](#api-ip-address-h) |
| `http_status_code` | `std::uint16_t` | [`"http_status.h"`](#api-http-status-h) |
| `http_content_length` | `std::uint64_t` | [`"http_content_length.h"`](#api-http-content-length-h) |
| `http_priority_urgency` | `std::uint8_t` | [`"http_priority.h"`](#api-http-priority-h) |
| `hpack_table_size` | `std::uint32_t` | [`"http2_hpack.h"`](#api-http2-hpack-h) |
| `websocket_close_code` | `std::uint16_t` | [`"websocket_connection.h"`](#api-websocket-connection-h) |

ruvia-web 另有基于同一机制的强类型，见 [ruvia-web](ruvia-web.md)。

---

### 4.3 `"decimal_text.h"`
<a id="api-decimal-text-h"></a>

通用十进制文本原语（不限于 HTTP）：严格整数解析、饱和解析与加法、整数与浮点格式化。全部为头内模板或 `constexpr`/`inline` 函数。源码：[`ruvia-http/include/decimal_text.h`](../ruvia-http/include/decimal_text.h)

#### 4.3.1 自由函数与常量

```cpp
namespace ruvia {
template </* std::integral，且不是 bool、char、wchar_t、char8_t、char16_t、char32_t */ t_type>
[[nodiscard]] constexpr std::optional<t_type> parse_integer(std::string_view text) noexcept;

template <std::unsigned_integral value_type>
[[nodiscard]] constexpr value_type saturating_add(value_type left, value_type right) noexcept;

[[nodiscard]] constexpr std::uint64_t accumulate_decimal_digit(std::uint64_t value, unsigned digit, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept;

[[nodiscard]] constexpr std::optional<std::uint64_t> parse_saturating_decimal(std::string_view digits, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept;

inline constexpr std::size_t max_decimal_size = 20;

[[nodiscard]] constexpr std::size_t decimal_size(std::uint64_t value) noexcept;

template <std::integral integer_type>
[[nodiscard]] inline std::string_view format_decimal(std::span<char, max_decimal_size> output, integer_type value) noexcept;

template <std::integral integer_type>
inline void append_decimal(std::string& output, integer_type value);

template <typename number_type>
void append_number(std::string& output, number_type value, [[maybe_unused]] const char* error_message);

template <typename number_type>
void append_finite_number(std::string& output, number_type value, const char* finite_error_message, const char* format_error_message);
}
```

| 符号 | 语义 |
| --- | --- |
| `parse_integer<T>(text)` | 唯一严格十进制整数解析（基于 `std::from_chars`，基数 10）。空输入、空白、前导 `+`、尾随字符或越界返回 `std::nullopt`；前导 `-` 仅对有符号类型有效，因此无符号结果恰为 `1*DIGIT`。`T` 不得是 `bool` 或字符类型。 |
| `saturating_add(left, right)` | 无符号整数唯一饱和加法：返回和，若回绕则返回该类型最大值。 |
| `accumulate_decimal_digit(value, digit, maximum)` | 把一位十进制数字 `digit`（0–9）追加到 `value` 末尾（`value * 10 + digit`），结果饱和到 `maximum`。前置条件：`value <= maximum`。供无法一次拿到整段数字的流式解析器逐位调用。 |
| `parse_saturating_decimal(digits, maximum)` | 解析溢出时钳制到上界的 `1*DIGIT` 字段（delta-seconds、`Max-Forwards`、字节范围位置等）：空输入或任一非数字字节返回 `std::nullopt`，否则返回饱和到 `maximum` 的值。 |
| `max_decimal_size` | `20`：64 位整数十进制形式的最大长度（`std::uint64_t` 最大值的 20 位，或 `std::int64_t` 最小值的符号加 19 位）。 |
| `decimal_size(value)` | `value` 的十进制位数（`0` 为 1）。 |
| `format_decimal(output, value)` | 唯一整数切片格式化入口：把 `value` 的十进制形式写到 `output` 开头并返回该前缀（借用 `output`）。有符号类型保留符号，所有宽度先扩宽到 64 位。已用 `decimal_size()` 量好精确切片的写入方应复制该结果，而不是在目标处就地格式化。 |
| `append_decimal(output, value)` | 唯一字符串追加入口：追加 `value` 的十进制形式；仅可能因分配失败抛出。 |
| `append_number(output, value, error_message)` | 追加算术值的最短十进制形式：整数走 `append_decimal`（不会失败）；浮点使用 `std::to_chars` 最短往返表示，格式化失败抛 `std::logic_error(error_message)`。 |
| `append_finite_number(output, value, finite_error_message, format_error_message)` | 浮点专用：NaN 或无穷大在修改 `output` 之前抛 `std::invalid_argument(finite_error_message)`，否则同 `append_number(output, value, format_error_message)`。 |

---

### 4.4 `"utf8.h"`
<a id="api-utf8-h"></a>

UTF-8 良构性的唯一判定点，被 WebSocket 文本与关闭原因、SSE 字段、JSON 解析与写出等共用。全部 `constexpr`、`noexcept`、零分配。源码：[`ruvia-http/include/utf8.h`](../ruvia-http/include/utf8.h)

#### 4.4.1 类型与自由函数

```cpp
namespace ruvia {
struct utf8_sequence final {
    std::size_t length_;
    bool well_formed_;
};

[[nodiscard]] constexpr utf8_sequence scan_utf8_sequence(std::string_view input, std::size_t i) noexcept;
[[nodiscard]] constexpr bool is_valid_utf8(std::string_view value) noexcept;
[[nodiscard]] constexpr std::string_view utf8_prefix(borrowed_text text, std::size_t max_bytes) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `utf8_sequence::length_` | 良构序列的字节数；不良构时为首个非法字节之前的最大子部分（Unicode 3.9 定义 D93b）的字节数。总是 `>= 1`。无默认成员初始化。 |
| `utf8_sequence::well_formed_` | 序列是否良构。 |
| `scan_utf8_sequence(input, i)` | 前置条件：`i < input.size()` 且 `input[i]` 作为无符号值 `>= 0x80`。按 Unicode 3.9 表 3-7 分类从 `input[i]` 开始的序列：拒绝过长形式（`C0`/`C1`、`E0 80–9F`、`F0 80–8F`）、UTF-16 代理（`ED A0–BF`）、超过 U+10FFFF（`F4 90–..`、`F5` 及以上）、游离或截断的续字节。调用方据 `length_` 推进，例如写出时每个最大子部分替换为一个 U+FFFD。 |
| `is_valid_utf8(value)` | 整段是否为良构 UTF-8；ASCII 字节直接跳过。 |
| `utf8_prefix(text, max_bytes)` | 单遍返回不超过字节上限的最长良构码点前缀；遇非法或截断序列即停止，零分配借用输入，拒绝临时拥有型字符串。 |

---

### 4.5 `"binary_text_encoding.h"`
<a id="api-binary-text-encoding-h"></a>

十六进制与 Base64（RFC 4648）的唯一编解码实现，供百分号编码、令牌/摘要编码、JWS 等使用。编码器支持输出与输入任意重叠（含就地）。源码：[`ruvia-http/include/binary_text_encoding.h`](../ruvia-http/include/binary_text_encoding.h)

#### 4.5.1 十六进制

```cpp
namespace ruvia {
[[nodiscard]] constexpr int http_decode_hex_nibble(char c) noexcept;
[[nodiscard]] constexpr char http_lower_hex_digit(int value) noexcept;
[[nodiscard]] constexpr char http_upper_hex_digit(int value) noexcept;
constexpr char* http_encode_lower_hex(char* output, std::span<const unsigned char> bytes) noexcept;
inline char* http_encode_lower_hex(char* output, std::string_view bytes) noexcept;
inline void http_append_lower_hex(std::string& output, std::span<const unsigned char> bytes);
inline void http_append_lower_hex(std::string& output, std::string_view bytes);
[[nodiscard]] constexpr std::optional<std::size_t> http_decode_hex(std::string_view input, std::span<std::uint8_t> output) noexcept;
[[nodiscard]] constexpr bool http_is_lower_hex(std::string_view text, std::size_t required_digits) noexcept;
constexpr char* http_encode_upper_hex(char* output, std::span<const unsigned char> bytes) noexcept;
inline char* http_encode_upper_hex(char* output, std::string_view bytes) noexcept;
inline void http_append_upper_hex(std::string& output, std::span<const unsigned char> bytes);
inline void http_append_upper_hex(std::string& output, std::string_view bytes);
}
```

| 符号 | 语义 |
| --- | --- |
| `http_decode_hex_nibble(c)` | 解码一位十六进制数字（大小写均可），非十六进制字符返回 `-1`。 |
| `http_lower_hex_digit(value)` / `http_upper_hex_digit(value)` | 把 `value` 的低 4 位编码为小写 / 大写十六进制数字。大小写由调用点选择：百分号编码须用大写（RFC 3986 §2.1），令牌/摘要惯用小写。 |
| `http_encode_lower_hex(output, bytes)` | 唯一字节序列到小写十六进制的编码器：从 `output` 起恰好写 `2 * bytes.size()` 个字符并返回末尾指针。`output` 可与 `bytes` 任意重叠（含就地）。`std::string_view` 重载按字节的 `unsigned char` 表示编码；该重载非 `constexpr`。 |
| `http_append_lower_hex(output, bytes)` | 把编码结果追加到 `output`。`bytes` 可以别名 `output`（直到并包括 `output.data()[output.size()]` 处的结尾 NUL）；别名输入在可能重分配的 `resize` 之后按偏移重新定位。结果长度无法容纳时抛 `std::length_error`，此时 `output` 不变。 |
| `http_decode_hex(input, output)` | 大小写兼容的零分配批量解码；奇数长度、非法数字、容量不足或会覆盖未读输入的重叠返回 `nullopt`；支持从输入起点就地解码。失败可能已写入有效前缀。 |
| `http_is_lower_hex(text, required_digits)` | 恰好指定长度且只含 `0-9a-f`。 |
| `http_encode_upper_hex` / `http_append_upper_hex` | 同小写版本的长度、重叠与追加契约，输出 `0-9A-F`。 |

#### 4.5.2 Base64

```cpp
namespace ruvia {
inline constexpr std::string_view http_base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view http_base64_url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum class http_base64_charset : std::uint8_t {
    standard,
    url_safe,
    both,  // decode only
};

enum class http_base64_padding : std::uint8_t {
    required,
    forbidden,
    optional,  // decode only
};

[[nodiscard]] constexpr std::size_t http_base64_encoded_size(std::size_t input_size, http_base64_padding padding = http_base64_padding::required);
inline void http_base64_encode(char* output, std::span<const std::uint8_t> input, http_base64_charset charset = http_base64_charset::standard, http_base64_padding padding = http_base64_padding::required);
[[nodiscard]] constexpr int http_base64_decode_char(char c) noexcept;
[[nodiscard]] constexpr int http_base64_url_decode_char(char c) noexcept;
[[nodiscard]] constexpr std::size_t http_base64_decoded_size_bound(std::size_t input_size) noexcept;
[[nodiscard]] constexpr std::optional<std::size_t> http_base64_decode(std::string_view input, std::span<std::uint8_t> output, http_base64_charset charset, http_base64_padding padding) noexcept;
}
```

| 枚举成员 | 值 | 含义 |
| --- | --- | --- |
| `http_base64_charset::standard` | 0 | RFC 4648 §4 标准字母表，第 62/63 位为 `+`、`/`。 |
| `http_base64_charset::url_safe` | 1 | RFC 4648 §5 URL 安全字母表，第 62/63 位为 `-`、`_`。 |
| `http_base64_padding::required` | 0 | 用 `=` 补齐最后一个量子，长度为 4 的倍数。 |
| `http_base64_padding::forbidden` | 1 | 不填充（RFC 4648 §3.2），例如 JWS base64url。 |
| `http_base64_charset::both` | 2 | 仅解码：同时接受标准与 URL-safe 字母表。 |
| `http_base64_padding::optional` | 2 | 仅解码：允许省略填充；出现 `=` 时仍须精确补齐末量子。 |

| 符号 | 语义 |
| --- | --- |
| `http_base64_alphabet` / `http_base64_url_alphabet` | 两种 64 字符字母表。 |
| `http_base64_encoded_size(input_size, padding)` | 编码后字符数；结果无法用 `std::size_t` 表示时抛 `std::length_error`。 |
| `http_base64_encode(output, input, charset, padding)` | 唯一 Base64 编码器。前置条件：`output` 至少有 `http_base64_encoded_size(input.size(), padding)` 个字符空间；可与 `input` 任意重叠（含就地）。不写结尾 NUL。 |
| `http_base64_decode_char(c)` | 标准字母表字符的 6 位值；其他字符（含 `=`）返回 `-1`。 |
| `http_base64_url_decode_char(c)` | URL 安全版本：`-`→62、`_`→63，`+`、`/` 与其他非法字符返回 `-1`。 |
| `http_base64_decoded_size_bound(input_size)` | `input_size` 个字符解码后的字节数上界；对合法的无填充输入是精确值。 |
| `http_base64_decode(input, output, charset, padding)` | 唯一 Base64 解码器，不分配。只接受规范输入：字符属于 `charset`；`=` 恰如 `padding` 要求（`required` 时长度须为 4 的倍数且填充只补齐末量子，`forbidden` 时不得出现 `=`）；末量子不得只有 1 个字符；末量子未用位必须为零（RFC 4648 §3.5）。返回写入的字节数；非法输入或 `output` 不足以容纳时返回 `std::nullopt`。`output` 可从 `input` 起点或之前开始（就地解码），或位于已编码字符之后；若起点落在编码字符内部（首字符之后），会覆盖未读输入，在任何写入前以 `std::nullopt` 拒绝。长度与所有权策略由调用方负责。 |

- **兼容解码**：`both` 与 `optional` 可组合；仍拒绝空白、非法字符、单字符末量子与非零未用位。编码器拒绝这两种仅解码选项，抛 `std::invalid_argument`。

---

### 4.6 `"url_encoding.h"`
<a id="api-url-encoding-h"></a>

URL 百分号/表单编解码、RFC 3986 URI-reference 补全转义编码、路由使用的路径规范形式比较与 `application/x-www-form-urlencoded` 对遍历。公开 include [`"binary_text_encoding.h"`](#api-binary-text-encoding-h) 与 [`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/url_encoding.h`](../ruvia-http/include/url_encoding.h)

#### 4.6.1 `enum class url_decode_mode`

```cpp
namespace ruvia {
enum class url_decode_mode : std::uint8_t { percent,
    path,
    form,
    strict_form };
}
```

| 成员 | 值 | 含义 |
| --- | --- | --- |
| `percent` | 0 | RFC 3986 pct-encoded 解码；`+` 为字面字符，畸形转义使解码失败。 |
| `path` | 1 | 同 `percent`，另外若转义解出 `/` 或 `\` 则失败，使解码后的路径文本不会出现路由与路径作用域中间件（仅按字面 `/` 分段）未曾看到的段边界。 |
| `form` | 2 | `application/x-www-form-urlencoded`：`+` 解为空格；与 WHATWG percent-decode 算法一致，不构成有效转义的 `%` 保留为字面字符。 |
| `strict_form` | 3 | `+` 解为空格，同时拒绝畸形 `%`；无需先校验再解码。 |

#### 4.6.2 百分号编解码

```cpp
namespace ruvia {
[[nodiscard]] inline bool has_url_encoding(std::string_view value, url_decode_mode mode) noexcept;
[[nodiscard]] constexpr std::optional<unsigned char> http_try_decode_percent_byte(std::string_view input, std::size_t index) noexcept;
[[nodiscard]] constexpr bool is_uri_unreserved_byte(unsigned char byte) noexcept;
inline void http_append_percent_byte(std::string& output, unsigned char byte);
[[nodiscard]] std::string_view percent_encode_uri_reference(borrowed_text reference, std::string& storage);
[[nodiscard]] std::string_view percent_encode_url_component(borrowed_text text, std::string& storage);
[[nodiscard]] inline bool validate_url_encoding(std::string_view value) noexcept;
[[nodiscard]] constexpr std::optional<std::size_t> decode_url_bytes(std::string_view input, char* output, url_decode_mode mode) noexcept;
[[nodiscard]] std::optional<std::string_view> decode_url_component(borrowed_text text, std::string& storage, url_decode_mode mode = url_decode_mode::percent);
}
```

| 符号 | 语义 |
| --- | --- |
| `has_url_encoding(value, mode)` | 是否含需解码的字符：`percent`/`path` 只看 `%`，`form`/`strict_form` 看 `%` 或 `+`。不校验转义合法性。 |
| `http_try_decode_percent_byte(input, index)` | 解码从 `input[index]` 开始的 `%HH` 三元组（RFC 3986 §2.1）；三元组被截断、不以 `%` 开始或含非十六进制数字时返回 `std::nullopt`。`index` 越界同样返回 `std::nullopt`。 |
| `is_uri_unreserved_byte(byte)` | RFC 3986 §2.3 unreserved：`ALPHA / DIGIT / "-" / "." / "_" / "~"`。这些字节编码与否等价（§6.2.2.2），编码器保持其字面形式。 |
| `http_append_percent_byte(output, byte)` | 追加规范的大写三元组 `%HH`；哪些字节需要转义由调用方决定。失败（分配）时 `output` 不变。 |
| `percent_encode_uri_reference(reference, storage)` | 为应用提供的 RFC 3986 URI-reference（例如 `Location` 字段值）补全转义：对在其位置不能字面出现的字节做百分号编码。已有的良构三元组与合法字节逐字节保留，因此正确编码的引用不会被二次编码（§2.4）。`[`、`]` 仅作为 authority 的 IP-literal 定界符保留；相对路径引用的第一段中的 `:` 被编码（§4.2）；userinfo 内的 `@` 被编码，只保留定界用的 `@`（§3.2.1）；只有第一个 `#` 开始片段。无需编码时返回 `reference` 本身且不分配；否则编码结果替换 `storage` 内容并返回其视图。失败（分配或长度错误）保留 `storage` 原内容。 |
| `percent_encode_url_component(text, storage)` | 单组件编码：仅保留 unreserved 字节，其余（含 `%`、`+`、分隔符、UTF-8 字节）转为大写 `%XX`；空格为 `%20`。无需转义时直接借用输入，否则原子替换 `storage`；支持输入别名。 |
| `validate_url_encoding(value)` | 每个 `%` 是否都开始一个合法三元组。 |
| `decode_url_bytes(input, output, mode)` | 唯一 URL 解码循环：按 `mode` 一遍把 `input` 解码到 `output` 并返回解码长度；`mode` 拒绝时返回 `std::nullopt`（`output` 可能已被部分写入）。前置条件：`output` 至少 `input.size()` 字节且与 `input` 不重叠。需要把多个解码值打包进同一缓冲的调用方直接使用它。 |
| `decode_url_component(text, storage, mode)` | 按 `mode` 解码一个 URL 组件。无可解码内容时返回 `text` 本身且不分配；否则解码字节替换 `storage` 内容并返回其视图；`mode` 拒绝时返回 `std::nullopt`，保留 `storage` 的原内容和容量。 |

`decode_url_component()` 与 `percent_encode_uri_reference()` 的借用规则：

- 文本参数为 [`borrowed_text`](#api-borrowed-text-h)：字符串字面量、`std::string_view` 和命名的 `std::string` 可直接传入；临时拥有型字符串（含 `const` 右值）无论内容是否需要转换都在编译期被拒绝。
- 返回值可能借用原始输入，也可能借用 `storage`；两者的 owner 都必须覆盖结果的消费，不要根据某次内容是否需要转换来推断寿命。需要独立结果时在 owner 仍有效期间复制视图。
- 输入可以别名 `storage`（例如对 `storage` 自身的内容就地解码或编码）。

```cpp
std::string storage;
const std::string raw = "a%20b";
if (const auto decoded = ruvia::decode_url_component(raw, storage)) {
    // *decoded == "a b"，借用 storage；raw 与 storage 须活到 *decoded 用完。
}
// ruvia::decode_url_component(std::string("a%20b"), storage);  // 编译期拒绝
```

#### 4.6.3 路径规范形式

```cpp
namespace ruvia {
inline constexpr int uri_path_separator_unit = 0x100;
[[nodiscard]] constexpr int next_uri_path_unit(std::string_view path, std::size_t& index) noexcept;
[[nodiscard]] constexpr int compare_uri_paths(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] constexpr bool uri_paths_equivalent(std::string_view left, std::string_view right) noexcept;
}
```

origin-form 路径的唯一规范身份，路由匹配、路径作用域中间件与回退、解码后的路由参数共用。路径只按字面 `/` 分段，每段取其百分号解码后的字节。因此十六进制大小写与编码的 unreserved 字符不会使两种拼写不同（RFC 3986 §6.2.2.1、§6.2.2.2），编码的 `%2F` 留在所在段内而不成为边界。该形式有意比 RFC 3986 等价更粗：编码的保留字符等于其字面形式（`%3A` 与 `:`），因为从两种拼写解码出的参数值相同；RFC 9110 §4.2.3 只禁止把等价 URI 拆到不同资源，不禁止合并不等价者。

| 符号 | 语义 |
| --- | --- |
| `uri_path_separator_unit` | `0x100`：字面 `/` 的规范单元，与任何字节值（0–255）不同。 |
| `next_uri_path_unit(path, index)` | 前置条件 `index < path.size()`。返回 `path[index]` 处的规范单元并把 `index` 推进过它：字面或解码后的字节（0–255）、字面 `/` 返回 `uri_path_separator_unit`、畸形转义返回 `-1`（仅推进 1 字节）。 |
| `compare_uri_paths(left, right)` | 按规范单元三路比较，返回负数、零或正数（同 `std::string_view::compare` 的符号约定）。对良构路径是全序，与 `uri_paths_equivalent` 一致，可用于有序查找。 |
| `uri_paths_equivalent(left, right)` | 两路径（或两段）规范形式是否相同；畸形转义永不相等；不含 `%` 的拼写逐字节比较。 |

#### 4.6.4 表单对遍历

```cpp
namespace ruvia {
enum class url_empty_pair_policy : std::uint8_t { skip, preserve };
template <typename visitor_type>
[[nodiscard]] bool visit_url_encoded_pairs(std::string_view input, visitor_type&& visitor, url_empty_pair_policy empty_pairs = url_empty_pair_policy::skip);
}
```

单遍扫描 `input`：以 `&` 分段，以段内第一个 `=` 分隔名称和值；无 `=` 时值为空。按输入顺序访问各对，保留重复名称。传给访问器的是借用 `input` 的原始视图，不解码、不校验百分号转义。访问器返回 `false` 时提前停止，函数也返回 `false`；返回 `void` 时遍历全部对，函数返回 `true`。访问器异常原样传播。

- **空段策略**：`url_empty_pair_policy::preserve` 对每个空段调用一次访问器，包含首尾 `&` 与 `&&`；空输入产生一对空名称/空值。默认 `skip` 不变。

---

### 4.7 `"ip_address.h"`
<a id="api-ip-address-h"></a>

与 socket 库无关的端口、IP 地址、端点与 CIDR 值类型，供 authority、客户端 IP 提取、PROXY 协议与 Web 端点配置共用。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h) 与 [`"checked_integer.h"`](#api-checked-integer-h)。源码：[`ruvia-http/include/ip_address.h`](../ruvia-http/include/ip_address.h)

#### 4.7.1 `class port_number`

```cpp
namespace ruvia {
class port_number final : public checked_integer<port_number, std::uint16_t, /* 未指定策略类型 */> {
public:
    using checked_integer::checked_integer;
};
static_assert(std::is_trivially_copyable_v<port_number>);
static_assert(sizeof(port_number) == sizeof(std::uint16_t));
}
```

传输无关的 16 位端口值，接口全部来自 [`checked_integer`](#api-checked-integer-h)：

- 值域 `0..65535`，按原始整数宽度检查；非法值构造时抛 `std::invalid_argument("port number must be in 0..65535")`，无异常探测用 `port_number::try_from_value()`。
- 整数到 `port_number` 隐式转换（合法整数可直接填入配置字段）；不接受浮点与枚举，也不隐式转换回整数，用 `value()` 解包。不要在调用前窄化。
- 默认构造为 `0`。零端口可表示，由各消费者定义零端口的含义；“缺省”用 `std::optional<port_number>` 表示，与显式 `0` 不同。例如 [`http_authorities_equal()`](#api-http-authority-h) 的默认端口为 `std::nullopt` 时，省略/空端口只与省略/空端口相等，不等于任何显式端口（含 `:0`）。已解析的协议端口和系统观察结果仍可能以 `std::uint16_t` 返回。

#### 4.7.2 `enum class ip_address_family` 与 `class ip_address`

```cpp
namespace ruvia {
enum class ip_address_family : std::uint8_t {
    ipv4,
    ipv6,
};

class ip_address final {
public:
    constexpr ip_address() noexcept = default;
    [[nodiscard]] static constexpr ip_address ipv4(const std::array<std::uint8_t, 4>& bytes) noexcept;
    [[nodiscard]] static constexpr ip_address ipv6(const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept;
    [[nodiscard]] static std::optional<ip_address> parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr ip_address_family family() const noexcept;
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint8_t> bytes() const&& = delete;
    [[nodiscard]] constexpr std::uint32_t scope_id() const noexcept;
    [[nodiscard]] constexpr bool is_ipv4_mapped() const noexcept;
    [[nodiscard]] constexpr ip_address normalized_ipv4() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint32_t> ipv4_uint32() const noexcept;
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_multicast() const noexcept;
    [[nodiscard]] bool is_link_local() const noexcept;
    [[nodiscard]] bool is_public_unicast() const noexcept;

    static constexpr std::size_t max_text_size = 50;
    [[nodiscard]] std::string_view format_to(std::span<char, max_text_size> output RUVIA_LIFETIMEBOUND) const noexcept;
    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const ip_address&, const ip_address&) noexcept = default;
    friend constexpr auto operator<=>(const ip_address&, const ip_address&) noexcept = default;
};
static_assert(std::is_trivially_copyable_v<ip_address>);
}
```

| 成员 | 值 | 含义 |
| --- | --- | --- |
| `ip_address_family::ipv4` | 0 | IPv4，4 字节地址。 |
| `ip_address_family::ipv6` | 1 | IPv6，16 字节地址，可带数字 zone。 |

`ip_address` 以网络字节序保存数字地址。IPv6 地址可带数字 zone（RFC 4007 §11，用于链路本地对端），`0` 表示无 zone。IPv4 映射的 IPv6 地址（RFC 4291 §2.5.5.2）在此是独立的 IPv6 值；要把它当作 IPv4 的消费者须显式规范化。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | `0.0.0.0`（IPv4）。 |
| `ipv4(bytes)` / `ipv6(bytes, scope_id)` | 由网络序字节构造。 |
| `parse(text)` | 解析完整地址字面量：不带前导零的点分十进制 IPv4，或 RFC 4291 §2.2 IPv6 文本并可带数字 `%zone`。方括号、端口、首尾空白与内嵌 NUL 均返回 `std::nullopt`。 |
| `family()` / `scope_id()` | 地址族与 zone（IPv4 恒为 0）。 |
| `bytes() const&` | IPv4 返回 4 字节、IPv6 返回 16 字节的网络序视图，借用 `*this`；右值重载 `= delete`，拒绝从临时地址取借用。 |
| `is_ipv4_mapped()` | 是否为无 zone 的 `::ffff:a.b.c.d`。 |
| `normalized_ipv4()` | 若是 IPv4 映射地址返回对应 IPv4，否则返回自身。 |
| `ipv4_uint32()` | 原生 IPv4 的网络序数值（`a.b.c.d == 0xaabbccdd`）；IPv6 返回 `nullopt`，映射地址须显式 `normalized_ipv4()`。 |
| `is_loopback()` / `is_multicast()` / `is_link_local()` | 按地址族分类；无 scope 的 IPv4-mapped IPv6 按 IPv4 分类。 |
| `is_public_unicast()` | 固定的保守特殊用途排除策略，不代表路由可达性。IPv4 排除未指定、私有、共享、回环、链路本地、文档、基准、多播及保留段；IPv6 只接受 `2000::/3`，再排除协议分配、文档、6to4 与旧 6bone 段。具体网段在公开头维护；带 scope 始终返回假。 |
| `max_text_size` | `50`：最长文本（8 组 4 位十六进制、7 个冒号、`%` 加 10 位 zone）。 |
| `format_to(output)` | RFC 5952 文本：IPv4 点分十进制；IPv6 小写十六进制并压缩最长的零组，IPv4 映射写成 `::ffff:a.b.c.d`，zone 写成 `%<scope>`。写到 `output` 开头并返回该前缀（借用 `output`），不分配。 |
| `to_string()` | 同 `format_to()` 的拥有型字符串。 |
| `operator==` / `operator<=>` | 按成员（地址族、字节、zone）的默认比较。 |

#### 4.7.3 `class ip_endpoint`

```cpp
namespace ruvia {
class ip_endpoint final {
public:
    constexpr ip_endpoint() noexcept = default;
    constexpr ip_endpoint(ip_address address, std::uint16_t port) noexcept;
    [[nodiscard]] constexpr ip_address address() const noexcept;
    [[nodiscard]] constexpr std::uint16_t port() const noexcept;
    friend constexpr bool operator==(const ip_endpoint&, const ip_endpoint&) noexcept = default;
};
static_assert(std::is_trivially_copyable_v<ip_endpoint>);
}
```

一个传输端点的地址与端口（默认 `0.0.0.0`、端口 `0`）。端口 `0` 表示端口未知（例如未记录端口的转发客户端地址）。`operator==` 逐成员比较。

#### 4.7.4 `class ip_cidr`

```cpp
namespace ruvia {
class ip_cidr final {
public:
    constexpr ip_cidr() noexcept = default;
    [[nodiscard]] static std::optional<ip_cidr> parse(std::string_view text) noexcept;
    [[nodiscard]] ip_address network() const noexcept;
    [[nodiscard]] constexpr std::uint8_t prefix_bits() const noexcept;
    [[nodiscard]] ip_address first_address() const noexcept;
    [[nodiscard]] ip_address last_address() const noexcept;
    [[nodiscard]] bool contains(const ip_address& address) const noexcept;
    [[nodiscard]] bool contains(std::string_view address_text) const noexcept;
    friend constexpr bool operator==(const ip_cidr&, const ip_cidr&) noexcept = default;
};
static_assert(std::is_trivially_copyable_v<ip_cidr>);
}
```

IPv4 或 IPv6 CIDR 前缀块（或不带前缀的单个地址，视为全宽前缀）。保留原生地址族与前缀宽度；IPv4 块同时匹配原生 IPv4 与无 scope 的 IPv4-mapped IPv6。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 网络全零、前缀 128 位，即只匹配 `::`。 |
| `parse(text)` | 单遍解析 `address` 或 `address/prefix`；前缀不超过 32（IPv4）或 128（IPv6），必须全零主机位，拒绝全部 IPv6 zone 拼写（含 `%0`）。非法返回 `nullopt`。 |
| `contains(address)` | 是否落在块内；IPv6 块匹配时忽略对端 scope，IPv4 块保持上述映射规则。 |
| `contains(address_text)` | 先 `ip_address::parse`；无法解析时返回 `false`。 |
| `operator==` | 按保存的网络与前缀长度比较。 |
| `network()` / `prefix_bits()` | 原地址族的网络地址与前缀位数，无需重新切分文本。 |
| `first_address()` / `last_address()` | 包含两端的完整地址区间，不扣除网络地址或广播地址；`/0` 与全宽前缀同样适用。 |

---

### 4.8 `"decode_step.h"`
<a id="api-decode-step-h"></a>

所有增量解析器/解码器共用的单步结果与生命周期契约。各族结果类型是 `decode_step` 的实例（共享备选项加本族负载类型），各族错误枚举保持独立并由 `decode_failure<E>::error()` 给出。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)（用于 `RUVIA_LIFETIMEBOUND`）。源码：[`ruvia-http/include/decode_step.h`](../ruvia-http/include/decode_step.h)

#### 4.8.1 `enum class decoder_state` 与处理器生命周期

```cpp
namespace ruvia {
enum class decoder_state : std::uint8_t {
    active,
    finished,
    failed,
};
}
```

| 成员 | 值 | 含义 |
| --- | --- | --- |
| `active` | 0 | 可继续接收输入。 |
| `finished` | 1 | 已完整处理一条消息/一个编码体。 |
| `failed` | 2 | 遇到数据错误（或资源错误）后停止。 |

有状态处理器统一暴露 `decoder_state state()` 与显式 `reset()`。只有 `reset()` 能让 `finished` 或 `failed` 的处理器回到 `active`；在此之前处理器不再接收输入，按两类之一处理后续调用：

| 类别 | 成员 | 终态后再次调用 |
| --- | --- | --- |
| 终态结果供提取的解析器 | [`http1_server_request_parser`](#api-http1-server-request-h) 与 [`http1_client_response_parser`](#api-http1-client-response-h) 的 `parse()`、[`proxy_protocol_parser`](#api-proxy-protocol-h) 与 [`sse_parser`](#api-http-server-sent-events-h) 的 `parse()`、[`multipart_parser::feed()`](#api-http-multipart-h) | 抛 `std::logic_error`。 |
| 流式解码器 | [`http_content_decoder`](#api-http-content-codec-h)、[`http1_chunked_body_decoder`](#api-http1-chunked-body-h)、[传输编码栈](#api-http-transfer-coding-h)、[`http1_body_decoder`](#api-http1-message-body-h)、[Capsule 解码器](#api-http-capsule-h) 的 `decode()` / `feed()` / `poll()`，以及 `multipart_parser::poll()` | 重放同一终态结果，`consumed_bytes()` 为 0，状态不变。 |

#### 4.8.2 备选项类型

```cpp
namespace ruvia {
struct decode_need_input final {};
struct decode_complete final {};

class decode_output final {
public:
    explicit constexpr decode_output(std::string_view bytes) noexcept;
    [[nodiscard]] constexpr std::string_view bytes() const noexcept;
};

template <typename error_type>
class decode_failure final {
public:
    explicit constexpr decode_failure(error_type error) noexcept;
    [[nodiscard]] constexpr error_type error() const noexcept;
};
}
```

| 类型 | 语义 |
| --- | --- |
| `decode_need_input` | 本步已消费能用的字节，需要更多输入。 |
| `decode_complete` | 处理器完成了消息，本步不携带其他负载。 |
| `decode_output` | 解码出的字节；`bytes()` 借用产出它的解码器所指明的存储（其输入或调用方的 scratch），有效期以该解码器的契约为准。构造时拒绝临时拥有型字符串和字符初始化列表。 |
| `decode_failure<error_type>` | 数据错误，携带报告它的族的错误枚举，由 `error()` 读取。 |

#### 4.8.3 `template <typename... alternative_types> class decode_step`

```cpp
namespace ruvia {
template <typename... alternative_types>
class decode_step final {
public:
    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    constexpr decode_step(std::size_t consumed_bytes, alternative_type alternative) noexcept(std::is_nothrow_move_constructible_v<alternative_type>);

    decode_step(const decode_step&) = default;
    decode_step(decode_step&&) = default;
    decode_step& operator=(const decode_step&) = delete;
    decode_step& operator=(decode_step&&) = delete;

    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept;

    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr bool holds() const noexcept;

    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr const alternative_type* get_if() const& noexcept RUVIA_LIFETIMEBOUND;
    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr alternative_type* get_if() & noexcept RUVIA_LIFETIMEBOUND;
    template <typename alternative_type>
    const alternative_type* get_if() const&& = delete;

    [[nodiscard]] constexpr decode_step with_consumed_bytes(std::size_t consumed_bytes) const;
};
}
```

一次增量解析/解码调用的结果：本步消费的输入字节数，加上本族备选项之一。

| 成员 | 语义 |
| --- | --- |
| 构造 | `alternative_type` 必须恰为某个 `alternative_types`（不做隐式转换）。类别在构造时固定。 |
| 复制/移动构造 | 默认；复制/移动赋值 `= delete`，结果不可被改写为另一类别。 |
| `consumed_bytes()` | 本步消费的输入字节数。调用方从输入中丢弃这么多字节，其余保留到下一次调用。 |
| `holds<T>()` | 是否持有 `T`。 |
| `get_if<T>() const&` / `get_if<T>() &` | 持有 `T` 时返回指向它的指针，否则 `nullptr`；指针借用该 step 对象。非 `const` 左值重载用于从拥有型负载中提取（负载的 `take_*() &&` 须经仍持有它的 step 调用）。 |
| `get_if<T>() const&& = delete` | 拒绝从临时 step 取借用指针；先把结果保存为命名变量。 |
| `with_consumed_bytes(n)` | 返回同一备选项、消费字节数为 `n` 的副本，供在自身分帧之后组合另一解码器的解码器使用。 |

```cpp
const auto step = decoder.decode(input, /* ... */);
input.remove_prefix(step.consumed_bytes());
if (const auto* out = step.get_if<ruvia::decode_output>()) {
    consume(out->bytes());
} else if (step.holds<ruvia::decode_need_input>()) {
    // 读取更多字节后以剩余输入加新字节再次调用
}
```

---

### 4.9 `"http_io_buffer.h"`
<a id="api-http-io-buffer-h"></a>

可复用缓冲的保留上限，以及面向传输输入的字节缓冲 `http_io_buffer`（可读窗口后接可写空间，扩容不初始化即将被读覆盖的字节）。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http_io_buffer.h`](../ruvia-http/include/http_io_buffer.h)

#### 4.9.1 保留上限

```cpp
namespace ruvia {
inline constexpr std::size_t http_retained_string_bytes = 4096;
inline void http_clear_string_retaining_small(std::string& target, std::size_t retained_bytes = http_retained_string_bytes) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `http_retained_string_bytes` | `4096`：可复用的 worker 本地临时字符串与缓冲（头部、客户端写缓冲）排空后保留的最大存储。更大的存储归还分配器，避免一次大消息长期占据空闲连接或 worker；不超过上限的缓冲复用而不重分配。 |
| `http_clear_string_retaining_small(target, retained_bytes)` | 清空 `target`；若 `capacity()` 超过 `retained_bytes` 则释放其存储（换成空字符串）。 |

#### 4.9.2 `class http_io_buffer`

```cpp
namespace ruvia {
class http_io_buffer final {
public:
    http_io_buffer() noexcept = default;
    http_io_buffer(const http_io_buffer&) = delete;
    http_io_buffer& operator=(const http_io_buffer&) = delete;
    http_io_buffer(http_io_buffer&& other) noexcept;
    http_io_buffer& operator=(http_io_buffer&& other) noexcept;
    ~http_io_buffer();

    [[nodiscard]] char* data() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const char* data() const& noexcept RUVIA_LIFETIMEBOUND;
    char* data() && = delete;
    const char* data() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view view() const&& = delete;
    [[nodiscard]] std::size_t capacity() const noexcept;

    [[nodiscard]] std::span<char> prepare(std::size_t bytes) & RUVIA_LIFETIMEBOUND;
    std::span<char> prepare(std::size_t) && = delete;
    [[nodiscard]] std::span<char> prepare_spare() & noexcept RUVIA_LIFETIMEBOUND;
    std::span<char> prepare_spare() && = delete;
    void commit(std::size_t bytes);
    void append(std::string_view bytes);
    void consume(std::size_t bytes) noexcept;

    void reserve(std::size_t bytes);
    void shrink_to(std::size_t bytes);
    void clear() noexcept;
    void clear_retaining_small(std::size_t retained_bytes = http_retained_string_bytes) noexcept;
};
}
```

一次读操作填充 `prepare()` 或 `prepare_spare()` 返回的 span，再以 `commit()` 发布实际写入的字节。未初始化字节永远不属于可读窗口。所有存储借用（`data()`、`view()`、`prepare()`、`prepare_spare()`）只接受左值宿主，右值重载 `= delete`；存储必须存活到可读视图或可写 span 被消费为止，包括挂起中的异步 I/O。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 空，无存储。 |
| 移动构造 | 接管存储与窗口，源对象变为无存储的空缓冲；不分配。复制与移动赋值均 `= delete`。 |
| `data()` / `view()` / `size()` / `empty()` | 可读字节。 |
| `capacity()` | 不重分配可容纳的字节数，含可读字节。 |
| `prepare(bytes)` | 返回恰好 `bytes` 个未初始化的可写字节，位于可读字节之后。已消费前缀覆盖存活字节且能腾出空间时先压缩，否则按几何级数增长以摊销小块读写。大小超出可寻址范围时抛 `std::length_error`；分配失败抛 `std::bad_alloc`，原窗口与已借用字节保留，可在恢复资源后重试。 |
| `prepare_spare()` | 返回当前可写尾部而不增长；仅当尾部为空且已消费前缀覆盖存活字节时回收前缀。即使 `capacity() > size()` 也可能返回空 span：需要空间的读取方须先 `prepare(n)`，并按实际 span 长度读取。 |
| `commit(bytes)` | 把可写空间的前 `bytes` 字节（调用方已写入）并入可读窗口；超过已准备空间时抛 `std::length_error` 且不改变状态。 |
| `append(bytes)` | 追加 `bytes` 的副本；`bytes` 可以别名本缓冲。若源完全位于可读窗口内且压缩后空间足够，直接复用存储，不分配。 |
| `consume(bytes)` | 丢弃前 `min(bytes, size())` 个可读字节，不移动数据。 |
| `reserve(bytes)` | 存储增长到至少 `bytes`（增长时恰为 `bytes`）；超出可寻址范围抛 `std::length_error`。 |
| `shrink_to(bytes)` | 释放超过 `max(size(), bytes)` 的存储。 |
| `clear()` | 丢弃全部可读字节，保留存储。 |
| `clear_retaining_small(retained_bytes)` | 丢弃全部可读字节，并把大于 `retained_bytes` 的存储归还分配器。 |

失效规则：`consume()` 只推进窗口，因此可读字节的视图在下一次 `prepare()`、`prepare_spare()`、`append()`、`reserve()`、`shrink_to()` 或 `clear_retaining_small()` 之前保持有效。压缩只在已消费前缀覆盖存活字节时移动数据，且优先使用已有的可写尾部。清空或重分配时先提交新状态再释放旧存储。

```cpp
ruvia::http_io_buffer buffer;
auto space = buffer.prepare(4096);
const std::size_t n = read_some(socket, space.data(), space.size());
buffer.commit(n);
const auto step = parser.parse(buffer.view());
buffer.consume(step.consumed_bytes());
```

---

### 4.10 `"http_protocol.h"`
<a id="api-http-protocol-h"></a>

跨协议共享的上限常量、显式有限/无限字节预算 `protocol_byte_limit`、协议错误异常 `http_protocol_error`，以及 HTTP/1 请求头解析错误 `http_parse_error` 及其到响应状态的映射。公开 include [`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http_protocol.h`](../ruvia-http/include/http_protocol.h)

#### 4.10.1 常量

```cpp
namespace ruvia {
inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_http_header_fields = 256;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_sse_event_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::uint32_t max_unprocessed_stream_resets = 1000;
}
```

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `max_http_header_bytes` | 65536（64 KiB） | 一个字段区段（头或 Trailer）的字节上限，按 [`http_field_section_budget`](#api-http-field-section-budget-h) 计费；也约束单个 chunk-size 行等。 |
| `max_http_header_fields` | 256 | 每个 HTTP/1 头或 Trailer 区段的字段行数上限，接收与生成均适用，与字节上限同时生效。最短字段行只有 4 字节，仅凭字节上限可让一个头携带约 16 K 个字段，其描述符远大于头部本身；256 与 Pingora 解析器上限相同。 |
| `default_max_buffered_body_bytes` | 16 MiB | 缓冲正文的默认上限。 |
| `default_max_websocket_message_bytes` | 16 MiB | WebSocket 消息的默认上限。 |
| `default_max_sse_event_bytes` | 16 MiB | `sse_parser` 组装单个 SSE 事件时可缓冲的字节数默认上限。 |
| `max_unprocessed_stream_resets` | 1000 | Rapid Reset（CVE-2023-44487）防线：多路复用对端放弃的请求流可领先于连接已完成响应的数量；超过时连接以 HTTP/2 `ENHANCE_YOUR_CALM` 或 HTTP/3 `H3_EXCESSIVE_LOAD` 结束。 |

#### 4.10.2 `class protocol_byte_limit`

```cpp
namespace ruvia {
class protocol_byte_limit final {
public:
    protocol_byte_limit() noexcept = default;
    [[nodiscard]] static protocol_byte_limit unlimited() noexcept;
    template <std::integral integer_type>
    [[nodiscard]] static protocol_byte_limit limited(integer_type bytes_value);
    [[nodiscard]] bool is_limited() const noexcept;
    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept;
    [[nodiscard]] bool exceeds(std::size_t bytes_value) const noexcept;
    [[nodiscard]] bool addition_exceeds(std::size_t current, std::size_t added) const noexcept;
    [[nodiscard]] std::size_t read_ceiling() const noexcept;
};
}
```

没有数值哨兵的协议字节上限：默认即不限量，配置的上限总是严格正数。

| 成员 | 语义 |
| --- | --- |
| 默认构造 / `unlimited()` | 不限量。 |
| `limited(bytes_value)` | 有限上限。接受原始整数并在窄化前检查：必须严格为正且可表示为 `std::size_t`，否则抛 `std::invalid_argument`（负数不会变成实际无界的正上限）。浮点与枚举不参与；不要预先窄化。`SIZE_MAX` 仍是有限上限，不是无限哨兵。 |
| `is_limited()` / `maximum()` | 是否有限；有限时 `maximum()` 返回上限，否则 `std::nullopt`。 |
| `exceeds(bytes_value)` | 有限且 `bytes_value > maximum` 时为 `true`。 |
| `addition_exceeds(current, added)` | `current + added` 会溢出 `std::size_t` 时总为 `true`；否则在有限时判断和是否超过上限（`current` 已超限也为 `true`）。 |
| `read_ceiling()` | 读取上限：有限时为上限，无限时为 `SIZE_MAX`。 |

```cpp
.max_message_bytes_ = ruvia::protocol_byte_limit::limited(256 * 1024),
```

#### 4.10.3 `class http_protocol_error`

```cpp
namespace ruvia {
class http_protocol_error final : public std::exception {
public:
    http_protocol_error(http_status_code status, static_text code, std::string_view message) noexcept;
    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] std::string_view code() const noexcept;
    [[nodiscard]] const char* what() const noexcept RUVIA_LIFETIMEBOUND override;
};
}
```

协议原语以响应状态拒绝输入时使用的异常/值，服务端驱动可把它映射到自身的应用错误模型。

| 成员 | 语义 |
| --- | --- |
| 构造 | `status` 为应答状态；`code` 为稳定的 snake_case 机器码（如 `"missing_host"`、`"conflicting_content_length"`），通过 [static_text](#api-borrowed-text-h) 在编译期要求静态存储期；`message` 只复制最多 127 字节的合法 UTF-8 前缀，遇到无效序列或放不下的码点即停止，以 NUL 结尾。不分配、不借用 `message`。 |
| `status()` / `code()` | 应答状态与机器码。 |
| `what()` | 返回借用异常对象内部存储的可读消息。 |

#### 4.10.4 `enum class http_parse_error` 与 `make_http_protocol_error`

```cpp
namespace ruvia {
enum class http_parse_error : std::uint8_t { header_too_large,
    uri_too_long,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding };

[[nodiscard]] http_protocol_error make_http_protocol_error(http_parse_error error) noexcept;
}
```

HTTP/1 服务端请求头解析（[`http1_server_request_parser`](#api-http1-server-request-h)）报告的错误。`make_http_protocol_error()` 给出服务端应答各错误时使用的 `http_protocol_error`；`code()` 与枚举成员名相同：

| 成员 | 值 | 含义 | 状态 |
| --- | --- | --- | --- |
| `header_too_large` | 0 | 请求头超过字节或字段行上限（第 257 行亦然）。 | 431 |
| `uri_too_long` | 1 | 请求目标过长。 | 414 |
| `invalid_request_line` | 2 | 请求行非法。 | 400 |
| `unsupported_http_version` | 3 | 不支持的 HTTP 版本。 | 505 |
| `invalid_request_target` | 4 | 请求目标非法。 | 400 |
| `invalid_header` | 5 | 字段行非法。 | 400 |
| `invalid_connection` | 6 | `Connection` 字段非法。 | 400 |
| `invalid_upgrade` | 7 | `Upgrade` 字段非法。 | 400 |
| `missing_host` | 8 | 缺少 `Host`。 | 400 |
| `invalid_host` | 9 | `Host` 非法。 | 400 |
| `invalid_content_length` | 10 | `Content-Length` 非法。 | 400 |
| `conflicting_content_length` | 11 | `Content-Length` 值互相冲突。 | 400 |
| `invalid_transfer_encoding` | 12 | `Transfer-Encoding` 非法。 | 400 |
| `unsupported_transfer_encoding` | 13 | 不支持的传输编码。 | 501 |

`make_http_protocol_error` 是一个按错误族重载的名字：凡能映射到响应的错误族都在其枚举旁边重载它（例如 [`http_request_body_failure`](#api-http-request-body-failure-h)、[`http1_chunk_decode_error`](#api-http1-chunked-body-h)、[`http1_body_decode_error`](#api-http1-message-body-h)、[`multipart_parse_error`](#api-http-multipart-h)、[`websocket_handshake_error`](#api-websocket-handshake-h) 等），各重载在其头中说明。

---

### 4.11 `"http_status.h"`
<a id="api-http-status-h"></a>

强类型 HTTP 状态码 `http_status_code`、稳定 RFC 注册名称常量 `namespace ruvia::http_status`、客户端接收状态 `http_received_status`、原因短语与可跟随重定向判定。公开 include [`"checked_integer.h"`](#api-checked-integer-h)。源码：[`ruvia-http/include/http_status.h`](../ruvia-http/include/http_status.h)

#### 4.11.1 `class http_status_code`

```cpp
namespace ruvia {
class http_status_code final : public checked_integer<http_status_code, std::uint16_t, /* 未指定策略类型 */> {
public:
    using checked_integer::checked_integer;
    [[nodiscard]] constexpr bool is_informational() const noexcept;
    [[nodiscard]] constexpr bool is_successful() const noexcept;
    [[nodiscard]] constexpr bool is_redirection() const noexcept;
    [[nodiscard]] constexpr bool is_client_error() const noexcept;
    [[nodiscard]] constexpr bool is_server_error() const noexcept;
    [[nodiscard]] constexpr bool is_error() const noexcept;
    [[nodiscard]] constexpr bool is_final() const noexcept;
};
static_assert(std::is_trivially_copyable_v<http_status_code>);
static_assert(sizeof(http_status_code) == sizeof(std::uint16_t));
}
```

受检状态码：完整 `100..599` 扩展空间均可表示，任意整数无法流入协议 API。机制见 [`checked_integer`](#api-checked-integer-h)：

- 整数构造为 `explicit`，非法值抛 `std::invalid_argument("HTTP status code must be in 100..599")`；`try_from_value()`、`is_valid_value()` 不抛异常。三者都在原始整数域检查，不要预先窄化到 `std::uint16_t`。
- 不可默认构造。
- 分类谓词：`is_informational()`（`< 200`）、`is_successful()`（`200..299`）、`is_redirection()`（`300..399`）、`is_client_error()`（`400..499`）、`is_server_error()`（`>= 500`）、`is_error()`（`>= 400`）、`is_final()`（`>= 200`）。

#### 4.11.2 `namespace ruvia::http_status`

```cpp
namespace ruvia::http_status {
inline constexpr auto continue_ = http_status_code(100);
// …… 下表全部常量均为 inline constexpr http_status_code
}
```

稳定的 RFC 分配名称只在此定义一次。未知扩展码与临时草案注册仍可经受检构造或 `try_from_value()` 使用，但不会把不稳定名称变成框架 API。全部常量（类型均为 `const http_status_code`）及 `http_reason_phrase()` 返回的短语：

| 常量 | 值 | 原因短语 |
| --- | --- | --- |
| `continue_` | 100 | Continue |
| `switching_protocols` | 101 | Switching Protocols |
| `processing` | 102 | Processing |
| `early_hints` | 103 | Early Hints |
| `ok` | 200 | OK |
| `created` | 201 | Created |
| `accepted` | 202 | Accepted |
| `non_authoritative_information` | 203 | Non-Authoritative Information |
| `no_content` | 204 | No Content |
| `reset_content` | 205 | Reset Content |
| `partial_content` | 206 | Partial Content |
| `multi_status` | 207 | Multi-Status |
| `already_reported` | 208 | Already Reported |
| `im_used` | 226 | IM Used |
| `multiple_choices` | 300 | Multiple Choices |
| `moved_permanently` | 301 | Moved Permanently |
| `found` | 302 | Found |
| `see_other` | 303 | See Other |
| `not_modified` | 304 | Not Modified |
| `use_proxy` | 305 | Use Proxy |
| `temporary_redirect` | 307 | Temporary Redirect |
| `permanent_redirect` | 308 | Permanent Redirect |
| `bad_request` | 400 | Bad Request |
| `unauthorized` | 401 | Unauthorized |
| `payment_required` | 402 | Payment Required |
| `forbidden` | 403 | Forbidden |
| `not_found` | 404 | Not Found |
| `method_not_allowed` | 405 | Method Not Allowed |
| `not_acceptable` | 406 | Not Acceptable |
| `proxy_authentication_required` | 407 | Proxy Authentication Required |
| `request_timeout` | 408 | Request Timeout |
| `conflict` | 409 | Conflict |
| `gone` | 410 | Gone |
| `length_required` | 411 | Length Required |
| `precondition_failed` | 412 | Precondition Failed |
| `content_too_large` | 413 | Content Too Large |
| `uri_too_long` | 414 | URI Too Long |
| `unsupported_media_type` | 415 | Unsupported Media Type |
| `range_not_satisfiable` | 416 | Range Not Satisfiable |
| `expectation_failed` | 417 | Expectation Failed |
| `misdirected_request` | 421 | Misdirected Request |
| `unprocessable_content` | 422 | Unprocessable Content |
| `locked` | 423 | Locked |
| `failed_dependency` | 424 | Failed Dependency |
| `too_early` | 425 | Too Early |
| `upgrade_required` | 426 | Upgrade Required |
| `precondition_required` | 428 | Precondition Required |
| `too_many_requests` | 429 | Too Many Requests |
| `request_header_fields_too_large` | 431 | Request Header Fields Too Large |
| `unavailable_for_legal_reasons` | 451 | Unavailable For Legal Reasons |
| `internal_server_error` | 500 | Internal Server Error |
| `not_implemented` | 501 | Not Implemented |
| `bad_gateway` | 502 | Bad Gateway |
| `service_unavailable` | 503 | Service Unavailable |
| `gateway_timeout` | 504 | Gateway Timeout |
| `http_version_not_supported` | 505 | HTTP Version Not Supported |
| `variant_also_negotiates` | 506 | Variant Also Negotiates |
| `insufficient_storage` | 507 | Insufficient Storage |
| `loop_detected` | 508 | Loop Detected |
| `not_extended` | 510 | Not Extended |
| `network_authentication_required` | 511 | Network Authentication Required |

#### 4.11.3 `class http_received_status`

```cpp
namespace ruvia {
class http_received_status final {
public:
    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_received_status> try_from_value(integer_type value) noexcept;
    [[nodiscard]] static constexpr std::optional<http_received_status> try_parse(std::string_view digits) noexcept;
    constexpr http_received_status(http_status_code status) noexcept;
    [[nodiscard]] constexpr http_status_code status() const noexcept;
    [[nodiscard]] constexpr std::uint16_t value() const noexcept;
    friend constexpr bool operator==(http_received_status, http_received_status) = default;
};
static_assert(std::is_trivially_copyable_v<http_received_status>);
}
```

客户端收到的状态码。RFC 9110 §15 规定 `100..599` 之外的三位数码对发送方非法，但接收方 SHOULD 按 5xx 处理该响应；因此收到 `000..099` 或 `600..999` 时 `status()` 为 `500`，`value()` 保留收到的原值；`100..599` 时两者一致。

| 成员 | 语义 |
| --- | --- |
| `try_from_value(value)` | 在原始整数域检查 `0..999`（负数或更大返回 `std::nullopt`），再按上述规则构造。 |
| `try_parse(digits)` | 解析 HTTP/1 状态行的 `status-code = 3DIGIT` 或 HTTP/2 `:status` 值：恰好三个 ASCII 数字，不允许符号或空格；否则 `std::nullopt`。 |
| `http_received_status(http_status_code)` | 隐式构造，`status()` 与 `value()` 一致。 |
| `status()` | 处理该响应时采用的状态。 |
| `value()` | 收到的三位数码。 |
| `operator==` | 两个成员都相等时相等。 |

#### 4.11.4 自由函数

```cpp
namespace ruvia {
[[nodiscard]] inline constexpr std::string_view http_reason_phrase(http_status_code status) noexcept;
[[nodiscard]] inline constexpr bool is_http_redirect_status(http_status_code status) noexcept;
}
```

| 函数 | 语义 |
| --- | --- |
| `http_reason_phrase(status)` | RFC 9112 reason-phrase 是可选的 HTTP/1 展示文本，不是响应语义。`http_status` 中的稳定码返回上表的常规短语（静态存储）；临时、扩展与未分配的码返回空视图。 |
| `is_http_redirect_status(status)` | 客户端可跟随 `Location` 的重定向码：301、302、303、307、308（RFC 9110 §15.4.2–15.4.4、§15.4.8、§15.4.9）。 |

---

### 4.12 `"http_message.h"`
<a id="api-http-message-h"></a>

HTTP 消息控制数据的枚举：协议版本、scheme 与方法分类，以及按精确线上令牌判断的方法属性。源码：[`ruvia-http/include/http_message.h`](../ruvia-http/include/http_message.h)

#### 4.12.1 枚举

```cpp
namespace ruvia {
enum class http_protocol_version : std::uint8_t {
    http10,
    http11,
    http2,
    http3,
};

enum class http_scheme : std::uint8_t {
    http,
    https,
};

enum class http_known_method : std::uint8_t { get,
    post,
    put,
    delete_,
    patch,
    head,
    options,
    connect,
    unknown };
}
```

协议版本是消息控制数据，不是借用的线上令牌：HTTP/1 在起始行携带它，HTTP/2 与 HTTP/3 由连接分帧确定。

| 成员 | 值 | 含义 |
| --- | --- | --- |
| `http_protocol_version::http10` | 0 | HTTP/1.0 |
| `http_protocol_version::http11` | 1 | HTTP/1.1 |
| `http_protocol_version::http2` | 2 | HTTP/2 |
| `http_protocol_version::http3` | 3 | HTTP/3 |
| `http_scheme::http` | 0 | `http` |
| `http_scheme::https` | 1 | `https` |
| `http_known_method::get` | 0 | `GET` |
| `http_known_method::post` | 1 | `POST` |
| `http_known_method::put` | 2 | `PUT` |
| `http_known_method::delete_` | 3 | `DELETE` |
| `http_known_method::patch` | 4 | `PATCH` |
| `http_known_method::head` | 5 | `HEAD` |
| `http_known_method::options` | 6 | `OPTIONS` |
| `http_known_method::connect` | 7 | `CONNECT` |
| `http_known_method::unknown` | 8 | 其他任何方法（含 `TRACE` 与扩展方法）。 |

HTTP 方法是可扩展、大小写敏感的令牌空间；`http_known_method` 只是框架固定的语义分类，从不作为线上值。

#### 4.12.2 自由函数

```cpp
namespace ruvia {
[[nodiscard]] constexpr std::uint32_t http_known_method_bit(http_known_method method) noexcept;
[[nodiscard]] http_known_method classify_http_method(std::string_view method) noexcept;
[[nodiscard]] std::string_view known_http_method_token(http_known_method method) noexcept;
[[nodiscard]] bool is_valid_http_method_token(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_safe(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_idempotent(std::string_view method) noexcept;
}
```

| 函数 | 语义 |
| --- | --- |
| `http_known_method_bit(method)` | 方法集合位掩码：`1 << 枚举值`；`unknown` 为 `0`。 |
| `classify_http_method(method)` | 按精确、大小写敏感的令牌分类；不匹配任何固定成员时返回 `unknown`（不校验令牌语法）。 |
| `known_http_method_token(method)` | 固定成员的规范令牌（静态存储）；`unknown` 返回空视图。 |
| `is_valid_http_method_token(method)` | 是否为非空 RFC 9110 `token`。 |
| `is_http_method_safe(method)` | RFC 9110 §9.2.1 安全方法：`GET`、`HEAD`、`OPTIONS`、`TRACE`。 |
| `is_http_method_idempotent(method)` | RFC 9110 §9.2.2 幂等方法：安全方法加 `PUT`、`DELETE`。未知扩展方法保守地既不安全也不幂等；`TRACE` 虽不是可路由的固定枚举成员，仍被识别。 |

---

### 4.13 `"http_content_length.h"`
<a id="api-http-content-length-h"></a>

出站 `Content-Length` 与范围尺寸的受检整数，以及接收方 `Content-Length` 字段值解析的唯一规则（所有协议版本共用）。公开 include [`"checked_integer.h"`](#api-checked-integer-h)。源码：[`ruvia-http/include/http_content_length.h`](../ruvia-http/include/http_content_length.h)

#### 4.13.1 `class http_content_length` 与解析函数

```cpp
namespace ruvia {
class http_content_length final : public checked_integer<http_content_length, std::uint64_t, /* 未指定策略类型 */> {
public:
    using checked_integer::checked_integer;
};
static_assert(std::is_trivially_copyable_v<http_content_length>);
static_assert(sizeof(http_content_length) == sizeof(std::uint64_t));

[[nodiscard]] std::optional<std::uint64_t> http_parse_content_length(std::string_view field_value) noexcept;
}
```

`http_content_length`（机制见 [`checked_integer`](#api-checked-integer-h)）：

- 值域 `0..UINT64_MAX`，在窄化前检查，负数不会回绕成巨大的有限长度；非法值抛 `std::invalid_argument("HTTP Content-Length must be in 0..18446744073709551615")`，无异常探测用 `try_from_value()`。
- 整数到 `http_content_length` 隐式转换，非负整数可直接填入；不接受浮点与枚举，不要预先转成无符号数；用 `value()` 解包。默认构造为 `0`。
- 出站长度与范围尺寸统一使用它，例如 [`http_request_content_plan::known_length()` / `streaming()`](#api-http-request-content-plan-h)（可选位置保留 `std::optional`）、[`resolve_http_byte_range_set()`](#api-http-range-h)、[`http_selected_representation_metadata::length_`](#api-http-representation-h)、[`http_response::file_body()` 等](#api-http-response-h)。已解析或已计算的协议长度仍以 `std::uint64_t` 返回。

`http_parse_content_length(field_value)`：解析接收到的 `Content-Length` 字段值（RFC 9110 §8.6）。合并重复字段行后可能是逗号分隔的十进制长度列表，所有成员必须一致；空成员、非数字、溢出或不一致的长度返回 `std::nullopt`，否则返回该长度。

---

### 4.14 `"http_field_section_budget.h"`
<a id="api-http-field-section-budget-h"></a>

HTTP 字段区段（头与 Trailer、请求与响应、接收与生成）的唯一字节与行数计费规则。公开 include [`"http_protocol.h"`](#api-http-protocol-h)（上限常量）。源码：[`ruvia-http/include/http_field_section_budget.h`](../ruvia-http/include/http_field_section_budget.h)

#### 4.14.1 常量、`class http_field_section_budget` 与自由函数

```cpp
namespace ruvia {
inline constexpr std::size_t http_field_line_overhead = 4;

class http_field_section_budget final {
public:
    [[nodiscard]] constexpr bool add(std::size_t bytes_value) noexcept;
    [[nodiscard]] constexpr bool add_field(std::size_t name_bytes, std::size_t value_bytes) noexcept;
    [[nodiscard]] constexpr bool add_field(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] constexpr std::size_t bytes() const noexcept;
};

[[nodiscard]] constexpr http_field_section_budget http_trailer_section_budget() noexcept;
}
```

规则：区段必须容纳于 [`max_http_header_bytes`](#api-http-protocol-h)（64 KiB），每个字段计 `name.size() + value.size() + http_field_line_overhead` 字节（与接收时的可选空白无关），且最多 [`max_http_header_fields`](#api-http-protocol-h)（256）个字段。HTTP/2 头列表是有意的例外：按 RFC 9113 §6.5.2 `SETTINGS_MAX_HEADER_LIST_SIZE` 的计法（名称、值加每字段 32 字节，含伪字段）对同一上限计费。该规则覆盖请求 Trailer 收集、解析后的请求构造、客户端请求写出、响应头与 Trailer 输出以及 WebSocket 握手。

| 符号 | 语义 |
| --- | --- |
| `http_field_line_overhead` | `4`：字段在名称与值之外的开销，即规范化 HTTP/1 字段行 `name: value\r\n` 中的 `": "` 与 CRLF。 |
| 默认构造（隐式） | 已计 0 字节、0 字段。 |
| `add(bytes_value)` | 计入固定头部字节（起始行、结尾空行等），不计字段数。超出字节上限返回 `false`。 |
| `add_field(name_bytes, value_bytes)` / `add_field(name, value)` | 计入一个字段；字段数已达上限或字节超限（含溢出）返回 `false`。 |
| `bytes()` | 已计字节数。 |
| `http_trailer_section_budget()` | Trailer 区段在第一个字段之前的预算：与头部一样预先计入结尾空行（2 字节），因此写出方生成的每个区段也满足解码器对完整编码区段的上限。 |

失败的计费不改变预算，调用方可选择丢弃该字段或拒绝整个区段。

---

### 4.15 `"http_date.h"`
<a id="api-http-date-h"></a>

HTTP-date 的格式化与解析，用于 `Date`、`Expires`、`Last-Modified`、`If-Modified-Since` 等字段。源码：[`ruvia-http/include/http_date.h`](../ruvia-http/include/http_date.h)

#### 4.15.1 常量与自由函数

```cpp
namespace ruvia {
inline constexpr std::size_t http_imf_fixdate_size = 29;
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `http_imf_fixdate_size` | `29`：IMF-fixdate 的线上宽度，例如 `Sun, 06 Nov 1994 08:49:37 GMT`。 |
| `format_http_date(value)` | 把 UTC 时间戳格式化为 IMF-fixdate（RFC 9110 §5.6.7）；结果是恰好 29 字节、不含结尾 NUL 的定长数组。时间戳无法表示（年份不在 `0000..9999`）时返回 `std::nullopt`。不依赖 C 运行库的时区设置。 |
| `parse_http_date(value)` | 解析 HTTP-date 的三种形式：IMF-fixdate、过时的 RFC 850 格式与 asctime 格式（RFC 9110 §5.6.7，原 RFC 7231 §7.1.1.1）。格式错误返回 `std::nullopt`。 |

## 5. 首部与请求模型

### 5.1 `"http_header.h"`
<a id="api-http-header-h"></a>

HTTP 字段行语法（RFC 9110 §5）的唯一所有者：ASCII 大小写工具、token / field-content 字节类、规范化名称与值谓词、引号感知的列表与参数遍历、拥有型字段 `http_header`、借用型字段视图、统一的只读首部参数 `http_header_range_view`、可编辑首部参数 `editable_http_headers`，以及“首行”与“三态唯一”两种接收字段查找。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)（提供 `borrowed_text`、`borrowed_span` 与 `RUVIA_LIFETIMEBOUND`）。源码：[`ruvia-http/include/http_header.h`](../ruvia-http/include/http_header.h)

#### 5.1.1 ASCII 工具与字段字节类

```cpp
namespace ruvia {

class http_response;
class http_response_headers;

[[nodiscard]] constexpr unsigned char http_ascii_to_lower(unsigned char c) noexcept;
[[nodiscard]] constexpr bool http_ascii_equals_ignore_case(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] constexpr bool http_ascii_starts_with_ignore_case(std::string_view text, std::string_view prefix) noexcept;
[[nodiscard]] constexpr bool http_ascii_ends_with_ignore_case(std::string_view text, std::string_view suffix) noexcept;
[[nodiscard]] inline std::string http_ascii_lowercase(std::string_view text);

[[nodiscard]] constexpr bool is_http_ows_char(char c) noexcept;
[[nodiscard]] constexpr std::string_view http_trim_ows(borrowed_text text) noexcept;

inline constexpr std::array<bool, 256> http_token_char_table = /* ... */;
inline constexpr std::array<bool, 256> http_field_value_char_table = /* ... */;

[[nodiscard]] constexpr bool is_http_token_char(unsigned char c) noexcept;
[[nodiscard]] constexpr bool is_http_field_value_char(unsigned char c) noexcept;
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept;
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept;
[[nodiscard]] inline bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] inline bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept;

}  // namespace ruvia
```

`http_response`、`http_response_headers` 仅为前置声明，定义见 [`"http_response.h"`](#api-http-response-h) 与 [`"http_response_headers.h"`](#api-http-response-headers-h)。

| 符号 | 语义 |
| --- | --- |
| `http_ascii_to_lower(c)` | 仅把 `A`–`Z` 转为小写，其余字节原样返回。 |
| `http_ascii_equals_ignore_case(left, right)` | 长度相同且逐字节 ASCII 不区分大小写相等时为 `true`；非 ASCII 字节按原值比较。字段名匹配统一使用此函数。 |
| `http_ascii_starts_with_ignore_case` / `http_ascii_ends_with_ignore_case` | 以同一 ASCII 规则判断前缀 / 后缀；`prefix` / `suffix` 长于 `text` 时为 `false`。 |
| `http_ascii_lowercase(text)` | 返回 ASCII 字母小写化后的拥有型副本，其他字节保留；可能抛 `std::bad_alloc`。 |
| `is_http_ows_char(c)` | OWS / BWS 字节：仅 SP 与 HTAB（RFC 9110 §5.6.3）。 |
| `http_trim_ows(text)` | 只去除两端 SP / HTAB；结果借用调用方输入（`borrowed_text` 拒绝临时拥有型字符串，见 [`"borrowed_text.h"`](#api-borrowed-text-h)）。 |
| `http_token_char_table` | 256 项 token 字节表：`0-9`、`A-Z`、`a-z` 与 ``!#$%&'*+-.^_`\|~``。 |
| `http_field_value_char_table` | 256 项 field-content 字节表：HTAB、`0x20`–`0x7E`、obs-text `0x80`–`0xFF`；CR、LF、NUL、其他控制字节与 DEL 排除。 |
| `is_http_token_char(c)` / `is_http_field_value_char(c)` | 对上述两表的单字节查表。 |
| `http_token_prefix_size(value)` | 返回第一个非 token 字节的下标；全部为 token 时返回 `value.size()`。 |
| `is_valid_http_field_value_bytes(value)` | 只检查字节集合（全部为 field-content 字节）；不检查首尾 OWS。空值为 `true`。 |
| `is_valid_http_header_name(name)` | `field-name = token`：非空且全部为 token 字节。 |
| `is_valid_http_header_value(value)` | 规范化字段值：field-content 字节且首尾不是 SP / HTAB；空值合法。接受字段行 OWS 的线格式解析器须先裁剪 OWS 再调用此谓词。 |
| `is_valid_http_response_field(name, value)` | 见下文。 |

`is_valid_http_response_field(name, value)` 校验单个出站响应字段，返回 `bool`、`noexcept`、不分配内存：检查名称 token 与规范化值语法，并复用响应发送方对已知字段 `Content-Type`、`Content-Encoding`、`Trailer` 的语义校验。它不应用请求专属规则（`Origin`、CORS 请求字段、`Expect`）或请求托管字段黑名单，不用于接收方校验，也不替代响应写出入口的版本相关连接 / 分帧检查（如 `Connection`、`Upgrade`、`TE`）、重复字段检查与完整消息约束；应用禁改首部政策由调用方保留。返回 `true` 不表示该字段在任意响应上下文中都可发送。请求侧对应谓词见 [`is_valid_http_request_application_field`](#api-http-request-h)。

#### 5.1.2 引号感知的列表与参数遍历

```cpp
namespace ruvia {

[[nodiscard]] constexpr std::size_t http_find_unquoted_delimiter(std::string_view value, std::size_t start, char delimiter) noexcept;

template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(std::string_view value, visitor_type&& visitor);

template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(std::string_view value, visitor_type&& visitor);

[[nodiscard]] constexpr std::string_view http_trim_quoted_field_value(borrowed_text text) noexcept;

class http_quoted_string_cursor final {
public:
    explicit constexpr http_quoted_string_cursor(borrowed_text input) noexcept;
    [[nodiscard]] constexpr bool next(unsigned char& out) noexcept;
    [[nodiscard]] constexpr bool closed() const noexcept;
    [[nodiscard]] constexpr std::size_t consumed() const noexcept;
};

[[nodiscard]] constexpr std::size_t http_quoted_string_size(std::string_view input) noexcept;
[[nodiscard]] constexpr bool http_is_quoted_string(std::string_view value) noexcept;

class http_field_name_list final {
public:
    explicit http_field_name_list(borrowed_text field) noexcept;
    [[nodiscard]] std::optional<std::string_view> next() noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

}  // namespace ruvia
```

| 符号 | 语义 |
| --- | --- |
| `http_find_unquoted_delimiter(value, start, delimiter)` | 返回 `start` 及其后第一个位于 RFC 9110 §5.6.4 quoted-string 之外的 `delimiter` 下标；没有时返回 `value.size()`（`start >= value.size()` 时同样返回 `value.size()`）。引号状态从 `value` 开头扫描；quoted-pair（如 `\"`）不结束字符串。这是唯一的引号感知分隔符扫描，下面所有遍历器均基于它。 |
| `http_visit_comma_separated_quoted_field_items(value, visitor)` | 按逗号切分字段成员，引号内（含转义引号）的逗号视为数据。每个成员先裁剪 OWS 再以 `std::string_view` 传给 `visitor`；空成员也会报告（对分帧敏感的调用方可据此拒绝）。`visitor` 返回 `false` 时停止遍历。视图借用输入字段。 |
| `http_visit_semicolon_parameters_quoted_field(value, visitor)` | 按分号切分参数，引号内分号视为数据；每段裁剪 OWS 后按第一个 `=` 拆分，名称与值再各自裁剪 OWS，以 `visitor(name, value)` 调用；无 `=` 的段被跳过。值保留引号。`visitor` 返回 `false` 时停止。不得用于 `Cookie`：RFC 6265 中 `"` 无特殊含义（Cookie 解析见 [`"http_cookie.h"`](#api-http-cookie-h)）。视图借用输入。 |
| `http_trim_quoted_field_value(text)` | 若两端恰为一对 DQUOTE 则去除，不反转义 quoted-pair；结果借用输入。 |
| `http_quoted_string_cursor` | 无分配的 quoted-string 读取器。输入须以开引号开始，否则构造即进入失败态。`next(out)` 每次校验并解码一个内容字节（qdtext 或 quoted-pair 的被转义字节，须为 HTAB、SP、VCHAR 或 obs-text），成功时写入 `out` 并返回 `true`；遇闭引号或畸形输入（缺闭引号、`\` 位于末尾、非法字节）返回 `false`。`closed()` 区分两种结束：`true` 表示正常遇到闭引号。`consumed()` 为已读取字节数（含两侧引号）。闭引号之后的字节永不读取。 |
| `http_quoted_string_size(input)` | `input` 开头的完整 quoted-string 的字节长度（含引号）；开头不是合法 quoted-string 时为 `0`。 |
| `http_is_quoted_string(value)` | `value` 整体恰为一个合法 quoted-string 时为 `true`。 |
| `http_field_name_list` | 无分配迭代逗号分隔的字段名列表（如 `Connection`、`Trailer`、`Vary` 的名称列表）。成员裁剪 OWS，空成员（包括整个列表为空）被忽略；返回的视图借用输入。反复调用 `next()` 直至返回 `std::nullopt`，再用 `valid()` 区分正常结束（`true`）与遇到非 token 成员（`false`）；未排空前 `valid()` 为 `false`。此类按普通逗号切分，不感知引号（字段名列表不含 quoted-string）。 |

#### 5.1.3 `class http_header` 与 `class http_header_view`

```cpp
namespace ruvia {

class http_header final {
public:
    http_header(std::string_view name, std::string_view value);
    http_header(const http_header& other);
    http_header(http_header&& other) noexcept;
    http_header& operator=(const http_header& other);
    http_header& operator=(http_header&& other) noexcept;
    ~http_header() noexcept;

    [[nodiscard]] static http_header copy_with_lowercase_name(std::string_view name, std::string_view value);

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view value() const&& = delete;
};

class http_header_view final {
public:
    constexpr http_header_view() noexcept = default;
    constexpr http_header_view(std::string_view name, std::string_view value) noexcept;

    template <typename name_type, typename value_type>
        requires(/* name_type 或 value_type 是临时拥有型字符串 */)
    http_header_view(name_type&& name, value_type&& value) = delete;

    [[nodiscard]] constexpr std::string_view name() const noexcept;
    [[nodiscard]] constexpr std::string_view value() const noexcept;
};

}  // namespace ruvia
```

`http_header` 是协议解析器与事件返回的拥有型字段，同一类型同时表示请求 / 响应的初始字段与 Trailer。

| 成员 | 语义 |
| --- | --- |
| `http_header(name, value)` | 把名称与值复制进一次分配的连续存储（两者皆空时不分配）。不校验语法。`name.size() + value.size()` 溢出 `size_t` 时抛 `std::length_error`；分配失败抛 `std::bad_alloc`。 |
| 拷贝构造 / 拷贝赋值 | 拥有型对象复制字段存储；库内部的静态借用型对象拷贝后仍借用原文。拷贝赋值先完整构造副本再交换，失败时目标保持原值。 |
| 移动构造 / 移动赋值 | 转移存储、不分配；源对象变为空（名称与值均为空视图）。移动赋值先提交新字段状态，再释放旧存储。 |
| `copy_with_lowercase_name(name, value)` | 与构造函数相同（一次分配），并在复制时把名称 ASCII 小写化；用于需要规范化字段名的拥有型边界。异常同构造函数。 |
| `name()` / `value()` | 借用本对象存储的视图，有效至对象被修改、移动或销毁。右值重载 `= delete`：禁止从临时 `http_header` 取得会立即悬垂的视图。 |

`http_header_view` 是非拥有型字段视图，按值保存两个 `std::string_view`。默认构造得到空名称与空值。删除的模板构造函数在任一实参为临时拥有型字符串（如 `std::string` 右值）时参与重载并被选中，从而在编译期拒绝必然悬垂的视图；字符串字面量、`std::string_view` 与拥有型字符串左值可正常构造。视图不校验内容。

#### 5.1.4 字段集合概念、`borrowed_http_headers` 与 `unique_lookup`

```cpp
namespace ruvia {

using borrowed_http_headers = borrowed_span<http_header_view>;

template <typename field_type>
concept http_header_field = requires(const std::remove_reference_t<field_type>& field) {
    { field.name() } -> std::convertible_to<std::string_view>;
    { field.value() } -> std::convertible_to<std::string_view>;
};

template <typename range_type>
concept http_header_field_range = std::ranges::forward_range<const std::remove_cvref_t<range_type>>
    && std::ranges::sized_range<const std::remove_cvref_t<range_type>>
    && http_header_field<std::ranges::range_reference_t<const std::remove_cvref_t<range_type>>>
    && requires(std::ranges::iterator_t<const std::remove_cvref_t<range_type>> cursor) {
           requires std::is_reference_v<decltype(*cursor)> || requires {
               { (*cursor).name() } -> std::convertible_to<std::string_view>;
               { (*cursor).value() } -> std::convertible_to<std::string_view>;
           };
       };

enum class unique_lookup_status : unsigned char { absent, found, repeated };

template <typename value_type>
struct unique_lookup final {
    unique_lookup_status status_{unique_lookup_status::absent};
    value_type value_{};
    constexpr void add_occurrence(value_type value) noexcept;
};

using http_unique_field_lookup = unique_lookup<std::string_view>;

class http_header_range_view;

template <typename message_type>
concept http_header_field_message = !http_header_field_range<message_type>
    && requires(const std::remove_cvref_t<message_type>& message) {
           { message.headers() } -> http_header_field_range;
           requires std::ranges::borrowed_range<decltype(message.headers())>;
       };

template <typename source_type>
concept http_header_field_source = std::same_as<std::remove_cvref_t<source_type>, http_header_range_view>
    || http_header_field_range<source_type> || http_header_field_message<source_type>;

}  // namespace ruvia
```

| 符号 | 语义 |
| --- | --- |
| `borrowed_http_headers` | 零拷贝首部视图 span，供请求头描述、响应、握手与客户端选项结构体持有。与 `borrowed_text` 相同，接受借用 span 和连续拥有型容器左值，拒绝临时拥有型容器（`std::vector`、`std::array`）与 `std::initializer_list`，因为其底层存储会在结构体被消费前到期。大小与 `std::span<const http_header_view>` 相同。借用规则见 [`"borrowed_text.h"`](#api-borrowed-text-h)。 |
| `http_header_field` | `name()` 与 `value()` 均可转换为 `std::string_view`，且字段文本必须在访问器返回后仍有效；编译期拒绝返回临时 `std::basic_string`。 |
| `http_header_field_range` | 元素满足 `http_header_field` 的有大小前向范围。迭代器按值产出字段时，`name()` 和 `value()` 必须能在右值上调用，且返回不依赖临时字段对象寿命的文本。 |
| `http_header_field_message` | 本身不是字段范围、但通过 `headers()` 返回字段范围左值引用或借用范围的消息（如 `http_request`、`http_response`、`http_client_response_head`）。拒绝按值返回拥有型容器的消息，即使消息本身是左值。 |
| `http_header_field_source` | 可读取字段的一切来源：字段范围、上述消息，或已绑定的 `http_header_range_view`。 |
| `unique_lookup<T>` | 在允许重名的集合中查找单个名称的结果（接收字段、JSON 对象成员、SQL 结果列共用此模板）。`value_` 仅在 `status_ == found` 时有意义；视图或指针类型的 `value_` 借用被搜索的存储。 |
| `add_occurrence(value)` | 状态迁移 `absent → found(value) → repeated`（清空 `value_`）；`repeated` 为终态。 |
| `http_unique_field_lookup` | HTTP 字段查找所用实例 `unique_lookup<std::string_view>`。 |

| `unique_lookup_status` 成员（底层 `unsigned char`） | 含义 |
| --- | --- |
| `absent` (0) | 没有该名称。 |
| `found` (1) | 恰好出现一次，`value_` 为其值。 |
| `repeated` (2) | 出现两次及以上；不解析为其中任何一次出现。 |

#### 5.1.5 `class http_header_range_view`

```cpp
namespace ruvia {

class http_header_range_view final {
public:
    using accessor_type = http_header_view (*)(const void*, std::size_t) noexcept;

    constexpr http_header_range_view() noexcept = default;
    template <http_header_field_range range_type>
    constexpr http_header_range_view(const range_type& fields) noexcept;
    template <http_header_field_message message_type>
    constexpr http_header_range_view(const message_type& message) noexcept;

    http_header_range_view(const http_header_range_view&) = delete;
    http_header_range_view& operator=(const http_header_range_view&) = delete;

    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr bool empty() const noexcept;
    [[nodiscard]] constexpr http_header_view operator[](std::size_t index) const noexcept;

    class iterator final {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = http_header_view;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;
        constexpr iterator(const void* data, accessor_type at, std::size_t index) noexcept;
        [[nodiscard]] constexpr http_header_view operator*() const noexcept;
        constexpr iterator& operator++() noexcept;
        constexpr iterator operator++(int) noexcept;
        [[nodiscard]] friend constexpr bool operator==(const iterator& left, const iterator& right) noexcept;
    };

    [[nodiscard]] constexpr iterator begin() const noexcept;
    [[nodiscard]] constexpr iterator end() const noexcept;
};

}  // namespace ruvia

namespace std::ranges {
template <>
inline constexpr bool enable_borrowed_range<ruvia::http_header_range_view> = true;
}
```

唯一的只读首部参数类型：读取字段的函数只需声明一次 `const http_header_range_view&` 形参，即可接受所有首部容器与消息。它是对字段范围或消息的类型擦除视图，借用来源直至接收它的调用返回；与 `argument_list` 类似，它也可绑定临时容器，该存储在整个调用表达式内有效。拷贝构造与拷贝赋值被删除（同时抑制移动），不能通过复制 / 移动返回、按值捕获或存入成员。它仍满足 `std::ranges::borrowed_range`：迭代器独立保存数据指针、访问器与下标，不依赖包装对象；不延长底层字段存储的寿命。显式把临时 owner 包成该视图也不能让借用逃逸到完整表达式之后。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 空视图（`size() == 0`）。 |
| 范围构造 / 消息构造 | 连续范围直接借用数据；非连续范围借用容器并内联保存迭代游标（迭代器须可平凡复制且不超过四个指针大小）。消息返回非连续范围时必须返回左值引用。不复制字段或文本。 |
| `size()` / `empty()` | 字段行数 / 是否为空。 |
| `operator[](index)` | 返回第 `index` 行，前置条件 `index < size()`；连续范围常数时间，前向范围从起点推进。 |
| `iterator` | 按值产出借用 `http_header_view`，副本独立推进；全范围遍历始终线性且零分配。 |
| `accessor_type` | 元素访问函数指针类型，仅为迭代器构造签名所需。 |

#### 5.1.6 接收字段查找

```cpp
namespace ruvia {

template <http_header_field_source source_type>
    requires (std::is_lvalue_reference_v<source_type&&>
              || std::ranges::borrowed_range<source_type>
              || requires(source_type&& source) {
                     { std::forward<source_type>(source).headers() } -> std::ranges::borrowed_range;
                 })
[[nodiscard]] constexpr http_unique_field_lookup find_unique_http_header(source_type&& fields, std::string_view name) noexcept;

template <http_header_field_source source_type>
    requires (std::is_lvalue_reference_v<source_type&&>
              || std::ranges::borrowed_range<source_type>
              || requires(source_type&& source) {
                     { std::forward<source_type>(source).headers() } -> std::ranges::borrowed_range;
                 })
[[nodiscard]] constexpr std::optional<std::string_view> find_first_http_header(source_type&& fields, std::string_view name) noexcept;

}  // namespace ruvia
```

上面将实现中的内部约束展开为等价的公开条件；内部约束的名称本身不是用户契约。两者都按 `http_ascii_equals_ignore_case` 匹配字段名，接受满足所示借用条件的 `http_header_field_source`（消息自动经 `headers()` 转发），不复制值；返回的视图借用来源的字段存储，而非 `fields` 包装对象。

- `find_unique_http_header`：三态查找，在第二次出现处停止扫描并返回 `repeated`。
- `find_first_http_header`：返回第一条匹配字段行的值，无匹配时为 `std::nullopt`。

| 来源 | 接受性与寿命 |
| --- | --- |
| 拥有型字段容器 / 消息左值（含 `const` 左值） | 接受；字段存储必须覆盖返回视图的使用期。 |
| 临时 `std::span`、`borrowed_http_headers`、`http_header_range_view` | 接受；均为借用范围，包装对象销毁不影响迭代器或返回视图，底层 owner 仍须存活。 |
| 临时消息，其按原值类别调用的 `headers()` 返回 `std::ranges::borrowed_range` | 接受；返回字段仍借用 `headers()` 所指存储，该存储必须覆盖结果使用期。 |
| 临时拥有型字段容器 / 消息（含 `std::move(owner)`，且不满足上述借用条件） | 约束不满足，编译期拒绝；没有可退回 `const source_type&` 的宽松重载。 |

```cpp
std::vector<ruvia::http_header> fields;  // owner 必须覆盖查找结果的使用期
auto unique = ruvia::find_unique_http_header(fields, "Content-Type");
auto first = ruvia::find_first_http_header(std::span<const ruvia::http_header>(fields), "Content-Type");
auto via_view = ruvia::find_first_http_header(ruvia::http_header_range_view(fields), "Content-Type");

struct borrowed_message {
    std::span<const ruvia::http_header> fields_;
    auto headers() const noexcept { return fields_; }
};
auto via_message = ruvia::find_unique_http_header(borrowed_message{fields}, "Content-Type");
// ruvia::find_first_http_header(std::move(fields), "Content-Type");  // 编译期拒绝
// ruvia::find_unique_http_header(std::vector<ruvia::http_header>{}, "Content-Type");  // 编译期拒绝
```

这些调用均保留零拷贝：需要在底层字段失效后保存结果时，由调用方显式复制文本；不要通过构造 span 或类型擦除视图绕过 owner 寿命。

接收字段单值查找统一规则：

- 接收侧单值 getter 遇重复字段一律取第一行（与 `http::HeaderMap::get`、Pingora `get_header`、Go `Header.Get` 一致）：`find_first_http_header`、[`http_request::header` / `cookie`](#api-http-request-h)、[`http_request_trailers::field`](#api-http-request-h)、[`http_client_response_head::header`](#api-http-client-response-head-h)。
- 不得择一的字段（`Location`、`Content-Type`、会话 ID 等）改用三态查找：`find_unique_http_header`、[`http_request::unique_header`](#api-http-request-h)、[`http_client_response_head::unique_header`](#api-http-client-response-head-h)。三者均返回 `http_unique_field_lookup`，仅 `found` 时 `value_` 有意义。
- [`http_request::last_raw_query_value`](#api-http-request-h) 取最后一个同名查询参数，不属此规则。
- `ruvia-web` 中遵循同一规则的访问器见 [ruvia-web](ruvia-web.md)。

#### 5.1.7 `class editable_http_headers`

```cpp
namespace ruvia {

class editable_http_headers final {
public:
    editable_http_headers(std::vector<http_header>& fields) noexcept;
    editable_http_headers(http_response_headers& block) noexcept;
    editable_http_headers(http_response& response) noexcept;
    template <http_header_field_range range_type>
        requires requires(range_type& fields, std::string_view name, std::string_view value) {
            fields.set(name, value);
            fields.append(name, value);
            { fields.remove(name) } noexcept;
        }
    editable_http_headers(range_type& fields) noexcept;

    [[nodiscard]] http_header_range_view fields() const noexcept;
    void remove(std::string_view name) noexcept;
    void remove_if(bool (*matches)(std::string_view field_name) noexcept) noexcept;
    void set(std::string_view name, std::string_view value);
    void append(std::string_view name, std::string_view value);

    template <std::invocable<editable_http_headers> edit_type>
    void transact(edit_type edit, std::size_t additional_fields = 0);
};

}  // namespace ruvia
```

唯一的可变首部参数：改写字段的函数只需声明一次 `editable_http_headers` 形参，即可接受 `std::vector<http_header>`、`http_response_headers` 或 `http_response` 左值（隐式转换，不接受右值）。容器借用至接收该参数的调用返回。目标为响应首部块或响应时沿用其字段规则：名称与值经校验，被拒绝时抛 `std::invalid_argument`（规则见 [`"http_response_headers.h"`](#api-http-response-headers-h)）；`std::vector<http_header>` 目标不校验。下列每项操作要么完整生效、要么容器不变；删除操作不会失败。

- **自定义候选容器**：可直接绑定具备 `set`、`append`、`remove` 的字段范围，不经 `vector<http_header>` 往返。操作沿用容器自身的校验与字段位置规则；每次变更须原子，`remove` 不抛且接受借用名称。`transact()` 直接编辑调用方候选容器，不隐式深拷贝，不承诺多次写入失败时回滚已完成的写入。

| 成员 | 语义 |
| --- | --- |
| `fields()` | 当前字段行的只读视图；之后任何修改都会使其失效。 |
| `remove(name)` | 删除所有名称（不区分大小写）为 `name` 的行；对响应目标，任何字段都不可能携带的名称（非 token 等）不删除任何内容。`name` 不得借用本容器。 |
| `remove_if(matches)` | 删除名称满足 `matches` 的所有行。 |
| `set(name, value)` | 保证 `name` 恰好剩一行：替换第一条既有行并删除其后的同名行，无既有行时追加。`name` / `value` 可借用被替换的行（新字段先构造）。 |
| `append(name, value)` | 再追加一行，保留既有同名行。 |
| `transact(edit, additional_fields = 0)` | 以一个首部事务执行多次写入：`edit` 以 `editable_http_headers` 调用。目标为 `http_response` 时，在其首部块的候选副本（预留 `additional_fields` 行余量）上编辑，`edit` 正常返回后才无失败地发布，因此抛出的写入使响应保持不变；目标为 `std::vector<http_header>` 或 `http_response_headers` 时，它们本身就是调用方自己的候选存储，直接编辑（中途抛出时已完成的写入保留）。`edit` 的异常原样传播。 |

### 5.2 `"http_request_head_view.h"`
<a id="api-http-request-head-view-h"></a>

待发送请求头的唯一借用描述 `http_request_head_view`，以及请求期望、字段来源与请求头种类枚举；HTTP/1、HTTP/2、HTTP/3 的请求写出 / 提交入口与 CONNECT-UDP 校验器都消费它。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_request_content_plan.h"`](#api-http-request-content-plan-h)。源码：[`ruvia-http/include/http_request_head_view.h`](../ruvia-http/include/http_request_head_view.h)

#### 5.2.1 枚举

```cpp
namespace ruvia {

enum class http_client_request_expectation : std::uint8_t {
    none,
    continue_,
};

enum class http_request_field_source : std::uint8_t { generated, forwarded };

enum class http_request_head_kind : std::uint8_t {
    regular,
    connect,
    extended_connect,
    protocol_upgrade,
};

}  // namespace ruvia
```

`http_client_request_expectation`：与传输无关的发送方策略，覆盖 RFC 9110 唯一标准化的请求期望。所有 writer 自行生成 `Expect` 字段，调用方无法造成线格式与状态机不一致。

| 成员 | 含义 |
| --- | --- |
| `none` (0) | 不发送 `Expect`。 |
| `continue_` (1) | 发送 `Expect: 100-continue` 并在收到 `100 (Continue)` 或最终响应前暂缓内容；仅在请求有内容时有效（见 [`http_client_expectation_is_valid`](#api-http-request-expectation-h)）。 |

`http_request_field_source`：区分转发收到的字段与新生成字段（RFC 9110 §2.2）。

| 成员 | 含义 |
| --- | --- |
| `generated` (0) | 缺省。按新生成字段的严格发送方语义校验。配置生成或重写的字段必须按此规则校验，不能借 `forwarded` 绕过配置边界。 |
| `forwarded` (1) | 转发已接收请求的字段，只切换值语义（见下）。 |

`forwarded` 的契约：

- 只切换值语义：`Content-Type`、`Origin`、`Access-Control-Request-Method`、`Access-Control-Request-Headers` 原样保留，由最终应用解释；`Content-Encoding` 与 HTTP/1 `Upgrade` 容忍合法列表空成员（包括空列表），不要求重建列表，非空成员仍须符合 coding token / upgrade protocol 语法；未知但合法的 content coding 不因本地不支持解码而被丢弃。依据 [RFC 9110 §2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.2)，generate 不包括 forward；[§5.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.1) 区分发送方新生成列表与接收方空成员容忍义务。OPTIONS 携带内容须有 `Content-Type` 是 §9.3.7 的生成要求，转发时不补造该字段；TRACE 请求仍禁止内容。
- 不改变：字段名 / 值字节安全与 CRLF 注入防护、字段节大小、方法 / 目标、Host / authority 一致性、内容分帧、`Content-Length` / `Expect` 所有权、singleton 次数。即使接收端容忍重复 `User-Agent` 或条件日期，转发 writer 也不切换整套接收方规则、不额外放行这些重复字段。
- 非法枚举值被拒绝：HTTP/1 返回 `invalid_header`，HTTP/2 与 HTTP/3 返回 `invalid_message`，不写出请求、不占用新流。
- 代理仍负责按 [RFC 9110 §7.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1) 移除 / 重建逐跳字段；`Connection`、`TE`、`Trailer` 及协议专属限制不放宽：HTTP/1 `Upgrade` 仍须有正确的 `Connection: Upgrade`，HTTP/2 继续禁止 `Connection`、`Upgrade` 等逐跳字段（RFC 9113 §8.2.2），CONNECT-UDP 的 `Capsule-Protocol` 与禁止表示 / 分帧字段规则仍生效。新生成的专用 WebSocket 握手与 PUSH_PROMISE 仍按 `generated` 检查。
- 不要把收到的字段当作新配置字段逐项送入 [`is_valid_http_request_application_field`](#api-http-request-h)。

`http_request_head_kind`：仅由方法与 `protocol_` 推导出的请求头种类。

| 成员 | 含义 |
| --- | --- |
| `regular` (0) | 非 CONNECT 方法且无 protocol。 |
| `connect` (1) | 无 protocol 的 CONNECT：到 `authority_` 的隧道（RFC 9110 §9.3.6）；无 scheme、无 target。 |
| `extended_connect` (2) | 带 protocol 的 CONNECT：Extended CONNECT（RFC 8441、RFC 9220），仅 HTTP/2 与 HTTP/3 承载。 |
| `protocol_upgrade` (3) | 带 protocol 的 GET：该协议的 HTTP/1.1 Upgrade 请求；HTTP/1 writer 用它生成 connect-udp 请求（RFC 9298 §3.2）。 |

#### 5.2.2 `struct http_request_head_view`

```cpp
namespace ruvia {

struct http_request_head_view final {
    borrowed_text method_{"GET"};
    std::optional<borrowed_text> protocol_{};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_content_plan content_{http_request_content_plan::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};

    [[nodiscard]] constexpr std::optional<http_request_head_kind> kind() const noexcept;
};

}  // namespace ruvia
```

消费者：`prepare_http1_client_request()`（[`"http1_client_request.h"`](#api-http1-client-request-h)）、`http2_connection::submit_request_head()` / `submit_push_request()`（[`"http2_connection.h"`](#api-http2-connection-h)）、`http3_connection::submit_request_head()`（[`"http3_connection.h"`](#api-http3-connection-h)）以及 CONNECT-UDP 校验器（[`"http_connect_udp.h"`](#api-http-connect-udp-h)）。字符串字面量、`std::string_view` 与拥有型字符串左值是合法输入；拥有型字符串临时量与临时首部容器在编译期被拒绝。所有视图必须存活且不变，直到消费它的同步准备或提交调用返回。

| 字段 | 语义 |
| --- | --- |
| `method_` | 请求方法，缺省 `"GET"`。 |
| `protocol_` | Extended CONNECT 的 `:protocol`，或 HTTP/1.1 Upgrade token；缺省无。 |
| `scheme_` | 缺省 `"https"`；HTTP/2 / HTTP/3 读取。HTTP/1 对 `regular` 与 `protocol_upgrade` 请求从其 `http_origin_view` 实参取 scheme 与 authority。 |
| `authority_` | HTTP/2 / HTTP/3 读取；`connect` 请求在所有版本中都用它命名隧道目标（形如 `example.test:443`）。 |
| `target_` | origin-form（或 asterisk-form）请求目标，即 HTTP/2 / HTTP/3 的 `:path`；缺省 `"/"`。 |
| `headers_` | 借用的首部表；HTTP/1 writer 允许它与写出的头缓冲重叠。 |
| `content_` | 请求内容计划，缺省 `none()`（不发送内容分帧）；`bytes()` 借用的内容须存活且不变直到运行时发送完毕；流式与“已知长度无字节”计划表示运行时稍后写入的内容。见 [`"http_request_content_plan.h"`](#api-http-request-content-plan-h)。 |
| `expectation_` | 发送方期望策略，缺省 `none`。 |
| `field_source_` | 字段来源，缺省 `generated`。普通、CONNECT、Extended CONNECT 与 CONNECT-UDP 请求一致消费。 |
| `kind()` | 按上表推导种类；protocol 出现在 CONNECT 与 GET 以外的方法上时返回 `std::nullopt`，所有消费者都拒绝此类请求头。方法比较区分大小写。 |

`content_` 与 `expectation_` 只属于 `regular` 请求，其他种类不携带请求内容。

### 5.3 `"http_request.h"`
<a id="api-http-request-h"></a>

服务端接收请求的语义模型 `http_request`（零拷贝借用协议输入）、直接构造已解析请求的 `make_parsed_http_request`、请求 Trailer 收集器 `http_request_trailers`，以及请求托管字段与应用字段校验谓词。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_field_section_budget.h"`](#api-http-field-section-budget-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_protocol.h"`](#api-http-protocol-h)。源码：[`ruvia-http/include/http_request.h`](../ruvia-http/include/http_request.h)

#### 5.3.1 请求字段谓词与自由函数

```cpp
namespace ruvia {

[[nodiscard]] bool is_http_protocol_managed_request_field(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept;
[[nodiscard]] constexpr std::string_view http_canonical_extended_connect_protocol(borrowed_text protocol) noexcept;

[[nodiscard]] http_request make_parsed_http_request(borrowed_text method, borrowed_text target,
    std::span<const http_header_view> headers, borrowed_span<std::byte> body = {});

[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

}  // namespace ruvia
```

| 函数 | 语义 |
| --- | --- |
| `is_http_protocol_managed_request_field(name)` | HTTP 协议层在为请求分帧和传输时自行生成或解释的字段：`Host`（RFC 9110 §7.2）、消息分帧 `Content-Length`、`Transfer-Encoding`、`Trailer`（§6.5、§8.6）、`Expect`（§10.1.1）、连接管理 `Connection` 及连接专属 `Keep-Alive`、`Proxy-Connection`、`TE`、`Upgrade`（§7.6.1）。代替应用构造请求的组件拥有这些字段，不得从应用接受。名称不区分大小写。 |
| `is_valid_http_request_application_field(name, value)` | 在应用配置或请求构造边界校验单个出站请求首部，`noexcept`、不分配。检查名称 token、规范化值（RFC 9110 §5.5，拒绝首尾 SP / HTAB 与非法控制字节），拒绝上述协议托管字段（不区分大小写），并复用请求 writer 对已知字段的发送方语义校验（如 `Content-Type`、`Content-Encoding`、`Origin` 与 CORS 请求字段）；合法未知扩展及其空值允许。HTTP/1 与 HTTP/2 的 `TE` 差异已被托管字段边界排除，因此无版本参数。不保留输入或跨调用状态；字段间重复与完整消息约束由调用方及最终 writer 负责；返回 `true` 不表示整个请求可发送。不得用于响应首部（用 [`is_valid_http_response_field`](#api-http-header-h)）或接收方校验。 |
| `http_canonical_extended_connect_protocol(protocol)` | RFC 9110 §7.8 对协议名不区分大小写，而 RFC 8441 §4 与 RFC 9220 §3 注册的 `:protocol` token 为小写 `websocket`：任何大小写的 `websocket` 映射为 `"websocket"`（静态存储），其他协议原样返回并借用 `protocol`。 |
| `make_parsed_http_request(method, target, headers, body = {})` | 不经线格式解析直接构造语义请求，见 5.3.4。 |
| `is_valid_http_connect_authority(authority)` | RFC 9110 CONNECT authority-form：须有非空 host 与显式端口。 |
| `is_valid_http_origin_form_target(target)` | 无分配校验 origin-form 请求目标；服务端路由与出站 HTTP / WebSocket 客户端共用同一规则。 |

#### 5.3.2 `class http_request_trailers`

```cpp
namespace ruvia {

enum class http_request_trailer_error : unsigned char { invalid_field, forbidden_field, section_too_large };

class http_request_trailers final {
public:
    http_request_trailers() noexcept = default;
    http_request_trailers(const http_request_trailers&) = delete;
    http_request_trailers& operator=(const http_request_trailers&) = delete;

    [[nodiscard]] std::optional<http_request_trailer_error> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header_view> fields);
    template </* 临时拥有型 http_header_view 范围 */ fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header> fields);
    template </* 临时拥有型 http_header 范围 */ fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;

    [[nodiscard]] std::optional<http_request_trailer_error> append_received(std::span<const http_header> fields);
    template </* 临时拥有型 http_header 范围 */ fields_type>
    std::optional<http_request_trailer_error> append_received(fields_type&&) = delete;

    [[nodiscard]] std::vector<http_header> take_fields() && noexcept;
    [[nodiscard]] std::span<const http_header> fields() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http_header> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> field(std::string_view) const&& = delete;
};

}  // namespace ruvia
```

| `http_request_trailer_error` 成员 | 含义 |
| --- | --- |
| `invalid_field` (0) | 名称或值语法非法；`append_received` 中还包括伪首部字段（RFC 9113 §8.1、RFC 9114 §4.3）与非小写名称。 |
| `forbidden_field` (1) | `append` 时名称不允许出现在请求 Trailer 中（RFC 9110 §6.5.1）。 |
| `section_too_large` (2) | 加入后超出 Trailer 字段节的字节或行数预算。 |

`http_request_trailers` 独立于初始请求头拥有解码后的终止请求字段；驱动在报告请求体成功结束之前填充它。字段节必须符合 [`http_field_section_budget`](#api-http-field-section-budget-h) 的字节与行数规则，起点为 `http_trailer_section_budget()`。无论经哪个重载或协议加入，保留的字段名一律为小写。不可拷贝；隐式移动不可用（拷贝被删除），通常由驱动以引用传递。

| 成员 | 语义 |
| --- | --- |
| `append(name, value)` / `append(span<const http_header_view>)` / `append(span<const http_header>)` | 生成方追加，全有或全无：先对全部字段计预算并校验，任何一个失败则返回对应错误且对象不变；成功后以小写名复制保存并返回 `std::nullopt`。不允许出现在请求 Trailer 中的名称报告 `forbidden_field`。分配失败抛 `std::bad_alloc`，对象不变。 |
| `append(fields_type&&) = delete`（两个模板） | 拒绝临时拥有型字段容器（如 `std::vector<http_header>` 右值）与 `initializer_list`，避免以将要销毁的存储作为参数来源的写法；传左值或 span。 |
| `append_received(span<const http_header>)` | 吸收任意协议版本解码得到的接收 Trailer 段（HTTP/1 [`http1_chunked_body_decoder`](#api-http1-chunked-body-h)、HTTP/2 / HTTP/3 消息结束事件产出同一视图），全有或全无：每个字段必须是名称小写的合法普通字段，伪首部字段使整个段畸形并报告 `invalid_field`；所有接收字段（包括被丢弃的）都计入字节预算，超限报告 `section_too_large`；不允许出现在请求 Trailer 中的字段被丢弃而不保留（RFC 9110 §6.5.1、RFC 9112 §7.1.2），不报告 `forbidden_field`。 |
| `append_received(fields_type&&) = delete` | 同上，拒绝临时拥有型容器。 |
| `take_fields() &&` | 移出已保存字段并把预算重置为 `http_trailer_section_budget()`；仅限右值调用。 |
| `fields() const&` | 借用已保存字段，有效至下一次追加、`take_fields()` 或对象销毁；右值重载删除。 |
| `field(name) const&` | 不区分大小写，第一条匹配行优先（见 [接收字段单值查找统一规则](#api-http-header-h)）；右值重载删除。 |

#### 5.3.3 `class http_request`

```cpp
namespace ruvia {

enum class http_request_target_form : std::uint8_t {
    origin,
    absolute,
    authority,
    asterisk,
    http2,
    http3,
};

class http_request final {
public:
    http_request(const http_request&) = delete;
    http_request& operator=(const http_request&) = delete;
    http_request(http_request&&) noexcept = default;
    http_request& operator=(http_request&& other) noexcept;

    [[nodiscard]] std::string_view method() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_known_method known_method() const noexcept;
    void reset() noexcept;
    [[nodiscard]] std::string_view target() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view scheme() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view authority() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view protocol() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_request_target_form target_form() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view query_string() const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;

    [[nodiscard]] std::span<const http_header_view> headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header_view> headers() const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view body() const&& = delete;
    [[nodiscard]] std::span<const std::byte> body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::byte> body_bytes() const&& = delete;

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_unique_field_lookup unique_header(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(std::string_view raw_name) const noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;
};

}  // namespace ruvia
```

| `http_request_target_form` 成员 | 含义 |
| --- | --- |
| `origin` (0) | HTTP/1 origin-form（`/path?query`）。 |
| `absolute` (1) | HTTP/1 absolute-form（`http://host/path`）。 |
| `authority` (2) | HTTP/1 authority-form（CONNECT `host:port`）。 |
| `asterisk` (3) | HTTP/1 asterisk-form（`OPTIONS *`）。 |
| `http2` (4) | HTTP/2 请求：目标组成部分由伪首部携带，而非单个 HTTP/1 request-target token。 |
| `http3` (5) | HTTP/3 请求，同上。 |

`http_request` 拥有紧凑的首部描述符块（在堆上，不在协程帧中；协议字段数上限与对象布局无关），字段文本与正文仍借用协议输入；对象只由解析器或 `make_parsed_http_request` 产生（无公开默认构造）。不可拷贝；移动构造转移描述符块；移动赋值先整体接管新对象再释放旧存储，不分配。所有返回的视图借用协议输入缓冲（或 `make_parsed_http_request` 的调用方存储），并在 `headers()` / `body()` 等左值限定访问器上额外受本对象生存期约束。

| 成员 | 语义 |
| --- | --- |
| `method()` / `known_method()` | 方法原文 / 已知方法分类（[`http_known_method`](#api-http-message-h)，未知方法为 `unknown`）。 |
| `reset()` | 恢复默认构造状态：先清除全部借用视图并恢复默认，再释放首部描述符。 |
| `target()` | 请求目标原文。HTTP/2 / HTTP/3 中为 `:path`。 |
| `scheme()` / `authority()` | 请求 scheme 与 authority（HTTP/2 / HTTP/3 来自伪首部）。 |
| `protocol()` | HTTP/2 `:protocol` 伪首部（RFC 8441 Extended CONNECT）；HTTP/1 请求及未携带 `:protocol` 的 HTTP/2 请求为空。 |
| `target_form()` | 见上表。 |
| `path()` / `query_string()` | 由请求目标拆分得到，均借用目标原文、保持百分号编码。origin-form：`path()` 为第一个 `?` 之前的部分，`query_string()` 为其后的部分（不含 `?`，无查询时为空）。absolute-form：`path()` 为 authority 之后的路径组成部分，路径为空时为 `"/"`；但 `OPTIONS` 且路径为空、无 `?` 时为 `"*"`（RFC 9112 §3.2.4），`query_string()` 同样为 `?` 之后的部分。asterisk-form：`path()` 为 `"*"`，`query_string()` 为空。authority-form（CONNECT）：`path()` 为整个目标（即 authority），`query_string()` 为空。 |
| `protocol_version()` | [`http_protocol_version`](#api-http-message-h)。 |
| `headers() const&` | 语义上的普通字段，不是线格式首部块的逐字节副本：HTTP/1 保留已接受的字段名拼写，但去除字段分帧 / OWS，并在 absolute-form 或 authority-form 目标下替换冲突的 `Host` 值；HTTP/2 字段名为小写，伪首部经 `method()` / `scheme()` / `authority()` / `protocol()` / `target()` 暴露，拆分的 `Cookie` 字段被合并，缺少 `Host` 时由合法 `:authority` 合成。重复普通字段保持线序。`Content-Type` 作为元数据保留、不解析媒体类型语法（解释或拒绝由应用负责）；`Content-Encoding` 在准入时同样只是元数据，内容编码解析器在选择解码器前校验完整栈；`Origin` 与 CORS 预检字段是元数据而非传输分帧，应用须先校验再推导任何跨源许可。右值重载删除。 |
| `body() const&` / `body_bytes() const&` | 已缓冲请求体的文本 / 字节视图（同一存储）；右值重载删除。 |
| `header(name)` | 不区分大小写的语义查找，第一条匹配行优先。 |
| `unique_header(name)` | 不区分大小写的三态查找：两行及以上为 `repeated`，否则 `found` 带唯一值；已知字段种类为 O(1)。值借用请求首部文本。 |
| `last_raw_query_value(raw_name)` | 在编码后的查询串上精确原文查找：`raw_name` 与返回值都保持百分号编码字节，不做 `application/x-www-form-urlencoded` 的 `+` 转换；重复参数时后者优先。 |
| `cookie(name)` | 区分大小写的 Cookie 查找；跨字段行按线序第一个匹配的 cookie-pair 优先。返回视图借用首部文本。 |

查找方法的首行 / 三态选择遵循 [接收字段单值查找统一规则](#api-http-header-h)。

#### 5.3.4 `make_parsed_http_request`

`make_parsed_http_request(method, target, headers, body = {})` 不解析线格式请求头，直接构造语义请求：

- 首部名称 / 值与正文字节借用调用方；按 `headers` 的精确数量分配描述符。调用方必须在返回的请求使用期间保持所有文本与正文存储存活。`method` / `target` 为 `borrowed_text`，`body` 为 `borrowed_span<std::byte>`，二者都拒绝临时拥有型容器。
- 方法、目标或首部字段非法时抛 [`http_protocol_error`](#api-http-protocol-h)。首部字段经过与 HTTP/1、HTTP/2 解析器相同的接收方字段规则，因此重复的 singleton 字段（如 `Range`、`If-Range`）抛出异常，而不是解析为其中一行。
- 首部节超出 `http_field_section_budget` 规则（`max_http_header_bytes`、`max_http_header_fields`）时抛出 `431 header_too_large` 错误。
- 不解释正文分帧字段，因为此 API 表示已经解析完毕的请求。

```cpp
const std::array headers{ruvia::http_header_view{"host", "example.test"},
                         ruvia::http_header_view{"content-type", "text/plain"}};
const auto request = ruvia::make_parsed_http_request("POST", "/items?id=1", headers);
// request.path() == "/items", request.query_string() == "id=1"
// request.header("Content-Type") == "text/plain"
```

### 5.4 `"http_request_expectation.h"`
<a id="api-http-request-expectation-h"></a>

`Expect` 字段（RFC 9110 §10.1.1）的发送方合法性检查与接收方增量状态，HTTP/1 与 HTTP/2 共用，保证两者对 `100-continue` 得出相同结论。无条件编译；公开 include [`"http_protocol.h"`](#api-http-protocol-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http_request_expectation.h`](../ruvia-http/include/http_request_expectation.h)

#### 5.4.1 类型与函数

```cpp
namespace ruvia {

enum class http_request_content_indication : std::uint8_t { no_content, will_follow };

[[nodiscard]] constexpr bool http_client_expectation_is_valid(bool has_continue, http_request_content_indication content) noexcept;

enum class http_unsupported_expectation_policy : std::uint8_t { ignore, reject };

enum class http_server_expectation_plan : std::uint8_t {
    no_action,
    send_continue,
    reject_unsupported,
};

[[nodiscard]] inline http_protocol_error http_unsupported_expectation_error() noexcept;

class http_request_expectations final {
public:
    void parse_field(std::string_view value) noexcept;
    [[nodiscard]] bool has_continue() const noexcept;
    [[nodiscard]] bool has_unsupported() const noexcept;
    void ignore_continue() noexcept;
    [[nodiscard]] http_server_expectation_plan server_plan(http_request_content_indication content,
        http_unsupported_expectation_policy unsupported_policy) const noexcept;
    [[nodiscard]] bool continue_before_switching_protocols() const noexcept;
};

}  // namespace ruvia
```

`http_request_content_indication`：分帧 / 生命周期所有者是否已确认请求内容将跟随初始头。保持强类型：HTTP/1 从请求体计划推导；HTTP/2 结合接收半流与剩余内容状态，使仅含元数据或已知为空的开放流不会冒充待接收内容。

| 成员 | 含义 |
| --- | --- |
| `no_content` (0) | 没有内容跟随。 |
| `will_follow` (1) | 内容将跟随初始头。 |

`http_unsupported_expectation_policy`：产品是否接受未知期望扩展。两种策略下 `Expect` 都是合法语法；HTTP 契约负责显式拒绝策略选定的协议响应。

| 成员 | 含义 |
| --- | --- |
| `ignore` (0) | 忽略不支持的期望成员。 |
| `reject` (1) | 以 `417 (Expectation Failed)` 拒绝。 |

`http_server_expectation_plan`：服务端对期望的处置。

| 成员 | 含义 |
| --- | --- |
| `no_action` (0) | 无需动作。 |
| `send_continue` (1) | 应发送 `100 (Continue)`。 |
| `reject_unsupported` (2) | 应以 `http_unsupported_expectation_error()` 拒绝。 |

| 符号 | 语义 |
| --- | --- |
| `http_client_expectation_is_valid(has_continue, content)` | RFC 9110 §10.1.1 禁止客户端对无内容请求生成 `100-continue`：`!has_continue \|\| content == will_follow`。 |
| `http_unsupported_expectation_error()` | 返回 `http_protocol_error(http_status::expectation_failed, static_text{"unsupported_expectation"}, "unsupported Expect header")`。 |
| `http_request_expectations` | 接收方对 `Expect` `#list` 的定长增量状态（平凡可拷贝，至多 1 字节）。重复字段行延续同一逻辑列表；空成员被忽略。唯一标准化成员是 `100-continue`（不区分大小写匹配）；其他非空成员一律记为“unsupported”这一语义事实，交由产品政策处理。 |
| `parse_field(value)` | 并入一个字段行的值；列表切分感知引号。 |
| `has_continue()` / `has_unsupported()` | 是否见过 `100-continue` / 其他成员。 |
| `ignore_continue()` | 清除 continue 事实：RFC 9110 要求 HTTP/1.0 接收方忽略 `100-continue`；unsupported 事实保留，以便产品仍可应用其扩展支持政策。 |
| `server_plan(content, unsupported_policy)` | 有 unsupported 且策略为 `reject` → `reject_unsupported`；否则有 continue 且 `content == will_follow` → `send_continue`；否则 `no_action`。 |
| `continue_before_switching_protocols()` | RFC 9110 §7.8：服务端同时收到 `Upgrade` 与含 `100-continue` 的 `Expect` 时，必须在 `101 (Switching Protocols)` 之前发送 `100 (Continue)`，即使头之后没有内容。返回 `has_continue()`；unsupported 成员由 `server_plan()` 在接受升级前处理。 |

### 5.5 `"http_request_content_plan.h"`
<a id="api-http-request-content-plan-h"></a>

HTTP/1、HTTP/2、HTTP/3 客户端共用的唯一出站请求内容契约 `http_request_content_plan`：无内容、已知长度（可选附带借用字节）或流式。它决定生成的 `Content-Length`（HTTP/1 无长度流式即 `chunked`）以及请求头是否结束请求流。另含按方法分类的发送方内容语义与项目共用的“无内容”备选项 `http_without_content`。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_content_length.h"`](#api-http-content-length-h)。源码：[`ruvia-http/include/http_request_content_plan.h`](../ruvia-http/include/http_request_content_plan.h)

#### 5.5.1 内容语义分类

```cpp
namespace ruvia {

enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics classify_http_request_content_semantics(std::string_view method) noexcept;

}  // namespace ruvia
```

请求携带内容时的 RFC 9110 发送方要求。显式零长度分帧不构成内容（接收方另行分帧）。方法比较区分大小写。

| 成员 | 含义 |
| --- | --- |
| `no_additional_requirements` (0) | 其他方法：无额外要求。 |
| `forbidden` (1) | `CONNECT`、`TRACE`：不得携带内容。 |
| `content_type_required` (2) | `OPTIONS`：生成的内容须带 `Content-Type`（§9.3.7）。 |

#### 5.5.2 `http_request_content_plan` 及其备选项

```cpp
namespace ruvia {

class http_without_content final {
public:
    constexpr http_without_content() noexcept = default;
};

class http_known_length_request_content final {
public:
    [[nodiscard]] constexpr http_content_length length() const noexcept;
    [[nodiscard]] constexpr std::optional<std::string_view> bytes() const noexcept;
};

class http_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<http_content_length> expected_length() const noexcept;
};

class http_request_content_plan final {
public:
    [[nodiscard]] static constexpr http_request_content_plan none() noexcept;
    [[nodiscard]] static constexpr http_request_content_plan known_length(http_content_length length) noexcept;
    [[nodiscard]] static constexpr http_request_content_plan bytes(borrowed_text value) noexcept;
    [[nodiscard]] static http_request_content_plan bytes(std::span<const std::byte> value) noexcept;
    template </* 临时拥有型 std::byte 范围 */ range_type>
    static http_request_content_plan bytes(range_type&&) = delete;
    template </* std::byte */ element_type>
    static http_request_content_plan bytes(std::initializer_list<element_type>) = delete;
    [[nodiscard]] static constexpr http_request_content_plan streaming(std::optional<http_content_length> length = {}) noexcept;

    [[nodiscard]] constexpr const http_without_content* without_content() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_without_content* without_content() const&& = delete;
    [[nodiscard]] constexpr const http_known_length_request_content* known_length_content() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_known_length_request_content* known_length_content() const&& = delete;
    [[nodiscard]] constexpr const http_streaming_request_content* streaming_content() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_streaming_request_content* streaming_content() const&& = delete;
    [[nodiscard]] constexpr std::optional<http_content_length> declared_length() const noexcept;
};

}  // namespace ruvia
```

`http_without_content` 是整个项目所有 variant 共用的唯一空“无内容”备选项。`http_known_length_request_content` 与 `http_streaming_request_content` 没有公开构造函数，只能由 `http_request_content_plan` 的工厂产生。计划可平凡拷贝，长度类型见 [`http_content_length`](#api-http-content-length-h)。

| 成员 | 语义 |
| --- | --- |
| `none()` | 无内容、不发送内容分帧；请求头结束请求流。 |
| `known_length(length)` | 已知长度、内容在头之后由驱动提交；仅当长度为零时头才结束流。零表示无内容：不要求 OPTIONS `Content-Type`，不能使用 `Expect: 100-continue`；TRACE 省略零值 `Content-Length`。 |
| `bytes(borrowed_text)` / `bytes(span<const std::byte>)` | 借用内容及其精确长度。`bytes("")` 与 `none()` 不同：它要求显式 `Content-Length: 0`。字节必须存活且不变直到发送完毕。 |
| `bytes(range_type&&) = delete` / `bytes(std::initializer_list<element_type>) = delete` | 拒绝临时拥有型字节容器和字节初始化列表（包括具名列表）；`borrowed_text` 重载拒绝临时拥有型字符串。 |
| `streaming(length = {})` | 流式内容，可选预期长度（有值时写出 `Content-Length`，HTTP/1 无值时为 `chunked`）。已知零长度会保持发送半流打开直至请求完成，不能发送内容，也不能等待 `100 Continue`。 |
| `without_content()` / `known_length_content()` / `streaming_content()` | 当前备选项匹配时返回指向计划内部的指针，否则为 `nullptr`；指针借用计划对象，右值重载删除。 |
| `http_known_length_request_content::length()` | 声明长度。 |
| `http_known_length_request_content::bytes()` | 由 `bytes()` 工厂设置的借用内容；`known_length(n)` 为 `std::nullopt`（驱动在头之后提交负载）。 |
| `http_streaming_request_content::expected_length()` | 流式工厂给出的预期长度。 |
| `declared_length()` | 请求头声明的 `Content-Length`：已知长度计划为其长度，流式为 `expected_length()`，`none()` 为 `std::nullopt`。 |

HTTP/1 `prepared_http1_client_request::content()` 返回同一计划（见 [`"http1_client_request.h"`](#api-http1-client-request-h)）。

```cpp
const std::string payload = R"({"name":"ruvia"})";
ruvia::http_request_head_view head{
    .method_ = "POST",
    .target_ = "/items",
    .content_ = ruvia::http_request_content_plan::bytes(payload),  // 借用 payload
};
```

### 5.6 `"http_request_body_failure.h"`
<a id="api-http-request-body-failure-h"></a>

服务端驱动在执行 HTTP 拥有的请求内容契约时检测到的请求体失败，以及它们与请求解码错误到 `http_protocol_error` 的映射：驱动提供运行时字节上限与 I/O 完成事实，HTTP 拥有最终响应状态与诊断。无条件编译；公开 include [`"http_content_coding.h"`](#api-http-content-coding-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http_request_body_failure.h`](../ruvia-http/include/http_request_body_failure.h)

#### 5.6.1 类型与函数

```cpp
namespace ruvia {

enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
    buffer_limit,
};

[[nodiscard]] inline http_protocol_error make_http_protocol_error(http_request_body_failure failure) noexcept;

enum class http_coding_field : std::uint8_t {
    content_encoding,
    transfer_encoding,
};

[[nodiscard]] inline http_protocol_error make_http_protocol_error(http_content_decode_error error, http_coding_field field) noexcept;

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(std::size_t size, protocol_byte_limit limit) noexcept;
[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(std::size_t current_size,
    std::size_t additional_size, protocol_byte_limit limit) noexcept;

}  // namespace ruvia
```

| `http_request_body_failure` 成员 | 含义与 `make_http_protocol_error` 映射 |
| --- | --- |
| `too_large` (0) | 请求体超过上限 → `413 content_too_large`，`"request_body_too_large"`。 |
| `incomplete` (1) | 请求体未完整到达 → `400 bad_request`，`"incomplete_request_body"`。 |
| `buffer_limit` (2) | 入站缓冲容量耗尽 → `503 service_unavailable`，`"inbound_buffer_limit"`。 |

枚举外的值映射为 `400`，`"invalid_request_body"`。

| `http_coding_field` 成员 | 含义 |
| --- | --- |
| `content_encoding` (0) | 解码失败的 coding 来自 `Content-Encoding`。 |
| `transfer_encoding` (1) | 解码失败的 coding 来自 `Transfer-Encoding`。 |

`make_http_protocol_error(error, field)`：请求体的 `Content-Encoding` 或 `Transfer-Encoding` 无法解码时的状态（[`http_content_decode_error`](#api-http-content-coding-h)），同一解码错误按字段给出不同状态：

| 条件 | 结果 |
| --- | --- |
| `decoded_size_exceeded`（任一字段） | 同 `too_large`（413）。 |
| `transfer_encoding` + `unsupported_coding` | `make_http_protocol_error(http_parse_error::unsupported_transfer_encoding)`（501）；实际不会出现，分帧阶段已先以 501 拒绝此类栈。 |
| `transfer_encoding` + 其他错误 | `400`，`"invalid_transfer_coding_body"`。 |
| `content_encoding` + `unsupported_coding` | `415 unsupported_media_type`，`"unsupported_content_coding"`。 |
| `content_encoding` + 其他错误 | `400`，`"invalid_content_coding"`。 |

`make_http_protocol_error` 是按各族错误枚举重载的唯一映射重载集，其余重载见各错误族所在头（如 [`"http_protocol.h"`](#api-http-protocol-h)）。

| 函数 | 语义 |
| --- | --- |
| `http_request_body_size_failure(size, limit)` | `limit.exceeds(size)` 时返回 `too_large`，否则 `std::nullopt`（`protocol_byte_limit` 见 [`"http_protocol.h"`](#api-http-protocol-h)）。 |
| `http_request_body_addition_failure(current_size, additional_size, limit)` | `limit.addition_exceeds(current_size, additional_size)` 时返回 `too_large`（不溢出地判断追加后是否超限），否则 `std::nullopt`。 |

### 5.7 `"http_client_response_head.h"`
<a id="api-http-client-response-head-h"></a>

客户端收到的最终响应头 `http_client_response_head`（拥有字段数据，原始输入可在解析完成后释放），以及提供给外部请求体写入器的响应侧信号。HTTP/1 响应计划与 HTTP/2 响应事件产出同一类型与信号。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http_client_response_head.h`](../ruvia-http/include/http_client_response_head.h)

#### 5.7.1 类型

```cpp
namespace ruvia {

enum class http_client_request_content_signal : std::uint8_t {
    continue_,
    exchange_complete,
};

class http_client_response_head final {
public:
    http_client_response_head(const http_client_response_head&) = delete;
    http_client_response_head& operator=(const http_client_response_head&) = delete;
    http_client_response_head(http_client_response_head&&) noexcept = default;
    http_client_response_head& operator=(http_client_response_head&&) = delete;

    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] http_received_status received_status() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept;

    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view name) const&& = delete;
    [[nodiscard]] http_unique_field_lookup unique_header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    http_unique_field_lookup unique_header(std::string_view name) const&& = delete;
    [[nodiscard]] std::vector<http_header> take_headers() && noexcept;
};

}  // namespace ruvia
```

| `http_client_request_content_signal` 成员 | 含义 |
| --- | --- |
| `continue_` (0) | 请求内容仍受 `Expect: 100-continue` 门控时收到 `100 (Continue)`：外部写入器开始发送请求内容。 |
| `exchange_complete` (1) | 请求内容仍受 `Expect` 门控（尚未收到 `100 (Continue)`）时收到最终响应头（RFC 9110 §10.1.1），HTTP/1、HTTP/2、HTTP/3 一致。HTTP/1 另有一种情况：响应将关闭连接，而外部写入器尚未写完请求内容（内容待发送、仍在等待 `100`，或已收到 `100` 但内容未完成），此时同样发出该信号。只有此信号取消待发送的请求内容；未收到它时，即使已收到提前的最终响应，写入器仍须完成请求（RFC 9110 §7.5）。 |

`http_client_response_head` 拥有响应字段；解码无字段的响应头不分配字段存储。对象只由协议解析器 / 事件产生（无公开构造函数）。HEAD 响应的字段即使在状态 205 下也仍是元数据：`Content-Length` 不会成为该响应的接收内容长度。

| 成员 | 语义 |
| --- | --- |
| 拷贝 / 拷贝赋值 / 移动赋值 | 删除。 |
| 移动构造 | 转移拥有的字段存储；源对象保留状态码与版本，字段为空。 |
| `status()` | 处理所依据的状态：收到的 `600..999` 按 `500` 处理（RFC 9110 §15）。类型见 [`"http_status.h"`](#api-http-status-h)。 |
| `received_status()` | 收到的原始状态码，包括非法的 `600..999`。 |
| `protocol_version()` | 响应协议版本。 |
| `content_length()` | 响应头声明的长度：HTTP/1 与 HTTP/3 取自合法的 `Content-Length` 字段，HTTP/2 取自流已接受的声明长度；没有声明时为 `std::nullopt`。HTTP/2 临时（`1xx`）响应头不设置该值。对 HEAD、`204`、`304` 等按规则无内容的响应，该值只是表示元数据；HTTP/1 中若此类响应的 `Content-Length` 不一致，则为 `std::nullopt`，不报错。 |
| `headers() const&` | 已解析字段行，保留线序与重复的元数据字段。`Content-Type` 不作为媒体类型解析、合并或替应用选择（即使语法非法）；`Content-Encoding` 同样保留而不解释 coding 列表语法，内容解码 API 独立于消息分帧校验它。右值重载删除。 |
| `header(name) const&` | 不区分大小写，第一条匹配行优先；右值重载删除。 |
| `unique_header(name) const&` | 不区分大小写的三态查找，用于不得择一的字段（`Location`、`Content-Type`）：两行及以上为 `repeated`；`found` 值借用本对象。右值重载删除。 |
| `take_headers() &&` | 把拥有的已解析字段转移给需要独立延长字段生存期、同时保留头元数据的消费者。 |

单值查找规则见 [接收字段单值查找统一规则](#api-http-header-h)。

## 6. 响应模型

本章描述服务端最终响应、临时响应、响应首部块、响应体表示与写计划，以及流式响应的提交计划。HTTP/1 序列化见 [`"http1_server_response.h"`](#api-http1-server-response-h)，HTTP/2、HTTP/3 提交见 [`"http2_connection.h"`](#api-http2-connection-h)、[`"http3_connection.h"`](#api-http3-connection-h)。

### 6.1 `"http_response.h"`
<a id="api-http-response-h"></a>

定义缓冲型最终响应 `http_response`（`200..599`，状态 + 首部块 + 一个正文表示）与 `1xx` 临时响应头视图 `http_interim_response_head`。首部规则全部归属 [`"http_response_headers.h"`](#api-http-response-headers-h)，正文表示类型与写计划归属 [`"http_response_body.h"`](#api-http-response-body-h)；本头只把二者组合发布。另公开 include [`"http_header.h"`](#api-http-header-h)、[`"http_status.h"`](#api-http-status-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_content_length.h"`](#api-http-content-length-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http_response.h`](../ruvia-http/include/http_response.h)

#### 6.1.1 `class http_response`

```cpp
namespace ruvia {
enum class http_connect_udp_error : std::uint8_t;   // 前置声明，定义见 http_connect_udp.h
class http1_response_head_plan;                     // 前置声明，定义见 http1_server_response.h
class http_request;                                 // 前置声明，定义见 http_request.h

class http_response final {
public:
    http_response() noexcept;
    http_response(const http_response&) = delete;
    http_response& operator=(const http_response&) = delete;
    http_response(http_response&& other) noexcept;
    http_response& operator=(http_response&& other) noexcept;

    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] const http_response_headers& headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_headers& headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;

    void status(http_status_code status);
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    void header_stable_view(borrowed_text key, borrowed_text value);
    void content_type(http_content_type type);
    void allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    void transfer_headers_from(const http_response& source, http_response_header_transfer mode);
    void commit_headers(http_response_headers&& staged) noexcept;

    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view body() const&& = delete;
    [[nodiscard]] std::string_view body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view body_bytes() const&& = delete;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const&& = delete;
    [[nodiscard]] bool has_multipart_file_body() const noexcept;
    [[nodiscard]] bool has_file_representation() const noexcept;
    void head_metadata_body() noexcept;
    [[nodiscard]] bool has_head_metadata_body() const noexcept;
    [[nodiscard]] std::size_t body_segment_count() const noexcept;
    [[nodiscard]] http_response_body_segment_view body_segment(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    http_response_body_segment_view body_segment(std::size_t index) const&& = delete;

    void body(std::string_view value);
    template <std::same_as<std::string> string_type>
    void body(string_type&& value) noexcept;
    void static_body(borrowed_text value) noexcept;
    void file_bytes_body(std::string&& value) noexcept;
    void file_body(http_native_path_view_type file, http_content_length size, http_content_length offset,
                   http_content_length length, http_response_file_identity identity,
                   http_response_file_descriptor opened = {});
    void multipart_file_body(http_native_path_view_type file, http_content_length size,
                             http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
                             http_response_file_descriptor opened = {});
    void content_range(http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve_headers(std::size_t count);
    void apply_content_encoding(std::string_view content_encoding);
    void replace_body_with_content_encoding(std::string&& value, std::string_view content_encoding);
};
}
```

默认构造状态为 `http_status::ok`、空首部、空正文。不可复制；移动转移状态、首部与正文，源对象保留为空首部、空正文、可继续使用。移动赋值先由新值完整接管，再释放旧首部与旧正文资源（包括调用 `http_response_file_descriptor` 的释放函数），释放回调中对源对象或本对象的重入写入得以保留，每个文件描述符只释放一次。

状态与首部：

| 成员 | 语义 |
| --- | --- |
| `status()` | 返回当前最终状态码。 |
| `status(http_status_code)` | 设置最终状态。仅接受 `200..599`；`101` 抛 `std::invalid_argument`（协议切换必须由专用协议驱动处理），其他非最终状态码同样抛 `std::invalid_argument`。临时 `1xx` 响应使用 `http_interim_response_head`。 |
| `headers() const&` | 借用整个首部块（只读），有效至本响应下一次首部变更或销毁；右值重载被删除，拒绝从临时响应取得悬垂引用。 |
| `header(name) const&` | 按名称大小写不敏感查找，语义同 [`http_response_headers::header`](#api-http-response-headers-h)；返回视图借用本响应。右值重载删除。 |
| `header(key, value)` / `header(key, value, options)` / `header_stable_view` / `content_type` / `allow_methods` / `remove_header` / `set_cookie` / `content_range` / `content_range_unsatisfied` / `add_vary_token` | 逐一转发到同名 `http_response_headers` 成员，校验、异常与借用规则以 [6.2](#api-http-response-headers-h) 为准。`allow_methods` 写出 `Allow`：先按 `method_mask` 的已知方法位，再追加扩展方法；扩展方法须为非空 HTTP token，可借用当前首部存储；结果字段由本响应拥有，变更后须刷新先前取得的视图。 |
| `reserve_headers(count)` | 转发到 `http_response_headers::reserve`。 |
| `transfer_headers_from(source, mode)` | 按 `mode` 把 `source` 的应用首部复制进本响应，规则见 `http_response_headers::transfer_from`；`assign` 保留目的端 `Content-Type`。 |
| `commit_headers(staged) noexcept` | 发布一个由 `headers().clone()` 起始、在外部操作期间编辑的首部事务；发布不分配、不失败，`staged` 被置为空且独立。 |

正文观察（返回视图均借用本响应，有效至本响应正文被替换、移动或销毁；所有右值重载均删除）：

| 成员 | 语义 |
| --- | --- |
| `body()` / `body_bytes()` | 字节正文（复制、拥有、静态借用或 `file_bytes_body`）的字节视图；文件、multipart 文件与空正文返回空视图。`body()` 与 `body_bytes()` 等价。 |
| `file_body() const&` | 文件正文或 multipart 文件正文时返回 [`http_response_file_view`](#api-http-response-body-h)（multipart 时描述整个文件 `[0, size)`），否则 `std::nullopt`。 |
| `has_multipart_file_body()` | 正文为 `multipart_file_body()` 设置的 `multipart/byteranges` 文件正文时为 `true`。 |
| `has_file_representation()` | 正文来自 `file_body()`、`multipart_file_body()` 或 `file_bytes_body()` 时为 `true`：这些字节是文件的表示，发送方原样传送（例如不再做动态内容编码）。 |
| `has_head_metadata_body()` | 当前正文由 `head_metadata_body()` 设置时为 `true`。 |
| `body_segment_count()` | 表示的存储片段数，不等于待发送字节数：空字节正文为 `0`；字节正文为 `1`；文件正文即使长度为零也为 `1`（保留路径、描述符与 identity）；multipart 文件正文为计划中的文件区间与边界元数据片段总数。 |
| `body_segment(index) const&` | 返回第 `index` 个片段的 `http_response_body_segment_view`（字节片段填 `bytes_`，文件片段填 `file_`）；越界抛 `std::out_of_range`。 |

长度为零的文件片段仍然存在，调用方必须完成适用的文件校验（打开、identity 验证），不能因写计划 `send_body() == false` 就丢弃它；`send_body()` 只表示方法/状态允许且正文长度非零时需要传送 payload（见 [`http_buffered_response_write_plan`](#api-http-response-body-h)）。

正文设置（每次设置都替换整个正文：先发布完整的新表示，再释放旧正文资源；释放回调中重入替换的结果保留。重新设置正文会清除 HEAD 元数据标记）：

| 成员 | 语义 |
| --- | --- |
| `body(std::string_view)` | 复制字节；空输入得到空正文（无 segment）。分配失败抛分配异常且正文不变。 |
| `body(std::string&&) noexcept` | 仅精确的非 const `std::string` 右值接管存储，不复制；空串得到空正文。 |
| `static_body(borrowed_text) noexcept` | 零拷贝借用 `value`；调用方须保证这些字节在本响应（或其移动目标）保留该正文期间有效且不变。`borrowed_text` 本身拒绝临时拥有型字符串，见 [`"borrowed_text.h"`](#api-borrowed-text-h)。空输入得到空正文。 |
| `file_bytes_body(std::string&&) noexcept` | 接管从文件读取的字节（整个文件、一个字节范围或 multipart byteranges 负载）。与 `body()` 的设置重载不同，即使为空也保持文件表示（`has_file_representation() == true`）。 |
| `file_body(file, size, offset, length, identity, opened = {})` | 设置文件正文：拥有原生路径副本，保留运行时不透明 identity；`opened` 若已占用，是运行时据以分帧的已打开描述符，由正文拥有并在销毁或替换时释放。`file` 为空或 `[offset, offset + length)` 超出 `size` 时抛 `std::invalid_argument`，正文不变，被拒绝的 `opened` 随调用释放。 |
| `multipart_file_body(file, size, identity, plan, opened = {})` | 设置 `multipart/byteranges` 文件正文，`plan` 为 [`http_multipart_byte_range_plan`](#api-http-range-h)。`file` 为空、计划无片段、无文件片段、某文件片段长度为零或超出 `size` 时抛 `std::invalid_argument`，正文不变，`opened` 被释放。 |
| `head_metadata_body() noexcept` | 把正文替换为仅描述 HEAD 所选表示的空正文：保留首部、释放原正文资源。序列化 HEAD 时保留显式 `Content-Length`（作为 GET 表示的长度）；未提供长度时不推断为零。标记随移动转移，不影响 GET 等其他方法的正文规则。 |

内容编码：

| 成员 | 语义 |
| --- | --- |
| `apply_content_encoding(coding)` | 用于流式编码：设置 `Content-Encoding`，移除 `Content-Length`。 |
| `replace_body_with_content_encoding(value, coding)` | 用于缓冲编码：原子替换正文为已编码字节 `value` 并更新编码相关元数据，设置新的 `Content-Length`。 |

两者共同规则：`coding` 须为非空 HTTP coding 列表，去除首尾 SP/HTAB 后存为字段值；非法时抛 `std::invalid_argument`，响应保持不变。成功时设置 `Content-Encoding`，把强 `ETag` 弱化为 `W/"..."`（RFC 9110 §8.8.3：编码表示与 identity 表示字节不同），丢弃不是带引号 entity-tag 的 `ETag`，并移除 `Accept-Ranges`（identity 表示的字节范围不能寻址编码表示，RFC 9110 §14.3）。

示例（见 [`examples/api_surface.cpp`](../examples/api_surface.cpp)）：

```cpp
ruvia::http_response response;
response.status(ruvia::http_status::non_authoritative_information);
response.header("X-Response-Remove", "drop");
response.body("response slot\n");
response.header("X-Response-Slot", "true", {.mode_ = ruvia::http_response_header_mode::append});
response.remove_header("X-Response-Remove");
```

#### 6.1.2 `class http_interim_response_head`

```cpp
class http_interim_response_head final {
public:
    explicit http_interim_response_head(http_status_code status_code, borrowed_http_headers headers = {});
    [[nodiscard]] constexpr http_status_code status() const noexcept;
    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept;
};
```

不可变、无正文的 `1xx` 进度响应头视图（如 `100 Continue`、`103 Early Hints`），先于最终响应发送。构造时状态码须为 `1xx` 且不是 `101`：`101` 抛 `std::invalid_argument`（协议切换转移连接所有权，需要专用协议驱动），其他非 `1xx` 状态同样抛 `std::invalid_argument`。`headers` 为借用的 [`borrowed_http_headers`](#api-http-header-h)：首部元素及其字符串须在同步的 sans-I/O 提交调用期间保持稳定。`status()` / `headers()` 返回构造值。

### 6.2 `"http_response_headers.h"`
<a id="api-http-response-headers-h"></a>

响应首部块 `http_response_headers`：拥有描述符存储、已知字段索引、全部字段变更规则以及克隆事务。`http_response` 的首部操作都转发到这里。依赖 [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_content_length.h"`](#api-http-content-length-h)；`set_cookie_plan` 见 [`"http_cookie.h"`](#api-http-cookie-h)。源码：[`ruvia-http/include/http_response_headers.h`](../ruvia-http/include/http_response_headers.h)

#### 6.2.1 类型与选项

```cpp
namespace ruvia {
class set_cookie_plan;

enum class http_response_header_mode : std::uint8_t {
    replace,
    append,
};

enum class http_response_header_transfer : std::uint8_t {
    merge,
    assign,
    apply,
};

enum class http_content_type : std::uint8_t {
    text_plain_utf8,
    text_html_utf8,
    application_json,
};

struct http_response_header_options {
    http_response_header_mode mode_{http_response_header_mode::replace};
};

struct http_response_header {
public:
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view value() const noexcept;
};
static_assert(std::is_trivially_copyable_v<http_response_header>);
static_assert(sizeof(http_response_header) <= 24);
}
```

| 枚举成员 | 含义 |
| --- | --- |
| `http_response_header_mode::replace`（0） | 默认。替换同名字段：保留第一行位置写入新值，并折叠其余同名行。 |
| `http_response_header_mode::append`（1） | 追加一行同名字段。不允许多行的字段（单值字段，如 `Content-Type`、`Content-Length`）以 `append` 写入时抛 `std::invalid_argument`；`Set-Cookie` 的 `append` 按 cookie 身份替换或追加（见 `set_cookie`）。 |
| `http_response_header_transfer::merge`（0） | 合并：单值字段仅在目的端缺失时复制；以追加方式加入的多行字段按出现次数补齐；`Set-Cookie` 按 cookie 身份替换或追加。 |
| `http_response_header_transfer::assign`（1） | 赋值：源端 `Content-Type` 被跳过（保留目的端 `Content-Type`）；源端有 `Set-Cookie` 时先移除目的端全部 `Set-Cookie` 再写入源端的；其他单值字段覆盖目的端。 |
| `http_response_header_transfer::apply`（2） | 应用：单值字段（含 `Content-Type`）覆盖目的端；`Set-Cookie` 按 cookie 身份替换或追加，不清除目的端其他 cookie。 |
| `http_content_type::text_plain_utf8`（0） | `text/plain; charset=UTF-8`。 |
| `http_content_type::text_html_utf8`（1） | `text/html; charset=UTF-8`。 |
| `http_content_type::application_json`（2） | `application/json`。 |

`http_content_type` 的字段行是库常量：设置时共享静态描述符，不分配也不重新做语法校验；非法枚举值抛 `std::invalid_argument`。

`http_response_header` 是首部块中一行的只读描述符（可平凡复制，不超过 24 字节），只能从 `http_response_headers` 迭代获得，不能由用户构造。`name()` / `value()` 借用首部块存储，有效至该块下一次变更、移动或销毁。

#### 6.2.2 `class http_response_headers`

```cpp
class http_response_headers final {
public:
    using value_type = http_response_header;
    using const_iterator = const http_response_header*;

    http_response_headers() noexcept;
    http_response_headers(http_response_headers&& other) noexcept;
    http_response_headers(const http_response_headers&) = delete;
    http_response_headers& operator=(const http_response_headers&) = delete;
    http_response_headers& operator=(http_response_headers&&) = delete;
    ~http_response_headers();

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const_iterator begin() const&& = delete;
    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const_iterator end() const&& = delete;
    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const_iterator cbegin() const&& = delete;
    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const_iterator cend() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;

    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    void header_stable_view(borrowed_text key, borrowed_text value);
    void content_type(http_content_type type);
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    void allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void content_range(http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve(std::size_t count);
    void transfer_from(const http_response_headers& source, http_response_header_transfer mode);
    [[nodiscard]] http_response_headers clone(std::size_t additional_headers = 0) const;
};
```

默认构造独立空首部块。移动构造转移全部描述符与已知字段索引，源块保持可用、独立且为空。复制与（移动）赋值被删除；把编辑后的克隆写回响应使用 `http_response::commit_headers`。

读取（迭代器与视图借用本块，有效至下一次变更、移动或销毁；右值重载全部删除以拒绝临时块）：

| 成员 | 语义 |
| --- | --- |
| `begin` / `end` / `cbegin` / `cend` | 按插入顺序遍历 `http_response_header`。 |
| `size()` / `empty()` | 字段行数 / 是否无字段。 |
| `header(name) const&` | 大小写不敏感查找第一行的值；不存在返回 `std::nullopt`。 |

变更（输入可以借用当前首部存储；变更后须刷新先前取得的视图。除注明外，失败时抛异常且首部块不变）：

| 成员 | 语义 |
| --- | --- |
| `header(key, value[, options])` | 校验字段名为 token、值为合法字段值（否则 `std::invalid_argument`），并对已知字段做发送方语义校验：非法 `Connection`、非法 `Upgrade`、出现 `TE`（不是响应字段）及已知字段值不合法均抛 `std::invalid_argument`。默认 `replace`，`options.mode_ == append` 时追加（规则见 `http_response_header_mode`）；非法 `mode_` 抛 `std::invalid_argument`。超出描述符可表示长度时抛 `std::length_error`。 |
| `header_stable_view(key, value)` | 校验同 `header()`（替换语义），但名称与值可作为借用视图保存而不复制。调用方须保证这些字节在本块及任何保留它们的克隆存续期间有效且不变；`borrowed_text` 拒绝拥有型临时字符串。 |
| `content_type(type)` | 以库常量替换 `Content-Type`。 |
| `remove_header(key)` | 删除所有同名行；字段名非法抛 `std::invalid_argument`；不存在时无操作。匹配键的存储保留到描述符表截短与索引重建完成之后。 |
| `set_cookie(plan)` | 追加一行 `Set-Cookie`；已有同名、同 `Domain`/`Path` 作用域且 `Partitioned` 状态相同的 cookie 时替换之（分区与非分区 cookie 可共存）。 |
| `allow_methods(mask, extension_methods = {})` | 写出 `Allow`，规则见 [6.1.1](#api-http-response-h)；扩展方法非 token 时抛 `std::invalid_argument`。 |
| `content_range(offset, length, size)` | 写出 `Content-Range: bytes offset-(offset+length-1)/size`。`length == 0` 或范围超出 `size` 时抛 `std::logic_error`。 |
| `content_range_unsatisfied(size)` | 写出 `416` 用的 `Content-Range: bytes */size`。 |
| `add_vary_token(token)` | 把一个字段名 token 或 `*` 合并进 `Vary`：跨行去重（大小写不敏感），已含该成员或已有通配 `*` 时不变且不分配；新增 `*` 复用静态描述符。合并直接写入最终字段存储，完成前保留旧字段，因此 `token` 可借用当前首部、分配失败后可重试。非法 token 抛 `std::invalid_argument`，超长抛 `std::length_error`。 |
| `reserve(count)` | 预留至少 `count` 个字段行；容量足够时不分配。扩容先切换到新描述符表再释放旧表，不复制字段字节；分配失败保留原表。 |
| `transfer_from(source, mode)` | 按 `http_response_header_transfer` 规则复制 `source` 的字段。仅接受 `merge` / `assign` / `apply`，其他值在任何变更之前抛 `std::invalid_argument`（`source` 为空也一样）。与现有字段无交集时原地追加；否则在暂存克隆上解析后发布，失败时本块不变。静态与借用描述符共享，拥有字节被复制。 |
| `clone(additional_headers = 0) const` | 生成独立副本并为额外 `additional_headers` 个字段预留：拥有字节被复制（两块可各自变更或销毁），静态/借用视图共享。字段总数无法容纳时在克隆前抛 `std::length_error`。克隆用作“仅首部事务”：围绕外部操作编辑，再以 `http_response::commit_headers` 无分配发布。 |

```cpp
auto staged = response.headers().clone(1);
staged.header("Cache-Control", "no-store");
// …可能失败的外部操作…
response.commit_headers(std::move(staged));   // 不分配、不失败；staged 变为空
```

### 6.3 `"http_response_body.h"`
<a id="api-http-response-body-h"></a>

响应体的公开词汇：原生路径辅助、文件 identity、文件描述符所有权、文件视图、正文片段视图，以及由“请求方法 × 响应状态”决定的正文计划与缓冲写计划（HEAD / `1xx` / `204` / `205` / `304` / CONNECT 规则的唯一定义处）。依赖 [`"http_message.h"`](#api-http-message-h)（`http_known_method`）、[`"http_status.h"`](#api-http-status-h)、[`"http_range.h"`](#api-http-range-h)（`http_multipart_byte_range_plan`）。正文存储本身是未公开的实现类型，只能通过 [`http_response`](#api-http-response-h) 的成员设置与观察。源码：[`ruvia-http/include/http_response_body.h`](../ruvia-http/include/http_response_body.h)

#### 6.3.1 原生路径

```cpp
using http_native_path_char_type = std::filesystem::path::value_type;
using http_native_path_string_type = std::basic_string<http_native_path_char_type>;
using http_native_path_view_type = std::basic_string_view<http_native_path_char_type>;

[[nodiscard]] inline http_native_path_view_type http_native_path_view(const std::filesystem::path& path RUVIA_LIFETIMEBOUND) noexcept;
http_native_path_view_type http_native_path_view(const std::filesystem::path&&) = delete;
inline void http_assign_native_path(http_native_path_string_type& output, const std::filesystem::path& path);
[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_char_type* path);
[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_string_type& path);
```

| 符号 | 语义 |
| --- | --- |
| `http_native_path_char_type` / `_string_type` / `_view_type` | 平台原生路径字符（Windows 为 `wchar_t`，其他平台为 `char`）及其字符串/视图，响应文件正文以此承载路径。 |
| `http_native_path_view(path)` | 返回借用 `path.native()` 的视图，有效至 `path` 变更或销毁；右值重载删除，拒绝临时 `std::filesystem::path`。 |
| `http_assign_native_path(output, path)` | 把 `path` 的原生文本复制进 `output`（可能抛分配异常）。 |
| `http_make_path_from_native_path(ptr)` | 由以 NUL 结尾的原生路径构造 `std::filesystem::path`；`nullptr` 得到空路径。 |
| `http_make_path_from_native_path(string)` | 同上，取 `string.c_str()`。 |

#### 6.3.2 `class http_response_file_identity`

```cpp
class http_response_file_identity final {
public:
    [[nodiscard]] static constexpr http_response_file_identity unchecked() noexcept;
    [[nodiscard]] static constexpr http_response_file_identity checked(std::array<std::uint64_t, 4> words) noexcept;
    [[nodiscard]] constexpr bool requires_validation() const noexcept;
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;
    friend constexpr bool operator==(const http_response_file_identity&, const http_response_file_identity&) noexcept = default;
};
```

运行时提供的不透明文件身份令牌，表示分帧响应时所依据的文件元数据。本 target 只传递它，不解释；运行时在打开实际文件句柄后据此校验文件未被替换。`unchecked()` 不要求校验（`words()` 全零）；`checked(words)` 携带 4 个 64 位字并要求校验。`requires_validation()` 区分二者。`words() const&` 借用本对象，右值重载删除。`operator==` 逐成员比较。

#### 6.3.3 `class http_response_file_descriptor`

```cpp
class http_response_file_descriptor final {
public:
    using native_type = std::uintptr_t;
    using release_type = void (*)(native_type) noexcept;

    constexpr http_response_file_descriptor() noexcept = default;
    constexpr http_response_file_descriptor(native_type native_value, release_type release) noexcept;
    http_response_file_descriptor(const http_response_file_descriptor&) = delete;
    http_response_file_descriptor& operator=(const http_response_file_descriptor&) = delete;
    constexpr http_response_file_descriptor(http_response_file_descriptor&& other) noexcept;
    http_response_file_descriptor& operator=(http_response_file_descriptor&& other) noexcept;
    ~http_response_file_descriptor();
    [[nodiscard]] constexpr explicit operator bool() const noexcept;
    [[nodiscard]] constexpr native_type native() const noexcept;
};
```

运行时已经打开、并据以分帧响应的原生文件，由响应正文携带，使传输层从同一描述符发送而不再解析路径。本 target 不对其做任何 I/O：只保存原生值，并在所属正文销毁或被替换时恰好调用一次运行时提供的 `release`。

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 未占用（`release == nullptr`）。 |
| `(native_value, release)` | 占用；`release` 为 `nullptr` 时视为未占用。 |
| 移动构造 | 接管，源变为未占用。 |
| 移动赋值 | 先让两端所有权与返回引用全部提交，再释放旧值；释放回调重入时状态一致。自赋值无操作。 |
| 析构 | 若占用，以原生值调用 `release` 一次。 |
| `explicit operator bool` | 是否占用。 |
| `native()` | 原生值；仅占用时有意义，在本对象存活期间借用。 |

#### 6.3.4 `class http_response_file_view` 与 `struct http_response_body_segment_view`

```cpp
class http_response_file_view final {
public:
    constexpr http_response_file_view(const http_native_path_char_type* native_path, std::uint64_t size,
                                      std::uint64_t offset, std::uint64_t length,
                                      http_response_file_identity identity,
                                      const http_response_file_descriptor* opened = nullptr) noexcept;
    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept;
    [[nodiscard]] std::filesystem::path to_path() const;
    [[nodiscard]] constexpr std::uint64_t size() const noexcept;
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept;
    [[nodiscard]] constexpr std::uint64_t length() const noexcept;
    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept;
    [[nodiscard]] constexpr const http_response_file_descriptor* opened_file() const noexcept;
};

class http_response;

struct http_response_body_segment_view final {
    std::string_view bytes_{};
    std::optional<http_response_file_view> file_{};
};
```

`http_response_file_view` 是响应文件的只读描述：路径与已打开文件均借用，有效期等于取得它的响应正文的生命周期。`native_path_c_str()` 为 NUL 结尾原生路径；`to_path()` 构造 `std::filesystem::path`（可能分配）；`size()` 为文件总长，`[offset(), offset() + length())` 为待发送区间；`identity()` 为运行时 identity；`opened_file()` 为分帧时的描述符，未占用时为 `nullptr`，此时传输层须自行打开 `native_path_c_str()` 并校验 `identity()`。构造时传入未占用的描述符等同于 `nullptr`。

`http_response_body_segment_view` 是一个正文片段的借用视图（由 `http_response::body_segment()` 返回）：字节片段设置 `bytes_`，文件片段设置 `file_`（此时 `bytes_` 为空）。其中的字节与路径仅在所属 `http_response` 存活且未被修改期间有效。

#### 6.3.5 正文计划与缓冲写计划

```cpp
enum class http_response_content_semantics : std::uint8_t {
    informational,
    protocol_switch,
    connect_tunnel,
    without_content,
    with_content,
};

class http_response_body_plan final {
public:
    [[nodiscard]] constexpr http_known_method request_method() const noexcept;
    [[nodiscard]] constexpr http_status_code response_status() const noexcept;
    [[nodiscard]] constexpr http_response_content_semantics content_semantics() const noexcept;
    [[nodiscard]] constexpr bool status_allows_body() const noexcept;
    [[nodiscard]] constexpr bool body_suppressed() const noexcept;
    [[nodiscard]] constexpr bool auto_content_length_allowed() const noexcept;
    [[nodiscard]] constexpr bool explicit_content_length_allowed() const noexcept;
    [[nodiscard]] constexpr bool transfer_encoding_allowed() const noexcept;
    [[nodiscard]] std::uint64_t buffered_representation_length(const http_response& response) const noexcept;
};

[[nodiscard]] http_response_body_plan plan_http_response_body(http_known_method request_method,
                                                              http_status_code response_status) noexcept;

class http_buffered_response_write_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept;
    [[nodiscard]] std::uint64_t content_length() const noexcept;
    [[nodiscard]] bool send_body() const noexcept;
    [[nodiscard]] bool matches_response(const http_response& response) const noexcept;
};

[[nodiscard]] http_buffered_response_write_plan plan_buffered_http_response_write(http_known_method request_method,
                                                                                  const http_response& response) noexcept;
```

`http_response_content_semantics`（按以下顺序判定）：

| 成员 | 条件与含义 |
| --- | --- |
| `informational`（0） | 状态为 `1xx` 且不是 `101`：临时响应，无内容。 |
| `protocol_switch`（1） | `101 Switching Protocols`：连接移交给新协议。 |
| `connect_tunnel`（2） | 请求方法为 `CONNECT` 且状态为 `2xx`：之后是隧道字节，不是消息内容（RFC 9110 §9.3.6）。 |
| `without_content`（3） | 请求方法为 `HEAD`，或状态为 `204` / `304`：响应不携带内容。 |
| `with_content`（4） | 其余情形（包括 `205`，其正文由 `status_allows_body()` 另行禁止）。 |

`plan_http_response_body(method, status)` 是所有协议共用的唯一正文规则；`http_response_body_plan` 只能由它构造，各访问器返回：

| 访问器 | 规则 |
| --- | --- |
| `request_method()` / `response_status()` / `content_semantics()` | 规划输入与上表分类。 |
| `status_allows_body()` | 状态不是 `1xx`、`204`、`205`、`304`。 |
| `body_suppressed()` | `!status_allows_body()`，或分类不是 `with_content`（即 HEAD、CONNECT 2xx 也抑制正文）。 |
| `auto_content_length_allowed()` | 非 CONNECT 隧道，且状态允许正文或为 `205`（RFC 9110 §15.3.6 允许 `205` 以 `Content-Length: 0` 表明无内容）。 |
| `explicit_content_length_allowed()` | 非 CONNECT 隧道，且状态允许正文或为 `304`（`304` 可携带所选表示的长度）。HEAD 在此为 `true`：显式长度描述 GET 表示，配合 `head_metadata_body()` 保留。 |
| `transfer_encoding_allowed()` | 非 CONNECT 隧道且状态允许正文。 |
| `buffered_representation_length(response)` | 状态不允许正文或为 CONNECT 隧道时为 `0`，否则为响应正文表示的字节长度（文件为区间长度，multipart 为计划总长）。 |

`plan_buffered_http_response_write(method, response)` 为缓冲响应捕获正文计划与规划时的表示长度：

| 成员 | 语义 |
| --- | --- |
| `body_plan()` | 方法/状态正文计划。 |
| `content_length()` | 规划时的 `buffered_representation_length(response)`。 |
| `send_body()` | `!body_plan().body_suppressed() && content_length() != 0`：是否需要传送 payload 字节。为 `false` 时仍可能存在零长度文件片段需要校验（见 [6.1.1](#api-http-response-h)）。 |
| `matches_response(response)` | `response` 的状态与当前表示长度仍与计划一致；HTTP/2、HTTP/3 提交缓冲响应时据此拒绝计划后被修改的响应。 |

### 6.4 `"http_response_stream.h"`
<a id="api-http-response-stream-h"></a>

流式响应（先提交响应头、再逐段写正文、最后可选 Trailer）在协议无关层面的规划：响应 Trailer 校验、分帧选择、Trailer 意图、头提交计划与规范化后的响应头。公开 include [`"http_response.h"`](#api-http-response-h)、[`"http_response_body.h"`](#api-http-response-body-h)、[`"http1_connection_plan.h"`](#api-http1-connection-plan-h)（`http1_framing_kind`）、[`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_status.h"`](#api-http-status-h)。HTTP/1 写出见 [`"http1_server_response.h"`](#api-http1-server-response-h)；Web 层流式写入器见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_response_stream.h`](../ruvia-http/include/http_response_stream.h)

#### 6.4.1 响应 Trailer

```cpp
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
};

[[nodiscard]] http_response_trailer_section validate_http_response_trailers(std::span<const http_header_view> trailers);
template </* 未指定实现类型：拥有元素的临时范围 */ headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(std::initializer_list<http_header_view>) = delete;
```

`validate_http_response_trailers` 按共享的响应 Trailer 规则校验一个完整的终止字段段，成功时返回借用凭证 `http_response_trailer_section`（不能由用户直接构造），不合法时抛 `std::invalid_argument`。凭证只借用输入 span：输入存储须存活到同步的协议提交消费完毕。两个 `= delete` 重载在编译期拒绝拥有元素的右值临时容器（如临时 `std::vector<http_header_view>`）与 `initializer_list`，因为它们会在凭证使用前销毁。`fields()` 返回校验过的字段，`empty()` 表示零字段。

#### 6.4.2 分帧、Trailer 意图与头后阶段

```cpp
struct http_multiplexed_frames final {
    [[nodiscard]] friend constexpr bool operator==(const http_multiplexed_frames&, const http_multiplexed_frames&) noexcept = default;
};

using http_response_stream_framing = std::variant<http1_framing_kind, http_multiplexed_frames>;

enum class http_response_trailer_intent : std::uint8_t { none, present };

[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(const http_response_trailer_section& section) noexcept;

enum class http_response_stream_head_disposition : std::uint8_t { body_open, message_ended };
```

`http_multiplexed_frames` 表示 HTTP/2（RFC 9113 §8.1）与 HTTP/3（RFC 9114 §4.1）以 DATA 帧界定内容、以尾随 HEADERS 帧携带 Trailer；它是无状态标签，任意两个值相等。

`http_response_stream_framing` 描述流式正文如何定界，HTTP/1 取 [`http1_framing_kind`](#api-http1-connection-plan-h) 之一：

- `chunked`：HTTP/1.1 默认。
- `close_delimited`：用于 HTTP/1.0。RFC 9112 §6.1 规定服务器不得向未表明 HTTP/1.1 的客户端发送 `Transfer-Encoding`，因此以关闭连接定界（RFC 9112 §6.3）；流同时声明 `Connection: close`，会话在结束后关闭套接字。
- `known_length`：响应自身声明的合法 `Content-Length` 且无 `Transfer-Encoding`（RFC 9112 §6.3），在 HTTP/1.1 与 HTTP/1.0 上都可定界并保持连接可复用。仅在头提交时选择，不由规划产生。

| 枚举成员 | 含义 |
| --- | --- |
| `http_response_trailer_intent::none`（0） | 本次头提交不预留终止 Trailer 段。 |
| `http_response_trailer_intent::present`（1） | 本次头提交承诺在正文后发送 Trailer 段。 |
| `http_response_stream_head_disposition::body_open`（0） | 初始响应头提交后正文开放，可写正文。 |
| `http_response_stream_head_disposition::message_ended`（1） | 正文被抑制（HEAD / `304` 等），消息在初始头处结束，Trailer 被丢弃。 |

Trailer 段是终止消息元数据，不是可独立排队的旁路；Trailer 是接收方可丢弃的可选元数据（RFC 9110 §6.5），因此无法携带它的分帧（HTTP/1.0 close-delimited、known length、正文被抑制）直接丢弃，只有永不携带内容的状态在头之前拒绝该意图。`response_trailer_intent(section)`：空段返回 `none`，否则 `present`——空段不是“未决定”，而是“不发送”的决定，必须在任何正文字节发出前由头声明。

#### 6.4.3 `class http_response_stream_commit_plan`

```cpp
class http_response_stream_commit_plan final {
public:
    [[nodiscard]] http_response_stream_framing framing() const noexcept;
    [[nodiscard]] http_response_body_plan body_plan() const noexcept;
    [[nodiscard]] bool trailers_available() const noexcept;
    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept;
    [[nodiscard]] bool status_permits_trailers() const noexcept;
    [[nodiscard]] bool trailer_intent_allowed() const noexcept;
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(
    http_response_stream_framing framing, http_known_method request_method,
    http_status_code response_status, http_response_trailer_intent trailer_intent) noexcept;
```

`plan_http_response_stream_commit` 以 `plan_http_response_body(request_method, response_status)` 得到正文计划并记录分帧与 Trailer 意图；全部派生事实都由 `body_plan()` 计算，不保存第二份状态码规则。

| 成员 | 语义 |
| --- | --- |
| `framing()` / `body_plan()` | 规划输入的分帧与 [正文计划](#api-http-response-body-h)。 |
| `trailers_available()` | 正文未被抑制，且分帧为 HTTP/1 `chunked`（RFC 9112 §7.1.2 要求 chunked 才能携带 Trailer）或多路复用帧。 |
| `head_disposition()` | 正文被抑制时 `message_ended`，否则 `body_open`。 |
| `status_permits_trailers()` | `body_plan().status_allows_body()`：`1xx`/`204`/`205`/`304` 没有可被 Trailer 终止的消息体。 |
| `trailer_intent_allowed()` | 未声明 `present`，或状态允许 Trailer。为 `false` 表示处理器错误（向不可能有内容的状态请求 Trailer）；其他不可用分帧只丢弃 Trailer。 |

#### 6.4.4 `class http_response_stream_head`

```cpp
class http_response_stream_head final {
public:
    [[nodiscard]] http_response& response() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_response& response() && = delete;
    [[nodiscard]] const http_response& response() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response& response() const&& = delete;
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;
};

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(http_response response,
                                                                          http_response_stream_commit_plan commit_plan);
```

`prepare_http_response_stream_head` 接管 `response`，把初始响应元数据规范化为与 `commit_plan` 一致，返回持有二者的 `http_response_stream_head`（只能由该函数构造）。应用设置的 `Transfer-Encoding` 必须恰好是 `chunked` coding（忽略 OWS 与 ASCII 大小写），它只是提示，由框架写出器替换为自身分帧；其他值在响应变更之前抛异常：HTTP/1 分帧抛 `http1_response_head_error`（见 [`"http1_server_response.h"`](#api-http1-server-response-h)），HTTP/2 / HTTP/3 抛 `std::invalid_argument`。

`response()` / `commit_plan()` 借用本对象，右值重载删除。之后再修改 `response()` 可能破坏与计划的一致性：本对象不是对可变响应的永久证明。

## 7. 内容编码、Multipart 与 SSE

### 7.1 `"http_content_coding.h"`
<a id="api-http-content-coding-h"></a>

内容编码与传输编码共用的唯一 coding 模型、有界编码栈、接收方 `Content-Encoding` 解析，以及动态压缩/预压缩的媒体类型可压缩性判定。依赖 [`"http_header.h"`](#api-http-header-h)、[`"http_status.h"`](#api-http-status-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。响应编码协商（`Accept-Encoding`）见 [`"http_content_negotiation.h"`](#api-http-content-negotiation-h)。源码：[`ruvia-http/include/http_content_coding.h`](../ruvia-http/include/http_content_coding.h)

#### 7.1.1 coding 与编码栈

```cpp
namespace ruvia {
class http_request;

enum class http_content_coding : std::uint8_t {
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
};

[[nodiscard]] std::string_view http_content_coding_token(http_content_coding coding) noexcept;
[[nodiscard]] std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept;
inline constexpr std::size_t max_content_codings = 8;
[[nodiscard]] constexpr bool is_http_transfer_coding(http_content_coding coding) noexcept;
[[nodiscard]] inline bool http_is_gzip_coding_token(std::string_view token) noexcept;
[[nodiscard]] inline constexpr std::string_view http_supported_request_content_codings() noexcept;

class http_content_codings final {
public:
    void push_back(http_content_coding coding);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const http_content_coding* data() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_content_coding* data() const&& = delete;
    [[nodiscard]] const http_content_coding* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_content_coding* begin() const&& = delete;
    [[nodiscard]] const http_content_coding* end() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_content_coding* end() const&& = delete;
    [[nodiscard]] const http_content_coding& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    const http_content_coding& operator[](std::size_t) const&& = delete;
};

enum class http_content_decode_error : std::uint8_t {
    unsupported_coding,
    invalid_content,
    decoded_size_exceeded,
};
}
```

| `http_content_coding` | token | 规范 |
| --- | --- | --- |
| `identity`（0） | `identity` | 无编码。 |
| `gzip`（1） | `gzip`（接收时 `x-gzip` 等同，RFC 9110 §8.4.1.3） | RFC 1952。 |
| `deflate`（2） | `deflate` | zlib 封装格式（RFC 1950，内含 RFC 1951 deflate 数据）；编码输出 zlib 头，解码要求 zlib 头，不接受 raw deflate 流。 |
| `brotli`（3） | `br` | RFC 7932。 |
| `zstd`（4） | `zstd` | RFC 8878，窗口受 RFC 9659 的 HTTP 上限（8 MiB）约束。 |

| 符号 | 语义 |
| --- | --- |
| `http_content_coding_token(c)` | 上表 token。 |
| `http_content_coding_from_token(token)` | 去除首尾 OWS 后大小写不敏感匹配；`x-gzip` 为 `gzip`；空或其他 token 返回 `std::nullopt`。 |
| `max_content_codings` | `8`。编码栈深度上限，防止对端构造无界解码级数：更长的 `Content-Encoding` 栈报告为不支持，更长的 `Transfer-Encoding` 栈被拒绝。 |
| `is_http_transfer_coding(c)` | 仅 `gzip` 与 `deflate` 为 `true`：RFC 9112 §7.2 只注册这两种压缩传输编码，`br`、`zstd` 只能作为内容编码。 |
| `http_is_gzip_coding_token(token)` | 已修剪 token 是否为 `gzip` 或 `x-gzip`（大小写不敏感）。 |
| `http_supported_request_content_codings()` | 请求正文可解码的编码，以 `Accept-Encoding` 字段值形式给出：`"gzip, deflate, br, zstd"`。 |

`http_content_codings` 是内容编码与传输编码共用的有界内联编码栈，按应用顺序存放；不持有内存资源，因此解析得到的分帧计划可以比其来源首部存储活得更久。`push_back` 在已有 `max_content_codings` 项时抛 `std::length_error`。`data` / `begin` / `end` / `operator[]` 借用本对象（`operator[]` 不检查下标），右值重载均删除。

`http_content_decode_error` 描述内容编码与传输编码解码共同的数据错误：

| 成员 | 含义 |
| --- | --- |
| `unsupported_coding`（0） | 当前构建缺少该内容编码的编解码器（仅用于内容编码）。 |
| `invalid_content` | 编码数据损坏、截断或带有不允许的尾随字节。 |
| `decoded_size_exceeded` | 解码结果（或任一中间表示、或解码器声明所需的工作内存）超过调用方上限。 |

#### 7.1.2 `Content-Encoding` 字段解析

```cpp
class http_unsupported_content_coding final {
public:
    constexpr http_unsupported_content_coding() noexcept = default;
    explicit constexpr http_unsupported_content_coding(bool depth_exceeded) noexcept;
    [[nodiscard]] static constexpr http_status_code status() noexcept;
    [[nodiscard]] constexpr bool depth_exceeded() const noexcept;
};

class http_invalid_content_coding_field final {
public:
    [[nodiscard]] static constexpr http_status_code status() noexcept;
};

class http_content_coding_field_result final {
public:
    http_content_coding_field_result(const http_content_coding_field_result&) = default;
    http_content_coding_field_result(http_content_coding_field_result&&) noexcept = default;
    http_content_coding_field_result& operator=(const http_content_coding_field_result&) = delete;
    http_content_coding_field_result& operator=(http_content_coding_field_result&&) = delete;
    [[nodiscard]] std::span<const http_content_coding> codings() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http_content_coding> codings() const&& = delete;
    [[nodiscard]] const http_unsupported_content_coding* unsupported() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_unsupported_content_coding* unsupported() const&& = delete;
    [[nodiscard]] const http_invalid_content_coding_field* invalid() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_invalid_content_coding_field* invalid() const&& = delete;
};

[[nodiscard]] http_content_coding_field_result parse_http_content_coding(std::string_view value) noexcept;
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(const http_header_range_view& headers) noexcept;
[[nodiscard]] http_content_coding_field_result request_content_coding(const http_request& request) noexcept;
[[nodiscard]] bool is_http_media_type_compressible(std::string_view content_type_value) noexcept;
```

`http_content_coding_field_result` 是三态之一（只能由解析函数产生，可复制/移动构造、不可赋值）：

| 状态 | 观察 | 含义 |
| --- | --- | --- |
| 可解码栈 | `unsupported() == nullptr && invalid() == nullptr`；`codings()` 按应用顺序列出（空 span 即 identity） | 本库可解码的编码栈。 |
| 不支持 | `unsupported() != nullptr`，`codings()` 为空 | 语法合法但含不支持的 coding，或栈深超过 `max_content_codings`（`depth_exceeded() == true`）。`status()` 为 `415 Unsupported Media Type`。 |
| 非法 | `invalid() != nullptr`，`codings()` 为空 | 字段语法错误；`status()` 为 `400 Bad Request`。 |

必须先检查 `invalid()` / `unsupported()` 再使用 `codings()`：错误状态下的空 span 不表示 identity。所有观察器借用结果对象，右值重载删除。`http_unsupported_content_coding` 默认构造的 `depth_exceeded()` 为 `false`。

| 函数 | 语义 |
| --- | --- |
| `parse_http_content_coding(value)` | 按接收方列表规则解析一个逻辑 `Content-Encoding` 字段值；空成员被忽略，空值即 identity。 |
| `parse_http_content_coding_headers(headers)` | 把 [`http_header_range_view`](#api-http-header-h) 中所有 `Content-Encoding` 行折叠为一个接收方判定；无字段或只有空成员即 identity。 |
| `request_content_coding(request)` | 使用解析器已分类的首部描述符读取已解析请求的 `Content-Encoding`，不重新扫描线上名称；请求接纳阶段不解释该字段。 |
| `is_http_media_type_compressible(content_type)` | 动态响应压缩与静态预压缩共用的白名单：`text/*`、`+json`、`+xml` 后缀以及常见文本、字体、wasm、图标媒体类型为 `true`；名称含 zip 容器的类型为 `false`。 |

### 7.2 `"http_content_codec.h"`
<a id="api-http-content-codec-h"></a>

整段缓冲编解码门面、唯一的内容编码器（整段与增量）、唯一的流式内容解码器、编解码器内部故障异常，以及每 worker 的编码器池与 RAII 租约。公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"http_codec_memory.h"`](#api-http-codec-memory-h)、[`"http_content_coding.h"`](#api-http-content-coding-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。Web 层缓冲响应的压缩预算策略见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_content_codec.h`](../ruvia-http/include/http_content_codec.h)

#### 7.2.1 异常、选项与整段门面

```cpp
class http_content_codec_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct http_content_encode_options final {
    std::size_t max_encoded_bytes_{};
};

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    http_codec_memory* memory_{nullptr};
};

struct http_content_encoder_options final {
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    http_codec_memory* memory_{nullptr};
    std::optional<int> compression_level_{};
};

[[nodiscard]] std::variant<std::string, http_content_decode_error>
decode_http_content(http_content_coding coding, std::string_view input, http_content_decode_options options);
[[nodiscard]] std::variant<std::string, http_content_decode_error>
decode_http_content(std::span<const http_content_coding> codings, std::string_view input, http_content_decode_options options);
[[nodiscard]] std::optional<std::string>
encode_http_content(http_content_coding coding, std::string_view input, http_content_encode_options options, http_content_encoder_options encoder_options = {});
```

错误分层：

- `http_content_codec_error`：编解码器无法运行（原生状态无法创建或重置、库报告内部错误）。它是内部故障，从不描述表示本身；编码、解码、增量编码器与传输解码器统一抛出它。分配异常保留原类型（`std::bad_alloc` 等）。
- `http_content_decode_error`（值）：表示本身的数据错误，见 [7.1.1](#api-http-content-coding-h)。
- 编码返回 `std::nullopt`：仅表示超出 `max_encoded_bytes_`，属正常结果。

三个选项结构是本头唯一的上限与内存配置来源：

| 字段 | 默认 | 契约 |
| --- | --- | --- |
| `http_content_encode_options::max_encoded_bytes_` | `0` | 一个完整表示的精确编码输出上限；会超出时编码返回 `std::nullopt`，`0` 只允许零字节编码。 |
| `http_content_decode_options::max_decoded_bytes_` | `0` | 累计解码输出的精确上限；超出报告 `decoded_size_exceeded`。 |
| `max_memory_bytes_`（`http_content_decode_options` / `http_content_encoder_options`） | `SIZE_MAX` | 单个原生 codec 的工作内存上限，只计负载字节；`SIZE_MAX` 表示不另设单 codec 上限。显式上限被超出时原生分配失败，以 `std::bad_alloc` 抛出且 codec 进入失败态（Brotli 解码的例外见下文）。identity 不使用原生内存。 |
| `memory_`（`http_content_decode_options` / `http_content_encoder_options`） | `nullptr` | 可选的请求共享预算与分配回调（[7.6](#api-http-codec-memory-h)），与 `max_memory_bytes_` 同时生效；`nullptr` 使用 `operator new` / `delete` 且无共享预算。所指对象须活过使用它的 codec。 |
| `http_content_encoder_options::compression_level_` | `std::nullopt` | `std::nullopt` 使用该 coding 的默认级别。显式值须在 gzip/deflate `-1..9`、Brotli `0..11`、zstd `ZSTD_minCLevel()..ZSTD_maxCLevel()` 内；identity 拒绝任何显式级别。违反时编码器构造抛 `std::invalid_argument`。 |

| 函数 | 语义 |
| --- | --- |
| `decode_http_content(coding, input, options)` | 解码一个完整表示。gzip 多成员与 zstd 多帧一直消费到输入末尾；Brotli 尾随字节被拒绝（`invalid_content`）。精确解码上限 `max_decoded_bytes_` 适用于整个表示；超过上限的 identity 表示在分配前即被拒绝（`decoded_size_exceeded`）。空表示对任何 coding 都解码为空内容。 |
| `decode_http_content(codings, input, options)` | 按 `codings`（应用顺序，即 `Content-Encoding` 顺序）逆序逐层解码；上述规则逐层适用，精确上限约束每个中间表示，每个中间表示在下一级完成前释放。 |
| `encode_http_content(coding, input, options, encoder_options = {})` | 等价于新建 `http_content_encoder(coding, encoder_options)` 并调用一次 `encode(input, options)`；构造与编码异常同 `http_content_encoder`。 |

解码器工作内存同样受解码上限约束：`br` 声明的环形缓冲超过该上限可容纳的大小时（约为上限的三倍加 3 MiB 固定表空间）直接返回 `decoded_size_exceeded`，不按对端声明的窗口分配；若显式 `max_memory_bytes_` 比该推导上限更紧，则按上表抛 `std::bad_alloc`。`zstd` 解码器按 RFC 9659 接受最大 8 MiB 窗口（协议要求的下限）。编码侧 Brotli 一律使用 19 位窗口，zstd 编码窗口不超过 RFC 9659 的 8 MiB。

#### 7.2.2 `class http_content_encoder`

```cpp
class http_content_encoder final {
public:
    explicit http_content_encoder(http_content_coding coding, http_content_encoder_options options = {});
    ~http_content_encoder();
    http_content_encoder(const http_content_encoder&) = delete;
    http_content_encoder& operator=(const http_content_encoder&) = delete;
    http_content_encoder(http_content_encoder&&) = delete;
    http_content_encoder& operator=(http_content_encoder&&) = delete;
    [[nodiscard]] http_content_coding coding() const noexcept;
    void write(std::string_view input, std::string& output, bool flush = false);
    void finish(std::string& output);
    [[nodiscard]] std::optional<std::string> encode(std::string_view input, http_content_encode_options options);
    void reset();
    [[nodiscard]] decoder_state state() const noexcept;
};
```

唯一的内容编码器：整段 `encode()` 与增量 `write()` / `finish()` 运行同一编解码流。不可复制、不可移动。

| 成员 | 语义 |
| --- | --- |
| 构造 | 未知 coding 或 `compression_level_` 违反 [7.2.1](#api-http-content-codec-h) 选项表抛 `std::invalid_argument`；原生状态无法创建抛 `http_content_codec_error`，内存上限或分配失败抛 `std::bad_alloc`（回调异常保留原类型）。 |
| `coding()` | 构造时的 coding。 |
| `write(input, output, flush = false)` | 消费全部 `input` 并把编码字节追加到调用方拥有的 `output`，不保留任一视图。`input` 可以借用 `output`，包括末尾的 NUL；压缩写入在追加可能使其失效前先做快照。`flush == true` 时输出所有待发字节，用于 SSE 等低延迟流。空输入合法（包括重复 flush），不结束流。 |
| `finish(output)` | 结束流并追加剩余字节；幂等。 |
| `encode(input, options)` | 在 `max_encoded_bytes_` 精确输出上限内编码一个完整表示（上限语义见 [7.2.1](#api-http-content-codec-h)）。只接受新流；增量写入已消费输入或已刷新编解码器时，抛 `std::logic_error` 且不改变流，调用方仍可继续 `write()` 或 `finish()`。 |
| `reset()` | 让任意阶段的编码器回到全新活动流。zlib 与 zstd 复用工作内存；Brotli 无原生重置，重建状态可能分配。重置失败（抛异常）使编码器保持 `failed`。 |
| `state()` | `active`：可 `write()`，新流还可 `encode()`；`finished`：成功 `finish()` 或 `encode()`（含返回 `std::nullopt`）之后；`failed`：编码或重置过程中抛出异常后。只有成功的 `reset()` 回到初始状态。 |

状态规则：编码过程中抛出的异常使流进入终态，因为输入/输出可能已部分推进。`finished` 时 `write()` / `encode()` 抛 `std::logic_error` 且不改变 `output`，`finish()` 为无操作；`failed` 时 `write()` / `finish()` / `encode()` 抛 `std::logic_error`。`coding()` 与 `state()` 在任何状态可用。`output` 可以在调用之间清空，并在编码器销毁后仍有效。分配异常在编解码器经其 C API 返回时保留原类型；内部 OOM 时直接退出的 Brotli 构建无法在此边界恢复。

#### 7.2.3 `class http_worker_codecs` 与 `class http_content_encoder_lease`

```cpp
class http_worker_codecs;

class http_content_encoder_lease final {
public:
    http_content_encoder_lease(const http_content_encoder_lease&) = delete;
    http_content_encoder_lease& operator=(const http_content_encoder_lease&) = delete;
    http_content_encoder_lease(http_content_encoder_lease&& other) noexcept;
    ~http_content_encoder_lease();
    [[nodiscard]] http_content_encoder& encoder() & noexcept RUVIA_LIFETIMEBOUND;
    http_content_encoder& encoder() && = delete;
    void discard() noexcept;
};

class http_worker_codecs final {
public:
    http_worker_codecs() noexcept = default;
    ~http_worker_codecs();
    http_worker_codecs(const http_worker_codecs&) = delete;
    http_worker_codecs& operator=(const http_worker_codecs&) = delete;
    [[nodiscard]] http_content_encoder_lease acquire_encoder(http_content_coding coding);
};
```

`http_worker_codecs` 是每 worker 的可复用内容编码器池，缓冲与流式响应共用，非线程安全（只能由所属 worker 使用）。`acquire_encoder(coding)` 从对应 coding 的池中取出或新建编码器，返回租约；`identity` 没有编码器，抛 `std::logic_error`。每种 coding 最多池化 8 个空闲编码器，归还时池已满或 `reset()` 失败则销毁该编码器。池析构只销毁空闲编码器；租约保存指向池的指针并在析构时归还，因此租约必须先于池销毁（或先 `discard()`）。

`http_content_encoder_lease` 是池化编码器的 RAII 租约（只能由池创建；可移动，移动后源租约为空）：

| 成员 | 语义 |
| --- | --- |
| `encoder() &` | 借用租用的编码器，有效至租约销毁、`discard()` 或被移动；右值重载删除，防止从临时租约取得悬垂引用。 |
| `discard() noexcept` | 销毁编码器而不归还（用于失败的编码器）；之后租约为空。 |
| 析构 | 非空且未处于异常展开中时，重置编码器并归还池；若析构发生于异常展开期间（`std::uncaught_exceptions()` 与创建时不同），则销毁编码器而不归还，失败的原生状态绝不跨表示复用。 |

```cpp
ruvia::http_worker_codecs codecs;
auto lease = codecs.acquire_encoder(ruvia::http_content_coding::gzip);
std::string out;
lease.encoder().write(chunk, out, /*flush=*/true);
lease.encoder().finish(out);
// lease 析构：reset() 后归还池
```

#### 7.2.4 `class http_content_decoder`

```cpp
using http_content_decode_failure = decode_failure<http_content_decode_error>;
using http_content_decode_result =
    decode_step<decode_need_input, decode_output, decode_complete, http_content_decode_failure>;

class http_content_decoder final {
public:
    http_content_decoder(http_content_coding coding, http_content_decode_options options);
    ~http_content_decoder();
    http_content_decoder(const http_content_decoder&) = delete;
    http_content_decoder& operator=(const http_content_decoder&) = delete;
    http_content_decoder(http_content_decoder&&) = delete;
    http_content_decoder& operator=(http_content_decoder&&) = delete;
    [[nodiscard]] http_content_decode_result decode(std::string_view input RUVIA_LIFETIMEBOUND, std::span<char> output RUVIA_LIFETIMEBOUND);
    [[nodiscard]] http_content_decode_result finish_input() noexcept;
    [[nodiscard]] decoder_state state() const noexcept;
    void reset();
};
```

| 符号 | 契约 |
| --- | --- |
| `http_content_decode_failure` | `decode_failure<http_content_decode_error>`，`error()` 见 [7.1.1](#api-http-content-coding-h)。 |
| `http_content_decode_result` | 单步结果：`decode_need_input`、`decode_output`、`decode_complete` 或 `http_content_decode_failure`（[4.8](#api-decode-step-h)）。 |
| 构造 | identity、gzip、deflate、Brotli、zstd 的唯一流式主链；整段解码与传输解码复用，不缓存完整输入。`coding` 不是已知枚举值抛 `std::invalid_argument`；原生状态无法创建抛 `http_content_codec_error`；内存上限或分配失败抛 `std::bad_alloc`。选项见 [7.2.1](#api-http-content-codec-h)，`options.memory_` 须活过解码器。不可复制或移动。 |
| `decode(input, output)` | 不保留 `input`。identity 输出直接借用 `input`（每步至多 `output.size()` 字节），压缩输出借用 `output`。`output` 为空，或压缩 coding 的非空 `input` 与 `output` 重叠，抛 `std::invalid_argument`。累计输出超过 `max_decoded_bytes_` 返回 `decoded_size_exceeded`；deflate、Brotli 流结束后同一输入仍有字节返回 `invalid_content`，gzip、zstd 继续下一成员 / 帧。原生库在同一步既产出有效字节又报告数据错误时，本步先交付这些字节（`state()` 已为 `failed`），下一次 `decode()` / `finish_input()` 返回类型化失败。 |
| `finish_input() noexcept` | 显式 EOF：identity、已到达编码结束边界或从未收到编码字节（空表示）时完成；否则即截断，返回 `invalid_content`。gzip 多成员与 zstd 多帧只在 EOF 确认最终边界。 |
| `state()` | `active` / `finished` / `failed`。终态下 `decode()` / `finish_input()` 消费 0 字节并重放终态结果。任何异常（含上述参数错误）都把解码器置为 `failed` 并保留原异常类型，之后返回失败结果。 |
| `reset()` | 以构造时的 coding 与选项开始新表示：zlib、zstd 复用原生状态，Brotli 重建（可能分配）。无法重启抛 `http_content_codec_error` 或分配异常，解码器保持 `failed`。 |

- **输入借用**：`decode()` 在编译期拒绝右值 `std::basic_string` 和字符初始化列表。传入左值字符串或显式视图时，调用方须保证 identity 输出仍在使用期间输入存储有效。
- **排空顺序**：输出须在复用 `output` 前消费；交付更多输入或 EOF 前，先以 `decode({}, output)` 排空剩余输出。按 `consumed_bytes()` 丢弃输入前缀。

### 7.3 `"http_transfer_coding.h"`
<a id="api-http-transfer-coding-h"></a>

`Transfer-Encoding` 压缩传输编码（`gzip`、`deflate`）的唯一增量解码入口，结果采用 [`decode_step`](#api-decode-step-h) 三态契约。公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"http_content_coding.h"`](#api-http-content-coding-h)、[`"http_protocol.h"`](#api-http-protocol-h)（`protocol_byte_limit`）、[`"borrowed_text.h"`](#api-borrowed-text-h)；`http_codec_memory` 仅前置声明，传入非空预算时须包含 [`"http_codec_memory.h"`](#api-http-codec-memory-h)。HTTP/1 正文解码器如何组合它见 [`"http1_message_body.h"`](#api-http1-message-body-h)。源码：[`ruvia-http/include/http_transfer_coding.h`](../ruvia-http/include/http_transfer_coding.h)

#### 7.3.1 `class http_transfer_coding_stack_decoder`

```cpp
class http_codec_memory;

using http_transfer_coding_decode_failure = decode_failure<http_content_decode_error>;
using http_transfer_coding_decode_result =
    decode_step<decode_need_input, decode_output, decode_complete, http_transfer_coding_decode_failure>;

class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit, http_codec_memory* memory = nullptr);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output RUVIA_LIFETIMEBOUND);
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;
    [[nodiscard]] decoder_state state() const noexcept;
    void reset();
};
```

一次 `decode()` / `finish_input()` 的结果是以下之一：`decode_need_input`（需要更多输入）、`decode_output`（解码字节位于调用方 `output` 存储中）、`decode_complete`（所有编码层结束）、`http_transfer_coding_decode_failure`（数据错误，`error()` 为 [`http_content_decode_error`](#api-http-content-coding-h)）。`consumed_bytes()` 给出本步消费的输入字节数。

| 成员 | 语义 |
| --- | --- |
| 构造 | 复制协议顺序的 `codings` 并逆序解码。`codings` 为空或多于 `max_content_codings`（8），或任一 coding 不满足 `is_http_transfer_coding()`（只允许 `gzip`、`deflate`），抛 `std::invalid_argument`。`decoded_limit` 由每一层独立执行（取 `read_ceiling()`）。`memory` 为所有层共享的 [`http_codec_memory`](#api-http-codec-memory-h) 请求预算，不另设单 codec 上限；`nullptr` 使用标准分配器且无共享预算；非空时须活过解码器。编解码器无法创建抛 `http_content_codec_error`，预算不足或分配失败抛 `std::bad_alloc`。 |
| `decode(input, output)` | 消费输入并把解码字节写入 `output`；解码器处于 `active` 时，`output` 为空或与 `input` 重叠会抛 `std::invalid_argument`，不消费输入也不改变状态。输出视图借用调用方暂存区，复用前须消费；请求更多输入前须先用 `decode({}, scratch)` 排空输出。不保留 `input`。 |
| `finish_input() noexcept` | 提交分帧 EOF；层间仍有待解码字节时返回 `decode_need_input`。线上从未收到字节时各层按空内容完成；线上已收到字节而某内层未收到任何字节时返回 `invalid_content`；未到结束边界的截断层同样返回 `invalid_content`。 |
| `state()` | `active` / `finished` / `failed`。 |
| `reset()` | 用构造时的 codings 与上限开始新的编码体，保留编解码器存储；编解码器无法重启时抛 `http_content_codec_error`，解码器随后不可用。 |

数据错误以类型化失败值返回，并保留精确的线上消费量。编解码器异常（包括分配失败）原样抛出，解码器立即进入 `failed`，后续调用返回 `invalid_content`。已完成或已失败的解码器重复其终态结果且不消费输入（编码结束后多余字节的含义由调用方裁决），只有 `reset()` 开始另一个编码体。解码器须保持在稳定地址直至销毁（不可移动）。

传输编码限制：`Transfer-Encoding` 只接受 `gzip`（含 `x-gzip`）与 `deflate`（RFC 9112 §7.2）；`br`、`zstd`、`identity` 及未知 coding 由 HTTP/1 解析报告为不支持（`501`），见 [`"http1_message_body.h"`](#api-http1-message-body-h)。

### 7.4 `"http_multipart.h"`
<a id="api-http-multipart-h"></a>

`multipart/form-data`（RFC 7578，边界语法 RFC 2046）的边界提取、一次性缓冲解析与流式增量解析。畸形或超限的对端字节以值报告，不抛异常。依赖 [`"decode_step.h"`](#api-decode-step-h)、[`"http_io_buffer.h"`](#api-http-io-buffer-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"http_status.h"`](#api-http-status-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。Web 层 `c.req().multipart()` / `multipart_reader` 见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_multipart.h`](../ruvia-http/include/http_multipart.h)

#### 7.4.1 `class multipart_boundary` 与边界提取

```cpp
class multipart_boundary final {
public:
    explicit multipart_boundary(std::string_view value);
    [[nodiscard]] static std::optional<multipart_boundary> try_from_value(std::string_view value) noexcept;
    [[nodiscard]] constexpr std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view value() const&& = delete;
};

enum class multipart_boundary_error : std::uint8_t {
    not_form_data,
    invalid_boundary,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(multipart_boundary_error error) noexcept;
[[nodiscard]] std::variant<multipart_boundary, multipart_boundary_error>
parse_multipart_boundary(std::string_view content_type_value);
```

`multipart_boundary` 以内联存储（最多 70 字节）保存校验过的 RFC 2046 边界值，不分配，所有解析器消费同一值，不会引用悬垂的 `Content-Type` 子串。合法值：1–70 字节，由 `0-9A-Za-z'()+_,-./:=?` 与空格组成，且不以空格结尾。显式构造遇非法值抛 `std::invalid_argument`；`try_from_value` 返回 `std::nullopt` 而不抛。`value() const&` 借用本对象，右值重载删除。

`parse_multipart_boundary(content_type)` 从 `multipart/form-data` 的 `boundary` 参数提取边界：MIME quoted-pair 被解码进拥有的 `multipart_boundary`；空参数槽被忽略；仍要求非空且唯一的 `boundary`。

| `multipart_boundary_error` | 含义 |
| --- | --- |
| `not_form_data`（0） | 媒体类型不是 `multipart/form-data`。 |
| `invalid_boundary`（1） | 声明是 `multipart/form-data`，但 `boundary` 缺失、重复、为空或语法非法。 |

`make_http_protocol_error(multipart_boundary_error)` 把错误映射为 [`http_protocol_error`](#api-http-protocol-h)（含应答状态）。

#### 7.4.2 解析选项、part 与错误

```cpp
struct multipart_parse_options final {
    multipart_boundary boundary_;
    std::size_t max_parts_{1024};
    std::size_t max_metadata_bytes_{1024 * 1024};
};

class multipart_part final {
public:
    multipart_part(const multipart_part&) = default;
    multipart_part(multipart_part&&) noexcept = default;
    multipart_part& operator=(const multipart_part&) = default;
    multipart_part& operator=(multipart_part&&) noexcept = default;
    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view filename() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view filename() const&& = delete;
    [[nodiscard]] bool has_filename() const noexcept;
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view body() const noexcept;
};

enum class multipart_chunk_phase : std::uint8_t {
    complete,
    first,
    middle,
    last,
};

class multipart_stream_part final {
public:
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view filename() const noexcept;
    [[nodiscard]] constexpr bool has_filename() const noexcept;
    [[nodiscard]] std::string_view content_type() const noexcept;
    [[nodiscard]] std::string_view body() const noexcept;
    [[nodiscard]] constexpr multipart_chunk_phase phase() const noexcept;
};

enum class multipart_parse_error : std::uint8_t {
    incomplete_body,
    invalid_delimiter,
    preamble_too_large,
    part_headers_too_large,
    invalid_part_headers,
    invalid_content_disposition,
    missing_field_name,
    delimiter_line_too_large,
    too_many_parts,
    metadata_too_large,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(multipart_parse_error error) noexcept;
using multipart_parse_failure = decode_failure<multipart_parse_error>;
using multipart_poll_result = decode_step<decode_need_input, multipart_stream_part, decode_complete, multipart_parse_failure>;
using multipart_body_parse_result = std::variant<std::vector<multipart_part>, multipart_parse_error>;
```

`multipart_parse_options`：`boundary_` 无默认值（必须提供）；`max_parts_` 默认 `1024`；`max_metadata_bytes_` 默认 `1024 * 1024`（1 MiB），按累计线上 part 首部字节限制解码后的名称、文件名与元数据。

`multipart_part`（一次性解析结果，不能由用户构造）：`name()` 为 `Content-Disposition` 的 `name`，`filename()` 为 `filename`（`has_filename()` 区分“无 filename 参数”与空 filename），`content_type()` 为 part 的 `Content-Type`（未给出时为空），`body()` 为 part 正文。借用规则：未改写的名称、文件名、`Content-Type` 与正文借用调用方拥有的请求体；折叠续行的首部块与转义名称由对象自身拥有字符串存储。`name` / `filename` / `content_type` 的右值重载删除（可能指向对象内部存储）；`body()` 始终借用原始输入，因此无右值限制。复制赋值先准备全部元数据再提交，失败时两端保持原值；移动不分配。

`multipart_stream_part`（流式 `poll()` 产出，不能由用户构造）：同名访问器语义同上，但所有视图仅在下一次 `feed()`、`finish_input()`、`poll()` 或 `reset()` 之前有效。`phase()`：

| `multipart_chunk_phase` | 含义 |
| --- | --- |
| `complete`（0） | 本块即整个 part 正文。 |
| `first`（1） | 跨多块的 part 的第一块。 |
| `middle`（2） | 中间块。 |
| `last`（3） | 最后一块（part 结束）。 |

| `multipart_parse_error` | 含义 |
| --- | --- |
| `incomplete_body`（0） | 已调用 `finish_input()`（或一次性解析到达输入末尾）时仍处于等待分隔符、part 首部或 part 正文的状态，即收尾分隔符之前输入结束。 |
| `invalid_delimiter` | 首个分隔符查找结果非法，或 part 之后的分隔符行既不是 part 分隔符也不是收尾分隔符。 |
| `preamble_too_large` | 首个分隔符之前的前导内容超过 64 KiB。 |
| `part_headers_too_large` | 单个 part 首部块（含结尾 CRLF CRLF）超过 64 KiB。 |
| `invalid_part_headers` | 首部行缺少 `:`、字段名或字段值非法，或 `Content-Type` 重复或不是合法媒体类型。 |
| `invalid_content_disposition` | `Content-Disposition` 缺失、重复、不是 `form-data`，或其参数语法非法/参数名重复。 |
| `missing_field_name` | `Content-Disposition` 合法但缺少 `name` 参数。 |
| `delimiter_line_too_large` | 分隔符行（含尾随填充）超过 64 KiB。 |
| `too_many_parts` | 开始解析新 part 时已达到 `max_parts_`。 |
| `metadata_too_large` | 累计 part 首部块字节（含结尾 CRLF CRLF）超过 `max_metadata_bytes_`。 |

`make_http_protocol_error(multipart_parse_error)` 把错误映射为 [`http_protocol_error`](#api-http-protocol-h)。`multipart_poll_result` 为 [`decode_step`](#api-decode-step-h)：需要输入、一个借用 part 块、正文结束或失败；解析器自持缓冲输入，`consumed_bytes()` 恒为零。`multipart_body_parse_result` 为全部 part 或首个失败（与 `poll()` 报告的相同）。

#### 7.4.3 `class multipart_parser` 与一次性解析

```cpp
class multipart_parser final {
public:
    explicit multipart_parser(multipart_parse_options options);
    multipart_parser(const multipart_parser&) = delete;
    multipart_parser& operator=(const multipart_parser&) = delete;
    multipart_parser(multipart_parser&&) = delete;
    multipart_parser& operator=(multipart_parser&&) = delete;
    void feed(std::string_view chunk);
    void finish_input() noexcept;
    [[nodiscard]] multipart_poll_result poll();
    [[nodiscard]] decoder_state state() const noexcept;
    void reset() noexcept;
};

[[nodiscard]] multipart_body_parse_result parse_multipart_body(std::string_view body, multipart_parse_options options);
template </* 未指定实现类型：拥有字符的临时字符串 */ body_type>
multipart_body_parse_result parse_multipart_body(body_type&&, multipart_parse_options) = delete;
```

| 成员 | 语义 |
| --- | --- |
| `feed(chunk)` | 把输入复制进解析器自有缓冲（复用 [`http_io_buffer`](#api-http-io-buffer-h)，增长时只复制未消费字节），`chunk` 可借用最近一次 part 正文；分配失败时该正文视图仍有效，可原样重试。解析器不处于活动状态或输入已结束时抛 `std::logic_error`。 |
| `finish_input() noexcept` | 标记外层 HTTP 正文结束，使恰好在 EOF 结束的收尾分隔符与跨块拆分的行可被区分。 |
| `poll()` | 推进解析并返回下一个结果。失败是终态：之后的 `poll()` 重复同一错误。若拥有 part 元数据时抛出分配异常，该 part 保持待定且不消耗限额，资源恢复后可重试 `poll()`。 |
| `state()` | `active` / `finished` / `failed`。 |
| `reset() noexcept` | 以构造时的边界与限额开始新正文。 |

- **缓冲回收**：part 元数据复制完成后即释放超过 4 KiB 的首部展开缓冲。`poll()` 返回正文结束或失败时，以及 `reset()` 时，释放其他超过 4 KiB 的暂存缓冲。最后一个 part 的视图在返回时仍有效。

`parse_multipart_body(body, options)` 无 I/O 地解析完整 `multipart/form-data` 正文，结果中 part 正文借用 `body`；来自折叠首部块的名称、文件名与 `Content-Type` 拥有存储，未改写的首部块继续借用 `body`。`= delete` 模板重载拒绝拥有字符的右值临时字符串（如临时 `std::string`），因为返回的 part 会借用它。

```cpp
const auto boundary = ruvia::parse_multipart_boundary(content_type);
if (const auto* b = std::get_if<ruvia::multipart_boundary>(&boundary)) {
    auto result = ruvia::parse_multipart_body(body, {.boundary_ = *b});
    if (const auto* parts = std::get_if<std::vector<ruvia::multipart_part>>(&result)) {
        for (const auto& part : *parts) { /* part.name(), part.filename(), part.body() 借用 body */ }
    }
}
```

### 7.5 `"http_server_sent_events.h"`
<a id="api-http-server-sent-events-h"></a>

Server-Sent Events（WHATWG HTML “Server-sent events”）的服务端消息格式化与客户端 `text/event-stream` 增量解析。依赖 [`"decode_step.h"`](#api-decode-step-h)、[`"http_protocol.h"`](#api-http-protocol-h)（`protocol_byte_limit`、`default_max_sse_event_bytes`）、[`"borrowed_text.h"`](#api-borrowed-text-h)。Web 层 `sse_writer` 见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_server_sent_events.h`](../ruvia-http/include/http_server_sent_events.h)

#### 7.5.1 `struct sse_message` 与格式化

```cpp
struct sse_message final {
    std::optional<::ruvia::borrowed_text> data_{};
    ::ruvia::borrowed_text event_{};
    std::optional<::ruvia::borrowed_text> id_{};
    std::optional<std::chrono::milliseconds> retry_{};
};

[[nodiscard]] std::string format_sse_message(const sse_message& message);
```

`sse_message` 可在格式化前保存，字段以 [`borrowed_text`](#api-borrowed-text-h) 零拷贝借用文本，同时拒绝临时拥有型字符串，避免保存的消息带着已经悬垂的视图。字段默认均为空/缺失。`data_` 缺失时不输出 `data` 字段（只有 event/id/retry 的块不会派发 MessageEvent）；`data_` 存在但为空时输出 `data:`，派发一个数据为空的事件。

`format_sse_message(message)` 生成一个完整的 UTF-8 事件块，按顺序输出：非空 `event_` 为 `event: <type>`；`id_` 存在时为 `id: <id>`（空 id 输出 `id:`）；`retry_` 存在时为 `retry: <毫秒>`；`data_` 存在时按 CR/LF 换行拆分为多行 `data: <line>`；最后一个空行结束。以下情形抛 `std::invalid_argument`：`event_` 或 `id_` 含 CR/LF、`id_` 含 NUL、`retry_` 为负、任一文本字段不是合法 UTF-8。结果超出字符串容量抛 `std::length_error`。

- **单遍扫描**：`event` 和 `id` 在校验 UTF-8 时同时检查禁用字符；`data` 在校验 UTF-8 时拆分 CR、LF、CRLF，末尾换行保留为空 `data` 行。

示例（Web 层写入同一结构，见 [`examples/streaming.cpp`](../examples/streaming.cpp)）：

```cpp
const std::string frame = ruvia::format_sse_message(
    {.data_ = "heartbeat", .event_ = "tick", .id_ = "2", .retry_ = std::chrono::milliseconds{3000}});
// "event: tick\nid: 2\nretry: 3000\ndata: heartbeat\n\n"
```

#### 7.5.2 `class sse_parser`

```cpp
struct sse_parser_options final {
    protocol_byte_limit event_limit_{protocol_byte_limit::limited(default_max_sse_event_bytes)};
};

enum class sse_parse_error : std::uint8_t {
    event_limit_exceeded,
};

class sse_event final {
public:
    [[nodiscard]] constexpr std::string_view type() const noexcept;
    [[nodiscard]] constexpr std::string_view data() const noexcept;
    [[nodiscard]] constexpr std::string_view last_event_id() const noexcept;
};

using sse_parse_failure = decode_failure<sse_parse_error>;
using sse_parse_result = decode_step<decode_need_input, sse_event, sse_parse_failure>;

class sse_parser final {
public:
    explicit sse_parser(sse_parser_options options = {});
    sse_parser(const sse_parser&) = delete;
    sse_parser& operator=(const sse_parser&) = delete;
    sse_parser(sse_parser&& other) noexcept;
    sse_parser& operator=(sse_parser&&) = delete;
    ~sse_parser() = default;
    [[nodiscard]] sse_parse_result parse(std::string_view input) & RUVIA_LIFETIMEBOUND;
    sse_parse_result parse(std::string_view input) && = delete;
    void finish() noexcept;
    void reset() noexcept;
    [[nodiscard]] decoder_state state() const noexcept;
    [[nodiscard]] std::string_view last_event_id() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view last_event_id() const&& = delete;
    [[nodiscard]] std::optional<std::chrono::milliseconds> reconnection_time() const noexcept;
};
```

`sse_parser_options::event_limit_` 默认 `protocol_byte_limit::limited(default_max_sse_event_bytes)`（16 MiB，见 [`"http_protocol.h"`](#api-http-protocol-h)）。预算涵盖数据行及其 LF、事件类型和待提交的 `event` 或 `id` 字段值，包括 UTF-8 替换产生的字节。注释与被忽略字段不占配额。

| `sse_parse_error` | 含义 |
| --- | --- |
| `event_limit_exceeded`（0） | 一个事件超过 `event_limit_`；解析器进入 `failed`。 |

`sse_event`（WHATWG HTML “Dispatch the event” 派发的一个事件，只能由解析器产生）：`type()` 为事件类型，未命名时为 `"message"`；`data()` 为以 LF 连接的数据行，不带末尾 LF；`last_event_id()` 为派发时的最后事件 ID。视图借用解析器，有效至解析器下一次 `parse()`、`finish()`、`reset()`、移动或销毁；所有视图都是合法 UTF-8。

`sse_parser` 是一个响应正文的 sans-I/O `text/event-stream` 解析器（WHATWG HTML “Parsing an event stream”），接受任意大小的分块：

- 跳过一个前导 UTF-8 BOM；行结束于 CRLF、LF 或 CR（包括跨块拆分的 CRLF）。
- 事件值中的非法 UTF-8 按最大子序列替换为 U+FFFD。
- 处理 `event`、`data`、`id`、`retry` 字段；注释与其他字段被忽略。`id` 遇到 NUL 时即丢弃已缓冲的值并跳过行内剩余字节，NUL 前的字节仍受配额限制；`retry` 仅接受非空 ASCII 数字（超出范围饱和）。
- 每个输入字节只扫描一次：未完成行只保留其字段路由后的值，从不缓存原始行。流结束时丢弃未完成的行或事件。
- **缓冲回收**：处理完字段后，已丢弃或被替换的字段值仅保留不超过 4 KiB 的暂存容量。
- **异常**：解析中若分配失败，丢弃未完成的事件并进入 `failed`，保留已提交的 `Last-Event-ID`，原样抛出异常。

| 成员 | 语义 |
| --- | --- |
| `parse(input) &` | 消费输入直到下一个事件完整、流失败或输入耗尽。结果为 `decode_need_input`、`sse_event` 或 `sse_parse_failure`；`consumed_bytes()` 计数：直到完成事件的空行为止、直到导致失败的字段字节为止，或需要更多输入时为全部输入。调用方以未消费后缀再次调用。非 `active` 状态调用抛 `std::logic_error`。右值重载删除：返回的事件借用解析器，临时解析器会立即悬垂。 |
| `finish() noexcept` | 流结束：`active` 解析器丢弃未完成事件并进入 `finished`；其他状态无效果。 |
| `reset() noexcept` | 唯一离开 `finished` / `failed` 的方式：开始新流，保留不超过 4 KiB 的缓冲容量。 |
| `state()` | `active` / `finished` / `failed`。 |
| `last_event_id() const&` | 空行提交最新合法 `id`（包括空值），转移存储而不复制。流结束或失败时丢弃未提交的 `id`，保留已提交的 `Last-Event-ID`。视图借用解析器，右值重载删除。 |
| `reconnection_time()` | 最近一个合法 `retry` 字段；未收到前为 `std::nullopt`。 |
| 移动构造 | 接管解析进度与重连元数据；已返回事件的视图失效。源对象不再保有重连元数据，进入 `failed`，须 `reset()` 后复用。 |

```cpp
ruvia::sse_parser parser;
std::string_view rest = chunk;
for (;;) {
    auto step = parser.parse(rest);
    rest.remove_prefix(step.consumed_bytes());
    if (const auto* event = step.get_if<ruvia::sse_event>()) {
        handle(event->type(), event->data());       // 在下一次 parse() 前消费
        continue;
    }
    if (step.holds<ruvia::sse_parse_failure>()) { /* event_limit_exceeded */ }
    break;                                           // decode_need_input：等待下一块
}
```

### 7.6 `"http_codec_memory.h"`
<a id="api-http-codec-memory-h"></a>

一个请求内全部原生 codec（内容编码器、内容解码器、传输编码栈解码器）共享的工作内存预算与可选分配回调。只依赖标准库。使用方见 [7.2](#api-http-content-codec-h) 选项表的 `memory_` 与 [7.3](#api-http-transfer-coding-h) 构造参数 `memory`。源码：[`ruvia-http/include/http_codec_memory.h`](../ruvia-http/include/http_codec_memory.h)

```cpp
namespace ruvia {
struct http_codec_allocation_callbacks final {
    void* context_{nullptr};
    void* (*allocate_)(std::size_t bytes, std::size_t alignment, void* context){nullptr};
    void (*deallocate_)(void* pointer, std::size_t bytes, std::size_t alignment, void* context) noexcept {nullptr};
};

class http_codec_memory final {
public:
    explicit http_codec_memory(std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_allocation_callbacks callbacks = {});
    http_codec_memory(const http_codec_memory&) = delete;
    http_codec_memory& operator=(const http_codec_memory&) = delete;
    [[nodiscard]] std::size_t live_bytes() const noexcept;
    [[nodiscard]] std::size_t max_bytes() const noexcept;
};
}
```

| 公开入口 | 契约 |
| --- | --- |
| `http_codec_allocation_callbacks` | `context_` 原样传给两个回调。`allocate_` 返回至少 `bytes` 字节、按 `alignment` 对齐的块，可抛任意异常，返回 `nullptr` 视为 `std::bad_alloc`；`deallocate_` 收到与分配时相同的 `pointer` / `bytes` / `alignment`，不得抛异常。回调收到的 `bytes` 含库内部记录块大小的前缀，因而大于计入预算的负载字节。 |
| `http_codec_memory(max_bytes, callbacks)` | 请求级原生 codec 工作内存预算，默认无上限、使用 `operator new` / `delete`。`allocate_` 与 `deallocate_` 必须同时为空或同时非空，否则抛 `std::invalid_argument`。不引入池、Arena 或 pmr 层。不可复制或移动。 |
| `live_bytes()` / `max_bytes()` | 当前活跃原生负载字节（不含分配器元数据）与构造时上限；全部绑定的 codec 释放后 `live_bytes()` 归零。 |

- **寿命与并发**：非线程安全；其地址及回调上下文必须活到所有绑定的 encoder、decoder 与 transfer decoder 销毁。
- **异常边界**：单次负载请求超过 `max_bytes() - live_bytes()` 时不调用回调、产生 `std::bad_alloc`；回调异常在 C 边界捕获，返回 C++ 后保留原类型重抛。异常后 codec 的状态见 [7.2](#api-http-content-codec-h)、[7.3](#api-http-transfer-coding-h)。

## 8. 目标、源站、转发与连接元数据

### 8.1 `"http_authority.h"`
<a id="api-http-authority-h"></a>

RFC 3986 / RFC 9110 authority 与 HTTP 源站（origin）的借用型值：解析 `Host` / `:authority`、按 scheme 默认端口比较 authority、构造源站并序列化其 authority。端口输入使用 [`"ip_address.h"`](#api-ip-address-h) 的 `port_number`，scheme 使用 [`"http_message.h"`](#api-http-message-h) 的 `http_scheme`，借用输入使用 [`"borrowed_text.h"`](#api-borrowed-text-h) 的 `borrowed_text`。源码：[`ruvia-http/include/http_authority.h`](../ruvia-http/include/http_authority.h)

#### 8.1.1 IP 字面量与 authority 解析

```cpp
namespace ruvia {
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

enum class http_host_kind : std::uint8_t { reg_name, ipv4, ipv6, ipv_future };
enum class http_authority_policy : std::uint8_t { rfc, endpoint };

class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept;
    [[nodiscard]] constexpr http_host_kind host_kind() const noexcept;
    [[nodiscard]] constexpr std::string_view port_text() const noexcept;
    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept;
    [[nodiscard]] constexpr std::uint16_t effective_port(port_number default_port) const noexcept;
};

[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value, http_authority_policy policy = http_authority_policy::rfc) noexcept;
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept;
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, std::optional<port_number> default_port) noexcept;
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `is_valid_http_ipv4_literal` / `is_valid_http_ipv6_literal` | 校验数值 IP 主机语法（RFC 3986 `IPv4address` / `IPv6address`），输入不带 URI 方括号。 |
| `http_authority_port_kind::absent` | authority 中没有 `:port` 部分。 |
| `http_authority_port_kind::empty` | 出现 `:` 但端口为空（如 `example.com:`）；语法上与 `absent` 区分，比较 HTTP 源站时两者都映射为 scheme 默认端口。 |
| `http_authority_port_kind::value` | 显式数值端口（含 `:0`）。 |
| `http_authority_view` | 已校验、借用型的 RFC 3986 authority（不含 userinfo）。只能由 `parse_http_authority` 获得；视图借用传入文本，文本须在使用期间存活且不变。 |
| `host()` | 主机文本；IP-literal 保留方括号（如 `[::1]`）。 |
| `host_kind()` / `port_text()` | 已解析的主机类别与原始端口数字切片（保留前导零）；空端口与缺省端口通过 `port_kind()` 区分。 |
| `port_kind()` | 端口形态，见上表。 |
| `port()` | 仅 `port_kind() == value` 时有值。 |
| `effective_port(default_port)` | 显式端口，否则返回调用方给定的默认值。该数值本身不能保留“缺省/空端口”与“显式端口”的区别；做 authority 等价判断应使用 `http_authorities_equal`。 |
| `parse_http_authority(value, policy)` | 默认 `rfc` 保留 RFC 合法的 IPvFuture、空端口、零端口与前导零。`endpoint` 为严格端点配置策略，额外拒绝这四类输入；两者均拒绝 userinfo、zone 与越界端口。非法返回 `nullopt`。 |
| `parse_http_authority_host(value)` | 解析 `Host` 字段 authority 并返回借用的主机文本（IP-literal 保留方括号）。非法 authority 无值；语法上合法的空主机返回有值的空视图。 |
| `http_authorities_equal(left, right, default_port)` | 主机按大小写不敏感比较，端口先按 scheme 默认端口回填再比较。`default_port` 为 `std::nullopt` 表示无已知 scheme 默认值：此时缺省/空端口只与缺省/空端口相等，不等于任何显式端口（含 `:0`）。任一侧非法则不相等。 |
| `is_valid_http_serialized_origin(value)` | 校验 WHATWG 序列化 origin（`scheme://host[:port]`），不是通用 URI 或 origin 列表校验。 |

端口约定：本头所有端口输入使用 `port_number`（受检整数，见 [`"checked_integer.h"`](#api-checked-integer-h)），合法整数可直接传入，窄化前按原始宽度检查 `0..65535`，越界抛出 `std::invalid_argument`；无异常探测用 `port_number::try_from_value()`。已解析端口以 `std::uint16_t` 返回。零端口与缺省端口语义不同。

#### 8.1.2 `class http_origin_view` 与 `make_http_origin_authority`

```cpp
namespace ruvia {
struct http_origin_options final {
    borrowed_text host_{};
    std::optional<port_number> port_{};
};

class http_origin_view final {
public:
    [[nodiscard]] static http_origin_view for_scheme(http_scheme scheme, http_origin_options options);
    [[nodiscard]] static http_origin_view http(http_origin_options options);
    [[nodiscard]] static http_origin_view https(http_origin_options options);

    [[nodiscard]] constexpr http_scheme scheme() const noexcept;
    [[nodiscard]] constexpr std::string_view host() const noexcept;
    [[nodiscard]] constexpr std::uint16_t port() const noexcept;
};

[[nodiscard]] std::string make_http_origin_authority(const http_origin_view& origin);
}
```

| 符号 | 语义 |
| --- | --- |
| `http_origin_options::host_` | 借用的 RFC 3986 `uri-host`，IP-literal 必须带方括号（如 `"[::1]"`）；存储须在源站值被使用期间存活且不变。 |
| `http_origin_options::port_` | 可选端口；缺省时回填 scheme 默认端口（`http` 80，`https` 443）。 |
| `for_scheme(scheme, options)` | 按运行期 `http_scheme` 构造源站。主机为空抛 `std::invalid_argument`（"HTTP origin host must not be empty"），主机语法非法抛 `std::invalid_argument`（"HTTP origin host is invalid"）；因此可观察到的源站主机总是合法非空。 |
| `http(options)` / `https(options)` | 等价于 `for_scheme(http_scheme::http / https, options)`。 |
| `scheme()` / `host()` / `port()` | 访问器；`host()` 只含 `uri-host`（借用 `options.host_`），端口单独由 `port()` 给出（已回填默认值）。 |
| `make_http_origin_authority(origin)` | 返回拥有型 RFC 3986 authority：保留 IP-literal 方括号，仅当端口不等于 scheme 默认端口时附加 `:port`。返回值拥有字节，源站本身仍是借用。 |

```cpp
auto origin = ruvia::http_origin_view::https({.host_ = "example.com", .port_ = 8443});
std::string authority = ruvia::make_http_origin_authority(origin); // "example.com:8443"
```

### 8.2 `"http_target_origin.h"`
<a id="api-http-target-origin-h"></a>

RFC 9110 §7.4 目标源判定：服务器运行时提供连接事实（是否 TLS、对端是否可信网关），由已解析请求（任一 HTTP 版本，见 [`"http_request.h"`](#api-http-request-h)）得出“接受 / 需按证书校验主机 / 421 Misdirected Request”。零分配。证书匹配本身与监听器行为属于 Web 层，见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_target_origin.h`](../ruvia-http/include/http_target_origin.h)

#### 8.2.1 类型与判定函数

```cpp
namespace ruvia {
class http_request;

struct http_target_origin_connection final {
    bool secured_{false};
    bool trusted_gateway_{false};
};

enum class http_target_origin_action : std::uint8_t {
    accept,
    verify_certificate,
    misdirected,
};

enum class http_target_host_kind : std::uint8_t {
    dns_name,
    ipv4,
    ipv6,
};

class http_target_origin final {
public:
    static constexpr std::size_t max_dns_name_size = 253;

    [[nodiscard]] constexpr http_target_origin_action action() const noexcept;
    [[nodiscard]] constexpr http_target_host_kind host_kind() const noexcept;
    [[nodiscard]] constexpr std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view host() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> address() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint8_t> address() const&& = delete;

    [[nodiscard]] static constexpr http_target_origin accepted() noexcept;
    [[nodiscard]] static constexpr http_target_origin misdirected() noexcept;
    [[nodiscard]] static constexpr http_target_origin dns_name(std::string_view host) noexcept;
    template </* 临时拥有型字符串类型 */ host_type>
    static void dns_name(host_type&&) = delete;
    [[nodiscard]] static constexpr http_target_origin decoded_dns_name(std::string_view host) noexcept;
    [[nodiscard]] static constexpr http_target_origin ip_address(http_target_host_kind kind, std::span<const std::uint8_t> address) noexcept;
};

[[nodiscard]] http_target_origin plan_http_target_origin(const http_request& request, http_target_origin_connection connection) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `http_target_origin_connection::secured_` | 请求经 TLS 安全连接到达。 |
| `http_target_origin_connection::trusted_gateway_` | 对端是受信网关（配置的反向代理），由其为目标 scheme/authority 担保，§7.4 豁免连接检查。 |
| `http_target_origin_action::accept` | 处理前无需在本连接上校验任何内容。 |
| `http_target_origin_action::verify_certificate` | 安全连接上的 `https` 目标：仅当本连接出示的证书对 `host()`（或 `address()`）有效时才处理。 |
| `http_target_origin_action::misdirected` | 本连接不能承载该目标：应答 `421 Misdirected Request`（RFC 9110 §15.5.20）。 |
| `http_target_host_kind::dns_name` | reg-name，已去掉绝对域名的单个末尾点。 |
| `http_target_host_kind::ipv4` / `ipv6` | IP 地址主机。 |
| `max_dns_name_size` | DNS 名最大长度 253。 |
| `action()` / `host_kind()` | 判定结果与主机种类；主机数据仅在 `verify_certificate` 时有意义。 |
| `host()` | `dns_name`：主机文本（已解码百分号编码的 unreserved 八位组，去掉单个末尾点，保留大小写）；`ipv4`/`ipv6` 为空。视图借用本对象（由 `decoded_dns_name` 构造时）或 `dns_name` 的输入；右值重载删除，禁止从临时结果取视图。 |
| `address()` | `ipv4`/`ipv6`：网络字节序地址（4 或 16 字节）；`dns_name` 为空。借用本对象，右值重载删除。 |
| `accepted()` / `misdirected()` | 构造对应判定。 |
| `dns_name(host)` | 借用 `host` 构造 `verify_certificate`；空或超过 253 字节返回 `misdirected()`。删除的模板重载拒绝临时拥有型字符串（如 `std::string` 右值），防止悬垂借用。 |
| `decoded_dns_name(host)` | 把 `host` 复制进对象内部存储（用于解码后的临时文本）；长度规则同 `dns_name`。 |
| `ip_address(kind, address)` | 复制地址字节；`kind` 非 `ipv4`/`ipv6` 或长度不符（4/16）返回 `misdirected()`。 |
| `plan_http_target_origin(request, connection)` | 判定规则见下。 |

`plan_http_target_origin` 规则（按顺序）：

- 受信网关：`accept`。
- 明文连接上的 `https` 目标（absolute-form scheme 或 HTTP/2 `:scheme`，大小写不敏感）：`misdirected`。
- 安全连接上 scheme 为 `https` 的目标（显式，或 origin-form / asterisk-form 隐含，RFC 9112 §3.3）：以 `:authority`（否则 `Host`）的主机构造 `verify_certificate`；IPvFuture 主机（证书无法命名）为 `misdirected`。
- 其余均 `accept`：任一传输上的 `http` 目标、CONNECT 隧道目标（authority-form，或无 `:scheme` 的 HTTP/2 CONNECT）、空 authority 的目标（由服务器配置的默认值补全）。
- 不比较端口：证书命名主机而非端口。

### 8.3 `"http_proxy_forwarding.h"`
<a id="api-http-proxy-forwarding-h"></a>

代理转发辅助：请求目标规范化、`Max-Forwards`（RFC 9110 §7.6.2）、`Connection` 提名字段与逐跳字段判定（RFC 9110 §7.6.1）、禁止出现在 trailer 中的字段名。字段视图类型与 `http_header_field_source` 来自 [`"http_header.h"`](#api-http-header-h)，方法枚举来自 [`"http_message.h"`](#api-http-message-h)，`borrowed_text` 来自 [`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http_proxy_forwarding.h`](../ruvia-http/include/http_proxy_forwarding.h)

#### 8.3.1 请求目标规范化

```cpp
namespace ruvia {
[[nodiscard]] std::string normalize_http_proxy_request_target(std::string_view method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(http_known_method method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(const http_request& request);
}
```

- 文本重载：absolute-form（`scheme://authority...`，scheme 合法且 authority 非空）转为 origin-form，路径为空时补 `/` 并保留查询；`OPTIONS` 且路径和查询都为空时转为 `*`；空目标、`/` 开头、`*`、`CONNECT` 方法以及其他形式原样保留。字符串方法先经 `classify_http_method` 分类。
- `http_request` 重载：authority-form 请求返回 `authority()`；否则返回 `path()`，有查询时附加 `?query`。
- 返回拥有型字符串；可能因分配抛出 `std::bad_alloc`。

#### 8.3.2 `Max-Forwards`

```cpp
namespace ruvia {
enum class http_max_forwards_status : std::uint8_t {
    absent,
    valid,
    invalid,
};

class http_max_forwards_result final {
public:
    [[nodiscard]] static constexpr http_max_forwards_result absent() noexcept;
    [[nodiscard]] static constexpr http_max_forwards_result invalid() noexcept;
    [[nodiscard]] static constexpr http_max_forwards_result valid(std::uint64_t current) noexcept;

    [[nodiscard]] constexpr http_max_forwards_status status() const noexcept;
    [[nodiscard]] constexpr bool is_absent() const noexcept;
    [[nodiscard]] constexpr bool is_valid() const noexcept;
    [[nodiscard]] constexpr bool is_invalid() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> current() const noexcept;
    [[nodiscard]] constexpr bool exhausted() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> decremented() const noexcept;
    [[nodiscard]] std::string format_decremented() const;
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_max_forwards_value(std::string_view field_value, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(std::string_view field_value, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(const http_header_range_view& headers, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `http_max_forwards_status::absent` | 无 `Max-Forwards` 字段。 |
| `http_max_forwards_status::valid` | 单个合法十进制值。 |
| `http_max_forwards_status::invalid` | 非法值或字段重复。 |
| `absent()` / `invalid()` / `valid(current)` | 静态工厂。 |
| `status()` / `is_absent()` / `is_valid()` / `is_invalid()` | 状态观察。 |
| `current()` | `valid` 时为跳数，否则 `std::nullopt`。 |
| `exhausted()` | `valid` 且跳数为 0：代理须自行应答（TRACE/OPTIONS），不得再转发。 |
| `decremented()` | `valid` 且大于 0 时返回减一后的值，否则 `std::nullopt`。 |
| `format_decremented()` | `decremented()` 的十进制文本；无值时返回空字符串。 |
| `parse_http_max_forwards_value(value, max_saturation)` | 去除首尾 OWS 后解析十进制数字，超过 `max_saturation` 时饱和为该值；非数字或空返回 `std::nullopt`。 |
| `parse_http_max_forwards(value, …)` | 单值版本：`valid` 或 `invalid`（不会是 `absent`）。 |
| `parse_http_max_forwards(headers, …)` | 遍历字段块（名称大小写不敏感）：无该字段为 `absent`，出现多于一行为 `invalid`，否则按单值解析。 |

#### 8.3.3 `class http_connection_nominated_fields` 与逐跳判定

```cpp
namespace ruvia {
enum class http_connection_fields_status : std::uint8_t {
    valid,
    invalid,
    budget_exceeded,
};

class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    static constexpr std::size_t capacity = 128;
    static constexpr std::size_t max_input_bytes = 16384;

    void update(borrowed_text connection_field_value) noexcept;
    void update_headers(const http_header_range_view& headers) noexcept;
    template <typename source_type>
        requires http_header_field_source<source_type> && (!std::is_lvalue_reference_v<source_type&&>) && (!std::ranges::borrowed_range<source_type>)
    void update_headers(source_type&&) = delete;
    void update_headers(std::initializer_list<http_header_view>) = delete;

    [[nodiscard]] constexpr http_connection_fields_status status() const noexcept;

    [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
    [[nodiscard]] constexpr bool has_close() const noexcept;
    [[nodiscard]] constexpr bool has_keep_alive() const noexcept;
    [[nodiscard]] constexpr bool has_upgrade() const noexcept;
    [[nodiscard]] constexpr bool has_te() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const std::string_view> tokens() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::string_view> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;
    [[nodiscard]] std::string rebuild_connection_field(bool retain_upgrade = false, bool include_non_standard = true) const;
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(borrowed_text connection_field_value) noexcept;
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(const http_header_range_view& headers) noexcept;
template <typename source_type>
    requires http_header_field_source<source_type> && (!std::is_lvalue_reference_v<source_type&&>) && (!std::ranges::borrowed_range<source_type>)
http_connection_nominated_fields parse_http_connection_nominated_fields(source_type&&) = delete;
http_connection_nominated_fields parse_http_connection_nominated_fields(std::initializer_list<http_header_view>) = delete;

[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;
[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name, const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;
[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name, const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;
[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;
}
```

| 符号 | 语义 |
| --- | --- |
| `http_connection_nominated_fields` | 固定容量借用 token 集合，按 ASCII 大小写不敏感去重，保留首次出现的原始拼写；解析不分配，不复制或小写化文本。输入 owner 必须保持存活且不变；`= delete` 重载拒绝临时拥有型字段容器 / 消息与 `initializer_list`（[`http_header_field_source`](#api-http-header-h)）。 |
| `capacity` / `max_input_bytes` | 上限常量（值见声明）；超出语义只在下方状态表定义。 |
| `update(value)` | 解析一个 `Connection` 字段值（逗号分隔、成员首尾 OWS 被修剪）并并入集合；结果状态见下方状态表。 |
| `update_headers(headers)` | 按出现顺序对每个 `Connection` 行（名称大小写不敏感）调用 `update`，共享同一集合与累计预算。 |
| `status()` | 当前 `http_connection_fields_status`。 |
| `contains(name)` | 大小写不敏感地判断 token 是否被提名。 |
| `has_close()` / `has_keep_alive()` / `has_upgrade()` / `has_te()` | 是否出现 `close` / `keep-alive` / `Upgrade` / `TE`。 |
| `empty()` / `size()` / `tokens()` | token 集合；`tokens()` 借用本对象，右值重载删除。 |
| `non_standard_options()` | 不属于标准连接选项（见 `is_http_standard_connection_option`）的 token 的 ASCII 小写副本。 |
| `rebuild_connection_field(retain_upgrade, include_non_standard)` | 生成转发用的 `Connection` 值：`retain_upgrade` 且提名了 `Upgrade` 时以 `Upgrade` 开头；`include_non_standard` 时追加非标准 token 的 ASCII 小写形式（`", "` 分隔）。可能返回空串。 |
| `parse_http_connection_nominated_fields(...)` | 以默认构造的集合调用 `update(value)` 或 `update_headers(headers)` 后返回；借用与删除重载同上。 |
| `is_http_hop_by_hop_field_name(name)` | 协议逐跳字段：`Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Transfer-Encoding`、`Trailer`、`Upgrade`、`Proxy-Authenticate`、`Proxy-Authorization`（大小写不敏感）。 |
| `is_http_standard_connection_option(option)` | `close` 或上述逐跳字段名（含 `keep-alive`）。 |
| `is_http_hop_by_hop_or_nominated_field(name, nominated, retain_upgrade)` | 协议逐跳字段或被 `Connection` 提名的字段；`retain_upgrade` 且已提名 `Upgrade` 时，`Upgrade` 本身不视为待剥离。 |
| `is_http_hop_by_hop_trailer_field(name, nominated)` | trailer 字段是否为逐跳或被提名字段。 |
| `is_forbidden_http_request_trailer_field_name` / `is_forbidden_http_response_trailer_field_name` | 字段名是否不得出现在请求 / 响应 trailer 中（RFC 9110 §6.5.1）；HTTP/1 chunked、HTTP/2、HTTP/3 的 trailer 收发校验使用同一规则。名称一律按 ASCII 大小写不敏感比较，完整清单见下文。 |

`http_connection_fields_status` 是 `Connection` 解析结果的唯一权威状态（`capacity`、`max_input_bytes` 与空成员上限只在此处定义）：

| 状态 | 进入条件 |
| --- | --- |
| `valid`（0） | 迄今所有 `update` 的成员都是合法字段名 token，且未触及下列任一上限。 |
| `invalid`（1） | 某个非空成员不是合法字段名 token。 |
| `budget_exceeded`（2） | 三个相互独立的上限之一被超出：所有 `update` 累计输入字节将超过 `max_input_bytes`（16384）——在解析该值前判定，该值不并入任何 token；不同 token 数将超过 `capacity`（128），重复 token 不计数；单次 `update`（即一个字段值）内的空成员超过 32 个。 |

- **粘滞**：一旦离开 `valid`，后续 `update` / `update_headers` 为无操作，只有新建对象才能重新开始。
- **部分结果不可用**：失败时集合与 `has_*()` 可能已含失败字段值中失败点之前的 token；观察器与 `is_http_hop_by_hop_or_nominated_field` / `is_http_hop_by_hop_trailer_field` 不检查 `status()`。转发方必须先拒绝任何非 `valid` 状态，不能只使用已解析前缀。

两个 trailer 判定函数的完整禁止集合（名称均按 ASCII 大小写不敏感比较，不在集合中的名称返回 `false`）：

- 请求与响应共同禁止：`Host`、`Content-Length`、`Transfer-Encoding`、`Connection`、`Content-Encoding`、`Content-Type`、`Cookie`、`Expect`、`If-Match`、`If-Modified-Since`、`If-None-Match`、`If-Range`、`If-Unmodified-Since`、`Range`、`Upgrade`、`Authorization`、`TE`、`Trailer`、`Proxy-Authorization`、`Keep-Alive`、`Set-Cookie`、`Max-Forwards`、`Cache-Control`、`Content-Range`、`Proxy-Connection`、`Proxy-Authenticate`。
- 请求额外禁止：`Access-Control-Request-Headers`、`Access-Control-Request-Method`、`Origin`、`Accept-Ranges`。
- 响应额外禁止：
  - 库管理的标准响应字段中，除 `ETag`（RFC 9110 §8.8.3）与 `Accept-Ranges`（§14.3）外全部禁止，即 `Vary`、`Date`、`Server`、`Allow`、`Last-Modified`、`Location`、`Access-Control-Allow-Origin`、`Access-Control-Allow-Credentials`、`Access-Control-Allow-Methods`、`Access-Control-Allow-Headers`、`Access-Control-Max-Age`、`Access-Control-Expose-Headers`。
  - 另禁止 `Age`、`Pragma`、`Expires`、`Warning`、`Retry-After`、`X-Frame-Options`、`Referrer-Policy`、`Clear-Site-Data`、`X-XSS-Protection`、`WWW-Authenticate`、`Permissions-Policy`、`Content-Disposition`、`X-Content-Type-Options`、`Content-Security-Policy`、`Strict-Transport-Security`、`Content-Security-Policy-Report-Only`。
  - 响应 trailer 中允许 `ETag` 与 `Accept-Ranges`；请求 trailer 中 `Accept-Ranges` 仍被禁止。

### 8.4 `"http_client_ip.h"`
<a id="api-http-client-ip-h"></a>

无状态客户端 IP 提取器：当直连对端属于受信代理集合时，按优先级从 `Forwarded`（RFC 7239）、逗号列表型（`X-Forwarded-For`）或单 IP 型转发头解析真实客户端地址，并可选提取 `X-Forwarded-Proto` / `Forwarded` 的 `proto`。依赖 [`"ip_address.h"`](#api-ip-address-h)（`ip_address`、`ip_cidr`）与 [`"http_header.h"`](#api-http-header-h)。Web 层 `trusted_proxy_config` 如何构造该提取器见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_client_ip.h`](../ruvia-http/include/http_client_ip.h)

#### 8.4.1 选项与结果

```cpp
namespace ruvia {
enum class http_forwarded_header_kind : std::uint8_t {
    auto_detect,
    rfc7239_forwarded,
    comma_separated_list,
    single_ip,
};

struct http_forwarded_header_spec final {
    http_forwarded_header_spec() noexcept = default;
    http_forwarded_header_spec(std::string_view name, http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect);
    http_forwarded_header_spec(const char* name, http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect);
    http_forwarded_header_spec(const std::string& name, http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect);

    std::string name_;
    http_forwarded_header_kind kind_{http_forwarded_header_kind::auto_detect};
};

struct http_client_ip_options final {
    http_client_ip_options() noexcept = default;

    bool enabled_{true};
    std::vector<std::string> trusted_proxies_;
    std::vector<http_forwarded_header_spec> forwarding_headers_;
    bool trust_x_forwarded_proto_{false};
};

struct http_client_ip_result final {
    ip_address address_{};
    bool from_forwarded_header_{false};
    std::optional<http_scheme> scheme_{};

    [[nodiscard]] constexpr ip_address address() const noexcept;
    [[nodiscard]] constexpr bool from_forwarded_header() const noexcept;
    [[nodiscard]] constexpr std::optional<http_scheme> scheme() const noexcept;

    friend constexpr bool operator==(const http_client_ip_result&, const http_client_ip_result&) noexcept = default;
};
}
```

| 符号 | 语义 |
| --- | --- |
| `http_forwarded_header_kind::auto_detect` | 按名称推断：`Forwarded` → `rfc7239_forwarded`，`X-Forwarded-For` → `comma_separated_list`，其余 → `single_ip`。 |
| `http_forwarded_header_kind::rfc7239_forwarded` | RFC 7239 元素列表，读取每个元素的 `for=` 与 `proto=`（同一元素内重复参数使该参数失效）。 |
| `http_forwarded_header_kind::comma_separated_list` | 逗号分隔的节点地址列表（`X-Forwarded-For` 形态）。 |
| `http_forwarded_header_kind::single_ip` | 必须恰好一行、不含逗号的单个地址；地址属于受信代理时不采用。 |
| `http_forwarded_header_spec` | 一个转发头名称与种类；三个转换构造函数均复制名称。 |
| `http_client_ip_options::enabled_` | 关闭时 `resolve` 始终返回对端地址。 |
| `trusted_proxies_` | 按 [`ip_cidr::parse`](#api-ip-address-h) 解析受信代理地址或 CIDR。要求主机位为零，不接受 IPv6 zone；不自动扩大受信网段。 |
| `forwarding_headers_` | 按优先级排列的转发头；为空时默认 `{"X-Forwarded-For", "Forwarded"}`。 |
| `trust_x_forwarded_proto_` | 是否把与 `X-Forwarded-For` 对齐的 `X-Forwarded-Proto` 项作为 scheme；仅当每个受信代理都清洗并追加匹配的 `X-Forwarded-For` / `X-Forwarded-Proto` 元素时开启。 |
| `http_client_ip_result::address_` / `address()` | 解析出的客户端地址（IPv4 映射地址已规范化为 IPv4）。 |
| `from_forwarded_header_` / `from_forwarded_header()` | 结果是否来自转发头。 |
| `scheme_` / `scheme()` | 转发头给出的 `http` / `https`；无可信来源时为空。 |
| `operator==` | 逐成员默认比较。 |

#### 8.4.2 `class http_client_ip_extractor`

```cpp
namespace ruvia {
class http_client_ip_extractor final {
public:
    http_client_ip_extractor() noexcept;
    explicit http_client_ip_extractor(const http_client_ip_options& config);

    [[nodiscard]] constexpr bool enabled() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] constexpr bool trust_x_forwarded_proto() const noexcept;
    [[nodiscard]] bool is_trusted_proxy(const ip_address& address) const noexcept;
    [[nodiscard]] bool is_trusted_proxy(std::string_view address_text) const noexcept;

    [[nodiscard]] http_client_ip_result resolve(const ip_address& peer_ip, const http_header_range_view& headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(std::string_view peer_ip, const http_header_range_view& headers) const noexcept;
};
}
```

| 成员 | 语义 |
| --- | --- |
| 默认构造 | 禁用的提取器（`enabled() == false`）。 |
| `http_client_ip_extractor(config)` | 编译选项。受信代理项不是合法 IP/CIDR，或转发头名称不是合法 HTTP 字段名时抛 `std::invalid_argument`。 |
| `enabled()` / `trust_x_forwarded_proto()` | 对应选项。 |
| `empty()` | 禁用或没有受信代理时为 `true`（此时 `resolve` 不会读取转发头）。 |
| `is_trusted_proxy(address)` | 地址是否落入任一受信 CIDR（IPv4 块也匹配 `::ffff:a.b.c.d`）；文本重载解析失败返回 `false`。 |
| `resolve(peer_ip, headers)` | 对端不受信、提取器禁用或为空时返回对端地址（`from_forwarded_header_ == false`）。否则按优先级取第一个存在且能给出结果的转发头：从右向左跳过受信代理跳，取最右的不受信跳为客户端；该跳地址不可解析时回退到其右侧最近的受信代理地址；所有跳都受信时取最左跳。全部转发头都无结果时返回对端地址。只给出 scheme 而无地址时以对端地址补全。 |
| `resolve(std::string_view peer_ip, headers)` | 对端文本无法解析时返回 `std::nullopt`，否则同上。 |

`X-Forwarded-Proto` 仅在 `trust_x_forwarded_proto_`、头种类为 `comma_separated_list` 且名称为 `X-Forwarded-For` 时参与：其项数必须与 `X-Forwarded-For` 链长度相同，取与客户端跳同位置的项。对象构造后无状态，可在多线程中并发调用 `const` 成员。

### 8.5 `"proxy_protocol.h"`
<a id="api-proxy-protocol-h"></a>

[HAProxy PROXY protocol](https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt) v1/v2 的 Sans-I/O 单遍增量解析器，用于一条连接的开头。结果使用 [`"decode_step.h"`](#api-decode-step-h) 的 `decode_step` 三态，地址使用 [`"ip_address.h"`](#api-ip-address-h) 的 `ip_endpoint`。监听器强制前缀的启用方式见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/proxy_protocol.h`](../ruvia-http/include/proxy_protocol.h)

#### 8.5.1 类型与常量

```cpp
namespace ruvia {
enum class proxy_protocol_version : std::uint8_t {
    v1,
    v2,
};

enum class proxy_protocol_command : std::uint8_t {
    local,
    proxy,
};

enum class proxy_protocol_transport : std::uint8_t {
    unspecified,
    stream,
    datagram,
};

struct proxy_protocol_tlv final {
    std::uint8_t type_{0};
    std::span<const std::byte> value_{};
};

struct proxy_protocol_parser_options final {
    std::size_t max_v2_header_bytes_{4096};
};

class proxy_protocol_header final {
public:
    [[nodiscard]] constexpr proxy_protocol_version version() const noexcept;
    [[nodiscard]] constexpr proxy_protocol_command command() const noexcept;
    [[nodiscard]] constexpr proxy_protocol_transport transport() const noexcept;
    [[nodiscard]] constexpr std::optional<ip_endpoint> source() const noexcept;
    [[nodiscard]] constexpr std::optional<ip_endpoint> destination() const noexcept;
    [[nodiscard]] constexpr std::span<const proxy_protocol_tlv> tlvs() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const proxy_protocol_tlv> tlvs() const&& = delete;
};

enum class proxy_protocol_parse_error : std::uint8_t {
    invalid_signature,
    v1_invalid_line_ending,
    v1_line_too_long,
    v1_unsupported_protocol,
    v1_invalid_fields,
    v1_invalid_address,
    v1_invalid_port,
    v2_unsupported_version,
    v2_unassigned_command,
    v2_unassigned_address_family,
    v2_unassigned_transport,
    v2_header_too_large,
    v2_short_address_block,
    v2_tlv_overrun,
};

using proxy_protocol_parse_failure = decode_failure<proxy_protocol_parse_error>;
using proxy_protocol_parse_result = decode_step<decode_need_input, decode_complete, proxy_protocol_parse_failure>;
}
```

| 符号 | 语义 |
| --- | --- |
| `proxy_protocol_version::v1` / `v2` | 文本行格式（§2.1）/ 二进制格式（§2.2）。 |
| `proxy_protocol_command::local` | v2 LOCAL：代理自身发起的连接（如健康检查），接收方保留真实端点。 |
| `proxy_protocol_command::proxy` | 代转客户端连接；v1 恒为 `proxy`。 |
| `proxy_protocol_transport::unspecified` | UNSPEC 传输；v1 `UNKNOWN`。 |
| `proxy_protocol_transport::stream` | 流式（TCP）；v1 `TCP4`/`TCP6`。 |
| `proxy_protocol_transport::datagram` | 数据报（UDP），仅 v2。 |
| `proxy_protocol_tlv` | 一个 v2 TLV：`type_` 与原始 `value_`；`value_` 借用解析器缓冲。 |
| `max_v2_header_bytes_` | 整个 v2 头（16 字节固定部分 + 地址块 + TLV）的上限，默认 4096，必须至少 52（容纳 AF_INET6 地址块 16 + 36 字节）。v1 行始终限制在规范最大 107 字节（含 CRLF）。 |
| `version()` / `command()` / `transport()` | 头字段。 |
| `source()` / `destination()` | PROXY 命令 + AF_INET/AF_INET6 + stream 或 datagram（v1 `TCP4`/`TCP6`）时为原始客户端与其到达的目标；LOCAL、v1 `UNKNOWN`、AF_UNSPEC、AF_UNIX 与 UNSPEC 传输为 `std::nullopt`，接收方应保留真实连接端点（§2.1、§2.2）。 |
| `tlvs()` | 已知地址族的 v2 PROXY 头地址块之后的全部 TLV，原始、按线上顺序；v1、LOCAL（地址块不透明）与 AF_UNSPEC（地址长度未知）为空。借用产生它的解析器，右值重载删除。 |

`proxy_protocol_parse_error` 逐项（任一错误都使解析器以 fail-closed 进入失败态）：

| 枚举值 | 含义 |
| --- | --- |
| `invalid_signature` | 首字节既不是 v1 `"PROXY "` 也不是 12 字节 v2 签名的开头。 |
| `v1_invalid_line_ending` | §2.1：CR 后不是 LF，或出现裸 LF。 |
| `v1_line_too_long` | §2.1：107 字节内没有 CRLF。 |
| `v1_unsupported_protocol` | 协议不是 `TCP4`、`TCP6` 或 `UNKNOWN`。 |
| `v1_invalid_fields` | 不是四个以单空格分隔的字段。 |
| `v1_invalid_address` | 不是所声明地址族的规范文本。 |
| `v1_invalid_port` | 不是 `0..65535` 且无前导零的十进制数。 |
| `v2_unsupported_version` | §2.2：版本半字节不是 2。 |
| `v2_unassigned_command` | 命令半字节未分配。 |
| `v2_unassigned_address_family` | 地址族半字节未分配。 |
| `v2_unassigned_transport` | 传输协议半字节未分配。 |
| `v2_header_too_large` | 长度超过 `max_v2_header_bytes_`。 |
| `v2_short_address_block` | PROXY 命令的长度不足以覆盖其地址块。 |
| `v2_tlv_overrun` | TLV 头或值越过头尾。 |

`proxy_protocol_parse_result` 三态：`decode_need_input`（输入全部被消费，需更多字节）、`decode_complete`（头完整）、`proxy_protocol_parse_failure`（`error()` 给出上表枚举）。`consumed_bytes()` 只计头字节，绝不越过头尾，调用方把 `input[consumed_bytes()..]` 交给下一协议；失败时为 0，且不留下可复用的字节。

#### 8.5.2 `class proxy_protocol_parser`

```cpp
namespace ruvia {
class proxy_protocol_parser final {
public:
    explicit proxy_protocol_parser(proxy_protocol_parser_options options = {});
    proxy_protocol_parser(const proxy_protocol_parser&) = delete;
    proxy_protocol_parser& operator=(const proxy_protocol_parser&) = delete;
    proxy_protocol_parser(proxy_protocol_parser&& other) noexcept;
    proxy_protocol_parser& operator=(proxy_protocol_parser&&) = delete;
    ~proxy_protocol_parser() = default;

    [[nodiscard]] proxy_protocol_parse_result parse(std::span<const std::byte> input) &;
    proxy_protocol_parse_result parse(std::span<const std::byte> input) && = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    [[nodiscard]] const proxy_protocol_header& header() const& RUVIA_LIFETIMEBOUND;
    const proxy_protocol_header& header() const&& = delete;

    void reset() noexcept;
};
}
```

| 成员 | 语义 |
| --- | --- |
| 构造 | `max_v2_header_bytes_ < 52` 抛 `std::invalid_argument`。 |
| 拷贝 / 移动赋值 | 删除。移动构造 `noexcept`：新对象接管解析进度及 TLV 视图借用的缓冲，已有 TLV 视图继续有效。源对象进入 `failed`，复用前须 `reset()`。 |
| `parse(input) &` | 以任意切分的块喂入字节；每字节只扫描一次，只缓冲头字节。首字节必须开始 v1 或 v2 签名，不对其他输入透传。非法或超限输入返回失败并使解析器进入 `decoder_state::failed`；缓冲分配抛异常时同样进入 failed 后传播异常。解析器非 `active` 时调用抛 `std::logic_error`。右值重载删除，避免在临时解析器上解析后视图悬垂。 |
| `state()` | `active` / `finished` / `failed`（见 [`"decode_step.h"`](#api-decode-step-h)）。 |
| `header()` | 仅 `finished` 时可用，否则抛 `std::logic_error`。视图在 `reset()` 或析构前有效；右值重载删除。 |
| `reset()` | 唯一离开 finished / failed 的途径。 |

- **缓冲回收**：头部完成且无 TLV、解析失败或重置时，归还超过 4 KiB 的暂存缓冲。有 TLV 的头部保留视图借用的存储。`LOCAL` 或 `AF_UNSPEC` 的 v2 数据块直接跳过，不拷贝。

```cpp
ruvia::proxy_protocol_parser parser;
auto step = parser.parse(bytes);
if (step.holds<ruvia::decode_complete>()) {
    const auto& header = parser.header();
    auto rest = bytes.subspan(step.consumed_bytes()); // 交给 TLS / HTTP
}
```

### 8.6 `"tls_client_hello.h"`
<a id="api-tls-client-hello-h"></a>

无 OpenSSL 依赖的 TLS ClientHello 原始握手解析（过滤 GREASE），并计算 JA3、JA4（区分 TCP `t` / QUIC `q`）与 PeetPrint 指纹。HTTP/2 Akamai 指纹与 HTTP/3 指纹收集器分别见 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http3_connection.h"`](#api-http3-connection-h)；Web 层的 ClientHello 记录回调见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/tls_client_hello.h`](../ruvia-http/include/tls_client_hello.h)

#### 8.6.1 类型与函数

```cpp
namespace ruvia {
[[nodiscard]] constexpr bool is_tls_grease_value(std::uint16_t value) noexcept;

struct tls_client_hello_fingerprints final {
    std::string ja3_;
    std::string ja3_raw_;
    std::string ja4_;
    std::string peetprint_;
    std::string peetprint_hash_;
};

class tls_client_hello_info final {
public:
    tls_client_hello_info() noexcept = default;
    [[nodiscard]] constexpr std::uint16_t legacy_version() const noexcept;
    [[nodiscard]] std::uint16_t version() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> cipher_suites() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint16_t> cipher_suites() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> extensions() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint16_t> extensions() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_groups() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint16_t> supported_groups() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> ec_point_formats() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint8_t> ec_point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::string> alpn() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::string> alpn() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> signature_algorithms() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint16_t> signature_algorithms() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_versions() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::uint16_t> supported_versions() const&& = delete;
    [[nodiscard]] std::string_view server_name() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view server_name() const&& = delete;

    [[nodiscard]] std::string ja3_raw() const;
    [[nodiscard]] std::string ja3() const;
    [[nodiscard]] std::string ja4(bool quic = false) const;
    [[nodiscard]] std::string peetprint() const;
    [[nodiscard]] std::string peetprint_hash() const;
    [[nodiscard]] tls_client_hello_fingerprints fingerprints(bool quic = false) const;

    std::uint16_t legacy_version_{0};
    std::vector<std::uint16_t> cipher_suites_;
    std::vector<std::uint16_t> extensions_;
    std::vector<std::uint16_t> supported_groups_;
    std::vector<std::uint8_t> ec_point_formats_;
    std::vector<std::string> alpn_;
    std::vector<std::uint16_t> signature_algorithms_;
    std::vector<std::uint16_t> supported_versions_;
    std::string server_name_;
};

[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(std::span<const std::byte> message);
[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message);

[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::span<const std::byte> message, bool quic = false);
[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::string_view message, bool quic = false);
[[nodiscard]] inline tls_client_hello_fingerprints compute_tls_client_hello_fingerprints(const tls_client_hello_info& hello, bool quic = false);
}
```

| 符号 | 语义 |
| --- | --- |
| `is_tls_grease_value(value)` | RFC 8701 GREASE 值判定（`0x?a?a` 且高低字节相等）。 |
| `tls_client_hello_fingerprints` | `ja3_raw_`（JA3 原文）、`ja3_`（其 MD5 小写十六进制）、`ja4_`、`peetprint_`（原文）、`peetprint_hash_`（其 MD5）。 |
| 公开字段 `legacy_version_` … `server_name_` | 解析出的 ClientHello 字段（已过滤 GREASE），按线上顺序；访问器返回同一数据的借用视图，右值重载删除。 |
| `legacy_version()` | `legacy_version` 字段。 |
| `version()` | `supported_versions` 非空时取其最大值，否则 `legacy_version_`。 |
| `cipher_suites()` / `extensions()` / `supported_groups()` / `ec_point_formats()` / `alpn()` / `signature_algorithms()` / `supported_versions()` / `server_name()` | 对应字段的借用视图，有效期至对象修改或销毁。 |
| `ja3_raw()` | `legacy_version,ciphers,extensions,groups,point_formats`（各列表 `-` 连接的十进制）。 |
| `ja3()` | `ja3_raw()` 的 MD5 小写十六进制。 |
| `ja4(quic)` | JA4：前缀 `q`（`quic == true`）或 `t`、版本码、`d`（SNI 为域名）/`i`、两位密码套件数与扩展数（上限 99）、首个 ALPN 首尾字符（非字母数字时为首字节十六进制，无 ALPN 为 `00`）；随后 `_` 分隔的排序密码套件 SHA-256 前 12 位，与排序扩展（去除 SNI `0x0000` 与 ALPN `0x0010`）加签名算法的 SHA-256 前 12 位。 |
| `peetprint()` | 以 `\|` 分隔：supported_versions（无则 legacy_version）、密码套件、扩展、groups、point formats、ALPN（`-` 连接）、签名算法。 |
| `peetprint_hash()` | `peetprint()` 的 MD5。 |
| `fingerprints(quic)` | 一次性计算上述全部字段。 |
| `parse_tls_client_hello(message)` | 输入为完整 ClientHello 握手消息（`msg_type` 1 + 3 字节长度 + 正文），也接受长度恰好匹配的单个 5 字节 TLS 记录头（ContentType 0x16）包装。长度不一致、类型不符或任何字段越界返回 `std::nullopt`；可因分配抛出。 |
| `compute_tls_client_hello_fingerprints(message, quic)` | 解析后计算指纹；解析失败返回 `std::nullopt`。 |
| `compute_tls_client_hello_fingerprints(hello, quic)` | 等价于 `hello.fingerprints(quic)`。 |

### 8.7 `"http_client_redirect.h"`
<a id="api-http-client-redirect-h"></a>

出站 HTTP 重定向协议辅助：按 RFC 9110 §15.4 改写方法与正文处置、分类 authority 是否同源、把 `Location` URI-reference 解析为拥有型 `http`/`https` 目标。不含传输策略：重定向次数、跨源授权、连接选择与重试由外部 I/O 运行时负责（Web 客户端策略见 [ruvia-web](ruvia-web.md)）。依赖 [`"http_authority.h"`](#api-http-authority-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http_client_redirect.h`](../ruvia-http/include/http_client_redirect.h)

#### 8.7.1 请求改写规划

```cpp
namespace ruvia {
enum class http_client_redirect_content_disposition : std::uint8_t {
    preserve,
    drop,
};

struct http_client_redirect_request_plan_options final {
    http_status_code status_;
};

class http_client_redirect_request_plan final {
public:
    http_client_redirect_request_plan(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan& operator=(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan(http_client_redirect_request_plan&&) noexcept = default;
    http_client_redirect_request_plan& operator=(http_client_redirect_request_plan&&) = delete;

    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view method() const&& = delete;
    [[nodiscard]] constexpr http_client_redirect_content_disposition content_disposition() const noexcept;
};

[[nodiscard]] http_client_redirect_request_plan plan_http_client_redirect_request(const http_request_head_view& request, http_client_redirect_request_plan_options options);
}
```

| 符号 | 语义 |
| --- | --- |
| `http_client_redirect_content_disposition::preserve` | 重定向请求保留原表示内容。 |
| `http_client_redirect_content_disposition::drop` | I/O 所有者构造重定向请求时必须同时省略表示内容与内容相关字段（RFC 9110 §15.4）。 |
| `status_` | 重定向响应状态码。 |
| `plan_http_client_redirect_request(request, options)` | `303`：`HEAD` 保持 `HEAD`，其余改为 `GET`，正文 `drop`；`301`/`302` 且方法为 `POST`：改为 `GET` 并 `drop`；其余保持原方法与 `preserve`。可因分配抛出。 |
| `method()` | 改写后的方法，复制在计划内部，不继承请求存储的生命周期；视图借用计划，右值重载删除。 |
| 拷贝 / 移动赋值 | 删除；只可移动构造。 |

#### 8.7.2 同源分类

```cpp
namespace ruvia {
enum class http_client_origin_authority_status : std::uint8_t {
    same_origin,
    different_origin,
    invalid_authority,
};

[[nodiscard]] http_client_origin_authority_status classify_http_client_origin_authority(const http_origin_view& origin, std::string_view authority) noexcept;
}
```

| 枚举值 | 含义 |
| --- | --- |
| `same_origin` | `authority` 的主机与有效端口（按 `origin.scheme()` 默认端口回填）与 `origin` 一致。 |
| `different_origin` | 合法但主机或端口不同。 |
| `invalid_authority` | `authority` 语法非法。 |

比较规则：IPv6 字面量按地址比较（与零压缩、前导零或内嵌 IPv4 尾部写法无关）；其他主机按 URI 大小写与百分号编码等价规则比较；不做 DNS 解析。

#### 8.7.3 目标解析

```cpp
namespace ruvia {
enum class http_client_redirect_resolution_error : std::uint8_t {
    invalid_current_target,
    invalid_location,
    unsupported_scheme,
};

struct http_client_redirect_target_options final {
    borrowed_text current_target_{};
    borrowed_text location_{};
};

class http_client_resolved_redirect final {
public:
    http_client_resolved_redirect(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect& operator=(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect(http_client_resolved_redirect&&) noexcept = default;
    http_client_resolved_redirect& operator=(http_client_resolved_redirect&&) = delete;

    [[nodiscard]] std::string_view target() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view target() const&& = delete;
    [[nodiscard]] http_scheme scheme() const noexcept;
    [[nodiscard]] std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool cross_origin() const noexcept;
    [[nodiscard]] http_origin_view origin() const& RUVIA_LIFETIMEBOUND;
    http_origin_view origin() const&& = delete;
};

[[nodiscard]] std::variant<http_client_resolved_redirect, http_client_redirect_resolution_error> resolve_http_client_redirect_target(const http_origin_view& origin, http_client_redirect_target_options options);
}
```

| 符号 | 语义 |
| --- | --- |
| `invalid_current_target` | `current_target_` 不是合法 origin-form 目标。 |
| `invalid_location` | `Location` 语法非法（含非法片段、`scheme:` 后无 `//`、非法 authority 或路径/查询）。 |
| `unsupported_scheme` | `Location` 的 scheme 不是 `http`/`https`；响应本身合法，由 I/O 所有者决定交出响应还是使交换失败。 |
| `current_target_` / `location_` | 当前请求的 origin-form 目标与 `Location` 字段值（借用）。 |
| `target()` | 目标源站上的 origin-form 目标（拥有，借用视图）。 |
| `scheme()` / `host()` / `port()` | 目标源站；`host()` 为 `uri-host`，IP-literal 保留方括号，与 `http_origin_view` 工厂契约一致。 |
| `cross_origin()` | scheme/host/port 三元组任一与请求源站不同即为 `true`；按 RFC 9110 §15.4 与 Fetch 规范，I/O 所有者跟随前须丢弃凭据（`Authorization`、`Proxy-Authorization`、不属于新源站的 Cookie）。 |
| `origin()` | 由 `host()` 构造的 `http_origin_view`，借用本对象存储，仅在本对象存活期间有效；右值重载删除。 |
| `resolve_http_client_redirect_target(origin, options)` | 去除 `Location` 首尾 OWS 并剥离片段后，按 RFC 3986 相对 `origin` 与 `current_target_` 解析；不同 http/https 源站是成功分支，由 `cross_origin()` 分类，I/O 所有者在其上实施跨源策略（剥离凭据、拒绝 TLS 降级）。可因分配抛出。 |

示例：[`examples/http_client_advanced.cpp`](../examples/http_client_advanced.cpp) 在重定向回调中使用 `resolve_http_client_redirect_target`。

### 8.8 `"http_connection_advertisement.h"`
<a id="api-http-connection-advertisement-h"></a>

HTTP/2、HTTP/3 共用的连接通告值：RFC 8336 / RFC 9412 `ORIGIN` 与 RFC 7838 `ALTSVC` 的拥有型结果与 Sans-I/O 编解码。编码器先校验限制再分配，直接写入最终拥有结果。源站权威性与连接合并需要运行时的 TLS 身份，不在此判定；连接如何发送/接收这些帧见 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http3_connection.h"`](#api-http3-connection-h)。源码：[`ruvia-http/include/http_connection_advertisement.h`](../ruvia-http/include/http_connection_advertisement.h)

#### 8.8.1 类型与编解码

```cpp
namespace ruvia {
struct http_origin_advertisement final {
    std::vector<std::string> origins_;
    http_origin_advertisement() noexcept = default;
    http_origin_advertisement(const http_origin_advertisement&) = default;
    http_origin_advertisement(http_origin_advertisement&&) noexcept = default;
    http_origin_advertisement& operator=(const http_origin_advertisement&) = default;
    http_origin_advertisement& operator=(http_origin_advertisement&&) noexcept = default;
};

struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::string origin_;
    std::string field_value_;
    http_alternative_service_advertisement() noexcept = default;
    http_alternative_service_advertisement(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement(http_alternative_service_advertisement&&) noexcept = default;
    http_alternative_service_advertisement& operator=(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement& operator=(http_alternative_service_advertisement&&) noexcept = default;
};

enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
};

[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error> decode_http_origin_advertisement(std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error> encode_http2_origin_frame(std::span<const std::string_view> origins, std::uint32_t max_frame_size = 16384);
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error> decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error> encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size = 16384);
}
```

| 符号 | 语义 |
| --- | --- |
| `http_origin_advertisement::origins_` | 解码出的合法序列化源站，线上顺序。 |
| `http_alternative_service_advertisement` | `stream_id_`：所在流；`origin_`：流 0 时的显式源站，请求流上为空（继承请求源站）；`field_value_`：`Alt-Svc` 字段值（传输权威性与缓存策略在外部）。 |
| 拷贝/移动 | 两个值类型的拷贝赋值在分配失败时保留两端原值；移动不分配。 |
| `malformed_payload` | 载荷截断：长度前缀不完整或长度超出剩余字节。 |
| `invalid_origin` | 源站不是合法序列化 origin（编码输入，或 ALTSVC 非空 origin）。 |
| `invalid_field` | ALTSVC 字段值不是合法字段值。 |
| `invalid_stream` | ALTSVC 流号与源站不匹配（流 0 必须带 origin，非 0 流必须不带）或超过 31 位。 |
| `limit` | 编码结果超过帧载荷上限。 |
| `decode_http_origin_advertisement(payload)` | 解码 ORIGIN 载荷（2 字节长度前缀的条目序列）；非法序列化源站被忽略，截断条目使整个载荷被拒绝（`malformed_payload`）。 |
| `encode_http2_origin_frame(origins, max_frame_size)` | 编码完整 HTTP/2 `ORIGIN` 帧（9 字节帧头，类型 0xc，流 0）；任一源站非法返回 `invalid_origin`，载荷超过 `min(max_frame_size, 2^24-1)` 返回 `limit`。 |
| `decode_http2_alternative_service(stream_id, payload)` | 解码 HTTP/2 `ALTSVC` 载荷，错误见上表。 |
| `encode_http2_alternative_service_frame(stream_id, origin, field_value, max_frame_size)` | 编码完整 HTTP/2 `ALTSVC` 帧；校验顺序为流号、源站、字段值、长度上限。 |

所有函数可因分配抛出 `std::bad_alloc`。

### 8.9 `"http_priority.h"`
<a id="api-http-priority-h"></a>

RFC 9218 可扩展优先级：`Priority` 字段（RFC 8941 Dictionary）解析与编码、HTTP/2 `PRIORITY_UPDATE` 帧编解码，以及受检的 `http_priority_urgency`。受检整数机制见 [`"checked_integer.h"`](#api-checked-integer-h)；字段块类型来自 [`"http_header.h"`](#api-http-header-h)。源码：[`ruvia-http/include/http_priority.h`](../ruvia-http/include/http_priority.h)

#### 8.9.1 类型

```cpp
namespace ruvia {
class http_priority_urgency final : public checked_integer<http_priority_urgency, std::uint8_t, /* 未指定实现类型 */> {
public:
    using checked_integer::checked_integer;
};

struct http_priority final {
    http_priority_urgency urgency_{};
    bool incremental_{false};
};

struct http_priority_fields final {
    std::optional<http_priority_urgency> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept;
};

enum class http_priority_error : std::uint8_t {
    invalid_syntax,
    invalid_value,
    invalid_frame,
    output_too_small,
};

struct http_priority_update final {
    std::uint64_t element_id_{0};
    bool push_{false};
    http_priority_fields fields_{};
};
}
```

| 符号 | 语义 |
| --- | --- |
| `http_priority_urgency` | RFC 9218 §4.1 urgency，值域 `0..7`，默认构造为 `3`；平凡可复制，大小等于 `std::uint8_t`。策略类型属实现细节；可用操作为 `checked_integer` 提供的全部操作：本地整数可隐式构造，窄化前检查，越界抛 `std::invalid_argument`（"HTTP priority urgency must be between 0 and 7"）；`try_from_value()` 无异常探测；`value()` 取整数；比较运算。不隐式接受浮点或枚举，也不隐式转换回整数。 |
| `http_priority` | 生效优先级：`urgency_` 默认 3，`incremental_` 默认 `false`。 |
| `http_priority_fields` | 解析后的参数，缺省参数保持 `std::nullopt`（供中介合并响应参数）。 |
| `request_priority()` | 用默认值回填缺省参数。 |
| `http_priority_error::invalid_syntax` | 结构化字段语法错误。 |
| `http_priority_error::invalid_value` | PRIORITY_UPDATE 被优先元素 ID 为 0，或编码时流号为 0 / 超过 31 位。 |
| `http_priority_error::invalid_frame` | PRIORITY_UPDATE 载荷不足 4 字节。 |
| `http_priority_error::output_too_small` | 输出缓冲不足。 |
| `http_priority_update` | 解码的 PRIORITY_UPDATE：`element_id_` 被优先流号，`push_` 对偶数 ID（服务器推送流）为 `true`，`fields_` 为优先级参数。 |

接收端按 RFC 9218 §4 忽略越界或类型不符的对端参数，不套用本地构造的异常规则。

#### 8.9.2 自由函数

```cpp
namespace ruvia {
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept;
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(const http_header_range_view& headers) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t stream_id, http_priority_fields fields_value) noexcept;
}
```

| 函数 | 语义 |
| --- | --- |
| `parse_http_priority(value)` | 解析一个 Dictionary：未知成员、参数类型不符与越界值被忽略；结构化语法错误返回 `invalid_syntax`。 |
| `parse_http_priority(headers)` | 按线上顺序读取全部 `Priority` 字段行，不分配；后出现的成员覆盖先前成员（包括非法值）。 |
| `encode_http_priority(output, fields)` | 写入 `u=N`、`i`（`true`）或 `i=?0`（`false`），缺省参数省略；返回写入字节数，缓冲不足返回 `output_too_small`（不写入）。 |
| `decode_http2_priority_update(payload)` | 解码 HTTP/2 `PRIORITY_UPDATE` 载荷（4 字节被优先流号 + Priority 字段值）；错误见上表，字段值错误透传。 |
| `encode_http2_priority_update(output, stream_id, fields)` | 写入完整帧（9 字节帧头 + 4 字节流号 + 字段值），返回总字节数；流号非法返回 `invalid_value`，缓冲不足返回 `output_too_small`。 |

```cpp
auto parsed = ruvia::parse_http_priority("u=2, i");
if (auto* fields = std::get_if<ruvia::http_priority_fields>(&parsed)) {
    ruvia::http_priority effective = fields->request_priority(); // urgency 2, incremental true
}
ruvia::http_priority_fields local{.urgency_ = 2, .incremental_ = true};
```

### 8.10 `"http_uri.h"`
<a id="api-http-uri-h"></a>

DNS 名称原语、`host[:port]` 分隔符拆分与零拷贝 HTTP(S)/WS(S) URL 拆解。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_authority.h"`](#api-http-authority-h)（`http_authority_view`、`http_authority_policy`）、[`"http_message.h"`](#api-http-message-h)。源码：[`ruvia-http/include/http_uri.h`](../ruvia-http/include/http_uri.h)

```cpp
namespace ruvia {
[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept;
[[nodiscard]] constexpr bool is_dns_subdomain_of(std::string_view host, std::string_view domain) noexcept;

[[nodiscard]] bool split_uri_host_port(std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept;
template </* 未指定实现类型：拥有字符的临时字符串 */ text_type>
bool split_uri_host_port(text_type&&, std::string_view&, std::string_view&, bool&) = delete;

class http_url_view final {
public:
    [[nodiscard]] constexpr std::string_view scheme() const noexcept;
    [[nodiscard]] constexpr std::string_view authority_text() const noexcept;
    [[nodiscard]] constexpr const http_authority_view& authority() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_authority_view& authority() const&& = delete;
    [[nodiscard]] constexpr std::string_view path() const noexcept;
    [[nodiscard]] constexpr bool has_path() const noexcept;
    [[nodiscard]] constexpr std::optional<std::string_view> query() const noexcept;
    [[nodiscard]] constexpr std::optional<std::string_view> fragment() const noexcept;
    [[nodiscard]] constexpr std::array<std::string_view, 2> target() const noexcept;
};

[[nodiscard]] std::optional<http_url_view> parse_http_url(borrowed_text value, http_authority_policy policy = http_authority_policy::rfc) noexcept;
}
```

| 公开入口 | 契约 |
| --- | --- |
| `is_valid_dns_domain_name(name)` | ASCII LDH 域名，标签 1–63 字节、总长至多 253；拒绝根点、空标签、首尾连字符、Unicode 与百分号转义；IDNA 与公共后缀策略由调用方负责。 |
| `is_dns_subdomain_of(host, domain)` | 前置条件：两者均为已校验域名（本函数不校验）。按标签边界、ASCII 大小写不敏感匹配严格子域；相同名称不是子域。 |
| `split_uri_host_port(value, host, port, has_port)` | 借用拆分 RFC 3986 `host [":" port]`，保留 IP-literal 方括号；只校验分隔符，不校验 host / port 语法，不替代 authority 校验。拒绝不配对或不在开头的方括号、`]` 后除 `:` 外的字节、port 中的 `:` / `[` / `]`；空 host / port 可表示。成功时输出借用 `value`，失败返回 `false` 且输出未指定。`= delete` 重载拒绝拥有字符的临时字符串。 |
| `parse_http_url(value, policy = rfc)` | 零分配拆解完整 URL：scheme 须为 `http` / `https` / `ws` / `wss`（ASCII 大小写不敏感）且后接 `//`；authority 按 `policy` 经 [`parse_http_authority`](#api-http-authority-h) 解析（拒绝 userinfo 与 IPv6 zone，`endpoint` 严格策略见 8.1）；path、query、fragment 只接受 RFC 3986 字符（pchar 与 `/`，query / fragment 另含 `?`）及良构百分号转义，因而拒绝裸空白、反斜线与非 ASCII，path 另拒绝编码 NUL（`%00`）。不做解码或点段归一化。任一不满足返回 `std::nullopt`。 |
| `http_url_view` | 只能由 `parse_http_url` 产生。`scheme()` / `authority_text()` 为原始拼写（保留大小写与端口数字），`authority()` 为已解析 authority（右值重载删除），无重复解析。 |
| `path()` / `has_path()` | 路径切片；缺省路径读取为静态 `"/"`，`has_path()` 区分是否实际存在。 |
| `query()` / `fragment()` | `std::nullopt` 表示缺省，空视图表示显式的 `?` / `#`；fragment 被校验但不进入请求目标。 |
| `target()` | 两片按序拼接即 origin-form target：有路径时为原始 path（含 `?query`）与空片；缺省路径时为静态 `"/"` 与原始 `?query`（无 query 时为空片）。不分配输入中不存在的连续字符串。 |

- **寿命**：所有非缺省切片借用原始输入；`borrowed_text` 拒绝临时拥有型字符串。

## 9. 缓存、条件请求、范围、Cookie、CORS 与内容协商

### 9.1 `"http_cache.h"`
<a id="api-http-cache-h"></a>

RFC 9111 / RFC 9213 缓存相关字段的解析与改写原语：`Cache-Control` 指令、定向缓存头（如 `CDN-Cache-Control`）、共享缓存的字段级 `private` / `no-cache` 剥离、客户端 `max-age` 重写、`Vary` 规格与缓存键、`Age` 计算以及 `304 Not Modified` 首部清洗与合并。本头只报告线路上的指令，新鲜度计算与复用策略由调用方负责。无条件编译；公开依赖 [`"http_header.h"`](#api-http-header-h)（`http_header_range_view`、`editable_http_headers`）、[`"http_response.h"`](#api-http-response-h)、[`"decimal_text.h"`](#api-decimal-text-h)（`saturating_add`）与 [`"borrowed_text.h"`](#api-borrowed-text-h)（`RUVIA_LIFETIMEBOUND`）。源码：[`ruvia-http/include/http_cache.h`](../ruvia-http/include/http_cache.h)

#### 9.1.1 `enum class cache_control_directive`

```cpp
enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_ = 1U << 5,
    public_ = 1U << 6,
    immutable = 1U << 7,
    only_if_cached = 1U << 8,
    max_stale_any = 1U << 9,
    private_unqualified = 1U << 10,
    no_cache_unqualified = 1U << 11,
};
```

每个枚举值是单一位掩码，供 `cache_control::has()` 查询。

| 成员 | 值 | 含义 |
| --- | --- | --- |
| `no_store` | `1U << 0` | 出现无参数 `no-store`（RFC 9111 §5.2.1.5 / §5.2.2.5） |
| `no_cache` | `1U << 1` | 出现 `no-cache`，无论无参数还是带字段列表（RFC 9111 §5.2.1.4 / §5.2.2.4） |
| `no_transform` | `1U << 2` | 出现无参数 `no-transform`（§5.2.1.6 / §5.2.2.6） |
| `must_revalidate` | `1U << 3` | 出现无参数 `must-revalidate`（§5.2.2.2） |
| `proxy_revalidate` | `1U << 4` | 出现无参数 `proxy-revalidate`（§5.2.2.8） |
| `private_` | `1U << 5` | 出现 `private`，无论无参数还是带字段列表（§5.2.2.7） |
| `public_` | `1U << 6` | 出现无参数 `public`（§5.2.2.9） |
| `immutable` | `1U << 7` | 出现无参数 `immutable`（RFC 8246） |
| `only_if_cached` | `1U << 8` | 出现无参数 `only-if-cached`（§5.2.1.7） |
| `max_stale_any` | `1U << 9` | 出现不带参数的 `max-stale`，即接受任意陈旧程度（§5.2.1.2） |
| `private_unqualified` | `1U << 10` | `private` 为无限定形式（整个响应私有），或字段列表无法完整保存而保守降级为无限定 |
| `no_cache_unqualified` | `1U << 11` | `no-cache` 为无限定形式，或字段列表无法完整保存而保守降级为无限定 |

#### 9.1.2 `class cache_control`

```cpp
class cache_control final {
public:
    [[nodiscard]] constexpr bool has(cache_control_directive directive) const noexcept;

    [[nodiscard]] constexpr bool no_store() const noexcept;
    [[nodiscard]] constexpr bool no_cache() const noexcept;
    [[nodiscard]] constexpr bool no_transform() const noexcept;
    [[nodiscard]] constexpr bool must_revalidate() const noexcept;
    [[nodiscard]] constexpr bool proxy_revalidate() const noexcept;
    [[nodiscard]] constexpr bool is_private() const noexcept;
    [[nodiscard]] constexpr bool is_unqualified_private() const noexcept;
    [[nodiscard]] constexpr bool is_field_qualified_private() const noexcept;
    [[nodiscard]] constexpr bool is_unqualified_no_cache() const noexcept;
    [[nodiscard]] constexpr bool is_field_qualified_no_cache() const noexcept;
    [[nodiscard]] constexpr bool is_public() const noexcept;
    [[nodiscard]] constexpr bool immutable() const noexcept;
    [[nodiscard]] constexpr bool only_if_cached() const noexcept;
    [[nodiscard]] constexpr bool max_stale_any() const noexcept;

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_age() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> max_stale() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> min_fresh() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> s_maxage() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept;

    [[nodiscard]] constexpr std::size_t private_field_count() const noexcept;
    [[nodiscard]] constexpr std::string_view private_field(std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view private_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_private_field(std::string_view field_name) const noexcept;

    [[nodiscard]] constexpr std::size_t no_cache_field_count() const noexcept;
    [[nodiscard]] constexpr std::string_view no_cache_field(std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view no_cache_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_no_cache_field(std::string_view field_name) const noexcept;
};
```

已解析的指令集合（RFC 9111 §5.2）。可拷贝的值类型，默认构造为“无任何指令”；不分配内存，字段名列表保存在对象内联存储中。通常由 `parse_cache_control()`、`cache_control_field_parser::finish()` 或 `parse_targeted_cache_control()` 产生。

| 成员 | 语义 |
| --- | --- |
| `has(directive)` | `directive` 必须是恰好一位的掩码，否则返回 `false`；返回该位是否置位 |
| `no_store()` / `no_cache()` / `no_transform()` / `must_revalidate()` / `proxy_revalidate()` / `is_public()` / `immutable()` / `only_if_cached()` / `max_stale_any()` | 等价于 `has()` 对应枚举值 |
| `is_private()` | `private` 出现（任一形式） |
| `is_unqualified_private()` | `private` 为无限定形式（或降级为无限定） |
| `is_field_qualified_private()` | `is_private() && !is_unqualified_private()`，即仅有 `private="field, ..."` |
| `is_unqualified_no_cache()` / `is_field_qualified_no_cache()` | `no-cache` 的对应判定 |
| `max_age()` / `max_stale()` / `min_fresh()` / `s_maxage()` / `stale_while_revalidate()` / `stale_if_error()` | delta-seconds 指令的值；`std::nullopt` 表示指令缺失或其参数无效。`max_stale()` 在无参数 `max-stale` 时为空，此时 `max_stale_any()` 为真 |
| `private_field_count()` / `no_cache_field_count()` | 限定字段数量；对应指令为无限定形式时恒为 `0` |
| `private_field(index)` / `no_cache_field(index)` | 第 `index` 个限定字段名（已转为 ASCII 小写、去重）；越界或无限定形式时返回空视图。返回视图借用 `*this` 的内联存储，有效至该对象销毁或被赋值；右值重载 `= delete`，拒绝从临时 `cache_control` 取得悬空视图 |
| `has_private_field(name)` / `has_no_cache_field(name)` | ASCII 大小写不敏感地判断 `name` 是否被限定；无限定形式时返回 `false` |

字段级限定列表的容量：每个指令至多 16 个字段名、合计 256 字节、单名不超过 255 字节。列表语法无效、为空或超出容量时，该指令保守地降级为无限定形式（`*_unqualified` 置位、列表清空），因此调用方不会因截断而漏剥离字段。

#### 9.1.3 `class cache_control_field_parser` 与 `parse_cache_control`

```cpp
class cache_control_field_parser final {
public:
    void update(std::string_view field_value) noexcept;
    [[nodiscard]] cache_control finish() const noexcept;
};

[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
[[nodiscard]] cache_control parse_cache_control(const http_header_range_view& headers) noexcept;
```

`cache_control_field_parser` 把多行 `Cache-Control` 当作同一个逗号分隔指令列表增量解析（RFC 9110 §5.3）。默认构造即可使用；`update()` 每次吸收一行字段值，状态跨调用保留；`finish()` 返回当前结果快照，可多次调用，不重置状态。

解析规则（`parse_cache_control` 同样适用）：

- 指令名 ASCII 大小写不敏感；未知指令被忽略；布尔指令带参数（如 `no-store=1`）不被识别。
- delta-seconds 指令（`max-age`、`s-maxage`、`max-stale=N`、`min-fresh`、`stale-while-revalidate`、`stale-if-error`）以首次出现为准，即使首次出现的参数无效（结果为 `std::nullopt`）也不会被后续行覆盖。参数可以是 DQUOTE 包裹的数字（quoted-pair 按 RFC 9110 §5.6.4 解码，`"6\0"` 等价于 `60`）；语法合法但溢出的值饱和为 `std::uint64_t` 最大值；空或含非数字字节的参数无效。
- `private` / `no-cache` 的无参数形式在任意位置出现都会覆盖限定形式并清空字段列表；限定形式的参数可以是 token 或 quoted-string，内容为字段名列表。

`parse_cache_control(value)` 解析单行或已用逗号连接的多行值；`parse_cache_control(headers)` 遍历首部块中所有名为 `Cache-Control`（大小写不敏感）的行。二者均不分配、不抛出。

#### 9.1.4 定向缓存控制（RFC 9213）

```cpp
struct targeted_cache_control_selection final {
    cache_control control_{};
    std::string_view selected_header_{};
    bool targeted_{false};
};

[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept;
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(const http_header_range_view& headers, std::string_view header_name = "CDN-Cache-Control") noexcept;

[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control(const http_header_range_view& headers, std::span<const std::string_view> target_headers) noexcept;
```

| 符号 | 语义 |
| --- | --- |
| `parse_targeted_cache_control(value)` | 将字段值按 RFC 8941 Structured Field Dictionary 解析为缓存指令（RFC 9213 §2.1）。字段为空、仅含空白或任何语法错误时返回 `std::nullopt`（整个字段无效）。键按 SF 规则区分大小写；同名成员后出现者覆盖先出现者。布尔指令取 SF 布尔值（`?0` 清除）；delta 指令只接受非负 Integer，否则视为缺失；`private` / `no-cache` 取 `?1`（无限定）或 String/Token 字段列表（限定，规则同 [9.1.2](#api-http-cache-h)）；`max-stale` 为 `?1` 时置 `max_stale_any` |
| `parse_targeted_cache_control(headers, header_name)` | 把首部块中所有名为 `header_name` 的行合并为一个 SF 字典解析；`header_name` 为空时返回 `std::nullopt` |
| `resolve_targeted_cache_control(headers, target_headers)` | RFC 9213 §2.2 目标列表选择：按 `target_headers` 顺序逐个求值，跳过空名；名称为 `Cache-Control`（大小写不敏感）时只要块中存在该字段即选中标准解析；其他名称取第一个存在且有效、非空的定向字段。全部不匹配时回退到标准 `Cache-Control` |
| `targeted_cache_control_selection::control_` | 选中的指令集合 |
| `targeted_cache_control_selection::selected_header_` | 选中的字段名：命中目标列表时借用 `target_headers` 中对应元素所指向的文本（调用方须保证其存活）；回退时为静态字面量 `"Cache-Control"`，块中没有 `Cache-Control` 时为空 |
| `targeted_cache_control_selection::targeted_` | 是否选中了定向字段（`Cache-Control` 选中或回退时为 `false`） |

#### 9.1.5 共享缓存字段剥离与 `no-transform`

```cpp
bool strip_http_cache_private_headers(editable_http_headers headers, const cache_control& control, std::span<const std::string_view> protected_fields = {}) noexcept;
bool strip_http_cache_no_cache_headers(editable_http_headers headers, const cache_control& control) noexcept;

[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(const http_header_range_view& headers) noexcept;
```

| 函数 | 语义 |
| --- | --- |
| `strip_http_cache_private_headers` | RFC 9111 §5.2.2.7：`no-store`、无限定 `private` 或提名命中受保护名称时返回 `false`，整个首部块不变；否则删除全部被提名字段。CDN 可传入不可删除的表示元数据名称，不隐式假定宿主策略。 |
| `strip_http_cache_no_cache_headers` | RFC 9111 §5.2.2.4 的对应操作：`no-store` 或无限定 `no-cache` 时返回 `false`（未经验证不得复用）；否则删除 `no-cache="..."` 列出的字段并返回 `true` |
| `http_cache_control_has_no_transform` | 等价于 `parse_cache_control(...).no_transform()`，供代理与压缩路径判断是否允许变换表示（RFC 9110 §7.7） |

`editable_http_headers` 的绑定规则与单步原子性见 [`"http_header.h"`](#api-http-header-h)；删除操作不会失败，因此两个剥离函数为 `noexcept`。

#### 9.1.6 客户端 `max-age` 重写

```cpp
[[nodiscard]] std::string rewrite_http_cache_control_max_age(std::string_view field_value, std::optional<std::uint64_t> client_max_age);
[[nodiscard]] std::string rewrite_http_cache_control_max_age(const http_header_range_view& headers, std::optional<std::uint64_t> client_max_age);
void rewrite_http_response_cache_max_age(editable_http_headers response_headers, std::optional<std::uint64_t> client_max_age);
```

- **浏览器 TTL**：仅改写 `max-age`，保留 `s-maxage` 与其他指令原文。有值时追加新 `max-age`；`nullopt` 只移除已有 `max-age`，不强加 TTL。
- **响应改写**：合并全部 `Cache-Control` 行；设置 TTL 时移除 `Expires`，只删除 `max-age` 时保留 `Expires`。写入失败保持原样。

#### 9.1.7 `class http_vary_spec` 与 `Vary` 缓存键

```cpp
class http_vary_spec final {
public:
    http_vary_spec() noexcept = default;

    void update(std::string_view field_value);
    void update_headers(const http_header_range_view& headers);

    [[nodiscard]] constexpr bool is_wildcard() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::span<const std::string> field_names() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::string> field_names() const&& = delete;
};

[[nodiscard]] http_vary_spec parse_http_vary(std::string_view field_value);
[[nodiscard]] http_vary_spec parse_http_vary(const http_header_range_view& headers);

[[nodiscard]] std::optional<std::string> format_http_vary_cache_key(const http_vary_spec& vary, const http_header_range_view& request_headers);

[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary, const http_header_range_view& first_request, const http_header_range_view& second_request);
```

| 符号 | 语义 |
| --- | --- |
| `http_vary_spec` | 多行 `Vary` 的规范化规格（RFC 9111 §4.1）：字段名转为 ASCII 小写、去重，并按固定 ASCII 顺序排序 |
| `update(field_value)` | 吸收一行 `Vary` 值；先按 ASCII 大小写不敏感查重，仅新增名称才小写化并拥有存储，重复名称不分配；非法字段名被跳过；出现 `*` 时置通配并清空名单，此后的 `update` 不再生效。可能抛出 `std::bad_alloc` |
| `update_headers(headers)` | 对块中每个 `Vary` 行调用 `update` |
| `is_wildcard()` | 是否出现 `Vary: *`（响应不可由缓存满足后续请求） |
| `empty()` | 既非通配也没有字段名 |
| `field_names()` | 排序后的小写字段名；视图借用 `*this`，在下一次 `update` / `update_headers` 或对象销毁前有效；右值重载 `= delete` |
| `parse_http_vary(...)` | 构造并 `update` / `update_headers` 一次的便捷函数 |
| `format_http_vary_cache_key(vary, request_headers)` | 通配时返回 `std::nullopt`；否则按 `field_names()` 顺序为每个字段生成一行：请求含该字段时为 `name:1:value`（多行以 `", "` 连接，每个值去除首尾 OWS 并把内部连续 SP/HTAB 折叠为单个空格，直接追加到最终键），缺失时为 `name:0`，每行以 `\n` 结尾。缺失与空值因此可区分 |
| `http_vary_requests_match(vary, a, b)` | 两个请求的缓存键均存在且相等时返回 `true`；通配时恒为 `false` |

#### 9.1.8 `Age` 计算

```cpp
struct http_cache_age_input final {
    std::optional<std::time_t> date_value_{};
    std::optional<std::uint64_t> age_value_{};
    std::time_t request_time_{0};
    std::time_t response_time_{0};
    std::time_t now_{0};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept;

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(const http_cache_age_input& input) noexcept;
[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(std::optional<std::time_t> date_value, std::optional<std::uint64_t> age_value, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept;
[[nodiscard]] std::uint64_t compute_http_cache_age(const http_header_range_view& response_headers, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept;
```

- `http_cache_age_input`：RFC 9111 §4.2.3 的输入。`date_value_` 为响应 `Date`，`age_value_` 为响应 `Age`，`request_time_` / `response_time_` 为本地发出请求与收到响应的时刻，`now_` 为当前时刻（均为 Unix 秒）。
- **`parse_http_age`**：按 RFC 9111 §5.1 只解析首个逗号分隔成员。去除首尾 OWS 后，空值、引号形式或非数字返回 `std::nullopt`；溢出饱和为 `std::uint64_t` 最大值。
- **`compute_http_cache_age`**：按 RFC 9111 §4.2.3 计算 `current_age`：`apparent_age = max(0, response_time - date_value)`（无 `Date` 时为 0），`corrected_age_value = age_value + (response_time - request_time)`，`corrected_initial_age = max(apparent_age, corrected_age_value)`，再加上 `resident_time = now - response_time`。负差值取 0，加法饱和。首部块重载取第一个可解析的 `Date` 行；只检查第一行 `Age`，即使该行的首个成员无效也不回退到后续行。

#### 9.1.9 `304 Not Modified` 首部

```cpp
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;

void sanitize_http_304_response_headers(editable_http_headers headers) noexcept;

enum class http_cache_validator_match : std::uint8_t { none, unvalidated, weak, strong };
[[nodiscard]] http_cache_validator_match match_http_cache_validators(const http_header_range_view& cached_headers, const http_header_range_view& validating_headers) noexcept;
bool merge_http_304_response_headers(editable_http_headers cached_headers, const http_header_range_view& not_modified_headers);
bool weaken_http_cache_etag(editable_http_headers headers);
```

| 函数 | 语义 |
| --- | --- |
| `is_http_304_forbidden_header(name)` | 大小写不敏感地判断字段是否属于不得出现在 `304` 中、也不得在 `304` 刷新时覆盖存储表示的表示元数据或分帧字段（RFC 9110 §15.4.5、RFC 9111 §4.3.4）：`Content-Type`、`Content-Length`、`Content-Range`、`Content-Encoding`、`Content-Language`、`Content-Disposition`、`Content-MD5`、`Content-Digest`、`Repr-Digest`、`Digest`、`Transfer-Encoding`、`Trailer` |
| `sanitize_http_304_response_headers(headers)` | 删除上述全部字段；不会失败 |
| `merge_http_304_response_headers(cached, not_modified)` | RFC 9111 §4.3.4 刷新：从 `304` 中筛出名称与值合法、非逐跳、未被 `Connection` 提名（规则见 [`"http_proxy_forwarding.h"`](#api-http-proxy-forwarding-h)）且非上表禁止的行；对每个出现的字段名，先删除存储块中全部同名行，再按 `304` 的顺序写回：同名首行替换，后续同名行追加，但 `Date`、`ETag`、`Expires`、`Content-Location`、`Last-Modified` 为单值字段，后出现的行替换先前行。`not_modified_headers` 可以整体或部分借用 `cached_headers` 自身（先复制候选行再改写）。多行改写在 `transact` 中执行：目标为 `http_response` 时全部成功或保持原样。可能抛出 `std::bad_alloc` |
| `match_http_cache_validators(cached, validating)` | ETag 优先：新强标签须与存储强标签精确匹配，新弱标签按弱比较。否则比较 Last-Modified；存储 Date 至少比 Last-Modified 晚 60 秒才为强校验。显式 Content-Encoding 须与存储编码栈一致，缺省则保留存储编码。返回 `none` 表示不匹配，`unvalidated` 表示双方均无校验器。 |
| `weaken_http_cache_etag(headers)` | 编码转换后复用实体标签规则弱化 ETag；重复或非法标签拒绝且不修改，缺省或已经弱化成功。 |

- **304 准入**：`merge_http_304_response_headers` 先检查校验器、编码与 Connection 语法/预算；拒绝返回 `false` 且不修改缓存，成功才执行上述合并并返回 `true`。

### 9.2 `"http_representation.h"`
<a id="api-http-representation-h"></a>

源站对“已选中、已存在的表示”求值条件请求（RFC 9110 §13）与 `Range` 的原语：方法资格表、条件首部提取、实体标签校验与强/弱比较、日期前提条件、`If-Range` 判定，以及把这些结果合成为唯一响应选择的 `plan_http_representation_response()`。不负责缓存未命中或表示选择。无条件编译；公开依赖 [`"http_range.h"`](#api-http-range-h)（`http_byte_range`、`http_byte_range_set`）、[`"http_content_length.h"`](#api-http-content-length-h)、[`"http_status.h"`](#api-http-status-h)、[`"http_message.h"`](#api-http-message-h)（`http_known_method`）与 [`"borrowed_text.h"`](#api-borrowed-text-h)；以 `const http_request&` 为参数的函数需要 [`"http_request.h"`](#api-http-request-h)。源码：[`ruvia-http/include/http_representation.h`](../ruvia-http/include/http_representation.h)

#### 9.2.1 方法资格与条件首部

```cpp
struct http_conditional_method_plan final {
    bool evaluates_preconditions_;
    bool uses_not_modified_response_;
    bool evaluates_if_modified_since_;
    bool evaluates_range_;
};

[[nodiscard]] inline constexpr http_conditional_method_plan plan_http_conditional_method(http_known_method method) noexcept;

struct http_conditional_headers final {
    std::string_view if_unmodified_since_;
    std::string_view if_modified_since_;
    std::string_view range_;
    std::string_view if_range_;
    bool has_if_range_;
};

[[nodiscard]] http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept;
http_conditional_headers get_http_conditional_headers(const http_request&&) = delete;
```

`http_conditional_method_plan` 的四个字段（无默认初始化值，聚合初始化时必须给出）：

| 字段 | 含义 |
| --- | --- |
| `evaluates_preconditions_` | 该方法求值 `If-Match` / `If-None-Match` / `If-Unmodified-Since` |
| `uses_not_modified_response_` | `If-None-Match` 命中时回答 `304`（否则回答 `412`） |
| `evaluates_if_modified_since_` | 求值 `If-Modified-Since` |
| `evaluates_range_` | 求值 `Range` / `If-Range` |

`plan_http_conditional_method(method)` 的固定映射：

| `http_known_method` | 前提条件 | `304` | `If-Modified-Since` | `Range` |
| --- | --- | --- | --- | --- |
| `get` | 是 | 是 | 是 | 是 |
| `head` | 是 | 是 | 是 | 否 |
| `post` / `put` / `delete_` / `patch` | 是 | 否 | 否 | 否 |
| `options` / `connect` / `unknown` | 否 | 否 | 否 | 否 |

`get_http_conditional_headers(request)` 返回借用 `request` 的视图，有效期与请求首部存储相同；右值重载 `= delete`，拒绝从临时请求取视图。`If-Modified-Since` 或 `If-Unmodified-Since` 出现多行时视为无效日期条件（RFC 9110 §13.1.3、§13.1.4），对应视图为空，而不是把最后一行当作有效条件。`has_if_range_` 单独记录 `If-Range` 是否存在，因为空值的 `If-Range` 仍是存在的字段。

#### 9.2.2 实体标签前提条件

```cpp
struct http_etag_field_condition final {
    bool present_{false};
    bool valid_{true};
    bool matched_{false};
    bool wildcard_{false};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;
    [[nodiscard]] bool matches() const noexcept;
};

struct http_etag_preconditions final {
    http_etag_field_condition if_match_;
    http_etag_field_condition if_none_match_;
};

[[nodiscard]] http_etag_preconditions get_http_etag_preconditions(const http_request& request, std::string_view etag) noexcept;
```

| 成员 | 语义 |
| --- | --- |
| `http_etag_field_condition` | 一个实体标签条件字段跨全部字段行累积的结果，等价于逗号连接后的整体列表，但不分配 |
| `present_` | 至少吸收过一行 |
| `valid_` | 整个列表语法有效。列表格式错误、`*` 与其他成员并存（包括跨行）、或 `*` 出现多次时为 `false`。调用方必须把“无效”与“未匹配”区别对待 |
| `matched_` | 某个成员与 `expected` 比较相等 |
| `wildcard_` | 出现 `*` |
| `update(value, expected, strong)` | 吸收一行；`strong` 为真时按强比较，否则按弱比较（RFC 9110 §8.8.3.2） |
| `matches()` | `valid_ && (wildcard_ \|\| matched_)` |
| `get_http_etag_preconditions(request, etag)` | 用 `etag` 求值请求中的全部 `If-Match` 行（强比较，RFC 9110 §13.1.1）与全部 `If-None-Match` 行（弱比较，§13.1.2）。结果不借用请求 |

#### 9.2.3 日期前提条件与 `If-Range`

```cpp
[[nodiscard]] bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept;
[[nodiscard]] bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept;

[[nodiscard]] bool http_if_range_allows(std::string_view header_value, std::string_view etag, std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept;
```

| 函数 | 语义 |
| --- | --- |
| `http_date_not_modified(value, modified)` | `If-Modified-Since` 判定：`value` 为合法 HTTP-date（[`"http_date.h"`](#api-http-date-h)）且 `modified <= date` 时为 `true`；日期非法时为 `false`（条件被忽略，不回答 `304`） |
| `http_date_unmodified(value, modified)` | `If-Unmodified-Since` 判定：日期非法（条件被忽略）或 `modified <= date` 时为 `true` |
| `http_if_range_allows(value, etag, modified, strong_date)` | `If-Range` 是否仍授权范围响应（RFC 9110 §13.1.5）。空值返回 `false`。值以 `"` 或 `W/` 开头时按实体标签处理，只有与 `etag` 强比较相等才返回 `true`（弱标签永不匹配）。否则按日期处理：要求 `strong_date` 为真、`modified` 有值且与解析出的日期完全相等（不同于 `If-Modified-Since` 的 `<=`） |

#### 9.2.4 实体标签工具

```cpp
[[nodiscard]] bool is_valid_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_weak_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_strong_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string weaken_http_etag(std::string_view etag);
```

单值实体标签（RFC 9110 §8.8.3）的校验、比较与弱化：

| 函数 | 语义 |
| --- | --- |
| `is_valid_http_entity_tag` | `[W/]"etagc*"`：可选的 `W/` 前缀（区分大小写）后为 DQUOTE 包围的 `etagc`（`%x21`、`%x23-7E` 或 obs-text）；不做 OWS 修剪 |
| `is_weak_http_entity_tag` / `is_strong_http_entity_tag` | 合法且带 / 不带 `W/` 前缀 |
| `http_strong_etag_equals` | 双方都是合法强标签且字节相等 |
| `http_weak_etag_equals` | 双方都合法，忽略 `W/` 前缀后 opaque-tag 相等 |
| `weaken_http_etag` | 先去除首尾 OWS；合法强标签返回加 `W/` 前缀的副本，其余输入（弱标签或非法值）原样返回修剪后的副本。可能抛出 `std::bad_alloc` |

#### 9.2.5 表示元数据与选项

```cpp
struct http_selected_representation_metadata final {
    http_content_length length_{};
    borrowed_text etag_{};
    std::optional<std::time_t> last_modified_{};
    bool strong_date_validator_{false};
    bool content_coded_{false};
};

enum class http_range_request_policy {
    ignore,
    honor_byte_ranges,
};

struct http_representation_response_options final {
    http_status_code normal_status_{http_status::ok};
    http_range_request_policy range_policy_{http_range_request_policy::ignore};
};
```

`http_selected_representation_metadata` 描述已经选中的现有表示：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `length_` | `0` | 表示长度（[`http_content_length`](#api-http-content-length-h)，构造前检查 `0..UINT64_MAX`，负数抛出 `std::invalid_argument`） |
| `etag_` | 空 | 当前实体标签；`borrowed_text` 拒绝临时拥有型字符串（见 [`"borrowed_text.h"`](#api-borrowed-text-h)），仅在规划期间借用，计划不保留任何输入视图 |
| `last_modified_` | 空 | `Last-Modified` 对应的 Unix 秒；缺失时日期条件不被求值 |
| `strong_date_validator_` | `false` | 仅在 `last_modified_` 也存在时授权日期形式的 `If-Range`；普通日期前提条件不需要强验证器 |
| `content_coded_` | `false` | 表示带内容编码（`Content-Encoding`）。`multipart/byteranges` 的各部分无法声明整体表示的编码，首部级 `Content-Encoding` 又会声称 multipart 体本身被编码（RFC 9110 §8.4、§14.6），因此对此类表示的多范围请求回答完整表示（RFC 9110 §14.2 允许忽略 `Range`）；单范围仍是单部分 `206` |

`enum class http_range_request_policy`（底层类型 `int`）：

| 成员 | 含义 |
| --- | --- |
| `ignore` | 不处理 `Range`，总是回答完整表示（默认） |
| `honor_byte_ranges` | 按字节范围回答 `206` / `416` |

`http_representation_response_options`：`normal_status_` 为求值前提条件与 `Range` 之前源站本应返回的状态（默认 `200`）；`range_policy_` 默认 `ignore`。

#### 9.2.6 `class http_representation_response_plan` 与 `plan_http_representation_response`

```cpp
class http_representation_response_plan final {
public:
    struct full_type final {
        http_status_code status_;
    };
    struct not_modified_type final {};
    struct precondition_failed_type final {};
    struct range_unsatisfiable_type final {};
    using partial_type = http_byte_range;
    using multipart_type = http_byte_range_set;

    [[nodiscard]] const full_type* full() const& noexcept;
    [[nodiscard]] const full_type* full() const&& = delete;
    [[nodiscard]] const not_modified_type* not_modified() const& noexcept;
    [[nodiscard]] const not_modified_type* not_modified() const&& = delete;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const& noexcept;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const&& = delete;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const& noexcept;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const&& = delete;
    [[nodiscard]] const partial_type* partial() const& noexcept;
    [[nodiscard]] const partial_type* partial() const&& = delete;
    [[nodiscard]] const multipart_type* multipart_ranges() const& noexcept;
    [[nodiscard]] const multipart_type* multipart_ranges() const&& = delete;
    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] bool authorized_by_if_range() const noexcept;
};

[[nodiscard]] http_representation_response_plan plan_http_representation_response(const http_request& request, http_selected_representation_metadata representation, http_representation_response_options options = {});
```

互斥的源站响应选择，只拥有状态与切片值；响应体是否允许发送与线路分帧在提交写出时决定。对象只能由 `plan_http_representation_response()` 产生（无公开构造函数），可拷贝。

| 成员 | 语义 |
| --- | --- |
| `full()` | 回答完整表示，`status_` 为 `options.normal_status_` |
| `not_modified()` | 回答 `304 Not Modified` |
| `precondition_failed()` | 回答 `412 Precondition Failed` |
| `range_unsatisfiable()` | 回答 `416 Range Not Satisfiable`（调用方应附 `Content-Range: bytes */<length>`） |
| `partial()` | 单范围 `206`，值为已解析的 [`http_byte_range`](#api-http-range-h) |
| `multipart_ranges()` | 多范围 `206 multipart/byteranges`，值为 [`http_byte_range_set`](#api-http-range-h)（可直接交给 `make_http_multipart_byte_range_plan()`） |
| 上述访问器 | 恰好一个返回非空指针；指针借用计划对象；右值重载 `= delete`，拒绝从临时计划取悬空指针 |
| `status()` | 对应状态码：完整为 `full()->status_`，其余为 `304` / `412` / `416` / `206` |
| `authorized_by_if_range()` | 当 `partial()` 或 `multipart_ranges()` 回答的请求带有匹配的 `If-Range` 时为 `true`。RFC 9110 §15.3.7：此时 `206` 不应重复必需字段（`Date`、`Cache-Control`、`ETag`、`Expires`、`Content-Location`、`Vary`，以及 `Content-Range` 或 multipart `Content-Type`）以外的表示字段；没有 `If-Range` 时 `206` 必须携带 `200` 会携带的全部表示字段 |

`plan_http_representation_response()` 应在常规请求检查之后、处理请求内容或执行方法之前调用，传入目标资源的当前表示。方法必须受支持且被允许；对扩展方法，调用方须确认其选择或修改该表示，非表示类扩展与重新定义条件语义的扩展不在本 API 范围内。求值顺序（RFC 9110 §13.2.2）：

1. `options.range_policy_` 不是已定义枚举值时抛出 `std::invalid_argument`。
2. 方法资格取自 `plan_http_conditional_method()`；未知方法（`TRACE` 除外）按 `POST` 类处理（求值前提条件，命中 `If-None-Match` 时回答 `412`）。`CONNECT`、`OPTIONS`、`TRACE` 忽略条件。方法不求值前提条件，或 `normal_status_` 既非 `2xx` 也非 `412` 时，回答完整表示。
3. 存在 `If-Match` 时，`matches()` 为假（含列表无效）回答 `412`；否则在 `last_modified_` 存在且 `If-Unmodified-Since` 有效时，日期不满足回答 `412`。
4. 存在 `If-None-Match` 时，`matches()` 为真则 `GET` / `HEAD` 回答 `304`、其他方法回答 `412`；不存在时，`GET` / `HEAD` 在 `last_modified_` 存在且 `If-Modified-Since` 满足时回答 `304`。
5. 仅当 `normal_status_ == 200`、方法为 `GET`、`range_policy_ == honor_byte_ranges` 且 `Range` 非空时继续：`If-Range` 存在但 `http_if_range_allows()` 为假回答完整表示；按默认 [`http_byte_range_options`](#api-http-range-h)（至多 16 个区间、`coalesce`）解析 `Range`；`ignored()` 回答完整表示；`unsatisfiable()` 回答 `416`；单区间回答 `partial()`；多区间且 `content_coded_` 为真回答完整表示，否则回答 `multipart_ranges()`。

可能抛出 `std::bad_alloc`（区间数超过内联容量时）。

### 9.3 `"http_range.h"`
<a id="api-http-range-h"></a>

字节范围（RFC 9110 §14）的解析与输出原语：`Range` 解析为有界区间集合（含区间上限与顺序策略）、`Content-Range` 解析、`206` 正文长度计算、流式单区间 / `multipart/byteranges` 切片器，以及适合 scatter-gather / 文件发送的拥有型 multipart 计划。无条件编译；公开依赖 [`"http_content_length.h"`](#api-http-content-length-h) 与 [`"borrowed_text.h"`](#api-borrowed-text-h)（`RUVIA_LIFETIMEBOUND`）；multipart 边界与媒体类型的校验规则分别来自 [`"http_multipart.h"`](#api-http-multipart-h) 的 `multipart_boundary` 与 [`"http_content_negotiation.h"`](#api-http-content-negotiation-h) 的 `is_valid_http_content_type_field_value`。源码：[`ruvia-http/include/http_range.h`](../ruvia-http/include/http_range.h)

#### 9.3.1 区间、选项与 `class http_byte_range_set`

```cpp
struct http_byte_range final {
    std::uint64_t offset_{};
    std::uint64_t length_{};

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept;
    [[nodiscard]] constexpr std::uint64_t length() const noexcept;
};

enum class http_byte_range_order_policy : std::uint8_t {
    coalesce,
    strict_ascending_disjoint,
};

struct http_byte_range_options final {
    std::size_t max_ranges_{16};
    http_byte_range_order_policy order_policy_{http_byte_range_order_policy::coalesce};
};

class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;

    http_byte_range_set() noexcept = default;

    [[nodiscard]] constexpr bool ignored() const noexcept;
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept;
    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] std::span<const range> ranges() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const range> ranges() const&& = delete;

    [[nodiscard]] const range* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    const range* begin() const&& = delete;

    [[nodiscard]] const range* end() const& noexcept RUVIA_LIFETIMEBOUND;
    const range* end() const&& = delete;

    [[nodiscard]] const range& operator[](std::size_t index) const& RUVIA_LIFETIMEBOUND;
    const range& operator[](std::size_t) const&& = delete;
};

[[nodiscard]] http_byte_range_set resolve_http_byte_range_set(std::string_view field_value, http_content_length representation_length_value, http_byte_range_options options = {});
```

- `http_byte_range`：已针对所选表示解析的区间，`offset_` 为首字节偏移，`length_` 为字节数（均默认 `0`）；`offset()` / `length()` 为同值访问器。
- `http_byte_range_options`：`max_ranges_` 默认 `16`，是保留区间数的上限；`order_policy_` 默认 `coalesce`。

`enum class http_byte_range_order_policy`：

| 成员 | 含义 |
| --- | --- |
| `coalesce` | 重叠或相邻的区间合并进其中最早出现的那个位置，其余区间保持请求顺序（结果不保证升序） |
| `strict_ascending_disjoint` | 保留每个请求区间（不合并）；任一区间的起点小于前一区间的末尾之后一字节（即回退或交叠；紧邻相接允许）时忽略整个 `Range` 字段 |

`http_byte_range_set`：有界结果。至多 16 个区间使用内联存储，不分配；`max_ranges_` 配置更大时溢出到堆。可拷贝。

| 成员 | 语义 |
| --- | --- |
| `capacity` | 内联容量 `16` |
| `ignored()` | `Range` 被忽略，应回答完整表示（默认构造对象即为此状态） |
| `unsatisfiable()` | 未被忽略但没有可满足区间，应回答 `416` |
| `size()` | 区间数 |
| `ranges()` / `begin()` / `end()` | 区间视图与迭代器，借用 `*this`，在对象销毁或被赋值前有效；右值重载 `= delete` |
| `operator[](index)` | `index >= size()` 时抛出 `std::out_of_range`；右值重载 `= delete` |

`resolve_http_byte_range_set(field_value, length, options)` 的规则：

- 结果为 `ignored()`：`max_ranges_ == 0`；去除 OWS 后不以 `bytes=`（大小写不敏感）开头；`bytes=` 之后为空；任一非空成员语法错误（缺少或多个 `-`、非数字、`first > last`）；空成员超过 32 个；仅含空成员；需要新增区间但已达到 `max_ranges_`；`strict_ascending_disjoint` 下出现非升序或交叠。已达上限时仍允许合并。
- 数值按饱和十进制解析。`first-last` 中 `last` 超出表示时截断到末字节；`first-` 取到末字节；`-suffix` 取末尾 `min(suffix, length)` 字节。`first >= length`、`-0` 或表示长度为 `0` 时该成员不可满足并被跳过。
- 至少存在一个非空成员但没有可满足区间时结果为 `unsatisfiable()`。
- **空表示**：长度为零仍完整校验语法；非法字段为 `ignored()`（完整响应），合法但不可满足为 `unsatisfiable()`（416），不需要假长度探测。
- 可能抛出 `std::bad_alloc`（`max_ranges_ > 16` 时）。

#### 9.3.2 `Content-Range` 与正文长度

```cpp
struct http_content_range final {
    std::uint64_t first_{};
    std::uint64_t last_{};
    std::uint64_t total_{};

    [[nodiscard]] constexpr std::uint64_t first() const noexcept;
    [[nodiscard]] constexpr std::uint64_t last() const noexcept;
    [[nodiscard]] constexpr std::uint64_t total() const noexcept;
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept;
    [[nodiscard]] constexpr std::uint64_t length() const noexcept;
    [[nodiscard]] constexpr http_byte_range range() const noexcept;

    friend constexpr bool operator==(const http_content_range&, const http_content_range&) noexcept = default;
};

[[nodiscard]] std::optional<http_content_range> parse_http_content_range(std::string_view field_value) noexcept;

[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(std::span<const http_byte_range> ranges, http_content_length representation_length, std::string_view media_type = {}, std::string_view boundary = {}) noexcept;
[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(const http_byte_range_set& ranges, http_content_length representation_length, std::string_view media_type = {}, std::string_view boundary = {}) noexcept;
```

| 符号 | 语义 |
| --- | --- |
| `http_content_range` | 已解析的 `bytes first-last/total`（RFC 9110 §14.4）。`first()` / `last()` / `total()` 返回原值；`offset()` 等于 `first_`；`length()` 为 `last_ - first_ + 1`；`range()` 为对应 `http_byte_range`；`operator==` 逐成员比较 |
| `parse_http_content_range(value)` | 严格要求 `bytes`（大小写不敏感）后恰好单个 SP，随后为 `first-last/total`，无前后空白；要求 `first <= last < total`。不支持未满足形式 `bytes */length` 与未知总长 `/*`，非法返回 `nullopt`。 |
| `compute_http_byte_range_content_length(ranges, length, media_type, boundary)` | 不分配、不生成正文，按切片器与文件计划共用的 multipart 分帧规则计算 `206` 正文字节数。区间为空、表示长度为 `0`、任一区间长度为 `0` 或越界时返回 `std::nullopt`；单区间返回其长度（忽略 `media_type` / `boundary`）；多区间要求 `media_type` 是合法 `Content-Type` 值、`boundary` 是合法 multipart 边界，并计入每部分的分隔行、`Content-Type`、`Content-Range` 与结尾边界，`uint64` 溢出时返回 `std::nullopt` |
| 集合重载 | `ignored()` 或 `unsatisfiable()` 时返回 `std::nullopt`，否则转发到 span 重载 |

#### 9.3.3 `class http_byte_range_slicer`

```cpp
class http_byte_range_slicer final {
public:
    http_byte_range_slicer(const http_byte_range_set& ranges, http_content_length representation_length, std::string_view media_type = {}, std::string_view boundary = {});
    http_byte_range_slicer(std::span<const http_byte_range> ranges, http_content_length representation_length, std::string_view media_type = {}, std::string_view boundary = {});

    [[nodiscard]] bool multipart() const noexcept;
    [[nodiscard]] std::uint64_t content_length() const noexcept;
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view content_range() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view content_range() const&& = delete;
    [[nodiscard]] std::uint64_t current_offset() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

    void write(std::string_view chunk, std::string& output);
    void write(std::uint64_t chunk_offset, std::string_view chunk, std::string& output);
    void finish(std::string& output);
    void reset(std::uint64_t start_offset = 0) noexcept;
};
```

流式字节范围切片器：接收按顺序到来的表示数据块（或从指定表示偏移开始的部分数据块），把属于所选区间的字节连同 multipart 分帧追加到调用方缓冲区。构造时复制区间、媒体类型与边界，此后不借用输入。

移动转移区间及切片进度。源对象不再报告完成，也不能继续 `write()` 或 `finish()`；重新赋值后才可复用，`reset()` 不恢复区间。

`write()` 的输入块可以借用输出缓冲区，所选字节包含字符串原末尾的 NUL 时也会原样追加。

| 成员 | 语义 |
| --- | --- |
| 构造函数 | 抛出 `std::invalid_argument`：区间为空（含集合 `ignored()`）、表示长度为 `0`、区间长度为 `0` 或越界、区间不是严格升序且不交叠、单区间时非空 `media_type` 不是合法 `Content-Type` 值、多区间时 `media_type` 或 `boundary` 不合法；多区间总长溢出 `uint64` 时抛出 `std::length_error`。`coalesce` 策略产生的集合可能不是升序，需改用 `strict_ascending_disjoint` 或下文的 multipart 计划 |
| `multipart()` | 区间数大于 1 |
| `content_length()` | 输出正文总字节数，与 `compute_http_byte_range_content_length()` 一致 |
| `content_type()` | 单区间为构造时的 `media_type`（可为空）；多区间为 `multipart/byteranges; boundary=<boundary>`，边界不是 token 时加引号。视图借用切片器；右值重载 `= delete` |
| `content_range()` | 单区间为 `bytes first-last/length`；多区间为空（各部分自带 `Content-Range`）。借用规则同上 |
| `current_offset()` | 已消费到的表示偏移 |
| `complete()` | 所有区间已输出完毕（多区间时包括结尾边界） |
| `write(chunk, output)` | 等价于 `write(current_offset(), chunk, output)` |
| `write(chunk_offset, chunk, output)` | `chunk_offset < current_offset()`、数据块越出表示、或 `chunk_offset` 跳过了尚未输出的区间字节时抛出 `std::invalid_argument`。数据块可以从当前偏移之后、下一个所需字节之前的任意位置开始。每个部分首次输出前追加 `--boundary\r\nContent-Type: ...\r\nContent-Range: bytes a-b/len\r\n\r\n`，部分结束追加 `\r\n`，最后追加 `--boundary--\r\n`。完成后再写入只推进 `current_offset()` |
| `finish(output)` | 未完成时抛出 `std::logic_error`；完成时不追加任何字节 |
| `reset(start_offset)` | 回到第一个区间的起点，`current_offset()` 设为 `start_offset`，`complete()` 复位 |

- **输入别名**：`chunk` 可借用 `output` 的存储。多段输出先保全借用的字节，再追加分段首部和边界。
- **写入失败**：异常不会改变 `output` 已有内容或切片进度；可用原输入重试。

#### 9.3.4 `class http_multipart_byte_range_plan`

```cpp
class http_multipart_byte_range_plan final {
public:
    enum class segment_kind : std::uint8_t {
        metadata,
        file,
    };

    struct segment final {
        segment_kind kind_{};
        std::size_t metadata_offset_{};
        std::size_t metadata_length_{};
        std::uint64_t file_offset_{};
        std::uint64_t file_length_{};
    };

    http_multipart_byte_range_plan(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan& operator=(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan(http_multipart_byte_range_plan&&) noexcept = default;
    http_multipart_byte_range_plan& operator=(http_multipart_byte_range_plan&&) = delete;

    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view metadata() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view metadata() const&& = delete;
    [[nodiscard]] std::span<const segment> segments() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const segment> segments() const&& = delete;
    [[nodiscard]] std::uint64_t content_length() const noexcept;
    [[nodiscard]] http_multipart_byte_range_plan clone() const;
};

[[nodiscard]] http_multipart_byte_range_plan make_http_multipart_byte_range_plan(const http_byte_range_set& ranges, http_content_length representation_length, std::string_view media_type, std::string_view boundary);
```

拥有复制的分帧元数据与有序文件切片描述的多范围计划，适合由传输层把元数据与文件区段以 scatter-gather 方式发送。仅可移动构造（拷贝与两种赋值均删除，显式复制用 `clone()`）。

`enum class http_multipart_byte_range_plan::segment_kind`：

| 成员 | 含义 |
| --- | --- |
| `metadata` | 线路片段来自 `metadata()` 中的 `[metadata_offset_, metadata_offset_ + metadata_length_)` |
| `file` | 线路片段是表示的 `[file_offset_, file_offset_ + file_length_)` 字节 |

| 成员 | 语义 |
| --- | --- |
| `segment` | 一个有序线路片段；未使用的偏移/长度字段为 `0` |
| `content_type()` | `multipart/byteranges; boundary=<boundary>`（非 token 边界加引号） |
| `metadata()` | 全部分帧文本拼接后的单一缓冲区 |
| `segments()` | 有序线路片段，不是 multipart 部分：相邻元数据可能合并为同一描述，消费者不得假定每个区间对应固定数量的片段 |
| 视图访问器 | 借用计划；右值重载 `= delete`；被移动后的对象返回空视图 |
| `content_length()` | 正文总字节数；被移动后的对象为 `0` |
| `clone()` | 深拷贝；被移动后的对象调用时抛出 `std::invalid_argument`；分配失败传播 `std::bad_alloc` |
| `make_http_multipart_byte_range_plan(...)` | 每个部分携带所选表示的 `Content-Type` 与自身 `Content-Range`（RFC 9110 §15.3.7.2），不携带 `Content-Encoding`（内容编码表示的切片本身不是编码体，`plan_http_representation_response()` 对编码表示的多范围请求回答完整表示，见 [9.2.5](#api-http-representation-h)）。抛出 `std::invalid_argument`：集合 `ignored()` / `unsatisfiable()`、区间数小于 2 或大于 `http_byte_range_set::capacity`、表示长度为 `0`、`media_type` 或 `boundary` 不合法、区间越界或长度为 `0`；大小溢出时抛出 `std::length_error`。不要求区间升序 |

出站长度参数统一使用 [`http_content_length`](#api-http-content-length-h)：非负整数可直接传入，负数在构造时抛出 `std::invalid_argument`；已解析或已计算的长度仍以 `std::uint64_t` 返回。

### 9.4 `"http_cookie.h"`
<a id="api-http-cookie-h"></a>

Cookie 协议规则（RFC 6265 / RFC 6265bis）：属性枚举与常量、`Cookie` 请求对写出、Cookie 值与名称前缀校验、`Cookie` 字段遍历、服务端 `Set-Cookie` 写出计划 `set_cookie_plan`，以及面向出站客户端的 `Set-Cookie` 借用解析 `parse_set_cookie()`。无条件编译；公开依赖 [`"http_header.h"`](#api-http-header-h)（`http_trim_ows`）与 [`"borrowed_text.h"`](#api-borrowed-text-h)。Web 层如何在响应上设置或删除 Cookie 见 [ruvia-web](ruvia-web.md)。源码：[`ruvia-http/include/http_cookie.h`](../ruvia-http/include/http_cookie.h)

#### 9.4.1 枚举与常量

```cpp
enum class cookie_prefix : std::uint8_t {
    secure,
    host,
};

enum class cookie_same_site : std::uint8_t {
    strict,
    lax,
    none,
};

enum class cookie_priority : std::uint8_t {
    low,
    medium,
    high,
};

enum class cookie_attribute_policy : std::uint8_t {
    omit,
    emit,
};

inline constexpr std::chrono::system_clock::time_point cookie_deletion_expires{};

inline constexpr std::int64_t max_cookie_age_seconds = 34560000;

enum class http_set_cookie_attribute : std::uint8_t {
    secure = 1U << 0,
    partitioned = 1U << 1,
    http_only = 1U << 2,
};
```

| 枚举 / 成员 | 含义 |
| --- | --- |
| `cookie_prefix::secure` | 名称写为 `__Secure-<name>`；要求 `Secure` |
| `cookie_prefix::host` | 名称写为 `__Host-<name>`；要求 `Secure`、`Path=/` 且无 `Domain` |
| `cookie_same_site::strict` / `lax` | 写出 `SameSite=Strict` / `SameSite=Lax` |
| `cookie_same_site::none` | 字面 `SameSite=None`；要求 `Secure` |
| `cookie_priority::low` / `medium` / `high` | 写出 `Priority=Low` / `Medium` / `High` |
| `cookie_attribute_policy::omit` | 不写出该布尔属性 |
| `cookie_attribute_policy::emit` | 写出该布尔属性 |
| `http_set_cookie_attribute::secure` | `1U << 0`：解析到 `Secure` |
| `http_set_cookie_attribute::partitioned` | `1U << 1`：解析到 `Partitioned` |
| `http_set_cookie_attribute::http_only` | `1U << 2`：解析到 `HttpOnly` |

- `cookie_deletion_expires`：删除 Cookie 用的 `Expires` 值（`Thu, 01 Jan 1970 00:00:00 GMT`，即 `system_clock` 纪元）。RFC 6265 §4.1.1 要求 `max-age-av` 首位非零，因此删除采用过去的 `Expires`（§3.1）而非 `Max-Age=0`。
- `max_cookie_age_seconds`：RFC 6265bis 的用户代理 Cookie 寿命上限（400 天）。接收侧（`parse_set_cookie()` 与 [`http_cookie_jar`](#api-http-cookie-jar-h)）据此封顶；`Set-Cookie` 写出方不强制，因为该上限约束用户代理而非服务端。

#### 9.4.2 `Cookie` 请求对

```cpp
[[nodiscard]] constexpr std::size_t cookie_request_pair_size(std::string_view name, std::string_view value) noexcept;

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value);

template <typename string>
void append_cookie_request_pair(string& header_value, std::string_view name, std::string_view value);
```

| 函数 | 语义 |
| --- | --- |
| `cookie_request_pair_size(name, value)` | `name.size() + 1 + value.size()`：一个 `name=value` 对的字节数。`=` 总会写出（包括空名），使形似 cookie-pair 的无名值仍位于 `=` 之后；多个对由调用方以 `"; "` 连接（RFC 6265 §4.2.1） |
| `write_cookie_request_pair(out, name, value)` | 在 `out` 写入恰好 `cookie_request_pair_size()` 字节并返回写入末尾。不校验内容。只有与写入区重叠的输入需要快照：重叠部分合计不超过 256 字节时使用栈缓冲，否则堆分配；未重叠输入不占临时内存，名称与值同时重叠时共享一份快照。分配失败抛出 `std::bad_alloc` 且输出未被修改 |
| `append_cookie_request_pair(header_value, name, value)` | 向字符串类容器追加一个对，非空时先追加 `"; "` 分隔符。`name` / `value` 可以借用 `header_value` 自身（包括其结尾 NUL）。长度不可表示时抛出 `std::length_error`；任何失败都恢复原内容（容量可能已增长）。扩容可能重新分配，使先前取得的 `header_value` 视图失效，重试时须重新获取 |

#### 9.4.3 校验、前缀与 `Cookie` 字段遍历

```cpp
[[nodiscard]] bool is_valid_cookie_value(std::string_view value) noexcept;
[[nodiscard]] std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept;
[[nodiscard]] bool cookie_prefix_requirements_met(cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept;
[[nodiscard]] std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept;

template <typename visitor_type>
inline void http_visit_cookie_pairs(std::string_view value, visitor_type&& visitor);
```

| 函数 | 语义 |
| --- | --- |
| `is_valid_cookie_value` | RFC 6265 §4.1.1 `cookie-value`：`*cookie-octet`，或一对 DQUOTE 包围的 `*cookie-octet`（引号属于值本身）。`cookie-octet` 排除控制字符、SP、`"`、`,`、`;`、`\` 与 `0x7F` 及以上字节；空值合法 |
| `cookie_name_prefix(name)` | 线路名称以 `__Host-` 或 `__Secure-` 开头时（与用户代理一样按 ASCII 大小写不敏感匹配，RFC 6265bis §4.1.3）返回对应前缀，否则 `std::nullopt` |
| `cookie_prefix_requirements_met(prefix, secure, path, domain)` | RFC 6265bis §4.1.3：`secure` 前缀要求 `secure`；`host` 前缀还要求 `path == "/"` 且 `domain` 为空。`path` 为 `Path` 属性值，缺失时传空 |
| `http_cookie_prefix_text(prefix)` | 返回静态字面量 `"__Secure-"` 或 `"__Host-"`；未定义枚举值返回空 |
| `http_visit_cookie_pairs(value, visitor)` | 单遍识别 `;` 分段和段内第一个 `=`；引号无特殊含义，不含 `=` 的段跳过。名称与值去除首尾 OWS；`visitor(name, value)` 返回 `false` 时停止。视图借用 `value` |

#### 9.4.4 `struct cookie_options`

```cpp
struct cookie_options final {
    ::ruvia::borrowed_text path_{"/"};
    ::ruvia::borrowed_text domain_{};
    std::optional<cookie_same_site> same_site_{};
    std::optional<cookie_priority> priority_{};
    std::optional<std::chrono::system_clock::time_point> expires_{};
    std::optional<std::chrono::seconds> max_age_{};
    std::optional<cookie_prefix> prefix_{};
    cookie_attribute_policy http_only_{cookie_attribute_policy::omit};
    cookie_attribute_policy secure_{cookie_attribute_policy::omit};
    cookie_attribute_policy partitioned_{cookie_attribute_policy::omit};
};
```

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `path_` | `"/"` | `Path` 属性；为空时不写出。`borrowed_text` 保持零拷贝但拒绝临时拥有型字符串，避免存储的选项值内含已悬空的视图；属性由 `set_cookie_plan` 借用至序列化完成 |
| `domain_` | 空 | `Domain` 属性；为空时不写出（host-only） |
| `same_site_` | 空 | `SameSite` 属性 |
| `priority_` | 空 | `Priority` 属性 |
| `expires_` | 空 | `Expires`：按 UTC 向下取整到秒，年份须不早于 1601；寿命上限属于用户代理 |
| `max_age_` | 空 | `Max-Age`：正值照写；`0` 表示立即过期，写为 `Expires=cookie_deletion_expires`（替代 `expires_`）且不写 `Max-Age`；负值在转为无符号计数前被拒绝 |
| `prefix_` | 空 | 类型化名称前缀，写在名称之前 |
| `http_only_` / `secure_` / `partitioned_` | `omit` | `HttpOnly` / `Secure` / `Partitioned` 布尔属性 |

#### 9.4.5 `class set_cookie_plan`

```cpp
class set_cookie_plan final {
public:
    set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options);

    template <typename name_type, typename value_type>
        requires(/* 未指定实现约束：name_type 或 value_type 为临时拥有型 std::basic_string<char, ...> */)
    set_cookie_plan(name_type&&, value_type&&, const cookie_options&) = delete;

    set_cookie_plan(std::string_view, std::string_view, cookie_options&&) = delete;
    set_cookie_plan(std::string_view, std::string_view, const cookie_options&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view wire_prefix() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view domain() const noexcept;

    void write(char* output) const;
};
```

在运行时分配输出缓冲区之前校验并固定 `Set-Cookie` 字段值的精确形状。计划借用 `name`、`value` 以及 `options` 的 `path_`、`domain_` 原文直至 `write()` 完成，因此删除了三类重载：任一参数为临时 `std::string` 等拥有型字符串、`cookie_options` 右值、`const cookie_options` 右值。计划本身可拷贝（拷贝仍借用同一输入）。

构造函数先计算完整线路长度（防止超长视图进入语法扫描或使后续写入长度回绕；`std::size_t` 无法表示时抛出 `std::length_error`），再校验，失败抛出 `std::invalid_argument`：

- 名称不是 token，或值不满足 `is_valid_cookie_value()`；
- 前缀 + 名称 + 值超过 4096 字节（RFC 6265bis §5.6 存储上限：发送方拒绝接收方会截断或丢弃的 Cookie）；
- `path_` 含 `%x20-7E` 以外字节或 `;`、超过 1024 字节、或带首尾空白；`domain_` 非空且不是 LDH 域名（每标签不超过 63 字节、总长不超过 253）；
- `expires_`（或 `max_age_ == 0` 产生的删除日期）早于 1601 年或无法表示为 HTTP-date；`max_age_` 为负；
- 枚举字段为未定义值；
- `SameSite=None` 或 `Partitioned` 未同时 `Secure`；
- 名称前缀约束未满足：类型化 `prefix_` 优先；未设置时对字面名称按大小写不敏感检测 `__Host-` / `__Secure-`，使写出的每个 Cookie 都能被用户代理实际存储。

| 成员 | 语义 |
| --- | --- |
| `size()` | 线路字节数，`write()` 恰好写入这么多字节 |
| `name()` | 不含类型化前缀的名称 |
| `wire_prefix()` | 类型化前缀文本（`"__Secure-"` / `"__Host-"`），无则为空 |
| `path()` / `domain()` | 借用的属性值 |
| `write(output)` | 向 `output` 写入 `size()` 字节；`output` 不得与借用输入重叠。写出顺序固定：`<prefix><name>=<value>`，然后按需 `; Path=`、`; Domain=`、`; Max-Age=`、`; Expires=`、`; HttpOnly`、`; Secure`、`; SameSite=`、`; Priority=`、`; Partitioned` |

示例（字段均为字面量或左值，满足借用要求）：

```cpp
const ruvia::cookie_options options{
    .same_site_ = ruvia::cookie_same_site::lax,
    .max_age_ = std::chrono::seconds(3600),
    .http_only_ = ruvia::cookie_attribute_policy::emit,
};
const ruvia::set_cookie_plan plan("session", "example", options);
std::string wire(plan.size(), '\0');
plan.write(wire.data());
// session=example; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax
```

#### 9.4.6 `class http_set_cookie_view` 与 `parse_set_cookie`

```cpp
class http_set_cookie_view final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept;
    [[nodiscard]] constexpr std::string_view value() const noexcept;
    [[nodiscard]] constexpr std::string_view path() const noexcept;
    [[nodiscard]] constexpr std::string_view domain() const noexcept;
    [[nodiscard]] constexpr std::optional<std::time_t> expires() const noexcept;
    [[nodiscard]] constexpr std::optional<std::int64_t> max_age_seconds() const noexcept;
    [[nodiscard]] constexpr bool has(http_set_cookie_attribute attribute) const noexcept;
    [[nodiscard]] constexpr bool http_only() const noexcept;
    [[nodiscard]] constexpr bool same_site_none() const noexcept;
    [[nodiscard]] constexpr std::optional<cookie_same_site> same_site() const noexcept;
};

[[nodiscard]] std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

template </* 未指定实现约束：临时拥有型 std::basic_string<char, ...> */ typename value_type>
std::optional<http_set_cookie_view> parse_set_cookie(value_type&&) = delete;
```

面向出站客户端运行时、按 RFC 6265 §5.2 用户代理算法得到的借用型、零分配 `Set-Cookie` 视图。只能由 `parse_set_cookie()` 产生；所有视图借用传入的字段值，因此拒绝临时拥有型字符串的重载为 `= delete`。

分段时同一遍识别 `;`、段内首个 `=` 并校验控制字节；后续 `=` 保留在值内。

`parse_set_cookie(value)` 返回 `std::nullopt` 的情形：首个 `;` 之前的 cookie-pair 不含 `=`、名称为空、名称加值超过 4096 字节、或名称、值、任一属性名或属性值含 HTAB 以外的控制字节（`0x00-0x08`、`0x0A-0x1F`、`0x7F`）。名称与值去除首尾 OWS。属性名大小写不敏感；未知属性与值超过 1024 字节的属性被忽略。

| 成员 | 语义 |
| --- | --- |
| `name()` / `value()` | cookie-pair 的名称与值 |
| `path()` | 最后一个 `Path` 的值；若最后一个值为空或不以 `/` 开头则为空，表示由调用方使用 default-path（RFC 6265 §5.2.4） |
| `domain()` | 最后一个非空 `Domain` 值，去掉一个前导 `.`，因此可能为空（host-only，例如 `Domain=.`）；空 `Domain` 被忽略。除调用方的域匹配外不做进一步校验 |
| `expires()` | 最后一个可解析的 `Expires`，按 RFC 6265 §5.1.1 cookie-date 算法（含其允许的后缀，及勘误 4148），两位年份映射 70–99 → 19xx、0–69 → 20xx，早于 1601 年无效；无效值被忽略。该算法不放宽另行定义的 HTTP-date 语法 |
| `max_age_seconds()` | 最后一个有效的 `Max-Age`：十进制校验与转换单遍完成，非法值（空、非数字、`+` 号等）不覆盖此前有效值；正值封顶为 `max_cookie_age_seconds`，仅对完整数字应用饱和（正溢出为上限，负溢出为 `INT64_MIN`）；可能为零或负数，由存储方判定为立即过期 |
| `has(attribute)` | 单一位掩码查询，非单一位返回 `false` |
| `http_only()` | `has(http_set_cookie_attribute::http_only)` |
| `same_site()` | 最后一个 `SameSite`：`Strict` / `Lax` / `None`（大小写不敏感）；其他值把结果重置为空 |
| `same_site_none()` | `same_site() == cookie_same_site::none` |

`Partitioned` 独立于 `Secure` 报告；来源策略（`Secure` 与方案、域匹配、前缀约束）由调用方负责，参见 [`http_cookie_jar`](#api-http-cookie-jar-h)。

### 9.5 `"http_cookie_jar.h"`
<a id="api-http-cookie-jar-h"></a>

固定源站的 Sans-I/O RFC 6265 客户端 Cookie 容器：吸收响应 `Set-Cookie`、按请求目标生成出站 `Cookie` 首部，执行域、路径、`Secure`、`SameSite=None`、`Partitioned` 与 `__Secure-` / `__Host-` 前缀校验，`Max-Age` / `Expires` 400 天封顶、过期淘汰与数量/字节预算。无条件编译；公开依赖 [`"http_header.h"`](#api-http-header-h) 与 [`"http_message.h"`](#api-http-message-h)（`http_scheme`）；以源站构造需要 [`"http_authority.h"`](#api-http-authority-h) 的 `http_origin_view`；解析规则来自 [`"http_cookie.h"`](#api-http-cookie-h)。源码：[`ruvia-http/include/http_cookie_jar.h`](../ruvia-http/include/http_cookie_jar.h)

#### 9.5.1 `struct http_cookie_jar_options`

```cpp
struct http_cookie_jar_options final {
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
};
```

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `max_cookies_` | `256` | 可存储的接收 Cookie 条目上限 |
| `max_cookie_bytes_` | `32 * 1024` | 全部条目名称、值、路径、域字节数之和的上限 |

#### 9.5.2 `class http_cookie_jar`

```cpp
class http_cookie_jar final {
public:
    explicit http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options = {});
    http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options = {});

    void add_configured_cookie(std::string_view name, std::string_view value);

    bool absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    void retain_response_cookies(std::string_view request_target, const http_header_range_view& headers, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    [[nodiscard]] std::string cookie_header(std::string_view request_target, bool include_received = true, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    template <typename fn_type>
    void visit_matching_cookies(std::string_view request_target, bool include_received, std::chrono::system_clock::time_point now, fn_type&& fn);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t storage_bytes() const noexcept;
    void clear() noexcept;

    [[nodiscard]] static bool cookie_domain_matches(std::string_view host, std::string_view domain) noexcept;
    [[nodiscard]] static bool cookie_path_matches(std::string_view request_path, std::string_view cookie_path) noexcept;
    [[nodiscard]] static std::string_view request_path_only(std::string_view target) noexcept;
    [[nodiscard]] static std::string_view default_cookie_path(std::string_view target) noexcept;
};
```

容器拥有全部存储（复制名称、值、路径、域），可拷贝、可移动；内部无同步，同一实例的并发调用需由调用方串行化。`request_target` 为 origin-form 请求目标（路径加可选查询）。

| 成员 | 语义 |
| --- | --- |
| 构造函数 | 固定源站的方案与主机；方括号包围的 IPv6 主机去掉括号保存。`http_origin_view` 重载取其 `scheme()` 与 `host()` |
| `add_configured_cookie(name, value)` | 追加一个应用配置的 Cookie：路径 `/`、host-only、非 `Secure`、永不过期；不校验内容，不受 `max_cookies_` / `max_cookie_bytes_` 拒绝，但其字节计入 `storage_bytes()`。配置 Cookie 不会被接收的同名 Cookie 替换，且总会发送（与 `include_received` 无关） |
| `absorb_set_cookie(target, value, now)` | 吸收一个 `Set-Cookie` 值。先淘汰已过期条目；再以 `parse_set_cookie()` 解析；以下情形拒绝并返回 `false`：解析失败、非 `https` 源站收到 `Secure`、`Domain` 与主机不域匹配、`SameSite=None` 或 `Partitioned` 缺少 `Secure`、名称前缀约束不满足、新增或替换后超出容量。返回 `true` 表示已存储、已替换或已按删除处理 |
| `retain_response_cookies(target, headers, now)` | 对首部块中每个 `Set-Cookie` 行（大小写不敏感）执行与 `absorb_set_cookie` 相同的处理，被拒绝的行静默忽略 |
| `cookie_header(target, include_received, now)` | 生成出站 `Cookie` 字段值（以 `"; "` 连接，经 `append_cookie_request_pair()` 写出）；没有匹配时返回空字符串 |
| `visit_matching_cookies(target, include_received, now, fn)` | 先在 `now` 不早于最早过期时刻时淘汰过期条目，再按存储顺序对每个匹配条目调用 `fn(std::string_view name, std::string_view value)`。匹配条件：配置 Cookie，或 `include_received` 为真时的接收 Cookie；`Secure` 条目仅在源站方案为 `https` 时匹配；`cookie_path_matches(request_path_only(target), path)` 成立。视图借用容器存储，在下一次修改容器前有效 |
| `size()` / `empty()` | 条目数（含配置 Cookie） |
| `storage_bytes()` | 全部条目名称、值、路径、域字节数之和 |
| `clear()` | 删除全部条目（含配置 Cookie） |
| `cookie_domain_matches(host, domain)` | RFC 6265 §5.1.3：`domain` 为空、或与 `host` 大小写不敏感相等时为真；`host` 为 IP 字面量时只允许相等；否则要求 `host` 是 `domain` 在标签边界上的 DNS 子域。不内置公共后缀列表 |
| `cookie_path_matches(request_path, cookie_path)` | RFC 6265 §5.1.4：Cookie 路径为空或 `/` 时匹配任何以 `/` 开头的路径；否则要求请求路径以 Cookie 路径开头，且二者相等、Cookie 路径以 `/` 结尾、或请求路径在该前缀之后紧接 `/` |
| `request_path_only(target)` | 截去第一个 `?` 或 `#` 及之后的部分；视图借用 `target` |
| `default_cookie_path(target)` | RFC 6265 §5.1.4 default-path：路径为空、不以 `/` 开头或只有一个 `/` 时为 `"/"`，否则为最后一个 `/` 之前的部分（不含该 `/`）；视图借用 `target` 或静态字面量 |

`request_path_only()` 与 `default_cookie_path()` 在编译期拒绝临时拥有型字符串和字符初始化列表。

接收 Cookie 的存储规则：

- 路径取 `Set-Cookie` 的 `Path`，缺失或无效时取 `default_cookie_path(target)`。
- **过期**：`Max-Age` 优先于 `Expires`。`Max-Age <= 0` 或 `Expires <= now` 删除同一标识的条目（不存在也返回 `true`）。有效 `Max-Age` 在解析时封顶为 `max_cookie_age_seconds`；`Expires` 最晚为 `now + 400 天`。期限计算保留时钟精度，超出可表示范围时饱和到时钟上界。两者均缺省时为会话 Cookie。
- 标识为（名称、路径、`Partitioned` 标志、域）：域为空时以源站主机代替，域比较大小写不敏感。命中时替换值、域、过期时间、`Secure` 与 `Partitioned`；值或域缩至原缓冲容量一半以下（含一半）时释放过大的堆缓冲。
- 新条目插入在第一个路径更短的条目之前，使输出顺序满足 RFC 6265 §5.4：路径更长者在前，同长度保持插入顺序。
- 容量：新增条目在条目数已达 `max_cookies_` 时被拒绝；新增或替换后字节总数超过 `max_cookie_bytes_` 时被拒绝。容器不会为腾出空间而淘汰未过期的条目，过期条目在吸收或生成首部时惰性淘汰。

### 9.6 `"http_cors.h"`
<a id="api-http-cors-h"></a>

无状态、预先校验的 CORS 策略原语（Fetch 标准 CORS 协议）：按请求求值预检、改写代理响应的 `Access-Control-*` 首部、为服务端响应应用 CORS 首部，以及 `Vary` 令牌合并。`ruvia-web` 的宿主级 CORS 配置复用该策略，其配置入口见 [ruvia-web](ruvia-web.md)。无条件编译；公开依赖 [`"http_header.h"`](#api-http-header-h)（`http_header`、`editable_http_headers`）、[`"http_response.h"`](#api-http-response-h) 与 [`"borrowed_text.h"`](#api-borrowed-text-h)；以 `const http_request&` 为参数的成员需要 [`"http_request.h"`](#api-http-request-h)。源码：[`ruvia-http/include/http_cors.h`](../ruvia-http/include/http_cors.h)

#### 9.6.1 `struct http_cors_policy_options`

```cpp
struct http_cors_policy_options final {
    bool allow_any_origin_{false};
    std::vector<std::string> allowed_origins_;
    std::vector<std::string> allowed_origin_suffixes_;
    std::vector<std::string> allowed_methods_;
    std::vector<std::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::vector<std::string> expose_headers_;
    bool allow_credentials_{false};
    std::optional<std::chrono::seconds> max_age_{};
};
```

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `allow_any_origin_` | `false` | 允许任意 Origin（响应 `Access-Control-Allow-Origin: *`） |
| `allowed_origins_` | 空 | 精确 WHATWG 序列化源（如 `"https://example.com"`、`"null"`）、`"*"`（等价于任意 Origin），或以 `.` / `*.` 开头（或含 `://.` / `://*.`）的尾缀模式（按 `allowed_origin_suffixes_` 规则处理） |
| `allowed_origin_suffixes_` | 空 | 在标签边界上匹配的域名尾缀：`"example.com"` 匹配该主机本身及其子域；`".example.com"` / `"*.example.com"` 只匹配子域；可加方案限定（`"https://.example.com"`，方案只能是 `http` / `https`）与端口限定 |
| `allowed_methods_` | 空 | 允许的预检方法 token；有 `"*"` 时允许任意合法方法 token，不再用同列表的具体方法生成固定授权首部 |
| `allowed_headers_` | 空 | 允许的请求首部名；`"*"` 表示反射 `Access-Control-Request-Headers` |
| `reflect_request_headers_` | `false` | 反射 `Access-Control-Request-Headers` |
| `expose_headers_` | 空 | 写入 `Access-Control-Expose-Headers` 的字段名 |
| `allow_credentials_` | `false` | 写出 `Access-Control-Allow-Credentials: true`；要求显式 Origin 列表或尾缀 |
| `max_age_` | 空 | 写出 `Access-Control-Max-Age` |

默认值的有效含义：`allowed_origins_` 与 `allowed_origin_suffixes_` 都没有有效条目时允许任意 Origin；`allowed_methods_` 为空时允许任意合法方法；`allowed_headers_` 为空时反射请求首部。因此默认构造的 `http_cors_policy_options{}` 允许任意 Origin（`*`）、任意合法方法并反射 `Access-Control-Request-Headers`。

#### 9.6.2 `struct http_cors_preflight_input` 与 `class http_cors_preflight_result`

```cpp
struct http_cors_preflight_input final {
    borrowed_text method_{"OPTIONS"};
    borrowed_text origin_{};
    borrowed_text access_control_request_method_{};
    borrowed_text access_control_request_headers_{};
};

class http_cors_preflight_result final {
public:
    http_cors_preflight_result() noexcept = default;
    http_cors_preflight_result(bool accepted, std::vector<http_header> headers) noexcept;

    [[nodiscard]] constexpr bool accepted() const noexcept;
    [[nodiscard]] constexpr explicit operator bool() const noexcept;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::vector<http_header> take_headers() && noexcept;
};
```

- `http_cors_preflight_input`：脱离 `http_request` 的预检输入；`method_` 默认 `"OPTIONS"`，其余为对应请求字段的值（多行 `Access-Control-Request-Headers` 由调用方以逗号连接）。`borrowed_text` 字段拒绝临时拥有型字符串。
- `http_cors_preflight_result`：默认构造为拒绝且无首部。`accepted()` 与显式 `operator bool` 报告是否接受；`headers()` 借用结果对象（右值重载 `= delete`）；`take_headers() &&` 移出首部向量，并把对象置为未接受。

#### 9.6.3 `class http_cors_policy`

```cpp
class http_cors_policy final {
public:
    explicit http_cors_policy(const http_cors_policy_options& config);

    [[nodiscard]] bool allow_any_origin() const noexcept;
    [[nodiscard]] bool allow_credentials() const noexcept;
    [[nodiscard]] bool reflect_request_headers() const noexcept;
    [[nodiscard]] bool allows_origin(std::string_view origin) const noexcept;
    [[nodiscard]] bool allows_method(std::string_view method) const noexcept;
    [[nodiscard]] bool allows_request_header(std::string_view name) const noexcept;

    [[nodiscard]] http_cors_preflight_result evaluate_preflight(http_cors_preflight_input input, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(borrowed_text method, const http_header_range_view& headers, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(const http_request& request, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;

    bool rewrite_response_headers(borrowed_text request_origin, editable_http_headers response_headers) const;
    bool apply_response_headers(const http_request& request, http_response& response) const;
};
```

构造后不可变，所有成员为 `const`，可在多线程间共享只读使用。构造函数复制并校验配置，失败抛出 `std::invalid_argument`：精确 Origin 不是 WHATWG 序列化源或 `"null"`；尾缀模式为空、方案不是 `http` / `https`、主机不是合法域名（IP 字面量、空端口、首尾为 `.` 均拒绝）；方法不是合法 token；首部名或公开首部名不是合法字段名；`max_age_` 为负；任意 Origin 与 `allow_credentials_` 同时生效（Fetch 禁止 `*` 搭配凭据，策略在构造时失败而不是反射任意 Origin）。

| 成员 | 语义 |
| --- | --- |
| `allow_any_origin()` / `allow_credentials()` / `reflect_request_headers()` | 归一化后的有效设置（含上节默认推导） |
| `allows_origin(origin)` | `origin` 必须是合法序列化源或 `"null"`；任意 Origin 时为真；`"null"` 仅在精确列表包含 `"null"` 时为真；精确源按方案与 authority ASCII 大小写不敏感比较；尾缀规则依次检查方案、端口、主机（非子域模式允许主机相等，子域模式要求标签边界上的 DNS 子域）；IP 字面量 Origin 不匹配尾缀规则 |
| `allows_method(method)` | 合法方法 token，且允许任意方法或在列表中（区分大小写，RFC 9110 §9.1） |
| `allows_request_header(name)` | 合法字段名，且反射模式或在列表中（大小写不敏感） |
| `evaluate_preflight(input)` | `method_` 必须精确为 `"OPTIONS"`、`origin_` 非空且被允许、`access_control_request_method_`（去除 OWS 后）非空且被允许；`access_control_request_headers_` 非空时每个名称都须被允许且列表语法完整合法（固定授权模式也一样）。接受时按序生成：`Access-Control-Allow-Origin`（任意 Origin 为 `*`，否则回显 Origin）、`Access-Control-Allow-Credentials: true`（启用凭据时）、`Access-Control-Allow-Methods`（配置列表，未配置时为请求的方法）、`Access-Control-Allow-Headers`（反射模式为请求列表合并成的单一字段，否则为配置列表；为空时省略）、`Access-Control-Max-Age`（配置时）、预检 `Vary`。拒绝时返回默认结果 |
| `evaluate_preflight(method, headers, maximum)` / `evaluate_preflight(request, maximum)` | 要求唯一的 `Origin` 与 `Access-Control-Request-Method`；逐行单遍扫描 `Access-Control-Request-Headers`，不先拼接输入。所有行共享调用方给定的名称数量上限（含重复名称；默认不额外收紧宿主限额），拒绝 `*`、空行与非法列表；仅反射输出时构造合并文本。输入结构体重载复用同一主链。 |
| `rewrite_response_headers(origin, headers)` | 用于转发/代理的实际响应：在一个首部事务中删除全部受控 `Access-Control-*` 首部并合并 `Vary: Origin`。`origin` 非空且被允许时写入 `Access-Control-Allow-Origin`、按需 `Access-Control-Allow-Credentials` 与 `Access-Control-Expose-Headers`。允许 `origin` 借用待编辑的首部。返回是否授权。目标为 `http_response` 时全部成功或保持原样 |
| `apply_response_headers(request, response)` | 服务端响应（含 `OPTIONS` 预检）：先合并 `Vary`（`OPTIONS` 使用预检维度；其他方法合并 `Origin`，通配策略也不例外）。随后在以下情形返回 `false` 且不添加授权首部：`Origin` 重复或存在但不被允许；`OPTIONS` 的 `Access-Control-Request-Method` 重复或不被允许；`OPTIONS` 的任一 `Access-Control-Request-Headers` 名称不被允许或列表无效。否则只补齐响应尚未设置的首部，不覆盖处理器已设置的值：`Access-Control-Allow-Origin`（任意 Origin 为 `*`；否则回显 `Origin`；没有 `Origin` 而配置恰为单个精确源且无尾缀时写该源）、`Access-Control-Allow-Credentials`；预检（`OPTIONS` 且同时携带 `Origin` 与 `Access-Control-Request-Method`）再补 `Access-Control-Allow-Methods`（配置列表；未固定时把响应已有的非空 `Allow` 提升为该字段；否则为请求的方法）、`Access-Control-Allow-Headers`（固定列表或合并后的反射列表）、`Access-Control-Max-Age`；非预检补 `Access-Control-Expose-Headers`。返回 `true` |

- **预检缓存维度**：`Origin`、`Access-Control-Request-Method`、`Access-Control-Request-Headers` 均计入 `Vary`；请求字段即使不反射，也会影响是否授权。

`evaluate_preflight`、`rewrite_response_headers`、`apply_response_headers` 可能抛出 `std::bad_alloc`。

#### 9.6.4 自由函数

```cpp
[[nodiscard]] bool is_http_cors_controlled_response_header(std::string_view name) noexcept;

void merge_http_vary_tokens(editable_http_headers headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(editable_http_headers headers, std::string_view token);
```

| 函数 | 语义 |
| --- | --- |
| `is_http_cors_controlled_response_header(name)` | 大小写不敏感地判断是否为策略控制的响应首部：`Access-Control-Allow-Origin`、`Access-Control-Allow-Credentials`、`Access-Control-Allow-Methods`、`Access-Control-Allow-Headers`、`Access-Control-Expose-Headers`、`Access-Control-Max-Age` |
| `merge_http_vary_tokens(headers, tokens)` | 逐个令牌去除 OWS、跳过空令牌；遇到 `*` 时若块中尚无 `*` 则把全部 `Vary` 行替换为 `Vary: *` 并结束；其他令牌必须是合法字段名，否则在修改前抛出 `std::invalid_argument`；块中已列出（大小写不敏感，或已有 `*`）的令牌与本次重复令牌被跳过。有缺失令牌时把现有 `Vary` 行的值与缺失令牌合并为一个 `Vary` 行；没有缺失令牌时不修改。写入失败可能抛出 `std::bad_alloc` |
| `merge_http_vary_token(headers, token)` | 单令牌形式 |

### 9.7 `"http_content_negotiation.h"`
<a id="api-http-content-negotiation-h"></a>

主动内容协商（RFC 9110 §12）原语：媒体类型截取与 `Content-Type` 校验、`Accept` / `Accept-Charset` / `Accept-Language` 的增量匹配，以及 `Accept-Encoding` 质量快照、响应编码候选集、选择结果与 `Content-Encoding` 可接受性求值。无条件编译；公开依赖 [`"http_content_coding.h"`](#api-http-content-coding-h)（`http_content_coding`）与 [`"borrowed_text.h"`](#api-borrowed-text-h)；以请求/响应为参数的函数需要 [`"http_request.h"`](#api-http-request-h) / [`"http_response.h"`](#api-http-response-h)。源码：[`ruvia-http/include/http_content_negotiation.h`](../ruvia-http/include/http_content_negotiation.h)

#### 9.7.1 媒体类型

```cpp
[[nodiscard]] std::string_view http_media_type_only(borrowed_text value) noexcept;

[[nodiscard]] bool is_valid_http_content_type_field_value(std::string_view value) noexcept;
[[nodiscard]] std::optional<int> parse_http_quality_value(std::string_view value) noexcept;
[[nodiscard]] std::optional<http_media_range> parse_http_media_range(borrowed_text value) noexcept;
```

- `http_media_type_only(value)`：返回第一个 `;` 之前、去除 OWS 的媒体类型部分（不做小写化）。视图借用输入；参数为 `borrowed_text`，因此拥有型字符串临时量无法传入。
- `is_valid_http_content_type_field_value(value)`：校验单个 `Content-Type` 值（RFC 9110 §8.3）：`type/subtype` 均为 token（不允许通配）；允许 RFC 9110 的空分号参数槽；非空参数必须是 `name=value`，`=` 两侧无空白，值为 token 或 quoted-string，参数名不得重复；非空参数超过 64 个时无效。
- **权重原语**：`parse_http_quality_value` 严格解析单项 qvalue（无首尾空白），返回千分制 `0..1000`，非法与合法零权重可区分。
- **媒体范围原语**：`http_media_range` 暴露借用的 `type_`、`subtype_`、`quality_`（默认 1000）与 `parameter_count_`；`parse_http_media_range` 直接支持 `*/*`、`type/*`，复用有界参数解析，无需修改输入来提取权重。

#### 9.7.2 `Accept` 匹配

```cpp
enum class http_accept_token_match_mode : std::uint8_t {
    exact,
    language_prefix,
};

class http_accept_match final {
public:
    void update_media_type(std::string_view field, std::string_view offered) noexcept;
    void update_token(std::string_view field, std::string_view offered, http_accept_token_match_mode mode) noexcept;

    [[nodiscard]] bool matched() const noexcept;
    [[nodiscard]] int quality() const noexcept;
};
```

| 成员 | 含义 |
| --- | --- |
| `http_accept_token_match_mode::exact` | 令牌按 ASCII 大小写不敏感完全匹配，或由 `*` 匹配（如 `Accept-Charset`） |
| `http_accept_token_match_mode::language_prefix` | RFC 4647 §3.3.1 基本过滤：语言范围等于标签，或在子标签边界上是其前缀（`en` 匹配 `en-US`，不匹配 `english`），或为 `*`（`Accept-Language`） |

`http_accept_match` 累积一个候选表示跨多行 `Accept*` 字段的匹配结果，等价于逗号连接后的字段，但不分配也不复制。默认构造为“无匹配”。对每个候选表示使用一个独立对象，对该字段的每一行调用一次 `update_*`。

- 更具体的匹配覆盖较不具体的匹配，包括 `q=0` 排除；同等具体度取最高质量，与字段顺序无关。
- `update_media_type(field, offered)`：`offered` 必须是合法媒体类型（不含通配），否则不更新。具体度依次为 `*/*` < `type/*` < `type/subtype`；同级时匹配的非 `q` 参数越多越具体。媒体范围允许空参数槽，它们不影响具体度与质量；非空参数约束候选类型，即使位于 `q` 之后。参数名大小写不敏感；参数值比较：`charset`，以及仅在 `text/plain` 上的 `format`、`delsp` 忽略 ASCII 大小写（[RFC 3676 §4](https://www.rfc-editor.org/rfc/rfc3676.html#section-4)），其他参数值区分大小写，带引号值按解码后的字节比较。媒体范围或候选类型的非空参数（含 `q`）超过 64 个时无效且永不匹配；非法 `q` 视为 `0`。
- `update_token(field, offered, mode)`：令牌字段只允许可选的 `q` 权重，未知或额外参数使该成员不可接受（质量 `0`）。`language_prefix` 模式取最长的匹配范围。
- `matched()`：存在具体度不小于通配且质量大于 0 的匹配。
- `quality()`：未匹配时为 `0`，否则为 HTTP qvalue 的千分制值（`1` 到 `1000`）。

#### 9.7.3 `Accept-Encoding` 质量

```cpp
struct http_accepted_encoding_quality {
    int explicit_quality_{-1};
    int wildcard_quality_{-1};

    void update(std::string_view accept_encoding, std::string_view coding) noexcept;

    [[nodiscard]] int quality(bool is_identity = false) const noexcept;
    [[nodiscard]] bool accepts(bool is_identity = false) const noexcept;
};

[[nodiscard]] bool http_accepts_encoding(std::string_view accept_encoding, std::string_view coding) noexcept;
[[nodiscard]] bool http_accepts_encoding(const http_request& request, std::string_view coding) noexcept;

struct http_response_coding_qualities final {
    bool field_present_{false};
    bool has_non_empty_item_{false};
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality deflate_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;

    void update(std::string_view accept_encoding) noexcept;

    [[nodiscard]] bool accepts(http_content_coding coding) const noexcept;
};
```

`Accept-Encoding` 协商（RFC 9110 §12.5.3）：请求表达的每种编码权重。所有质量值使用千分制，`-1` 表示未出现。

| 成员 | 语义 |
| --- | --- |
| `http_accepted_encoding_quality::explicit_quality_` | 显式列出该编码时的最高权重 |
| `http_accepted_encoding_quality::wildcard_quality_` | `*` 的最高权重 |
| `update(accept_encoding, coding)` | 吸收一行，为 `coding` 累积权重；同一编码重复出现取最高权重。`gzip` 与已弃用的 `x-gzip`、`compress` 与 `x-compress` 共享同一权重，名称大小写不敏感。成员若带 `q` 以外的参数或 `q` 非法，其权重为 `0` |
| `quality(is_identity)` | 显式权重优先于通配；对 `identity`，没有显式权重时默认 `1000`，除非 `*;q=0` 将其排除；对其他编码，没有显式权重时取正的通配权重，否则为 `0` |
| `accepts(is_identity)` | `quality(is_identity) > 0` |
| `http_accepts_encoding(value, coding)` | 单字段值判定；`value` 为空（显式空字段）时只接受 `identity` |
| `http_accepts_encoding(request, coding)` | 合并请求中全部 `Accept-Encoding` 行；字段缺失时接受任何编码。请求只在调用期间借用 |
| `http_response_coding_qualities` | 内置五种编码的质量快照。`field_present_`：至少吸收过一行；`has_non_empty_item_`：存在非空成员。缺失字段与显式空字段语义不同：缺失表示任何编码均可接受，空值只接受 identity（无编码）表示 |
| `http_response_coding_qualities::update(value)` | 吸收一行，置 `field_present_`，按 `http_content_coding_from_token()`（[`"http_content_coding.h"`](#api-http-content-coding-h)）映射已知编码，`*` 更新全部五种编码的通配权重 |
| `http_response_coding_qualities::accepts(coding)` | 该编码在快照中可接受：字段缺失时全部可接受；字段存在但无非空成员时只有 identity 可接受；否则按 `quality()` |

#### 9.7.4 响应编码候选与选择

```cpp
class http_response_coding_candidates final {
public:
    [[nodiscard]] static constexpr http_response_coding_candidates empty() noexcept;
    [[nodiscard]] static constexpr http_response_coding_candidates identity_only() noexcept;
    [[nodiscard]] static constexpr http_response_coding_candidates all() noexcept;

    constexpr http_response_coding_candidates& include(http_content_coding coding) noexcept;
    [[nodiscard]] constexpr bool contains(http_content_coding coding) const noexcept;
};

class http_response_coding_selection final {
public:
    [[nodiscard]] static http_response_coding_selection from_qualities(const http_response_coding_qualities& qualities) noexcept;
    [[nodiscard]] static http_response_coding_selection from_qualities(const http_response_coding_qualities& qualities, http_response_coding_candidates candidates) noexcept;

    [[nodiscard]] constexpr http_response_coding_selection select(http_response_coding_candidates candidates) const noexcept;
    [[nodiscard]] constexpr bool acceptable() const noexcept;
    [[nodiscard]] constexpr http_content_coding coding() const noexcept;
    [[nodiscard]] constexpr bool identity_accepted() const noexcept;
    [[nodiscard]] constexpr bool accepts(http_content_coding coding) const noexcept;
    [[nodiscard]] bool accepts(std::string_view coding, const http_request& request) const noexcept;
};

[[nodiscard]] http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept;
```

`http_response_coding_candidates`：表示策略实际能产生或取得的编码集合。用类型化集合代替原始 q 值整数，避免调用方重复实现 `Accept-Encoding` 排序。没有公开构造函数，从三个工厂开始：`empty()`（空集）、`identity_only()`、`all()`（`identity`、`gzip`、`deflate`、`brotli`、`zstd`）；`include(coding)` 加入一种编码并返回 `*this` 以便链式调用；`contains(coding)` 查询成员。

`http_response_coding_selection`：所选编码与 identity 回退判定来自同一份 `Accept-Encoding` 快照，避免运行时选择一个编码后又观察到过期或解析不同的 identity 质量。快照不借用请求。只能由 `from_qualities()`、`select()` 或 `http_response_coding_selection_for()` 得到。

| 成员 | 语义 |
| --- | --- |
| `from_qualities(qualities)` | 在全部可产生编码（`all()`）中选择 |
| `from_qualities(qualities, candidates)` | 在给定候选中选择：客户端 q 值最高者胜出；同分时按服务端偏好 `br > zstd > gzip > deflate > identity` 决胜；`q=0` 或客户端不接受的编码被排除。字段缺失时内置非 identity 编码的分值为 `999`、identity 为 `1000`，因此无 `Accept-Encoding` 时选择 identity；显式空字段只有 identity 可选 |
| `select(candidates)` | 用同一快照对另一组候选（例如某个表示存储实际持有的预压缩文件）重新排序，不再解析请求字段 |
| `acceptable()` | 客户端是否接受至少一个候选；为 `false` 表示协商失败，Web 层在需要发送内容时据此回答 `406 Not Acceptable`（见 [ruvia-web](ruvia-web.md)） |
| `coding()` | 所选编码；`!acceptable()` 时仅以 `identity` 作占位 |
| `identity_accepted()` | `accepts(http_content_coding::identity)` |
| `accepts(coding)` | 该内置编码对客户端是否可接受，与服务端偏好无关；用于处理器或表示存储已自行编码的响应。字段缺失时全部可接受 |
| `accepts(token, request)` | 自定义（扩展）编码：已知 token 转到上一重载；未知 token 借用 `request` 求值（仅在调用期间借用，快照不为扩展 token 分配存储） |
| `http_response_coding_selection_for(request)` | 把请求的全部 `Accept-Encoding` 行（RFC 9110 §5.3 合并重复列表字段）折叠为一个质量快照，并在全部可产生编码中选择。这是 HTTP/1、HTTP/2 与子请求共用的唯一 `Accept-Encoding` 偏好来源 |

#### 9.7.5 响应 `Content-Encoding` 可接受性

```cpp
enum class http_response_content_coding_status : std::uint8_t {
    absent,
    accepted,
    not_acceptable,
    invalid,
};

[[nodiscard]] http_response_content_coding_status evaluate_http_response_content_coding(const http_response_coding_selection& selection, const http_response& response, const http_request* request = nullptr);
```

| 成员 | 含义 |
| --- | --- |
| `absent` | 响应没有 `Content-Encoding` |
| `accepted` | 编码栈中的每个编码都可被客户端接受 |
| `not_acceptable` | 至少一个编码不被客户端接受 |
| `invalid` | 某个 `Content-Encoding` 行语法无效 |

`evaluate_http_response_content_coding(selection, response, request)` 一次性解析响应的全部 `Content-Encoding` 行：内置编码以 `selection.accepts()` 判定；扩展编码仅在 `request` 非空且请求含 `Accept-Encoding` 时，在一次 `Accept-Encoding` 遍历中统一求值（`compress` / `x-compress` 视为同一编码），否则视为可接受。语法无效优先报告为 `invalid`。不超过 8 种不同扩展编码时不分配；更多时使用临时存储，分配失败抛出 `std::bad_alloc`。

### 9.8 `"http_structured_field.h"`
<a id="api-http-structured-field-h"></a>

RFC 9651 Structured Field Values 的项目（item）、参数与内部列表扫描器：调用方组合输入游标与解析器，自行遍历顶层 List / Dictionary。连续 token / string 借用输入，跨字段行的 string 才拥有文本。公开 include [`"binary_text_encoding.h"`](#api-binary-text-encoding-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_header.h"`](#api-http-header-h)。源码：[`ruvia-http/include/http_structured_field.h`](../ruvia-http/include/http_structured_field.h)

#### 9.8.1 输入游标

```cpp
namespace ruvia {
struct http_structured_text_input {
    std::string_view remaining_;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] char peek() const noexcept;
    [[nodiscard]] const char* data() const noexcept;
    [[nodiscard]] std::size_t contiguous_size() const noexcept;
    void advance() noexcept;
};

template <typename iterator_type>
class http_structured_header_input final {
public:
    http_structured_header_input(iterator_type first, iterator_type last, std::string_view field_name) noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] char peek() const noexcept;
    [[nodiscard]] const char* data() const noexcept;
    [[nodiscard]] std::size_t contiguous_size() const noexcept;
    void advance() noexcept;
};
}
```

两种游标提供同一组操作，也是 `http_structured_parser<input_type>` 对 `input_type` 的要求（无正式 concept）：

| 操作 | 契约 |
| --- | --- |
| `empty()` | 输入已耗尽。 |
| `peek()` / `advance()` | 读取 / 跳过下一字节；前置条件 `!empty()`。 |
| `data()` / `contiguous_size()` | 当前连续片段的起点与剩余长度；单个视图不跨越片段。 |
| `http_structured_text_input` | 单段输入，聚合类型，片段即 `remaining_`；借用其指向的文本。 |
| `http_structured_header_input(first, last, field_name)` | 按序选出 `[first, last)` 中名称与 `field_name` ASCII 大小写不敏感相等的字段行（`*iterator` 须提供 `name()` / `value()`，如 [`http_header_view`](#api-http-header-h)），在相邻选中行之间注入 `", "`，流式折叠而不预先拼接；空字段行不被跳过。每个片段是一行剩余部分或注入的分隔符。借用迭代器范围、字段文本与 `field_name`，它们须在游标使用期间存活且不变。 |

#### 9.8.2 `struct http_structured_item` 与 `struct http_structured_parser`

```cpp
namespace ruvia {
struct http_structured_item {
    enum class kind_type { other, integer, boolean, token, string, inner_list } kind_{kind_type::other};
    std::int64_t integer_{0};
    bool boolean_{false};
    std::variant<std::string_view, std::string> text_{std::string_view{}};
    bool has_escapes_{false};

    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view text() const&& = delete;
};

template <typename input_type>
struct http_structured_parser {
    input_type input_;

    [[nodiscard]] bool empty() const noexcept;
    bool take(char ch);
    void spaces();
    void ows();
    std::string_view key();
    bool item(http_structured_item& value, bool retain_text = true);
    bool parameters();
    bool member(http_structured_item& value, bool retain_text = true);
};
}
```

| `http_structured_item::kind_type` | 产生条件与字段 |
| --- | --- |
| `other` | 默认值；也用于校验通过但不再细分的 Byte Sequence（`:base64:`，允许省略填充）与 Decimal（整数部分至多 12 位、小数 1–3 位），此时 `text()` 为空。 |
| `integer` | 至多 15 位十进制数字，可带 `-`；值在 `integer_`。 |
| `boolean` | `?0` / `?1`；值在 `boolean_`。 |
| `token` | sf-token；`text()` 借用输入。 |
| `string` | sf-string；`text()` 为引号内原始拼写（转义未还原），含 `\\` 或 `\"` 时 `has_escapes_` 为 `true`。 |
| `inner_list` | 仅由 `member()` 产生；内部项目只校验，不对外暴露，`text()` 为空。 |

`text() const&` 借用 `text_`：借用变体指向输入，拥有变体指向本对象；右值重载删除。RFC 9651 的 Date（`@`）与 Display String（`%"`）不受支持，`item()` 对其返回 `false`。

| 成员 | 契约 |
| --- | --- |
| `input_` | 聚合初始化的输入游标（[9.8.1](#api-http-structured-field-h)）。 |
| `empty()` | 输入已耗尽。 |
| `take(ch)` | 下一字节为 `ch` 时消费并返回 `true`。 |
| `spaces()` / `ows()` | 跳过 SP / 跳过 SP 与 HTAB。 |
| `key()` | 下一字节为小写字母或 `*` 时消费整个 key（小写字母、数字、`_`、`-`、`.`、`*`）并返回借用输入的视图；否则返回空视图且不消费。注入的 `", "` 分隔符保证 key 不跨字段行。 |
| `item(value, retain_text = true)` | 解析一个 bare item 写入 `value`（输入为空时直接返回 `false`，否则先重置 `value`）。`retain_text == false` 只校验与分类，不保留文本、不分配。 |
| `parameters()` | 解析零个或多个 `;` 参数（`;` 后可有 SP，key 后可接 `=` bare item）；参数值只校验不保留，参数不对外暴露。 |
| `member(value, retain_text = true)` | List / Dictionary 成员：`(` 开头时解析以 SP 分隔、各自带参数的内部列表及其参数，`value.kind_` 为 `inner_list`；否则为 `item()` 后接 `parameters()`。 |

- **按需拥有**：quoted-string 跨字段行时才把折叠文本复制进 `std::string`，避免把不连续地址伪装为单个视图；只有此时可能分配。
- **错误**：语法拒绝返回 `false`，此后输入位置与 `value` 内容未指定；分配失败（`std::bad_alloc`）正常传播，不通过 `noexcept` 终止进程。

## 10. HTTP/1.x

本章覆盖 HTTP/1.0 与 HTTP/1.1 的 sans-I/O 状态机：解析、分帧、正文解码与首部写出。全部类型不做网络 I/O，不持有套接字；驱动（例如 [ruvia-web](ruvia-web.md) 的 HTTP/1 会话）负责读写字节、超时与连接池。

服务端处理流程：

1. [`http1_server_request_parser::parse()`](#api-http1-server-request-h) 增量解析请求头，得到 `http_request`、请求体分帧计划 `http1_request_body_plan` 与连接计划 `http1_request_connection_plan`。
2. 以 `body_plan().framing()` 构造 [`http1_body_decoder`](#api-http1-message-body-h)，逐步去除分帧与传输编码。
3. 响应：缓冲响应经 `http1_commit_final_response()` + `plan_http1_buffered_response_head()`，流式响应经 `plan_http1_response_stream()` + `prepare_http1_response_stream_head()` 得到 `http1_response_head_plan`，再由 `append_http1_response_head()` 写入 `http_response_head_buffer`（见 [`"http1_server_response.h"`](#api-http1-server-response-h)）。

客户端处理流程：

1. [`prepare_http1_client_request()`](#api-http1-client-request-h) 把 `http_request_head_view` 写成请求头并给出内容计划；流式内容由 `http1_request_content_writer` 规划 chunk 分段。
2. `take_exchange_state()` 取出的交换状态构造 [`http1_client_response_parser`](#api-http1-client-response-h)，增量解析 `1xx` 与最终响应头，得到 `http1_client_response_plan`。
3. 以 `plan().framing()` 构造 `http1_body_decoder`（`obs_fold_ = accept`、`trailer_message_ = response`）解码响应正文。

所有分帧决定共用 [`"http1_connection_plan.h"`](#api-http1-connection-plan-h) 的 `http1_message_framing`。单步结果类型、终态重放与借用规则的跨头总述见[通用契约](#2-通用契约)与 [`"decode_step.h"`](#api-decode-step-h)。

### 10.1 `"http1_connection_plan.h"`
<a id="api-http1-connection-plan-h"></a>

HTTP/1 分帧值与请求侧连接复用计划：请求解析、客户端响应解析、服务端响应头计划与正文解码器共用的唯一分帧决定。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_content_coding.h"`](#api-http-content-coding-h)、[`"http_message.h"`](#api-http-message-h)。源码：[`ruvia-http/include/http1_connection_plan.h`](../ruvia-http/include/http1_connection_plan.h)

#### 10.1.1 枚举

```cpp
namespace ruvia {

enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};

enum class http1_framing_kind : std::uint8_t {
    no_content,
    known_length,
    chunked,
    close_delimited,
};

}  // namespace ruvia
```

`http1_request_body_consumption`：运行时在尝试复用传输前是否已读完整个请求消息。这是协议生命周期事实，不是产品策略开关：RFC 9112 §9.3 禁止在请求内容未读完时复用连接。

| 成员 | 值 | 含义 |
|---|---|---|
| `complete` | 0 | 请求内容已完整消费（或本来没有内容），连接计划不变。 |
| `incomplete` | 1 | 仍有未读的请求内容；`apply_request_body_consumption()` 据此要求关闭。 |

`http1_close_policy`：一次 HTTP/1 交换完成后连接是否复用。客户端请求写出器与服务端响应规划器用同一值表达同一线上决定。

| 成员 | 值 | 含义 |
|---|---|---|
| `allow_reuse` | 0 | 连接保持可复用（可回到连接池或继续读下一请求）。 |
| `close_after_response` | 1 | 写出 `Connection: close` 并在本次响应后关闭。 |

`http1_framing_kind`：HTTP/1 消息如何界定内容（RFC 9112 §6.3）。

| 成员 | 值 | 含义 |
|---|---|---|
| `no_content` | 0 | 消息在首部结束，之后没有属于本消息的字节。 |
| `known_length` | 1 | `Content-Length` 界定内容，长度可以为 0。 |
| `chunked` | 2 | 最后一个传输编码为 `chunked`，零长度 chunk 与 Trailer 区段结束消息。 |
| `close_delimited` | 3 | 连接关闭结束内容。 |

#### 10.1.2 `class http1_message_framing`

```cpp
class http1_message_framing final {
public:
    [[nodiscard]] static constexpr http1_message_framing no_content() noexcept;
    [[nodiscard]] static constexpr http1_message_framing known_length(std::uint64_t content_length) noexcept;
    [[nodiscard]] static constexpr http1_message_framing chunked(http_content_codings transfer_codings = {}) noexcept;
    [[nodiscard]] static constexpr http1_message_framing close_delimited(http_content_codings transfer_codings = {}) noexcept;

    [[nodiscard]] constexpr http1_framing_kind kind() const noexcept;
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept;
    [[nodiscard]] constexpr const http_content_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_content_codings& transfer_codings() const&& = delete;

    [[nodiscard]] constexpr bool requires_consumption() const noexcept;
};
```

每种分帧只携带自己的载荷：`known_length` 携带长度，`chunked` 与 `close_delimited` 携带分帧之前施加的传输编码。没有公开构造函数与默认构造，只能经四个静态工厂取得；值可复制。

| 成员 | 说明 |
|---|---|
| `no_content()` | 无内容分帧。 |
| `known_length(content_length)` | 定长分帧；`content_length` 可为 0（仍属有分帧的空内容，区别于 `no_content`）。 |
| `chunked(transfer_codings = {})` | 分块分帧；`transfer_codings` 按施加顺序列出位于最终 `chunked` 之前的传输编码（类型见 [`"http_content_coding.h"`](#api-http-content-coding-h)）。 |
| `close_delimited(transfer_codings = {})` | 关闭定界；`transfer_codings` 为最终传输编码不是 `chunked` 时的完整编码列表。 |
| `kind()` | 分帧种类。 |
| `content_length()` | 仅 `known_length` 时为声明长度，其余种类为 0。 |
| `transfer_codings()` | 仅 `chunked` / `close_delimited` 时可能非空。解码器先去除分帧，再按逆序去除这些编码。返回引用借用本对象（`RUVIA_LIFETIMEBOUND`，见 [`"borrowed_text.h"`](#api-borrowed-text-h)）；右值重载被删除，禁止从临时分帧值取出悬垂引用。 |
| `requires_consumption()` | 首部之后是否还有属于本消息的字节：`known_length` 且长度非 0、`chunked`（即使解码内容为空，也必须消费终止零 chunk）、`close_delimited` 为 `true`；`no_content` 与 `known_length(0)` 为 `false`。 |

#### 10.1.3 `class http1_request_connection_plan` 与自由函数

```cpp
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept;

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] constexpr http1_close_policy close_policy() const noexcept;
    [[nodiscard]] constexpr http1_request_connection_plan require_close() const noexcept;

    [[nodiscard]] constexpr bool tunnel_request() const noexcept;
    [[nodiscard]] constexpr http1_request_connection_plan with_tunnel_request() const noexcept;
};

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http1_request_connection(http_protocol_version version, bool close, bool keep_alive) noexcept;

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept;
```

不可变的请求侧 HTTP/1 持久连接契约。解析确定版本与初始复用策略；之后的请求体消费策略只能把它收紧为关闭，不能放宽。

| 成员 | 说明 |
|---|---|
| `http11_close()` | HTTP/1.1、`close_after_response`、非隧道请求。解析器的默认值与失败值基础。 |
| `protocol_version()` | 请求版本（`http_protocol_version`，见 [`"http_message.h"`](#api-http-message-h)），用于响应头序列化保留原版本。 |
| `close_policy()` | 当前复用策略。 |
| `require_close()` | 返回同版本、同隧道标记、`close_after_response` 的新计划。 |
| `tunnel_request()` | 是否为 CONNECT 请求。`2xx` 最终响应把连接变为隧道；其他任何最终响应必须关闭连接，因为客户端可能已乐观地发送隧道字节（RFC 9110 §9.3.6），这些字节绝不能被解析为后续请求。 |
| `with_tunnel_request()` | 返回带隧道标记的新计划，版本与复用策略不变。 |

自由函数：

| 函数 | 说明 |
|---|---|
| `plan_http1_request_connection(version, close, keep_alive)` | 按 RFC 9112 §9.3 计算请求版本与 `Connection` 选项要求的持久性：HTTP/1.1 除非出现 `close` 否则持久；HTTP/1.0 仅在有 `keep_alive` 且无 `close` 时持久。`version` 必须为 `http10` 或 `http11`。结果不带隧道标记，是唯一能以 `allow_reuse` 构造计划的入口。 |
| `apply_request_body_consumption(plan, consumption)` | `complete` 时原样返回，`incomplete` 时返回 `plan.require_close()`。 |

### 10.2 `"http1_server_request.h"`
<a id="api-http1-server-request-h"></a>

服务端 HTTP/1 请求头解析器、请求体分帧计划与明文非 HTTP 输入判定。公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"http1_connection_plan.h"`](#api-http1-connection-plan-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"http_request.h"`](#api-http-request-h)、[`"http_request_content_plan.h"`](#api-http-request-content-plan-h)、[`"http_request_expectation.h"`](#api-http-request-expectation-h)。源码：[`ruvia-http/include/http1_server_request.h`](../ruvia-http/include/http1_server_request.h)

#### 10.2.1 `class http1_request_body_plan`

```cpp
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_message_framing& framing() const& noexcept RUVIA_LIFETIMEBOUND;
    const http1_message_framing& framing() const&& = delete;

    [[nodiscard]] constexpr http1_request_body_consumption consumption_if_unread() const noexcept;

    [[nodiscard]] http_request_expectations expectations() const noexcept;

    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy unsupported_policy) const noexcept;
};
```

只由 HTTP/1 解析器产生、由运行时驱动消费的不可变分帧契约；调用方无法合成绕过线上校验的计划（无公开构造函数）。请求永远不会是 `close_delimited`（RFC 9112 §6.3）：分帧为 `no_content`、`known_length` 或 `chunked`。

| 成员 | 说明 |
|---|---|
| `framing()` | 请求体分帧（[`http1_message_framing`](#api-http1-connection-plan-h)），用于构造 `http1_body_decoder`。借用本计划；右值重载被删除。 |
| `consumption_if_unread()` | 运行时从未读取请求体时，响应须记录的消费状态：仅当 `framing().requires_consumption()` 时为 `incomplete`，否则 `complete`。 |
| `expectations()` | 请求的 `Expect` 解析结果（[`"http_request_expectation.h"`](#api-http-request-expectation-h)）。HTTP/1.0 请求的 `100-continue` 已被忽略。 |
| `expectation_plan(unsupported_policy)` | 以 `requires_consumption()` 决定内容指示（`will_follow` / `no_content`），调用 `http_request_expectations::server_plan()` 得到服务端 Expect 处置计划。 |

#### 10.2.2 明文非 HTTP 输入判定

```cpp
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(std::string_view buffer, http_parse_error error) noexcept;
```

明文监听器可静默丢弃请求行失败（`invalid_request_line` 或 `unsupported_http_version`）表明对端根本不在说 HTTP 的输入（例如 HTTP 端口上的 TLS 记录、SSH banner）。规则：

- `error` 不是上述两种之一：返回 `false`（照常应答）。
- 跳过被忽略的前导空行后，若首行（尚无 CRLF 时为当前全部可用字节）含有一个以空白分隔、形如 HTTP 版本的词（任意大小写的 `"HTTP/"` 前缀，或 token `"/"` DIGITs `"."` DIGITs），则判定为 HTTP，返回 `false`，应由 `make_http_protocol_error(error)` 应答（RFC 9112 §2.2、§3；错误到状态的映射见 [`"http_protocol.h"`](#api-http-protocol-h)）。
- 否则：首行已以 CRLF 结束，或首字节不能开始方法 token 时返回 `true`（丢弃）。
- 其他解析错误总是应答。

#### 10.2.3 `class http1_server_request_parser`

```cpp
using http1_server_request_parse_failure = decode_failure<http_parse_error>;

using http1_server_request_parse_result = decode_step<decode_need_input, decode_complete, http1_server_request_parse_failure>;

class http1_server_request_parser final {
public:
    http1_server_request_parser() noexcept;

    [[nodiscard]] http1_server_request_parse_result parse(std::string_view buffer);

    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ buffer_type>
    http1_server_request_parse_result parse(buffer_type&&) = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    void reset() noexcept;

    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_request& request() const&& = delete;
    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    const http1_request_body_plan& body_plan() const&& = delete;
    [[nodiscard]] const http1_request_connection_plan& connection_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    const http1_request_connection_plan& connection_plan() const&& = delete;

    [[nodiscard]] http_request take_request() && noexcept;
    [[nodiscard]] http1_request_body_plan take_body_plan() && noexcept;
    [[nodiscard]] http1_request_connection_plan take_connection_plan() && noexcept;
};
```

一次解析一个请求的 sans-I/O 请求头解析器。解析器拥有它派生的请求、请求体计划与连接计划；请求的描述符存储在同一请求头的多次 `parse()` 之间保留，由 `reset()` 释放。

结果类型：`parse()` 的单步结果是 [`decode_step`](#api-decode-step-h) 三态之一。

| 结果 | `consumed_bytes()` | 含义 |
|---|---|---|
| `decode_need_input` | 0 | 请求头尚不完整。 |
| `decode_complete` | 请求头精确字节数（含被忽略的前导空行，直到 CRLF CRLF） | 请求头已完整解析，之后的字节属于请求体或下一请求。 |
| `http1_server_request_parse_failure` | 0 | 请求头被拒绝；`error()` 为 `http_parse_error`（枚举与状态映射见 [`"http_protocol.h"`](#api-http-protocol-h)）。 |

成员语义：

| 成员 | 说明 |
|---|---|
| 默认构造 | `noexcept`；状态 `active`，请求为空，计划为默认值（`no_content` 分帧、`http11_close()`）。 |
| `parse(buffer)` | 解析 `buffer` 开头的请求头。`need_input` 之后以“同一字节 + 新输入”再次调用：已分类的字节不会重扫（解析器内部保存扫描进度；更短的 buffer 丢弃不再覆盖的进度）。完成或拒绝即结束本次解析；非 `active` 状态下调用抛 `std::logic_error`，直到 `reset()`。分配失败以 `std::bad_alloc` 传出且不发布完成状态；释放资源后可用同一 buffer 重试，但部分元数据可能残留，不保证回滚。 |
| `parse(buffer_type&&) = delete` | 拒绝右值临时 `std::basic_string<char, ...>`：完成后的 `request()` 零拷贝借用 `buffer`，临时字符串会立刻悬垂。 |
| `state()` | `active`：等待更多输入；`finished`：请求头已完成；`failed`：已拒绝。 |
| `reset()` | 开始下一请求：释放请求的描述符存储，恢复默认计划与扫描进度，状态回到 `active`。 |
| `request()` / `body_plan()` / `connection_plan()` | 只读借用解析结果；右值重载被删除。失败后保留已接受的请求版本与要求关闭的连接计划（错误响应不会被悄悄升级为 HTTP/1.1），请求本身被清空；未完成时请求为空。 |
| `take_request() &&` / `take_body_plan() &&` / `take_connection_plan() &&` | 以 `std::move(parser).take_*()` 移出一个结果并在原处留下默认值；此后解析器只可 `reset()` 或析构。取走请求即释放解析器中的描述符存储。 |

借用：完成后 `request()`（以及 `take_request()` 得到的 `http_request`）的方法、目标、路径、查询与首部视图零拷贝借用传给 `parse()` 的 `buffer`；调用方须保持这些字节存活且不变，直到不再使用请求。描述符数组按实际字段数增长，不按首部字节上限预留。

线上规则（RFC 9112）：

- 前导空行：请求行之前的 CRLF 或裸 LF 空行被忽略（§2.2），计入 `max_http_header_bytes`。
- 提前拒绝：请求行在其换行到达时即被分类，首字节不能开始方法 token 时立即报告 `invalid_request_line`；请求行之后任一以裸 LF 结尾的字段行立即报告 `invalid_header`（§2.2），不等待空行、64 KiB 上限或读超时。
- 首部上限：字节上限 `max_http_header_bytes`（64 KiB）与行数上限 `max_http_header_fields`（256）同时生效（见 [`"http_field_section_budget.h"`](#api-http-field-section-budget-h)）。第 257 个字段行或超出字节上限报告 `header_too_large`（映射为 `431`）；到达上限时仍处于请求目标中则报告 `uri_too_long`（`414`，§3）。
- 版本：`HTTP/1.0` 为 `http10`；`HTTP/1.x`（x 为 1–9）按 HTTP/1.1 处理（RFC 9110 §6.2）；主版本不为 1 报告 `unsupported_http_version`；格式不符报告 `invalid_request_line`。
- `Host`：HTTP/1.1 缺少 `Host` 报告 `missing_host`；absolute-form 与 authority-form 请求中，`Host` 字段值绑定为请求目标的 authority（§3.2.2、§3.3），应用不会观察到第二个路由事实。
- obs-fold：请求首部中的折行（以 SP/HTAB 开头的续行）报告 `invalid_header`。
- 分帧：`Transfer-Encoding` 与 `Content-Length` 同时出现、最终编码不是 `chunked`、或 HTTP/1.0 请求携带 `Transfer-Encoding`（§6.1，视为分帧故障）均报告 `invalid_transfer_encoding`；不支持的传输编码报告 `unsupported_transfer_encoding`；`Content-Length` 超出 `std::size_t` 报告 `invalid_content_length`。CONNECT 请求携带任何 `Transfer-Encoding` 报告 `invalid_transfer_encoding`，非零 `Content-Length` 报告 `invalid_content_length`（RFC 9110 §9.3.6）；TRACE 正文分帧明确，照常分帧与消费。
- 连接：`plan_http1_request_connection()` 按版本与 `Connection` 的 `close` / `keep-alive` 计算复用；CONNECT 附加隧道标记。请求中的 `TE` 被忽略；HTTP/1.0 请求的 `Upgrade` 只做通用字段值校验（RFC 9110 §7.8 要求忽略）。
- 失败：任何拒绝都使连接计划要求关闭（消息边界未知，RFC 9112 §6.1、§6.3）。

示例（服务端读取请求头与请求体；I/O 省略）：

```cpp
ruvia::http1_server_request_parser parser;
std::string input;  // 连接读缓冲；request() 借用其中的请求头字节
auto step = parser.parse(input);
if (step.holds<ruvia::decode_need_input>()) {
    // 追加读取后以同一 input 再次 parse()
} else if (const auto* failure = step.get_if<ruvia::http1_server_request_parse_failure>()) {
    if (!ruvia::should_drop_invalid_cleartext_http1_input(input, failure->error())) {
        const auto error = ruvia::make_http_protocol_error(failure->error());
        // 以 error.status() 应答，随后关闭连接
    }
} else {
    std::string_view rest = std::string_view(input).substr(step.consumed_bytes());
    ruvia::http1_body_decoder body(parser.body_plan().framing(),
                                   {.body_limit_ = ruvia::protocol_byte_limit::limited(1 << 20)});
    std::array<char, 16 * 1024> scratch;
    for (;;) {
        auto r = body.decode(rest, scratch);  // 具名左值 decoder；结果不拥有正文 / Trailer
        rest.remove_prefix(r.consumed_bytes());
        if (const auto* out = r.get_if<ruvia::decode_output>()) {
            // 消费 out->bytes()
        } else if (r.holds<ruvia::decode_complete>() || r.holds<ruvia::decode_need_input>()) {
            break;  // need_input：保留 rest 并读取更多
        } else if (const auto* f = r.get_if<ruvia::http1_body_decode_failure>()) {
            throw ruvia::make_http_protocol_error(f->error());
        } else if (const auto* trailers = r.get_if<ruvia::http1_body_trailers>()) {
            // 在下一次 decode / finish_input / reset 前消费 trailers->fields()；
            // 若需跨越该边界保存，复制字段到调用方拥有的存储。
        }
    }
}
```

### 10.3 `"http1_message_body.h"`
<a id="api-http1-message-body-h"></a>

服务端与客户端共用的唯一 HTTP/1 消息正文解码器：先去除分帧，再按逆序去除分帧携带的传输编码。公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"http1_chunked_body.h"`](#api-http1-chunked-body-h)、[`"http1_connection_plan.h"`](#api-http1-connection-plan-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"http_transfer_coding.h"`](#api-http-transfer-coding-h)。源码：[`ruvia-http/include/http1_message_body.h`](../ruvia-http/include/http1_message_body.h)

#### 10.3.1 错误与映射

```cpp
enum class http1_body_decode_error : std::uint8_t {
    invalid_framing,
    framing_limit_exceeded,
    body_limit_exceeded,
    invalid_transfer_coding,
    incomplete_body,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(http1_body_decode_error error) noexcept;

[[nodiscard]] std::string_view http1_response_body_error_message(http1_body_decode_error error) noexcept;
```

| 成员 | 值 | 含义 | 请求侧映射（`make_http_protocol_error`） | 响应侧诊断（`http1_response_body_error_message`） |
|---|---|---|---|---|
| `invalid_framing` | 0 | chunk 分帧或 Trailer 区段格式错误。 | `400`，code `invalid_chunked_body` | `invalid HTTP/1 response body framing` |
| `framing_limit_exceeded` | 1 | chunk-size 行、chunk-ext 预算或 Trailer 区段超出 `max_http_header_bytes`。 | `413`，code `chunked_framing_too_large` | `invalid HTTP/1 response body framing` |
| `body_limit_exceeded` | 2 | chunk 载荷或传输解码后内容超出 `http1_body_decoder_options::body_limit_`。 | `413`，code `request_body_too_large` | `HTTP/1 response body is too large` |
| `invalid_transfer_coding` | 3 | 编码字节损坏、编码结束后仍有字节、或完整的 chunked 分帧留下未完成的编码。 | `400`，code `invalid_transfer_coding_body` | `invalid HTTP/1 response transfer coding` |
| `incomplete_body` | 4 | 输入在分帧结束前结束（EOF）。 | `400`，code `incomplete_request_body` | `incomplete HTTP/1 response body` |

`make_http_protocol_error(http1_body_decode_error)` 给出服务端对请求体解码失败的应答状态，`http1_response_body_error_message()` 给出客户端报告响应体失败的诊断文本（静态字符串）。`make_http_protocol_error` 是按各错误族枚举重载的单一名称：本头定义 `http1_body_decode_error` 重载，`http1_chunk_decode_error` 重载见 [`"http1_chunked_body.h"`](#api-http1-chunked-body-h)，`http_parse_error` 重载见 [`"http_protocol.h"`](#api-http-protocol-h)，`http_request_body_failure` 与 `(http_content_decode_error, http_coding_field)` 重载见 [`"http_request_body_failure.h"`](#api-http-request-body-failure-h)，multipart 与 WebSocket 重载分别见 [`"http_multipart.h"`](#api-http-multipart-h) 与 [`"websocket_handshake.h"`](#api-websocket-handshake-h)。

#### 10.3.2 `class http1_body_trailers` 与结果类型

```cpp
class http1_body_trailers final {
public:
    [[nodiscard]] constexpr std::span<const http_header> fields() const noexcept;
};

using http1_body_decode_failure = decode_failure<http1_body_decode_error>;

using http1_body_decode_result = decode_step<decode_need_input, decode_output, http1_body_trailers, decode_complete, http1_body_decode_failure>;
```

`http1_body_trailers` 是结束 chunked 正文的 Trailer 区段（可能为空），只由解码器构造。`fields()` 中的字段已校验，归解码器所有，仅在区段保留了字段时才分配：字段名小写，接受的 obs-fold 已替换为 SP（RFC 9112 §5.2），RFC 9110 §6.5.1 禁止出现在该消息 Trailer 中的字段已被丢弃（RFC 9112 §7.1.2）。字段有效至下一次 `decode()`、`finish_input()` 或 `reset()` 调用，或解码器析构。

单步结果：

| 结果 | 含义 |
|---|---|
| `decode_need_input` | 需要更多输入（`finish_input()` 之后不再出现缺字节的 need_input，而是 `incomplete_body`）。 |
| `decode_output` | 内容字节；无传输编码时借用 `available`，否则借用 `scratch`。 |
| `http1_body_trailers` | chunked 正文的 Trailer 区段；在结束之前报告一次。 |
| `decode_complete` | 正文结束。 |
| `http1_body_decode_failure` | 失败，`error()` 为 `http1_body_decode_error`。 |

#### 10.3.3 `struct http1_body_decoder_options` 与 `class http1_body_decoder`

```cpp
struct http1_body_decoder_options final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
    http1_trailer_message trailer_message_{http1_trailer_message::request};
};

class http1_body_decoder final {
public:
    explicit http1_body_decoder(const http1_message_framing& framing, http1_body_decoder_options config = {});
    ~http1_body_decoder() = default;
    http1_body_decoder(const http1_body_decoder&) = delete;
    http1_body_decoder& operator=(const http1_body_decoder&) = delete;
    http1_body_decoder(http1_body_decoder&&) = delete;
    http1_body_decoder& operator=(http1_body_decoder&&) = delete;

    [[nodiscard]] http1_body_decode_result decode(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes = (std::numeric_limits<std::size_t>::max)()) & RUVIA_LIFETIMEBOUND;
    http1_body_decode_result decode(std::string_view, std::span<char>, std::size_t = 0) && = delete;
    [[nodiscard]] http1_body_decode_result finish_input(std::string_view available, std::span<char> scratch, std::size_t max_output_bytes = (std::numeric_limits<std::size_t>::max)()) & RUVIA_LIFETIMEBOUND;
    http1_body_decode_result finish_input(std::string_view, std::span<char>, std::size_t = 0) && = delete;
    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ input_type>
    http1_body_decode_result decode(input_type&&, std::span<char>, std::size_t = 0) & = delete;
    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ input_type>
    http1_body_decode_result finish_input(input_type&&, std::span<char>, std::size_t = 0) & = delete;

    void discard_content() noexcept;

    [[nodiscard]] decoder_state state() const noexcept;
    void reset();
};
```

`http1_body_decoder_options` 给出接收 HTTP/1 正文的两个角色之间的差异：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `body_limit_` | `protocol_byte_limit::unlimited()` | 限制 chunked 正文的 chunk 载荷字节，并独立限制每一层传输解码的输出（类型见 [`"http_protocol.h"`](#api-http-protocol-h)）。定长内容不经此限制：调用方须在构造解码器之前自行限制已知长度。 |
| `obs_fold_` | `http1_trailer_obs_fold::reject` | Trailer 区段 obs-fold 处置（见 [`"http1_chunked_body.h"`](#api-http1-chunked-body-h)）。 |
| `trailer_message_` | `http1_trailer_message::request` | Trailer 所属消息，决定丢弃哪些禁止字段。 |

服务端以 `http1_request_body_plan::framing()` 构造；客户端以 `http1_client_response_plan::framing()` 构造并设置 `obs_fold_ = accept`、`trailer_message_ = response`。定长内容直接转发，不经传输解码阶段。

| 成员 | 说明 |
|---|---|
| 构造函数 | 绑定分帧与配置。分帧中的编码不是传输编码时抛 `std::invalid_argument`；编解码器无法启动时抛 `http_content_codec_error`（见 [`"http_content_codec.h"`](#api-http-content-codec-h)）。不可复制、不可移动。 |
| `decode(available, scratch, max_output_bytes)` | 推进一步。输出在分帧不含传输编码时借用 `available`，否则借用 `scratch`；须在改变任一存储或进行下一次调用前消费输出。仅在活动状态解码传输编码时要求 `scratch` 非空，否则抛 `std::invalid_argument`；重放终态不需要暂存区。`max_output_bytes` 必须为正（为 0 抛 `std::invalid_argument`），每步最多发布这么多字节。调用方从 `available` 中精确删除 `consumed_bytes()`。`finish_input()` 之后的 `decode()` 沿用同一 EOF 状态：可以排出已缓冲的输入/输出，但缺失的正文字节报告 `incomplete_body` 而不是请求更多输入。 |
| `finish_input(...)` | 声明 EOF（单调，不可撤销）并提供剩余缓冲字节；之后的调用可以排出解码器输出，但不会再请求网络输入。`close_delimited` 无编码时 EOF 即正常结束；`known_length` 与 `chunked` 在分帧结束前遇到 EOF 报告 `incomplete_body`。 |
| 删除的 `decode` / `finish_input` 重载 | `&& = delete` 拒绝临时或 `std::move` 后的解码器，避免结果中的 Trailer 视图借用即将销毁的 owner；`&` 上的输入删除重载拒绝右值临时 `std::basic_string`，避免正文输出借用临时输入。两类限制互不替代。 |
| `discard_content()` | 自此起，分帧界定的内容被消费但不做传输解码、也不发布：单步只报告 need-input、Trailer、结束或分帧失败。用于接收方不得使用（`205` 响应，RFC 9110 §15.3.6）或不再需要（未读请求体）而连接仍可复用的内容。 |
| `state()` | `active` / `finished` / `failed`。 |
| `reset()` | 以构造时的分帧与配置开始另一正文。重建已丢弃的传输解码器时若分配失败，原正文的状态和进度保持不变。已有编解码器无法重启时抛 `http_content_codec_error`，此后解码器不可用。 |

`decode()` / `finish_input()` 的左值入口均带 `RUVIA_LIFETIMEBOUND`，结果不因此拥有正文或 Trailer。正文仍零拷贝借用 `available` / `scratch`；Trailer 借用解码器内部字段存储，必须在下一次 `decode()`、`finish_input()`、`reset()` 或解码器析构前消费。显式传入 `std::string_view` 不能延长输入 owner 寿命；必须同时满足输入 / scratch 与解码器各自的借用区间。

Trailer 分配失败时，用同一输入重试，不推进调用方的读取游标。解码器记住本步已处理但尚未报告的前缀，重试成功后将其计入 `consumed_bytes()`，不重复解码或扣减配额。

下一次有效解码调用在继续排出传输解码输出前回收已发布的 Trailer 存储，沿用[分块解码器](#api-http1-chunked-body-h)的缓冲保留上限。

终态：已完成或已失败的解码器不再消费输入，`decode()` 与 `finish_input()` 以 0 消费字节重放终态结果，绝不触碰后续消息的字节，直到 `reset()`。

传输编解码器异常原样抛出，正文解码器进入 `failed`；即使随后调用 `discard_content()`，仍重放 `invalid_transfer_coding`，直到 `reset()`。

### 10.4 `"http1_chunked_body.h"`
<a id="api-http1-chunked-body-h"></a>

HTTP/1 chunked 分帧的线格式常量、chunk-size 行格式化与增量 chunk 分帧解码器；`http1_body_decoder` 在 chunked 分帧下组合使用它。公开 include [`"binary_text_encoding.h"`](#api-binary-text-encoding-h)、[`"decode_step.h"`](#api-decode-step-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"http_request_body_failure.h"`](#api-http-request-body-failure-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http1_chunked_body.h`](../ruvia-http/include/http1_chunked_body.h)

#### 10.4.1 线格式常量与 `class http1_chunk_header`

```cpp
inline constexpr std::string_view http1_chunk_data_terminator = "\r\n";
inline constexpr std::string_view http1_last_chunk_prefix = "0\r\n";
inline constexpr std::string_view http1_trailer_section_terminator = "\r\n";

class http1_chunk_header final {
public:
    explicit http1_chunk_header(std::size_t chunk_size) noexcept;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view view() const&& = delete;
};
```

| 符号 | 说明 |
|---|---|
| `http1_chunk_data_terminator` | 每个 chunk 数据之后的 CRLF。 |
| `http1_last_chunk_prefix` | 最后一个 chunk（零长度）行 `0\r\n`，其后跟 Trailer 字段行。 |
| `http1_trailer_section_terminator` | 结束 Trailer 区段的空行 CRLF。 |
| `http1_chunk_header(chunk_size)` | 格式化一个 chunk 的 chunk-size 行（小写十六进制、无前导零、无 chunk-ext，含结尾 CRLF），存放在对象内联存储中；不分配。分帧决定归调用方，本值只格式化线上前缀。 |
| `view()` | 返回借用对象内联存储的行文本；右值重载被删除，禁止从临时对象取出悬垂视图。 |

#### 10.4.2 错误、结果与选项

```cpp
enum class http1_chunk_decode_error : std::uint8_t {
    invalid_framing,
    body_limit_exceeded,
    framing_limit_exceeded,
};

class http1_chunk_decode_complete final {
public:
    [[nodiscard]] constexpr std::span<const http_header> trailers() const noexcept;
};

using http1_chunk_decode_failure = decode_failure<http1_chunk_decode_error>;

using http1_chunk_decode_result = decode_step<decode_need_input, decode_output, http1_chunk_decode_complete, http1_chunk_decode_failure>;

enum class http1_trailer_obs_fold : std::uint8_t {
    reject,
    accept,
};

enum class http1_trailer_message : std::uint8_t {
    request,
    response,
};

struct http1_chunked_body_decoder_options final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
    http1_trailer_message trailer_message_{http1_trailer_message::request};
};
```

`http1_chunk_decode_error`：

| 成员 | 值 | 含义 |
|---|---|---|
| `invalid_framing` | 0 | 允许的前缀内 chunk 分帧或 Trailer 格式错误（含以裸 LF 结尾的行）。 |
| `body_limit_exceeded` | 1 | chunk 载荷累计超出 `body_limit_`。 |
| `framing_limit_exceeded` | 2 | 单个 chunk-size 行、Trailer 区段或每消息 chunk-ext 预算超出 `max_http_header_bytes`。 |

`http1_chunk_decode_complete` 表示 chunked 正文结束（含零长度 chunk 与 Trailer 区段），只由解码器构造；`trailers()` 是收到的 Trailer 区段（无则为空），字段借用产生它的解码器（有效期见下）。

`http1_trailer_obs_fold`：收到的 Trailer 区段如何对待 obs-fold（RFC 9112 §5.2）。服务端可以拒绝携带它的请求（默认）；用户代理必须在解释响应字段值前把每个 obs-fold 替换为 SP，因此响应解码器接受它。

| 成员 | 值 | 含义 |
|---|---|---|
| `reject` | 0 | 出现 obs-fold 即 `invalid_framing`。 |
| `accept` | 1 | 接受 obs-fold：按字段语法与区段预算校验后，以 HTTP 展开形式发布（obs-fold 及相邻 OWS 合并为一个 SP）。 |

`http1_trailer_message`：解码器接收的 Trailer 属于哪种消息。发送方不得在该消息 Trailer 中生成的字段（RFC 9110 §6.5.1）不是分帧错误，而是被丢弃、不发布（RFC 9112 §7.1.2）。

| 成员 | 值 | 含义 |
|---|---|---|
| `request` | 0 | 按请求 Trailer 规则过滤。 |
| `response` | 1 | 按响应 Trailer 规则过滤。 |

`http1_chunked_body_decoder_options` 字段：`body_limit_`（默认不限，计 chunk 载荷字节，载荷可能仍是传输编码字节）、`obs_fold_`（默认 `reject`）、`trailer_message_`（默认 `request`）。

#### 10.4.3 `class http1_chunked_body_decoder`

```cpp
class http1_chunked_body_decoder final {
public:
    explicit http1_chunked_body_decoder(http1_chunked_body_decoder_options config = {});
    ~http1_chunked_body_decoder();
    http1_chunked_body_decoder(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder& operator=(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept;
    http1_chunked_body_decoder& operator=(http1_chunked_body_decoder&&) noexcept;

    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available, std::size_t max_body_bytes = (std::numeric_limits<std::size_t>::max)()) & RUVIA_LIFETIMEBOUND;
    http1_chunk_decode_result decode(std::string_view, std::size_t = 0) && = delete;
    template </* 临时拥有型 std::basic_string<char, ...> 右值 */ input_type>
    http1_chunk_decode_result decode(input_type&&, std::size_t = 0) & = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    void reset() noexcept;
};
```

增量 sans-I/O chunk 分帧解码器，不做传输解码。不可复制，可 `noexcept` 移动。

| 成员 | 说明 |
|---|---|
| 构造函数 | 绑定配置。 |
| `decode(available, max_body_bytes)` | 推进一步；每次最多发布 `max_body_bytes` 字节 chunk 载荷（为 0 抛 `std::invalid_argument`）。载荷 `decode_output` 借用 `available`，仅在其被修改前有效；不完整的分帧保持未消费，由调用方保留。发布 Trailer 时可能因分配抛 `std::bad_alloc`（已发布字段被清空）。 |
| 删除的 `decode` 重载 | `&& = delete` 拒绝临时或 `std::move` 后的解码器，因为完成结果可能借用解码器拥有的 Trailer；`&` 上的 `decode(input_type&&, ...) = delete` 拒绝右值临时 `std::basic_string`，因为载荷视图借用输入。 |
| `state()` | `active` / `finished` / `failed`。 |
| `reset()` | 以构造时的配置开始新正文，保留 Trailer 存储容量。 |

`decode()` 仅在左值解码器上调用，并带 `RUVIA_LIFETIMEBOUND`；不会为延长结果寿命而复制正文或 Trailer。具名 owner 的用法如下，正文输入与解码器均须覆盖其各自的借用期：

```cpp
ruvia::http1_chunked_body_decoder decoder;
std::string wire = "0\r\n\r\n";
auto result = decoder.decode(wire);
if (const auto* done = result.get_if<ruvia::http1_chunk_decode_complete>()) {
    // 在下一次 decoder.decode() / reset() 或 decoder 移动、赋值、析构前消费 done->trailers()。
}
// ruvia::http1_chunked_body_decoder{}.decode(wire);  // 编译期拒绝临时 owner
// std::move(decoder).decode(wire);                  // 编译期拒绝右值 owner
// decoder.decode(std::string{"0\r\n\r\n"});         // 编译期拒绝临时输入 owner
```

分帧与上限契约：

- RFC 9112 不限制 chunk 数量；零长度 chunk 结束正文，因此空 chunk 不会重复出现。
- 每个 chunk-size 行（含扩展）与编码后的 Trailer 区段（含最终空行）各自独立受 `max_http_header_bytes`（64 KiB）约束。搜索在上限处停止，到达上限仍无终止符时报告 `framing_limit_exceeded`，不检查之后的字节。
- 同一消息所有 chunk-size 行中超出最短十六进制长度的字节（前导零与 chunk-ext）累计共享一份 `max_http_header_bytes` 预算（RFC 9112 §7.1.1），超出报告 `framing_limit_exceeded`（请求映射为 `413 chunked_framing_too_large`）。允许前缀内的格式错误仍报告 `invalid_framing`。
- **CRLF 分帧**：chunk-size 行和 chunk 数据分隔符中的裸 LF 到达即被拒绝（RFC 9112 §7.1）；数据后只有 CR 时，保留它并等待 LF。
- Trailer 区段还须满足 `http_field_section_budget` 字节规则（每字段 `name.size() + value.size() + 4`，结束空行加 2；见 [`"http_field_section_budget.h"`](#api-http-field-section-budget-h) 的 `http_trailer_section_budget()`）。obs-fold 按 `obs_fold_` 处理，禁止字段按 `trailer_message_` 丢弃。
- 完成结果按接收顺序发布剩余字段：字段名小写、值去掉首尾 OWS。字段归解码器所有，仅当区段保留了字段时才分配存储；视图有效至下一次 `decode()` 或 `reset()`，或解码器被移动、被赋值、析构。
- **缓冲回收**：下一次 `decode()` 或 `reset()` 使 Trailer 视图失效时，释放超过 4 KiB 的字段容器；校验失败或分配异常时同样回收。
- **分配失败**：保存 Trailer 抛出异常时，本次调用不提交分帧进度，可用相同输入重试。
- 终态：已完成或已失败的解码器不再消费输入，`decode()` 以 0 消费字节重放终态结果（完成结果不再携带 Trailer），直到 `reset()`。

#### 10.4.4 错误映射

```cpp
[[nodiscard]] inline http_protocol_error make_http_protocol_error(http1_chunk_decode_error error) noexcept;
```

| 错误 | 应答 |
|---|---|
| `invalid_framing` | `400`，code `invalid_chunked_body`，消息 `invalid chunked request body` |
| `body_limit_exceeded` | 等同 `make_http_protocol_error(http_request_body_failure::too_large)`：`413`，code `request_body_too_large` |
| `framing_limit_exceeded` | `413`，code `chunked_framing_too_large`，消息 `request body framing is too large` |

重载集归属见 10.3.1。

### 10.5 `"http1_server_response.h"`
<a id="api-http1-server-response-h"></a>

服务端 HTTP/1 响应侧：响应头缓冲、最终/流式/临时/CONNECT/升级响应头规划与序列化、响应侧连接持久性提交。公开 include [`"http1_connection_plan.h"`](#api-http1-connection-plan-h)、[`"http1_server_request.h"`](#api-http1-server-request-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_response.h"`](#api-http-response-h)、[`"http_response_stream.h"`](#api-http-response-stream-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http1_server_response.h`](../ruvia-http/include/http1_server_response.h)

#### 10.5.1 `class http1_response_head_error`

```cpp
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};
```

本端无法发送的 HTTP/1 响应头：临时、最终、CONNECT 或 CONNECT-UDP 响应头违反消息规则（RFC 9110、RFC 9112）或超出头部大小上限。它报告的是应用或运行时缺陷，绝不是对端错误，服务端应像处理其他处理器失败一样应答（Web 层在响应头尚未提交时转为独立错误响应，见 [ruvia-web](ruvia-web.md)）。派生自 `std::invalid_argument`，继承其构造函数。

#### 10.5.2 `class http_response_head_buffer`

```cpp
inline constexpr std::size_t response_head_stack_bytes = 512;
inline constexpr std::size_t response_head_retained_heap_bytes = std::size_t{4} * 1024;

class http_response_head_buffer final {
public:
    http_response_head_buffer() noexcept = default;

    http_response_head_buffer(const http_response_head_buffer& other);
    http_response_head_buffer(http_response_head_buffer&& other) noexcept;
    http_response_head_buffer& operator=(const http_response_head_buffer& other);
    http_response_head_buffer& operator=(http_response_head_buffer&& other) noexcept;
    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void append_unsigned(std::uint64_t value);
    void reserve_additional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool can_append_on_stack(std::size_t size) const noexcept;

    template <typename writer_type>
        requires std::is_nothrow_invocable_r_v<void, writer_type&, char*>
    void append_generated(std::size_t size, writer_type&& writer);

    [[nodiscard]] char* stack_cursor(std::size_t bound) & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] char* stack_cursor(std::size_t) && = delete;

    void commit_stack(const char* end);
};
```

可复用的序列化响应头 scratch：小响应头保存在 `response_head_stack_bytes`（512）字节的内联数组中，超出后溢出到堆；跨请求只保留不超过 `response_head_retained_heap_bytes`（4 KiB）的堆容量。

| 成员 | 说明 |
|---|---|
| 默认构造 | 空内联状态，不分配。 |
| 复制赋值 | 复制对端已提交内容；堆替换成功后才发布内联字节与状态，分配失败时两端保持原有已提交内容。 |
| 复制构造 | 独立复制对端已提交的字节，无论其位于内联还是堆存储。 |
| 移动构造 / 移动赋值 | 不分配；接管对端堆存储并复制其已提交状态。赋值先发布完整内容，再释放目标的旧堆存储。 |
| `reset()` | 先发布空内联状态，再按 `response_head_retained_heap_bytes` 清理堆存储（超出则释放）。重新使内联 cursor 可用。 |
| `append(std::string_view value)` | 追加文本；`value` 可与本缓冲区存储别名，包括 `stack_cursor()` 返回的已写区间。剩余容量足够时，自追加不分配；内联放不下时溢出到堆。超出 `std::string::max_size()` 抛 `std::length_error`，分配失败抛 `std::bad_alloc`。 |
| `append(char value)` | 追加一个字符。 |
| `append_unsigned(value)` | 追加十进制数字。 |
| `reserve_additional(size)` | 在当前长度之上预留 `size` 字节；能放进内联剩余空间时不溢出。需要溢出时先在堆上分配，失败则仍保持内联状态（已取得的内联 cursor 不失效）。 |
| `view()` | 已提交内容；借用本缓冲区，右值重载被删除。 |
| `can_append_on_stack(size)` | 仍处于内联状态且剩余空间不少于 `size`。 |
| `append_generated(size, writer)` | 生成恰好 `size` 字节：内联放得下时把内联 cursor 交给 `writer` 并提交，否则预留堆空间、扩展堆长度后交给 `writer`。`writer` 必须 `noexcept` 地写满 `size` 字节（约束 `std::is_nothrow_invocable_r_v<void, writer_type&, char*>`）。 |
| `stack_cursor(bound) &` | `bound` 能放进当前内联剩余空间时返回当前末尾的未提交内联 scratch，否则（含已溢出到堆）返回 `nullptr`。这是容量检查，不是独占预留：缓冲区必须存活且地址不变，期间的其他修改可能改变逻辑末尾。右值重载被删除。 |
| `commit_stack(end)` | 把内联已提交长度设为 `end`（必须是本对象内联数组内的端点，允许 one-past 端点，也允许缩短内容）。已溢出到堆时抛 `std::logic_error`；空指针或不属于本对象数组的端点抛 `std::invalid_argument`；拒绝时保留已提交内容。对外来数组不做指针减法，即使端点取自另一个存活缓冲区。溢出后须先 `reset()` 才能再取得内联 cursor。 |

#### 10.5.3 响应头计划

```cpp
enum class http1_response_content_source : std::uint8_t {
    buffered,
    streamed,
};

class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_message_framing& framing() const& noexcept RUVIA_LIFETIMEBOUND;
    const http1_message_framing& framing() const&& = delete;

    [[nodiscard]] constexpr http1_response_content_source content_source() const noexcept;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept;

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept;

    [[nodiscard]] constexpr bool send_body() const noexcept;
};

[[nodiscard]] inline http1_response_head_plan plan_http1_buffered_response_head(http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept;

[[nodiscard]] constexpr http1_response_head_plan plan_http1_response_stream_head(http_response_body_plan body_plan, http1_request_connection_plan connection_plan, http1_message_framing framing) noexcept;
```

`http1_response_content_source`：谁度量响应头声明的长度。

| 成员 | 值 | 含义 |
|---|---|---|
| `buffered` | 0 | 响应写出器按完整缓冲表示度量。 |
| `streamed` | 1 | 内容随头之后流出，长度由响应自身字段声明。 |

`http1_response_head_plan` 是最终 HTTP/1 响应头的唯一分帧协议值，把方法/状态的内容语义（`body_plan()`，见 [`"http_response_body.h"`](#api-http-response-body-h)）绑定到 HTTP/1 规划器选定的确切线上分帧。只能经下列工厂取得：

- `known_length` + `buffered`：写出器以规范的 `Content-Length` 界定完整表示。
- `known_length` + `streamed`：响应自身的 `Content-Length` 界定流（RFC 9112 §6.3）；运行时写原始正文字节，拒绝超出声明长度的写入，流在不足声明长度时结束则关闭连接（RFC 9112 §8）。
- `chunked`：写出器独占规范的 `Transfer-Encoding: chunked`，运行时发出匹配的 chunk 帧；应用首部不能重定义分帧。
- `close_delimited`：响应没有声明的消息体长度。允许内容时运行时关闭连接来界定它，因此应用的 `Content-Length` 与 `Transfer-Encoding` 不会进入线上响应头。

| 成员 | 说明 |
|---|---|
| `framing()` | 选定分帧；借用本计划，右值重载被删除。 |
| `content_source()` | 长度来源。 |
| `body_plan()` | 方法/状态的内容语义。 |
| `protocol_version()` | 请求的确切版本，用于序列化。 |
| `send_body()` | 缓冲响应在头之后是否写载荷字节：方法/状态允许正文且表示非空。 |
| `plan_http1_buffered_response_head(write_plan, connection_plan)` | 完整缓冲响应的头：`known_length(write_plan.content_length())`、`buffered`，正文语义取自 `write_plan`（由 [`plan_buffered_http_response_write()`](#api-http-response-body-h) 得到），版本取自 `connection_plan`。 |
| `plan_http1_response_stream_head(body_plan, connection_plan, framing)` | 由 `framing`（`chunked`、`close_delimited` 或响应自身声明的 `known_length`）界定的流式响应头，`streamed`。 |

响应长度语义：HTTP/1、HTTP/2 与 HTTP/3 共用同一 `Content-Length` 选择；HTTP/1 只在其上叠加 `chunked` 与 `close_delimited` 分帧，保留 HEAD 的表示元数据（HEAD 缓冲响应写出所选表示的长度）及 `204`、`205`（规范化为 `Content-Length: 0`）、CONNECT 规则。

#### 10.5.4 最终响应头与 Trailer 序列化

```cpp
void append_http1_response_head(const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan);

void append_http1_response_trailers(std::string& output, const http_response_trailer_section& trailers);
```

`append_http1_response_head(response, head, plan)`：把已定稿的 HTTP/1 响应头序列化进调用方拥有的可复用 scratch。`plan` 必须描述同一响应表示。

- 线上顺序：状态行（`HTTP/1.1 <code> <reason>`，原因短语为空时仍保留 SP）；应用字段（跳过 `Content-Length` 与 `Transfer-Encoding`）；未显式设置时的 `Date`；计划要求时的 `Transfer-Encoding: chunked`；规范 `Content-Length`；结束空行。
- 规范 `Content-Length` 的度量与写出共用同一十进制 token。`chunked` 分帧、以及允许内容的 `close_delimited` 不写 `Content-Length`；HEAD 等正文被抑制的响应保留表示长度元数据。
- 显式的 `Date` 原样保留，不读时钟；否则在时钟可用时写出缓存的当前 `Date`。
- 分帧归计划所有（RFC 9112 §6.1）：应用设置的 `Transfer-Encoding` 只有在值（忽略 OWS 与 ASCII 大小写）恰为单个 `chunked` 时才作为分帧提示接受，并被计划自身的分帧替换：只有分块流写出规范的 `Transfer-Encoding: chunked`，缓冲、定长流、HTTP/1.0 关闭定界、HEAD 与 `204` / `205` / `304` 都不写出。其他任何值（`gzip`、`gzip, chunked`、`identity`、空值或畸形列表）声明了写出器不会施加的编码，抛 `http1_response_head_error`。公开响应字段 setter 仍要求规范化字段值（首尾 SP/HTAB 在设置时以 `std::invalid_argument` 拒绝，见 [`"http_response_headers.h"`](#api-http-response-headers-h)）；写出器去除 OWS 不放宽该入口契约。HTTP/2 的对应规则见 [`"http2_connection.h"`](#api-http2-connection-h)。
- 先度量整个响应头再触碰输出：度量同时执行与请求解析相同的字段区段规则（[`"http_field_section_budget.h"`](#api-http-field-section-budget-h)）。
- 抛出 `http1_response_head_error`：显式 `Content-Length` 非法；`2xx` CONNECT 响应须走 CONNECT 隧道路径；`Transfer-Encoding` 不是恰好 `chunked`；HTTP/1.0 版本的分块载荷计划；字段违反多行或分帧相关规则（`Connection` / `Upgrade` 语法，未写出 chunked 时出现非空 `Trailer` 字段）；有 `Upgrade` 但 `Connection` 未列出 `upgrade`；响应头超出大小上限。计划的状态或表示与 `response` 不一致时抛 `std::invalid_argument`（调用方缺陷）。缓冲区增长失败抛 `std::length_error` / `std::bad_alloc`。

`append_http1_response_trailers(output, trailers)`：只追加 Trailer 字段行，chunk 定界符（`http1_last_chunk_prefix`、`http1_trailer_section_terminator`）归调用方。区段为空时不做任何事。输入可以与 `output` 别名。超出 Trailer 区段预算或 `output` 的最大长度抛 `std::length_error`；分配失败时 `output` 已有字节不变。`http_response_trailer_section` 见 [`"http_response_stream.h"`](#api-http-response-stream-h)。

#### 10.5.5 临时响应

```cpp
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept;
};

class prepared_http1_interim_response final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept;

    [[nodiscard]] constexpr http1_interim_connection_disposition connection_disposition() const noexcept;
};

[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer);
```

`http1_interim_connection_disposition`：编码临时响应后的连接生命周期。RFC 9112 要求 `Connection: close` 的发送方在含该选项的响应之后开始关闭，即使这会使请求得不到最终响应。

| 成员 | 值 | 含义 |
|---|---|---|
| `unchanged` | 0 | 临时响应不改变连接生命周期。 |
| `close_after_interim_response` | 1 | 临时响应含 `Connection: close`，写完该临时响应后立即开始关闭连接。 |

`prepare_http1_interim_response_head(response, head_buffer)`：不分配的 HTTP/1.1 临时响应头写出器（`http_interim_response_head` 见 [`"http_response.h"`](#api-http-response-h)）。

- 在触碰输出之前按 [`http_field_section_budget`](#api-http-field-section-budget-h) 校验完整的借用字段集（`max_http_header_fields` 行、`max_http_header_bytes` 即 64 KiB，含状态行与结束空行）；无法发送的头抛 `http1_response_head_error`：`Content-Length`、`Transfer-Encoding`、`Trailer`、`TE` 字段，重复的单例字段，非法字段，非法 `Connection` / `Upgrade`，有 `Upgrade` 而 `Connection` 未列出 `upgrade`，超出大小上限。
- `head_buffer` 不足时返回 `http1_interim_response_buffer_too_small`，其 `required_head_bytes()` 给出所需字节数；输出未被修改。
- 成功时事务性地写入 `head_buffer` 前缀（字段可与 `head_buffer` 别名），返回 `prepared_http1_interim_response`：`head()` 指向调用方输出缓冲区中的已编码字节（借用 `head_buffer`），`connection_disposition()` 给出上表处置。
- 不注入 `Server` 或 `Date`：编码的字段恰为 `http_interim_response_head` 所表示的字段。
- 两个结果类型只由该函数构造。

#### 10.5.6 流式响应计划与连接提交

```cpp
class http1_response_stream_plan final {
public:
    [[nodiscard]] http1_framing_kind framing() const noexcept;

    [[nodiscard]] http1_request_connection_plan request_connection_plan() const noexcept;

    [[nodiscard]] http1_close_policy close_policy() const noexcept;

    [[nodiscard]] http_known_method request_method() const noexcept;
};

[[nodiscard]] inline http1_response_stream_plan plan_http1_response_stream(const http1_server_request_parser& parsed_value, http1_close_policy close_policy, http1_request_body_consumption consumption) noexcept;

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response, http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only);

[[nodiscard]] http1_request_connection_plan http1_commit_final_response(http_response& response, http1_request_connection_plan plan);

class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept;
};

[[nodiscard]] prepared_http1_response_stream prepare_http1_response_stream_head(http_response response, const http1_response_stream_plan& plan, http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);
```

`http1_response_stream_plan`（只由 `plan_http1_response_stream()` 构造）是纯 HTTP/1 的流式响应提交前计划：运行时只提供类型化的产品策略（如每连接请求数上限）与请求体消费状态；HTTP 保留请求的关闭策略与候选分帧，直到响应状态已知。

| 成员 | 说明 |
|---|---|
| `framing()` | 响应未在提交时声明自身长度时，承载正文的流使用的分帧：HTTP/1.1 为 `chunked`，HTTP/1.0 为 `close_delimited`。 |
| `request_connection_plan()` | `apply_request_body_consumption(parser.connection_plan(), consumption)` 的结果。 |
| `close_policy()` | 运行时给出的产品策略。 |
| `request_method()` | 请求的 `http_known_method`。 |
| `plan_http1_response_stream(parsed_value, close_policy, consumption)` | 由已完成的解析器给出计划。路由在流式输出前已读完请求体时传 `complete`；解析器的请求体计划仍描述原始请求分帧，因此请求体未读时传 `body_plan().consumption_if_unread()`。提交时据此区分需要关闭定界的 HTTP/1.0 带正文流与已自定界的正文被抑制响应。 |

`http1_connection_close_field_policy` 与 `http1_mark_connection_close(response, field_policy)`：运行时的关闭裁决优先于 keep-alive，在套接字生命周期决定之后把响应的全部 `Connection` 字段合并为一个。

| 成员 | 值 | 含义 |
|---|---|---|
| `close_only` | 0 | 合并后的基础值为 `close`。 |
| `preserve_upgrade` | 1 | 基础值为 `close, Upgrade`：响应保留的 `Upgrade` 字段要求 `Connection` 列出 `Upgrade` 选项。 |

合并规则：去掉原有的 `close`、`keep-alive`、`Upgrade` 选项，保留应用在 `Connection` 中声明的其他自定义逐跳字段名（RFC 9110 §7.6.1），追加在基础值之后；结果以替换方式写回单个 `Connection` 字段。可能因分配抛出。

`http1_commit_final_response(response, plan)`：在运行时并入请求体完成状态与服务端策略之后，提交响应侧 HTTP/1 持久性；这是唯一的协议修改点。

- 响应自带 `Connection: close`，或隧道请求（CONNECT）的最终响应不是 `2xx`（失败的 CONNECT 绝不成为隧道，RFC 9931 §8），计划收紧为关闭。
- 关闭时调用 `http1_mark_connection_close()`（响应有 `Upgrade` 字段时用 `preserve_upgrade`）。
- 可复用时：响应有 `Upgrade` 字段而 `Connection` 未列出时补上 `Upgrade` 选项；HTTP/1.0 请求且响应未列出 `keep-alive` 时补上 `keep-alive`。已有 `Connection` 行被扩展而不是替换。
- 成功时返回的计划保留确切请求版本供响应头序列化。无法发送的头（例如没有 `Upgrade` 协议的 `426`，或 `Transfer-Encoding` 不是恰好 `chunked`）在修改响应之前抛 `http1_response_head_error`。

`prepare_http1_response_stream_head(response, plan, trailer_intent)`：流式响应提交。

- 分帧选择（RFC 9112 §6.3）：响应自身合法的 `Content-Length`、无 `Transfer-Encoding`（它覆盖 `Content-Length`）且正文未被抑制、状态允许正文时，在任一 HTTP/1 版本上选择 `known_length`（`response_head_plan().framing().content_length()` 给出长度），保留 `Content-Length`，连接可复用，不承载 Trailer；否则使用计划的 `chunked`（HTTP/1.1）或 `close_delimited`（HTTP/1.0）并移除 `Content-Length`。定长流的超长与不足由驱动拒绝并关闭连接（RFC 9112 §8）。
- 连接：仅当请求连接计划与运行时策略都允许复用，且分帧不是 `close_delimited`（或正文被抑制，已自定界）时保持复用，否则要求关闭；随后调用 `http1_commit_final_response()`。
- 响应随后按 `http_response_stream_commit_plan`（见 [`"http_response_stream.h"`](#api-http-response-stream-h)，`trailer_intent` 同样定义于该头）经 `prepare_http_response_stream_head()` 规范化。
- 返回的 `prepared_http1_response_stream` 把请求/版本/运行时约束与响应方法/状态和 `Connection` 选项合并，并把确切首部字节绑定到权威连接计划：`response()`（左值可变/只读访问，右值重载被删除）、`response_head_plan()`（传给 `append_http1_response_head()`）、`commit_plan()`、`connection_plan()`（提交后的权威连接计划）。
- 异常：与 `http1_commit_final_response()` 及 `prepare_http_response_stream_head()` 相同（无法发送的响应头抛 `http1_response_head_error`）。

#### 10.5.7 CONNECT 与协议升级

```cpp
[[nodiscard]] http1_response_head_plan prepare_http1_connect_response_head(const http_response& response, http_protocol_version version);

[[nodiscard]] bool http1_request_offers_upgrade(const http_request& request) noexcept;

[[nodiscard]] http1_response_head_plan prepare_http1_upgrade_response_head(http_response& response, const http_request& request);
```

| 函数 | 说明 |
|---|---|
| `prepare_http1_connect_response_head(response, version)` | 成功的 CONNECT 响应头结束 HTTP 分帧并把连接交给双工字节流，载荷属于该流而不是 HTTP 响应。要求 `version` 为 `http10` 或 `http11`、状态为 `2xx`、无正文与文件正文、无 `Content-Length` / `Transfer-Encoding` 字段，否则抛 `http1_response_head_error`。返回 `close_delimited`、`streamed` 的计划，正文语义按 CONNECT 方法与响应状态计算。 |
| `http1_request_offers_upgrade(request)` | RFC 9110 §7.8：HTTP/1.1 请求是否要求切换协议，即 `Upgrade` 字段提供至少一个协议，且 `Connection` 列出 `upgrade` 选项。其他请求的 `Upgrade` 被忽略，CONNECT 的也被忽略（其 `2xx` 建立隧道）；字段畸形时返回 `false`。 |
| `prepare_http1_upgrade_response_head(response, request)` | 把 `response`（不含内容或分帧字段的响应头）改写为 `101 Switching Protocols`，之后 HTTP/1.1 连接承载它所指定的协议（RFC 9110 §7.8、§15.2.2）：使用响应自己的 `Upgrade` 字段，否则取请求提供的第一个协议；缺少时补上 `Connection` 的 `Upgrade` 选项。请求未提供升级、响应选择了请求未提供的协议、或响应携带内容或分帧字段时，在修改 `response` 之前抛 `http1_response_head_error`。返回 `close_delimited` 计划（连接不再回到 HTTP）。 |

示例（缓冲响应头；请求来自 10.2 的解析器，`response` 由应用生成）：

```cpp
const auto connection = ruvia::http1_commit_final_response(
    response, ruvia::apply_request_body_consumption(parser.connection_plan(), consumption));
const auto head_plan = ruvia::plan_http1_buffered_response_head(
    ruvia::plan_buffered_http_response_write(parser.request().known_method(), response), connection);
ruvia::http_response_head_buffer head;  // 跨请求复用，下一请求前 reset()
ruvia::append_http1_response_head(response, head, head_plan);
// 写出 head.view()；head_plan.send_body() 时再写正文
// connection.close_policy() == close_after_response 时写完后关闭
```

### 10.6 `"http1_client_request.h"`
<a id="api-http1-client-request-h"></a>

出站 HTTP/1.1 请求头写出与流式请求内容规划。这些是与传输无关的 HTTP 值：`http_origin_view` 与 `http_request_head_view` 借用其字符串存储；响应内容由遵循分帧计划的外部 sans-I/O 驱动拥有；套接字/TLS 配置、连接池、重定向上限与超时也属于驱动。公开 include [`"http1_connection_plan.h"`](#api-http1-connection-plan-h)、[`"http_authority.h"`](#api-http-authority-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_request_content_plan.h"`](#api-http-request-content-plan-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)、[`"ip_address.h"`](#api-ip-address-h)。源码：[`ruvia-http/include/http1_client_request.h`](../ruvia-http/include/http1_client_request.h)

#### 10.6.1 `class http1_client_exchange_state`

```cpp
class http1_client_exchange_state final {
public:
    http1_client_exchange_state(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state& operator=(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state(http1_client_exchange_state&&) noexcept = default;
    http1_client_exchange_state& operator=(http1_client_exchange_state&&) = delete;
};
```

恰好一次 HTTP/1 响应交换的拥有型协议事实：方法、实际生效的关闭策略、初始请求内容状态，以及请求提供的 `Upgrade` 值。准备请求时从真正进入线上计划的事实创建它；转交给响应解析器后，不再依赖调用方的方法与首部存储。只有提供 `Upgrade` 时需要拥有动态存储，普通请求不分配。不可复制、不可移动赋值，仅可移动构造；唯一来源是 `prepared_http1_client_request::take_exchange_state()`。

#### 10.6.2 线策略与准备错误

```cpp
enum class http1_request_target_form : std::uint8_t {
    origin,
    absolute,
};

struct http1_client_request_wire_policy final {
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    http1_request_target_form target_form_{http1_request_target_form::origin};
};

enum class http1_client_request_prepare_error : std::uint8_t {
    invalid_method,
    invalid_target,
    invalid_connect_authority,
    unsupported_protocol,
    invalid_header,
    host_header_managed_by_writer,
    content_length_managed_by_writer,
    transfer_encoding_unsupported,
    trailer_requires_chunked_content,
    expect_header_managed_by_writer,
    invalid_connection,
    invalid_upgrade,
    upgrade_connection_option_required,
    te_connection_option_required,
    expectation_without_content,
    content_forbidden_for_method,
    options_content_type_required,
    header_too_large,
    invalid_close_policy,
    invalid_expectation,
    invalid_target_form,
};

[[nodiscard]] std::string_view http1_client_request_prepare_error_message(http1_client_request_prepare_error error) noexcept;
```

`http1_request_target_form`：非 CONNECT 请求行的目标形式（RFC 9112 §3.2）；CONNECT 总是 authority-form。

| 成员 | 值 | 含义 |
|---|---|---|
| `origin` | 0 | origin-form，或 `OPTIONS *` 的 asterisk-form：请求直接发往源站（§3.2.1）。 |
| `absolute` | 1 | absolute-form，用于经正向代理的请求（§3.2.2）：以源站的 `http://` / `https://` scheme 与 authority 前缀 origin-form 目标，生成的 `Host` 携带同一 authority（§3.2）；`OPTIONS *` 写为 authority 加空路径（§3.2.4）。 |

`http1_client_request_wire_policy`：请求准备中仅属 HTTP/1 的线上选择。请求的 expectation 与字段来源随 `http_request_head_view` 给出；运行时在释放 continue 门控正文之前等待多久属于运行时策略，不是 HTTP 消息模型设置。

| 字段 | 默认值 | 说明 |
|---|---|---|
| `close_policy_` | `http1_close_policy::allow_reuse` | `close_after_response` 时（调用方 `Connection` 未含 `close`）生成 `Connection: close`。调用方 `Connection: close` 同样使交换状态的关闭策略为 `close_after_response`。 |
| `target_form_` | `http1_request_target_form::origin` | 非 CONNECT 请求行的目标形式。 |

`http1_client_request_prepare_error`（`http1_client_request_prepare_error_message()` 返回对应的静态英文诊断文本）：

| 成员 | 值 | 含义 |
|---|---|---|
| `invalid_method` | 0 | 方法不是合法 token。 |
| `invalid_target` | 1 | 目标不是 origin-form（或 `OPTIONS` 的 `*`）。 |
| `invalid_connect_authority` | 2 | CONNECT 的 `authority_` 缺失，或不是端口非零的 `host:port`（RFC 9110 §9.3.6）。 |
| `unsupported_protocol` | 3 | `protocol_` 表示 HTTP/1.1 无法承载的请求：extended CONNECT、除 `connect-udp` 之外的 Upgrade，或其他方法上的协议（`kind()` 无值）。 |
| `invalid_header` | 4 | 字段名/值非法、字段语义不合法、`field_source_` 越界，或 CONNECT-UDP 握手校验失败。 |
| `host_header_managed_by_writer` | 5 | 调用方提供了 `Host`；`Host` 由写出器生成。 |
| `content_length_managed_by_writer` | 6 | 调用方提供了 `Content-Length`；长度由内容计划生成。 |
| `transfer_encoding_unsupported` | 7 | 调用方提供了 `Transfer-Encoding`；写出器只生成自己的 `chunked`。 |
| `trailer_requires_chunked_content` | 8 | `Trailer` 字段声明了 Trailer（RFC 9110 §6.6.2），但只有 chunked 请求内容能携带 Trailer（RFC 9112 §7.1.2）。 |
| `expect_header_managed_by_writer` | 9 | 调用方提供了 `Expect`；它由 `expectation_` 生成。 |
| `invalid_connection` | 10 | `Connection` 列表畸形，或点名了下一跳仍须看到的字段：分帧字段、`Host`、请求来源元数据（`Forwarded`、`X-Forwarded-For`、`X-Forwarded-Proto`、`X-Forwarded-Host`）或面向所有接收方的字段（RFC 9110 §7.6.1）；CONNECT-UDP 请求中调用方提供任何 `Connection` 字段。 |
| `invalid_upgrade` | 11 | `Upgrade` 列表畸形；CONNECT-UDP 请求中调用方提供任何 `Upgrade` 字段。 |
| `upgrade_connection_option_required` | 12 | 有 `Upgrade` 而 `Connection` 未列出 `upgrade`。 |
| `te_connection_option_required` | 13 | 有 `TE` 而 `Connection` 未列出 `TE`。 |
| `expectation_without_content` | 14 | `100-continue` expectation 但没有非空内容。 |
| `content_forbidden_for_method` | 15 | TRACE 上将跟随内容，或 CONNECT / 协议升级请求带任何内容计划（含 `bytes("")`）。 |
| `options_content_type_required` | 16 | 新生成（`generated`）的 OPTIONS 请求带内容却没有 `Content-Type`（RFC 9110 §9.3.7）。 |
| `header_too_large` | 17 | 请求头超出字段区段上限。 |
| `invalid_close_policy` | 18 | `close_policy_` 不是有效枚举值。 |
| `invalid_expectation` | 19 | `expectation_` 不是有效枚举值。 |
| `invalid_target_form` | 20 | 未知的目标形式，或 CONNECT 上使用 `absolute`。 |

#### 10.6.3 `prepare_http1_client_request`

```cpp
class http1_client_request_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept;
};

class prepared_http1_client_request final {
public:
    [[nodiscard]] constexpr std::string_view head() const& noexcept;
    [[nodiscard]] constexpr std::string_view head() const&& = delete;

    [[nodiscard]] constexpr const http_request_content_plan& content() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] constexpr const http_request_content_plan& content() const&& = delete;

    [[nodiscard]] constexpr bool continue_gated() const noexcept;

    [[nodiscard]] http1_client_exchange_state take_exchange_state() && noexcept;
};

using http1_client_request_prepare_result = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error>;

[[nodiscard]] http1_client_request_prepare_result prepare_http1_client_request(const http_origin_view& origin, const http_request_head_view& request, std::span<char> head_buffer RUVIA_LIFETIMEBOUND, http1_client_request_wire_policy policy = {});
```

`prepare_http1_client_request(origin, request, head_buffer, policy)` 为直连源站或（absolute-form）正向代理准备一个 HTTP/1.1 请求头。它在触碰调用方缓冲区之前校验完整请求，生成 `Host` 与精确的 `Content-Length`，并返回分离的头/内容视图，供 writev 式 I/O 或 `Expect: 100-continue` 门控使用。普通请求不分配；Upgrade 请求只拥有其提供的协议值以便之后校验 `101`。

结果（三选一）：

| 结果 | 说明 |
|---|---|
| `prepared_http1_client_request` | 成功；见下表。 |
| `http1_client_request_buffer_too_small` | `head_buffer` 不足，`required_head_bytes()` 给出所需字节数；缓冲区未被修改。只由准备函数构造。 |
| `http1_client_request_prepare_error` | 请求无法以 HTTP/1.1 发送（10.6.2）；缓冲区未被修改。 |

请求种类由 `request.kind()`（见 [`"http_request_head_view.h"`](#api-http-request-head-view-h)）选择请求行：

- `regular`：`origin`（[`"http_authority.h"`](#api-http-authority-h)）提供 scheme 与 authority（`Host`，以及 absolute-form 的目标前缀）；不读取 `request.scheme_` 与 `request.authority_`。端口等于 scheme 默认端口（`http` 80、`https` 443）时省略。
- `connect`：authority-form 指向 `request.authority_`（须带非零端口），端口总是写出，同一 token 用于目标与 `Host`；不写 `origin`（RFC 9112 §3.2.3）。转发不受信任的 TCP 载荷时，RFC 9931 §8 要求驱动等到成功的隧道计划之后才发送该载荷，除非 `close_after_response` 使请求带 `Connection: close`。
- `protocol_upgrade` 且 `protocol_` 为 `connect-udp`：生成 RFC 9298 §3.2 的 `GET` Upgrade 请求到 `origin`。校验 `Capsule-Protocol`；`Upgrade` 与 `Connection` 归写出器所有。RFC 9931 §6.3 禁止在服务器成功的协议升级响应之前发送 UDP 载荷。
- `extended_connect` 没有 HTTP/1.1 形式，返回 `unsupported_protocol`。

校验顺序：先校验线策略（`invalid_close_policy`、`invalid_expectation`、`invalid_target_form`），再按种类校验请求行、字段、内容与上限。

HTTP/1.1 CONNECT-UDP 在暂存字段前先检查字段数量，为自动生成的 `Connection` 和 `Upgrade` 留出预算；超限直接返回 `header_too_large`。

生成规则：

- 请求行之后依次写 `Host`、调用方字段（按原顺序）、内容分帧字段（`Content-Length` 或 `Transfer-Encoding: chunked`）、`Expect: 100-continue`（`expectation_` 要求时）、生成的 `Connection: close`、结束空行。生成的端口与 `Content-Length` token 由度量与写出共用。
- `Expect` 归写出器所有（`request.expectation_`），因此写出的字段与得到的内容门控不会不一致。
- 字段来源 `request.field_source_`（`generated` / `forwarded`）的值语义见 [`"http_request_head_view.h"`](#api-http-request-head-view-h)；`forwarded` 时 `Upgrade` 按接收方规则容忍空列表成员，OPTIONS 内容不要求 `Content-Type`（该要求只针对新生成的请求），写出器也不补造该字段。
- 内容计划（[`"http_request_content_plan.h"`](#api-http-request-content-plan-h)）按 HTTP/1 线上形态规范化后由 `content()` 返回：`none()` 无内容字段；带借用字节的已知长度写 `Content-Length`（显式空内容 `bytes("")` 写 `Content-Length: 0`，且不触发 TRACE 的内容禁止或 OPTIONS 的 `Content-Type` 要求）；不带字节的 `known_length(n)` 作为 `streaming(n)` 写 `Content-Length: n`，`known_length(0)` 规范化为显式空内容；无长度的 `streaming()` 写 `Transfer-Encoding: chunked`。
- 别名：元数据文本与首部描述符数组可以与 `head_buffer` 重叠，成功的准备在覆盖输出前已消费它们。借用的内容不得与写出的头字节重叠；它可以位于 `head_buffer` 未使用的尾部，并须在运行时发送前保持不变。
- 分配失败（仅 Upgrade 值的拥有存储）发生在修改输出之前，准备保持事务性。

`prepared_http1_client_request`（事务性准备好的 scatter-gather 请求）：

| 成员 | 说明 |
|---|---|
| `head()` | 指向调用方 `head_buffer` 的请求头字节；右值重载被删除。 |
| `content()` | 规范化后的内容计划：无内容、带借用字节的已知长度（含显式空内容）、或流式（无长度时 chunked）。借用字节须保持存活且不变直到发送。右值重载被删除。 |
| `continue_gated()` | 写出器已生成 `Expect: 100-continue`：I/O 所有者单独发送头，在收到 `100 Continue` 后或按其有限等待策略释放内容。 |
| `take_exchange_state() &&` | 移出独立的响应侧协议事实（`std::move(prepared).take_exchange_state()`）；之后 `head()` 与 `content()` 仍可读取，用于写请求或显式重试。 |

#### 10.6.4 `class http1_request_content_writer`

```cpp
enum class http1_request_content_write_error : std::uint8_t {
    awaiting_continue,
    stopped,
    write_pending,
    length_overflow,
    length_mismatch,
    invalid_trailer,
    trailers_require_chunked,
    output_too_small,
    header_too_large,
};

class http1_request_content_writer final {
public:
    class chunk final {
    public:
        [[nodiscard]] constexpr std::string_view prefix() const& noexcept RUVIA_LIFETIMEBOUND;
        std::string_view prefix() const&& = delete;

        [[nodiscard]] constexpr std::span<const char> payload() const noexcept;
        [[nodiscard]] constexpr std::string_view suffix() const noexcept;
    };
    explicit http1_request_content_writer(const prepared_http1_client_request& prepared) noexcept;
    void release_content() noexcept;
    void abort() noexcept;
    [[nodiscard]] std::variant<chunk, http1_request_content_write_error> plan_chunk(std::span<const char> payload RUVIA_LIFETIMEBOUND) noexcept;
    template </* 元素为 char 的临时、非 borrowed_range 连续容器 */ payload_type>
    std::variant<chunk, http1_request_content_write_error> plan_chunk(payload_type&&) noexcept = delete;
    std::variant<chunk, http1_request_content_write_error> plan_chunk(std::initializer_list<char>) noexcept = delete;
    void commit_chunk();
    [[nodiscard]] std::variant<std::string_view, http1_request_content_write_error> plan_finish(std::span<char> buffer RUVIA_LIFETIMEBOUND, std::span<const http_header_view> trailers = {}) noexcept;
    void commit_finish();
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] std::uint64_t committed_payload_bytes() const noexcept;
};
```

绑定到成功准备的流式请求的不可变分帧事实（前置条件：`prepared.content().streaming_content() != nullptr`；对非流式请求构造的写出器处于停止状态，所有规划返回 `stopped`）。声明了长度的流式内容按 `Content-Length` 原样写出；无长度时按 chunked 分帧。两阶段协议：`plan_*` 产出分段且不改变已提交计数，传输写完后 `commit_*` 提交。

`http1_request_content_write_error`：

| 成员 | 值 | 含义 |
|---|---|---|
| `awaiting_continue` | 0 | 请求带 continue 门控且尚未 `release_content()`。 |
| `stopped` | 1 | 写出器已 `abort()`、已完成，或请求不是流式内容。 |
| `write_pending` | 2 | 已有一个规划的 chunk 或结束分段尚未提交。 |
| `length_overflow` | 3 | 已提交字节加本次载荷超出 `std::uint64_t`。 |
| `length_mismatch` | 4 | 定长内容：本次载荷超出剩余声明长度，或结束时已提交字节不等于声明长度。 |
| `invalid_trailer` | 5 | Trailer 字段非法或为 Trailer 中禁止的字段。 |
| `trailers_require_chunked` | 6 | 定长内容不能携带 Trailer。 |
| `output_too_small` | 7 | `plan_finish()` 的 `buffer` 放不下结束分段。 |
| `header_too_large` | 8 | 最后 chunk 行与 Trailer 区段超出 Trailer 区段预算。 |

| 成员 | 说明 |
|---|---|
| 构造函数 | 从 `prepared` 读取门控（`continue_gated()`）与可选声明长度；不保留对 `prepared` 的引用。 |
| `release_content()` | 解除 continue 门控：收到 `100 Continue` 时或驱动的有限等待超时时调用。 |
| `abort()` | 停止写出器：传输失败必须中止；在上传完成前收到最终响应时必须 `abort()`，而不是隐式发送 FIN。之后所有规划返回 `stopped`，提交抛 `std::logic_error`。 |
| `plan_chunk(payload)` | 规划一个载荷分段。定长内容的 `prefix()` / `suffix()` 为空；chunked 且载荷非空时 `prefix()` 为十六进制 chunk-size 行、`suffix()` 为 CRLF；chunked 的空载荷不产生分帧（零长度 chunk 会结束正文）。载荷在 `commit_chunk()` 之前保持借用。 |
| 删除的 `plan_chunk` 重载 | 拒绝右值临时拥有型连续范围（`std::string`、`std::vector<char>`、`std::array<char, N>` 等）与 `std::initializer_list<char>`：`payload()` 借用输入，临时对象会在写出前销毁。左值与借用范围（`std::span`、`std::string_view`）照常绑定。 |
| `commit_chunk()` | 传输已消费规划的 chunk 后提交，累计载荷字节。没有待提交 chunk、已停止或已完成时抛 `std::logic_error`。 |
| `plan_finish(buffer, trailers = {})` | 规划结束分段并返回指向 `buffer` 的视图：chunked 时为 `0\r\n`、Trailer 字段行与结束空行；定长内容返回空视图，仍须成功 `commit_finish()`。Trailer 名与值可以借用 `buffer`。失败时 `buffer` 不变。 |
| `commit_finish()` | 结束分段已发送后提交，写出器进入完成状态。没有待提交结束分段、已停止或已完成时抛 `std::logic_error`。 |
| `finished()` | 是否已成功 `commit_finish()`。 |
| `committed_payload_bytes()` | 已提交的载荷字节数。 |

`chunk` 是写出器产出的只读分段快照：不允许默认构造，也不暴露可写的前缀缓冲、长度及载荷成员。保留具名 `chunk`，以 `prefix()`、`payload()`、`suffix()` 组成 scatter-gather 写入。

- `prefix()` 借用 `chunk` 自身的内联存储，只允许左值调用（右值重载被删除）；`chunk` 必须存活且不变，直到传输已消费前缀。
- `payload()` 仍借用原始输入，寿命独立于 `chunk`，调用方须保留输入直到消费完成。
- `suffix()` 指向静态常量，不借用 `chunk`。
- 复制或移动 `chunk` 保留独立的内联前缀与原有载荷借用，不分配、不复制载荷。

示例（流式 chunked 上传；`request` 的内容为 `http_request_content_plan::streaming()`）：

```cpp
std::array<char, 4096> head_buffer;
auto result = ruvia::prepare_http1_client_request(origin, request, head_buffer);
auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&result);
// 写出 prepared->head()
ruvia::http1_request_content_writer writer(*prepared);
ruvia::http1_client_response_parser parser(std::move(*prepared).take_exchange_state());

const auto planned = writer.plan_chunk(std::span<const char>(data));  // data 为左值缓冲
if (const auto* part = std::get_if<ruvia::http1_request_content_writer::chunk>(&planned)) {
    // writev(part->prefix(), part->payload(), part->suffix())，完成后：
    writer.commit_chunk();
}
std::array<char, 256> tail;
const auto finish = writer.plan_finish(tail);
// 写出 std::get<std::string_view>(finish) 后：
writer.commit_finish();
(void)parser.complete_request_content();
```

### 10.7 `"http1_client_response.h"`
<a id="api-http1-client-response-h"></a>

客户端 HTTP/1 响应头状态机：增量解析 `1xx` 临时响应与最终响应头，给出定长/分块/关闭定界的响应体分帧、CONNECT 隧道或协议升级决定（正文由 [`http1_body_decoder`](#api-http1-message-body-h) 解码）。公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"http1_client_request.h"`](#api-http1-client-request-h)、[`"http1_connection_plan.h"`](#api-http1-connection-plan-h)、[`"http_client_response_head.h"`](#api-http-client-response-head-h)、[`"http_content_coding.h"`](#api-http-content-coding-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http1_client_response.h`](../ruvia-http/include/http1_client_response.h)

#### 10.7.1 内容完成状态与 `class http1_client_response_plan`

```cpp
enum class http1_client_request_content_completion_status : std::uint8_t {
    completed,
    already_complete,
    exchange_terminal,
};

class http1_client_response_plan final {
public:
    [[nodiscard]] constexpr bool informational() const noexcept;

    [[nodiscard]] constexpr const http1_message_framing* framing() const& noexcept RUVIA_LIFETIMEBOUND;
    const http1_message_framing* framing() const&& = delete;

    [[nodiscard]] constexpr bool connect_tunnel() const noexcept;

    [[nodiscard]] constexpr bool protocol_upgrade() const noexcept;

    [[nodiscard]] constexpr http1_close_policy close_policy() const noexcept;

    [[nodiscard]] constexpr bool discards_content() const noexcept;

    [[nodiscard]] constexpr std::optional<http_client_request_content_signal> request_content_signal() const noexcept;
};
```

`http1_client_request_content_completion_status`：通知交换“外部运行时已写完准备好的请求内容计划中的每个字节”的结果。带内容的请求必须先有此事件，才能接受 `101 Switching Protocols`。

| 成员 | 值 | 含义 |
|---|---|---|
| `completed` | 0 | 本次调用记录了内容完成。 |
| `already_complete` | 1 | 内容此前已完成（或本来没有内容），状态不变。 |
| `exchange_terminal` | 2 | 响应头解析已终止（`finished` / `failed`）；这不是让外部请求写出器停止的指令，应遵循计划的内容信号。 |

`http1_client_response_plan` 是不可变的 RFC 9110/9112 响应分帧与生命周期契约，只由解析器构造。一个响应头恰好是下列之一：临时响应（交换继续等待下一响应头）、由 HTTP/1 分帧界定的最终响应、CONNECT 隧道、协议升级。分帧直接反映消息长度优先级与内容语义：`no_content` 是不能携带内容的最终响应（HEAD、`204`、`304`），而长度为 0 的 `known_length` 仍有分帧。

| 成员 | 说明 |
|---|---|
| `informational()` | `1xx`（`101` 除外）临时响应。 |
| `framing()` | 最终响应的分帧；其他三种为 `nullptr`。指针借用本计划，右值重载被删除。 |
| `connect_tunnel()` | 成功的 CONNECT（`2xx`）：隧道从首部之后开始。 |
| `protocol_upgrade()` | 合法的 `101` 协议切换。 |
| `close_policy()` | 自定界响应被消费后的连接生命周期。临时响应的 `allow_reuse` 只表示同一交换可以继续等待最终响应，不表示连接在最终响应之前可回池。关闭定界响应总是关闭；隧道与升级永不把连接还给 HTTP：三者都报告 `close_after_response`。 |
| `discards_content()` | 分帧为普通分帧的 `205` 响应。RFC 9110 §15.3.6 只禁止生成其内容，因此分帧界定的内容被消费后丢弃（`http1_body_decoder::discard_content()`），连接保持可复用。 |
| `request_content_signal()` | 对外部请求内容写出器的信号（`http_client_request_content_signal`，见 [`"http_client_response_head.h"`](#api-http-client-response-head-h)）。只有 `exchange_complete` 取消待发的请求内容。收到提前的最终响应但没有该信号时，外部写出器仍须完成请求，连接才能复用（RFC 9110 §7.5），即使响应头解析器已经终止。`continue_` 仅在 `100` 到达且内容仍受 Expect 门控时给出。 |

内容信号规则：

- 会关闭连接的最终响应到达时，若请求内容仍待发（含仍在等待 `100` 或已收到 `100` 但未完成），给出 `exchange_complete`。
- `100 Continue` 到达且内容仍受门控（尚未释放）时给出 `continue_`。
- 最终响应在 Expect 仍门控内容时到达，给出 `exchange_complete`；门控已被 `100` 或有限等待释放之后，最终响应本身不取消内容（RFC 9110 §7.5），由关闭响应提供停止信号。
- 其余情形无信号。

#### 10.7.2 解析错误

```cpp
enum class http1_client_response_parse_error : std::uint8_t {
    header_too_large,
    invalid_status_line,
    unsupported_http_version,
    invalid_status_code,
    invalid_reason_phrase,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding,
    content_length_and_transfer_encoding,
    invalid_protocol_switch,
};

[[nodiscard]] std::string_view http1_client_response_parse_error_message(http1_client_response_parse_error error) noexcept;
```

协议失败以不分配的类型化值报告；资源耗尽（展开字段、存放临时描述符、生成拥有型响应时）可能抛出。交换终止不是解析错误：交换完成或失败后再调用 `parse()` 抛 `std::logic_error`，直到 `reset()`。`http1_client_response_parse_error_message()` 返回对应的静态英文诊断文本。

| 成员 | 值 | 含义 |
|---|---|---|
| `header_too_large` | 0 | 响应头超出 `max_http_header_bytes`（64 KiB）或 `max_http_header_fields`（256）。 |
| `invalid_status_line` | 1 | 状态行缺少版本后的 SP，或以裸 LF 结尾。 |
| `unsupported_http_version` | 2 | 版本不是 `HTTP/1.x` 格式，或主版本不为 1。 |
| `invalid_status_code` | 3 | 状态码不是恰好三位 ASCII 数字，或其后缺少必需的 SP（原因短语为空也须保留）。 |
| `invalid_reason_phrase` | 4 | 原因短语含非法字节（原因短语可以以 SP/HTAB 开头或结尾，不做 OWS 修剪）。 |
| `invalid_header` | 5 | 字段行语法错误，或状态行之后任一字段行以裸 LF 结尾。 |
| `invalid_connection` | 6 | `Connection` 列表畸形。 |
| `invalid_upgrade` | 7 | `Upgrade` 列表畸形。 |
| `invalid_content_length` | 8 | 适用分帧的 `Content-Length` 非法，或超出 `std::size_t`。 |
| `conflicting_content_length` | 9 | 多个 `Content-Length` 值不一致。 |
| `invalid_transfer_encoding` | 10 | HTTP/1.1 带内容响应的 `Transfer-Encoding` 畸形。 |
| `unsupported_transfer_encoding` | 11 | 适用分帧的 `Transfer-Encoding` 含不支持的传输编码。 |
| `content_length_and_transfer_encoding` | 12 | HTTP/1.1 带内容响应同时出现 `Content-Length` 与 `Transfer-Encoding`。 |
| `invalid_protocol_switch` | 13 | 不可接受的 `101`（条件见 10.7.4）。 |

#### 10.7.3 `class http1_parsed_client_response_head` 与结果类型

```cpp
class http1_parsed_client_response_head final {
public:
    http1_parsed_client_response_head(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head& operator=(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head(http1_parsed_client_response_head&&) noexcept = default;
    http1_parsed_client_response_head& operator=(http1_parsed_client_response_head&&) = delete;

    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_client_response_head& head() const&& = delete;

    [[nodiscard]] http_client_response_head take_head() && noexcept;

    [[nodiscard]] const http1_client_response_plan& plan() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http1_client_response_plan& plan() const&& = delete;
};

using http1_client_response_parse_failure = decode_failure<http1_client_response_parse_error>;

using http1_client_response_parse_result = decode_step<decode_need_input, http1_parsed_client_response_head, http1_client_response_parse_failure>;
```

`http1_parsed_client_response_head` 是一个完整响应头，只由解析器构造，仅可移动构造。

| 成员 | 说明 |
|---|---|
| `head()` | 拥有状态与字段存储的 [`http_client_response_head`](#api-http-client-response-head-h)；原始输入可在解析完成后释放。右值重载被删除。 |
| `take_head() &&` | 移出响应头。 |
| `plan()` | 本响应头的 `http1_client_response_plan`；右值重载被删除。 |

`parse()` 的单步结果：

| 结果 | `consumed_bytes()` | 含义 |
|---|---|---|
| `decode_need_input` | 0 | 响应头尚不完整。 |
| `http1_parsed_client_response_head` | 精确到 CRLF CRLF 之后的响应头字节数 | 完整响应头；sans-I/O 驱动把其余字节直接交给正文/隧道/升级路径。该结果只可移动，经持有它的左值单步 `get_if<http1_parsed_client_response_head>()` 后 `std::move(*p).take_head()` 提取。 |
| `http1_client_response_parse_failure` | 0 | 响应头被拒绝，`error()` 为 `http1_client_response_parse_error`。 |

#### 10.7.4 `class http1_client_response_parser`

```cpp
class http1_client_response_parser final {
public:
    explicit http1_client_response_parser(http1_client_exchange_state exchange_state) noexcept;

    http1_client_response_parser(const http1_client_response_parser&) = delete;
    http1_client_response_parser& operator=(const http1_client_response_parser&) = delete;
    http1_client_response_parser(http1_client_response_parser&&) = delete;
    http1_client_response_parser& operator=(http1_client_response_parser&&) = delete;

    void release_request_content() noexcept;

    [[nodiscard]] http1_client_request_content_completion_status complete_request_content() noexcept;

    [[nodiscard]] http1_client_response_parse_result parse(std::string_view buffer);

    [[nodiscard]] decoder_state state() const noexcept;
    void reset(http1_client_exchange_state exchange_state) noexcept;
};
```

每请求一个的 HTTP/1 响应头状态机。构造时消费一次成功准备的请求的拥有型交换状态（[`http1_client_exchange_state`](#api-http1-client-request-h)）；临时响应推进同一交换，直到最终响应、CONNECT 隧道或协议切换完成它。不可复制、不可移动。

| 成员 | 说明 |
|---|---|
| 构造函数 | 接管交换状态；初始请求内容阶段为完成、待发或等待 `100`。 |
| `release_request_content()` | 报告驱动在未收到 `100` 时释放了 Expect 门控（例如有限等待超时）。它既不表示请求内容完成，也不能替代 `101` 之前所需的 `100`。终止的交换不受影响；始终遵循计划的请求内容信号。 |
| `complete_request_content()` | 在响应头仍待解析时报告请求内容已完成；返回值见 10.7.1。 |
| `parse(buffer)` | `need_input` 之后，向同一逻辑前缀追加数据再调用；解析出一个响应头之后，删除 `consumed_bytes()` 再提供后续响应（`1xx` 之后的下一响应头）。分配失败直接传出，不完成也不使交换失败；回收资源后以同一响应前缀重试。终态下调用抛 `std::logic_error`；完整响应头到达但输入不是先前前缀的延伸时也抛 `std::logic_error`。 |
| `state()` | `active`：等待响应头（临时响应保持 active）；`finished`：已解析最终响应、隧道或升级响应头；`failed`：响应头被拒绝。 |
| `reset(exchange_state)` | 开始另一个已准备请求的响应侧，清空全部进度。 |

解析规则：

- 提前拒绝：状态行在其换行到达时即校验，以裸 LF 结尾报告 `invalid_status_line`，其余错误与完整响应头相同；之后任一以裸 LF 结尾的字段行立即报告 `invalid_header`（RFC 9112 §2.2），不等待空行、64 KiB 上限或读超时，与服务端请求头的提前拒绝一致。非 HTTP 上游（如 SSH banner）因此在首行即失败。
- 状态码：`000..099` 与 `600..999` 按 `5xx` 处理（RFC 9110 §15）：`status()` 为 `500`，`received_status()` 保留原值（见 [`"http_status.h"`](#api-http-status-h) 的 `http_received_status` 与 [`"http_client_response_head.h"`](#api-http-client-response-head-h)）。
- 上限：任意数量的临时响应头可以先于最终响应，每个响应头各自受 `max_http_header_bytes` 与 `max_http_header_fields` 约束。临时字段描述符使用有界内联存储，需要时溢出到堆。首部校验是事务性的：只有协议校验成功后才分配拥有型响应头。
- obs-fold：响应字段区段中的 obs-fold 在字段校验前被替换为空格（状态行之后的空白不会折入原因短语）。正文 Trailer 的 obs-fold 由正文解码器以 `obs_fold_ = accept` 处理。
- 无内容响应：`1xx`、HEAD、`204`、`304` 响应在第一个空行结束，不论分帧字段如何（RFC 9112 §6.3 规则 1）；被忽略的分帧值不予解释（即使非法）。HEAD 与 `304` 的合法 `Content-Length` 作为所选表示的元数据保留，非法时只是不再提供已知长度。成功的 CONNECT 在空行处开始隧道。
- 带内容的最终响应分帧（HTTP/1.1）：同时有 `Content-Length` 与 `Transfer-Encoding` 报告 `content_length_and_transfer_encoding`；最终编码为 `chunked` 时为 `chunked(codings)`；最终编码不是 `chunked` 时为 `close_delimited(codings)` 并关闭；否则 `Content-Length` 给出 `known_length`；都没有时为 `close_delimited` 并关闭（无法回池）。
- HTTP/1.0 与 `Transfer-Encoding`（RFC 9112 §6.1）：视为分帧故障，忽略传输编码（不解析），回退到 `Content-Length` 或关闭定界，并在该响应后关闭连接；HEAD/`204`/`304` 同样关闭。若该故障出现在 HTTP/1.0 临时响应上，同一交换的最终响应也关闭，且之后的 `101` 不被接受。
- 辅助字段不选择分帧：初始 `Trailer` 值是不透明的声明元数据，只受普通字段字节与大小限制，不做 Trailer 名/列表校验，不要求 chunked 分帧；声明既不承诺也不授权实际 Trailer，实际 Trailer 字段仍受其独立限制。`TE` 没有响应语义；广告型 `Upgrade` 只在协商的 `101` 切换时才要求 `Connection: Upgrade`。这些接收方规则不放宽 HTTP/1 发送方要求。
- `Connection`：接收方只拒绝畸形列表；它不把发送方“不得提名端到端字段”的义务误作接收方拒绝规则（RFC 9110 §7.6.1）。
- `1xx`：永不终止交换，也不取消请求内容，其 `Connection` 选项不改变连接生命周期，最终响应才决定复用（RFC 9110 §15.2）。`100 Continue` 推进请求内容阶段。
- `101`：仅当响应为 HTTP/1.1、请求提供了 `Upgrade` 协议、请求内容已完成（`complete_request_content()` 或本来无内容）、交换未要求关闭、响应 `Connection` 列出 `upgrade` 且不含 `close`、响应 `Upgrade` 至少一个协议且全部为请求所提供、且之前没有 HTTP/1.0 临时响应分帧故障时接受，结果为 `protocol_upgrade()`；否则报告 `invalid_protocol_switch`。
- 最终响应的复用：先前有 HTTP/1.0 临时分帧故障、请求方法为 CONNECT（被拒绝的 CONNECT 保留普通响应分帧但总是在该响应后关闭，RFC 9931 §8，绝不回池）、交换的关闭策略为 `close_after_response`、或最终响应在 Expect 仍门控内容时到达（服务器可能仍在读取已承诺分帧的内容，RFC 9110 §10.1.1），一律关闭。否则：响应 `Connection: close` 或 HTTP/1.0 带 `Transfer-Encoding` 时关闭；HTTP/1.1 或带 `keep-alive` 时复用；其余关闭。

示例（读取响应头并解码正文）：

```cpp
std::string input;  // 连接读缓冲
auto step = parser.parse(input);
if (auto* parsed = step.get_if<ruvia::http1_parsed_client_response_head>()) {
    const auto& plan = parsed->plan();
    input.erase(0, step.consumed_bytes());
    if (const auto* framing = plan.framing()) {
        ruvia::http1_body_decoder body(*framing, {.obs_fold_ = ruvia::http1_trailer_obs_fold::accept,
                                                  .trailer_message_ = ruvia::http1_trailer_message::response});
        if (plan.discards_content()) {
            body.discard_content();
        }
        auto head = std::move(*parsed).take_head();
        // 按 10.3 循环 decode()；连接 EOF 时调用 finish_input()
    } else if (plan.informational()) {
        // 继续 parse() 下一响应头
    }
}
```

## 11. HTTP/2 与多路复用共享词汇

本章的头分两类。[`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)、[`"http2_frame.h"`](#api-http2-frame-h)、[`"http2_push.h"`](#api-http2-push-h) 无条件安装和编译：它们只含值类型、常量与内联函数，HTTP/2 与 HTTP/3 引擎（以及未启用 HTTP/2 的构建）共用。[`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http2_hpack.h"`](#api-http2-hpack-h) 仅在 CMake 选项 `RUVIA_ENABLE_HTTP2` 打开时安装并编译对应实现；该选项以 `PUBLIC` 编译定义 `RUVIA_ENABLE_HTTP2` 传播给 `ruvia::http` 的使用者，实现私有链接 `nghttp2`（分帧、HPACK、流状态、流量控制由 `nghttp2` 完成），`nghttp2` 类型不出现在公开头中。本章所有类型位于 `namespace ruvia`；借用、`decode_step` 与异常的跨头总则见[通用契约](#2-通用契约)。

### 11.1 `"http_multiplexed_stream.h"`
<a id="api-http-multiplexed-stream-h"></a>

`http2_connection` 与 `http3_connection` 共用的流级契约：连接角色、提交结果、两种协议共同发布的流事件形状、接收流控凭证与指纹采集预算。无条件编译；公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)（`RUVIA_LIFETIMEBOUND`）、[`"http_client_response_head.h"`](#api-http-client-response-head-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)。协议独有的枚举值只由对应协议返回。源码：[`ruvia-http/include/http_multiplexed_stream.h`](../ruvia-http/include/http_multiplexed_stream.h)

#### 11.1.1 角色与提交结果

```cpp
enum class http_connection_role : std::uint8_t {
    client,
    server,
};

enum class http_submit_status : std::uint8_t {
    accepted,
    closed,
    invalid_state,
    invalid_message,
    peer_capability_unavailable,
    peer_settings_pending,
    backpressured,
    peer_stream_limit_reached,
    local_stream_capacity_reached,
};

enum class http_finish_status : std::uint8_t {
    accepted,
    closed,
    invalid_message,
    backpressured,
};

enum class http_data_submit_status : std::uint8_t {
    complete,
    backpressured,
    expectation_pending,
    closed,
    invalid_message,
};

struct http_data_submit_result final {
    std::size_t consumed_bytes_{0};
    http_data_submit_status status_{http_data_submit_status::complete};
};

enum class http_response_head_error : std::uint8_t {
    closed,
    invalid_state,
    invalid_message,
    response_plan_mismatch,
    peer_stream_limit_reached,
};
```

`http_connection_role`：

| 值 | 含义 |
| --- | --- |
| `client` | 客户端连接：发起请求流、接收响应 |
| `server` | 服务端连接：接收请求流、提交响应 |

`http_submit_status`（首部与控制类提交的结果；未 `accepted` 的提交不发出任何帧）：

| 值 | 协议 | 含义 |
| --- | --- | --- |
| `accepted` | 共有 | 已提交，编码结果进入输出队列 |
| `closed` | 共有 | 预期内的竞态：流已复位或连接已失败；对新请求还表示连接不再接纳新流（对端 GOAWAY 或流标识耗尽） |
| `invalid_state` | 共有 | 角色或流状态无法接受该请求；不产生输出 |
| `invalid_message` | 共有 | 协议无法承载的消息或超出字段节上限；单独返回，使转发对端数据的调用方能作答 |
| `peer_capability_unavailable` | 共有 | 对端 SETTINGS 未启用该特性 |
| `peer_settings_pending` | 仅 HTTP/3 | 操作依赖尚未到达的对端 SETTINGS（RFC 9114 §7.2.4）；待 `peer_settings()` 报告后重试 |
| `backpressured` | 仅 HTTP/3 | `submit_origin_advertisement()` 发现控制流输出预算已满；确认释放后重试 |
| `peer_stream_limit_reached` | 仅 HTTP/2 | `submit_request_head()` 在对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 下没有空位（RFC 9113 §5.1.2）；请求流关闭后重试 |
| `local_stream_capacity_reached` | 仅 HTTP/2 | `submit_request_head()` 发现本端固定流容量已占满；请求流关闭后重试 |

`http_finish_status`（结束发送半边的结果）：

| 值 | 协议 | 含义 |
| --- | --- | --- |
| `accepted` | 共有 | 已结束发送半边 |
| `closed` | 共有 | 流已消失、被复位或其发送半边已放弃 |
| `invalid_message` | 共有 | 已提交 DATA 未达声明的 `Content-Length`，或 Trailer 节无法发送；状态不变，流保持打开 |
| `backpressured` | 仅 HTTP/2 | 空 Trailer 节以空 DATA 帧结束流，只在连接与流的发送窗口均为正时发送（RFC 9113 §6.9）；状态不变，窗口打开后重试 |

`http_data_submit_status` / `http_data_submit_result`：`consumed_bytes_` 是本次被复制进已序列化 DATA 的前缀长度；未消费后缀仍归调用方所有，引擎不保留任何输入视图。

| 值 | 协议 | 含义 |
| --- | --- | --- |
| `complete` | 共有 | 整个输入已被消费（不表示已写到 socket） |
| `backpressured` | 共有 | 只有 `consumed_bytes_`（可为 0）字节容纳进发送窗口或输出预算；调用方保存其余部分并重试 |
| `expectation_pending` | 仅 HTTP/2 | 请求内容在等待 `100 (Continue)`；消费 0 字节 |
| `closed` | 共有 | 流已消失或发送半边已放弃；消费 0 字节 |
| `invalid_message` | 共有 | 输入会超出声明的 `Content-Length`，或在不足该长度时结束流；什么都未消费 |

`http_response_head_error`（所有返回值携带型响应头提交的失败分支）：

| 值 | 协议 | 含义 |
| --- | --- | --- |
| `closed` | 共有 | 流已不存在或连接已失败 |
| `invalid_state` | 共有 | 已提交过最终响应头，或角色/流无法接受 |
| `invalid_message` | 共有 | 响应无法表示（连接专用字段、非最终状态码、非法 `Content-Length` 或 `Transfer-Encoding`） |
| `response_plan_mismatch` | 共有 | 写入计划不描述此响应 |
| `peer_stream_limit_reached` | 仅 HTTP/2 | 推送流在对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 下没有空位（RFC 9113 §8.4） |

两个引擎的对应入口：

| 类型 | HTTP/2 入口（[`"http2_connection.h"`](#api-http2-connection-h)） | HTTP/3 入口（[`"http3_connection.h"`](#api-http3-connection-h)） |
| --- | --- | --- |
| `http_connection_role` | `role()` | `role()` |
| [`http_request_head_view`](#api-http-request-head-view-h) | 客户端 `submit_request_head(request)`（流号由 HTTP/2 分配，返回 `std::variant<std::uint32_t, http_submit_status>`）；服务端 `submit_push_request()` | 客户端 `submit_request_head(stream_id, request)`（流号由 QUIC 分配） |
| `http_submit_status` | 请求头失败分支、临时响应、CONNECT 响应、reset、PING、SETTINGS、ORIGIN/ALTSVC、PRIORITY_UPDATE、`begin_drain()` / `finish_drain()` | 见 HTTP/3 章节 |
| `http_data_submit_result` | `submit_data(id, bytes, end_stream, max_payload_bytes)` | `submit_data(...)` |
| `http_finish_status` | `finish_request()`、`finish_response()`、`finish_stream()` | 同名方法 |
| `http_response_head_error` | 响应头、WebSocket 握手响应、推送响应提交的失败分支 | 响应头提交的失败分支 |
| 流事件模板（11.1.4） | `http2_*_event` 别名，流号 `std::uint32_t` | `http3_*_event` 别名，流号 `std::int64_t` |

#### 11.1.2 `struct http_fingerprint_options`

```cpp
struct http_fingerprint_options final {
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
};
```

HTTP/2 与 HTTP/3 指纹采集器（[`http2_akamai_fingerprint_collector`](#api-http2-connection-h) 及 HTTP/3 对应类型）的观察预算：在第一个请求字段块完成前，观察到的帧数超过 `max_frames_`，或帧与字段累计字节超过 `max_bytes_`，采集器即进入失败状态。

#### 11.1.3 `class http_received_data_credit`

```cpp
class http_received_data_credit final {
public:
    ~http_received_data_credit();
    http_received_data_credit(const http_received_data_credit&) = delete;
    http_received_data_credit& operator=(const http_received_data_credit&) = delete;
    http_received_data_credit(http_received_data_credit&& other) noexcept;
    http_received_data_credit& operator=(http_received_data_credit&&) = delete;
    void merge(http_received_data_credit&& other);
    [[nodiscard]] bool valid() const noexcept;
};
```

一个事件 DATA 载荷的接收流控凭证（RFC 9113 §6.9、RFC 9000 §4.1）。凭证是线性的，只有一条归还路径：

- 析构：销毁仍有效的凭证时，把其精确字节数无异常地归还签发连接；连接据此扩大接收窗口并按需发送连接级与流级窗口更新。消费者应持有凭证直到这些字节被真正消费，从而形成精确背压。
- 生命周期：凭证保活其归还目标，可比签发连接（门面对象）长寿；归还到已不存在的流或连接时被丢弃。凭证不能经其他连接归还。
- 移动：只可移动构造，移动后源对象无效；复制与赋值均被删除（避免重复或丢失记账）。
- `merge(other)`：合并同一连接同一流的凭证，不归还任何字节；成功时消耗 `other`。`other` 无效或属于不同连接/流时抛 `std::logic_error`，字节数溢出时抛 `std::overflow_error`。
- `valid()`：持有归还目标且字节数非零时为 `true`。

凭证的归还目标采用非原子引用计数，凭证只能在签发连接所在的工作线程上移动、合并与销毁。

#### 11.1.4 流事件模板

```cpp
template <typename stream_id_type>
class http_stream_informational_head_event final {
public:
    http_stream_informational_head_event(http_stream_informational_head_event&&) noexcept = default;
    http_stream_informational_head_event& operator=(http_stream_informational_head_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept;
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept;
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal() const noexcept;
};

template <typename stream_id_type>
class http_stream_response_head_event final {
public:
    http_stream_response_head_event(http_stream_response_head_event&&) noexcept = default;
    http_stream_response_head_event& operator=(http_stream_response_head_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept;
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept;
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal() const noexcept;
    [[nodiscard]] bool stream_ended() const noexcept;
};

enum class http_stream_data_kind : std::uint8_t {
    message,
    tunnel,
};

template <typename stream_id_type, http_stream_data_kind data_kind>
class http_stream_data_event final {
public:
    static constexpr http_stream_data_kind kind = data_kind;
    http_stream_data_event(const http_stream_data_event&) = delete;
    http_stream_data_event& operator=(const http_stream_data_event&) = delete;
    http_stream_data_event(http_stream_data_event&& other) noexcept;
    http_stream_data_event& operator=(http_stream_data_event&& other) noexcept;
    [[nodiscard]] stream_id_type stream_id() const noexcept;
    [[nodiscard]] std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view bytes() const&& = delete;
    [[nodiscard]] http_received_data_credit take_credit() & noexcept;
};

enum class http_message_content_semantics : std::uint8_t {
    content,
    metadata_only,
};

template <typename stream_id_type>
class http_stream_message_end_event final {
public:
    http_stream_message_end_event(http_stream_message_end_event&&) noexcept = default;
    http_stream_message_end_event& operator=(http_stream_message_end_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept;
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::vector<http_header> take_trailers() && noexcept;
    [[nodiscard]] http_message_content_semantics content_semantics() const noexcept;
};

enum class http_stream_close_source : std::uint8_t {
    local,
    peer,
    peer_goaway,
};

template <typename stream_id_type, typename error_code_type>
class http_stream_closed_event final {
public:
    [[nodiscard]] constexpr stream_id_type stream_id() const noexcept;
    [[nodiscard]] constexpr std::optional<error_code_type> error() const noexcept;
    [[nodiscard]] constexpr std::optional<http_stream_close_source> source() const noexcept;
};

template <typename stream_id_type>
class http_stream_request_unprocessed_event final {
public:
    [[nodiscard]] constexpr stream_id_type stream_id() const noexcept;
};
```

这些事件只由流引擎构造（构造函数不公开）；每个协议头以别名给出实例化（如 `http2_data_event`），流标识保留协议自身的类型（RFC 9113 §5.1.1、RFC 9000 §2.1）。所有 `const&&` 访问器被删除：拒绝从右值事件取借用视图，避免视图悬垂于临时事件。

| 模板 | 成员与语义 |
| --- | --- |
| `http_stream_informational_head_event` | 客户端：一个 1xx 响应头。`head()` 借用事件；`take_head() &&` 移出 [`http_client_response_head`](#api-http-client-response-head-h)。`request_content_signal()` 在以 `Expect: 100-continue` 提交的请求收到首个 `100 (Continue)` 时为 `continue_`，否则为空 |
| `http_stream_response_head_event` | 客户端：最终响应头。字段为已校验的常规字段；头内保留原始三位状态码（`received_status()`），`status()` 按 RFC 9110 §15 处理（600..999 视作 500）。`request_content_signal()` 在请求内容仍等待 `100 (Continue)` 时到达最终头则为 `exchange_complete`。`stream_ended()`：HEADERS 帧本身结束了流，之后没有内容或隧道 DATA；该值在发布时固定，之后结束流的帧不会设置它 |
| `http_stream_data_event` | 一条流的 DATA 载荷。`kind` 是类型的一部分，事件 variant 因而可区分正文与隧道。`bytes()` 默认借用产生该事件的引擎输入（即 `feed()` 的输入存储），除非引擎被要求把字节复制进事件（`http3_connection_options::own_data_events_`）。凭证随事件保存，直到 `take_credit() &` 交给消费者；移动赋值会归还目标对象中未取走的凭证 |
| `http_stream_message_end_event` | 对端正常结束发送半边：请求或响应内容完整（HTTP/3 中也可是隧道接收方向结束）。`trailers()` 是结束前的 Trailer 节，无则为空，字段名小写（RFC 9113 §8.2.2、RFC 9114 §4.2）。客户端收到的是已校验字段，且 RFC 9110 §6.5.1 禁止出现在响应 Trailer 中的字段已被丢弃；服务端应交给 `http_request_trailers::append_received()`（见 [`"http_request.h"`](#api-http-request-h)）。span 借用事件，事件移动、销毁或 `take_trailers()` 移出后失效。`content_semantics()` 见下表 |
| `http_stream_closed_event` | 流以非“消息交换完成”的方式结束。`error()` 是结束它的复位所带协议错误码；`source()` 是放弃该流的端点（引擎知道时）。HTTP/2 只为被放弃的流发布此事件，且两值恒有（错误码可为 `NO_ERROR`）；HTTP/3 为 QUIC 回收的每条流在两个方向均关闭后发布，正常关闭时 `error()` 为空，`source()` 恒空（由前一个传输动作事件的 `origin()` 给出端点）。此后不再有事件指名该流；已报告为 request-unprocessed 的流不发布关闭 |
| `http_stream_request_unprocessed_event` | 仅客户端：本端打开的请求流未被服务端处理，可重试（RFC 9113 §8.7，RFC 9114 §4.1.1 与 §5.2）。在 GOAWAY 事件之后，为该 GOAWAY 排除的每条打开请求流发布；也为服务端在最终响应头之前拒绝的流发布（`RST_STREAM(REFUSED_STREAM)`、`H3_REQUEST_REJECTED`），后者连接仍可用。此后不再有消息事件或关闭事件指名该流 |

| 枚举值 | 含义 |
| --- | --- |
| `http_stream_data_kind::message` | 请求或响应消息内容；HTTP/3 的 CONNECT 隧道字节也用此种类 |
| `http_stream_data_kind::tunnel` | 仅 HTTP/2：CONNECT 隧道字节，包括服务端接受或拒绝隧道之前收到的字节 |
| `http_message_content_semantics::content` | 完成的消息携带内容语义 |
| `http_message_content_semantics::metadata_only` | 响应的 `Content-Length` 描述了它并不携带的表示（HEAD 响应、204、304），不应用内容解码 |
| `http_stream_close_source::local` | 本端：引擎检测到的流错误 |
| `http_stream_close_source::peer` | 对端复位了该流 |
| `http_stream_close_source::peer_goaway` | 仅 HTTP/2：对端 GOAWAY 排除了该流（RFC 9113 §6.8） |

#### 11.1.5 排空状态词汇

两个引擎的同名排空查询具有以下确切含义：

| 查询 | 含义 |
| --- | --- |
| `draining()` | 本端开始了排空（`begin_drain()` / `finish_drain()`），或对端发送了 GOAWAY |
| `drain_final()` | 最终本地 GOAWAY 已排队。HTTP/2 在收到合法对端 GOAWAY 时也进入；HTTP/3 只经由自身的 `finish_drain()` 进入 |
| `drained()` | `drain_final()` 且其接纳的流已全部结束。HTTP/2 计入所有被跟踪的流（含未释放的所有者租约）；HTTP/3 计入其最终 GOAWAY 以下的客户端发起请求流。因此 HTTP/2 在对端 GOAWAY 后可变为真，HTTP/3 仅收到对端 GOAWAY 时恒为 `false` |

### 11.2 `"http2_frame.h"`
<a id="api-http2-frame-h"></a>

HTTP/2 帧层词汇：连接前言、帧头常量与编解码、帧类型/错误码/设置标识枚举，以及明文监听器上的前言探测。无条件编译，全部为 `constexpr` / `inline` 定义，不依赖 `nghttp2`，也不依赖本 target 其他头。源码：[`ruvia-http/include/http2_frame.h`](../ruvia-http/include/http2_frame.h)

#### 11.2.1 常量与枚举

```cpp
inline constexpr std::string_view http2_client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t http2_frame_header_bytes = 9;
inline constexpr std::uint32_t http2_default_max_frame_size = 16 * 1024;
inline constexpr std::uint32_t http2_max_frame_size = 16 * 1024 * 1024 - 1;

enum class http2_frame_type : std::uint8_t {
    data = 0x0, headers = 0x1, priority = 0x2, rst_stream = 0x3, settings = 0x4,
    push_promise = 0x5, ping = 0x6, goaway = 0x7, window_update = 0x8,
    continuation = 0x9, alternative_service = 0xa, origin = 0xc, priority_update = 0x10
};
enum class http2_error_code : std::uint32_t {
    no_error = 0x0, protocol_error = 0x1, internal_error = 0x2, flow_control_error = 0x3,
    settings_timeout = 0x4, stream_closed = 0x5, frame_size_error = 0x6, refused_stream = 0x7,
    cancel = 0x8, compression_error = 0x9, connect_error = 0xa, enhance_your_calm = 0xb,
    inadequate_security = 0xc, http11_required = 0xd
};
enum class http2_setting_id : std::uint16_t {
    header_table_size = 0x1, enable_push = 0x2, max_concurrent_streams = 0x3,
    initial_window_size = 0x4, max_frame_size = 0x5, max_header_list_size = 0x6,
    enable_connect_protocol = 0x8, no_rfc7540_priorities = 0x9
};
```

| 常量 | 含义 |
| --- | --- |
| `http2_client_preface` | 客户端连接前言（RFC 9113 §3.4），24 字节 |
| `http2_frame_header_bytes` | 帧头长度 9 字节（RFC 9113 §4.1） |
| `http2_default_max_frame_size` | `SETTINGS_MAX_FRAME_SIZE` 初始值 16384 |
| `http2_max_frame_size` | 帧载荷长度上限 2^24-1 |

`http2_frame_type`（线上帧类型码）：

| 值 | 码 | 含义 |
| --- | --- | --- |
| `data` | 0x0 | DATA（RFC 9113 §6.1） |
| `headers` | 0x1 | HEADERS（§6.2） |
| `priority` | 0x2 | PRIORITY（§6.3，已弃用） |
| `rst_stream` | 0x3 | RST_STREAM（§6.4） |
| `settings` | 0x4 | SETTINGS（§6.5） |
| `push_promise` | 0x5 | PUSH_PROMISE（§6.6） |
| `ping` | 0x6 | PING（§6.7） |
| `goaway` | 0x7 | GOAWAY（§6.8） |
| `window_update` | 0x8 | WINDOW_UPDATE（§6.9） |
| `continuation` | 0x9 | CONTINUATION（§6.10） |
| `alternative_service` | 0xa | ALTSVC（RFC 7838 §4） |
| `origin` | 0xc | ORIGIN（RFC 8336） |
| `priority_update` | 0x10 | PRIORITY_UPDATE（RFC 9218 §7.1） |

`http2_error_code`（RFC 9113 §7 错误码）：

| 值 | 码 | 含义 |
| --- | --- | --- |
| `no_error` | 0x0 | 无错误（正常关闭、优雅 GOAWAY） |
| `protocol_error` | 0x1 | 未指明的协议错误 |
| `internal_error` | 0x2 | 实现内部错误 |
| `flow_control_error` | 0x3 | 违反流量控制 |
| `settings_timeout` | 0x4 | SETTINGS 未及时确认 |
| `stream_closed` | 0x5 | 流半关闭后收到帧 |
| `frame_size_error` | 0x6 | 帧大小非法 |
| `refused_stream` | 0x7 | 流在处理前被拒绝，可重试 |
| `cancel` | 0x8 | 流不再需要 |
| `compression_error` | 0x9 | 无法维持字段压缩上下文 |
| `connect_error` | 0xa | CONNECT 建立的连接被复位或异常关闭 |
| `enhance_your_calm` | 0xb | 对端行为可能产生过量负载 |
| `inadequate_security` | 0xc | 传输层安全属性不满足最低要求 |
| `http11_required` | 0xd | 要求改用 HTTP/1.1 |

`http2_setting_id`（SETTINGS 参数标识）：

| 值 | 码 | 含义 |
| --- | --- | --- |
| `header_table_size` | 0x1 | HPACK 动态表上限（RFC 9113 §6.5.2） |
| `enable_push` | 0x2 | 是否允许服务端推送 |
| `max_concurrent_streams` | 0x3 | 最大并发流数 |
| `initial_window_size` | 0x4 | 流级初始发送窗口 |
| `max_frame_size` | 0x5 | 最大帧载荷 |
| `max_header_list_size` | 0x6 | 字段列表大小建议上限 |
| `enable_connect_protocol` | 0x8 | Extended CONNECT（RFC 8441 §3） |
| `no_rfc7540_priorities` | 0x9 | 停用 RFC 7540 优先级（RFC 9218 §2.1） |

#### 11.2.2 帧头编解码

```cpp
struct http2_frame_header final {
    std::uint32_t length_{0};
    std::uint8_t type_{0};
    std::uint8_t flags_{0};
    std::uint32_t stream_id_{0};
};

[[nodiscard]] inline constexpr std::optional<http2_frame_header> parse_http2_frame_header(std::span<const char> bytes_value) noexcept;
[[nodiscard]] inline constexpr bool encode_http2_frame_header(std::span<char> output, std::uint32_t length, http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id) noexcept;
```

- `http2_frame_header`：解码后的 9 字节帧头。`type_` 保留原始类型字节（可为未知扩展类型），不强制转换为 `http2_frame_type`。
- `parse_http2_frame_header()`：读取 `bytes_value` 的前 9 字节（RFC 9113 §4.1），不足 9 字节返回 `std::nullopt`；流标识的保留位被忽略。只解析，不校验长度上限或类型语义，不消费输入。
- `encode_http2_frame_header()`：把帧头写入 `output` 的前 9 字节并返回 `true`；`output` 不足 9 字节、`length > http2_max_frame_size` 或 `stream_id > 0x7fffffff` 时返回 `false` 且不写入。

#### 11.2.3 明文前言探测

```cpp
enum class http2_cleartext_preface_probe : std::uint8_t {
    http1,
    need_input,
    complete_preface,
    drop_connection,
};

[[nodiscard]] inline http2_cleartext_preface_probe probe_http2_cleartext_preface(std::string_view current) noexcept;
```

在明文监听器上、HTTP/1 解析之前对已到达字节分类（prior-knowledge h2c）：

| 值 | 条件与含义 |
| --- | --- |
| `http1` | 输入为空，或既不是前言前缀也不以 `"PRI "` 开头：按 HTTP/1 处理 |
| `need_input` | 输入是 `http2_client_preface` 的真前缀：需要更多字节 |
| `complete_preface` | 输入以完整前言开头：按 HTTP/2 处理 |
| `drop_connection` | 以 `"PRI "` 开头但不可能成为前言：应丢弃连接 |

不消费输入；调用方在 `complete_preface` 后把包括前言在内的全部字节交给 HTTP/2 引擎。

### 11.3 `"http2_push.h"`
<a id="api-http2-push-h"></a>

服务端推送（RFC 9113 §8.4）的共享值类型：客户端收到的 PUSH_PROMISE 事件与服务端推送提交的错误码。无条件编译；公开 include [`"http_header.h"`](#api-http-header-h)。源码：[`ruvia-http/include/http2_push.h`](../ruvia-http/include/http2_push.h)

#### 11.3.1 类型

```cpp
struct http_push_request final {
    http_push_request() noexcept = default;
    http_push_request(const http_push_request&) = delete;
    http_push_request& operator=(const http_push_request&) = delete;
    http_push_request(http_push_request&&) noexcept = default;
    http_push_request& operator=(http_push_request&&) = delete;
    std::string method_;
    std::string scheme_;
    std::string authority_;
    std::string target_;
    std::vector<http_header> headers_;
};

struct http2_push_promise_event final {
    std::uint32_t associated_stream_id_;
    std::uint32_t promised_stream_id_;
    http_push_request request_;
};

enum class http2_push_submit_error : std::uint8_t {
    invalid_state,
    push_disabled,
    stream_limit,
    invalid_request,
};
```

- `http_push_request`：脱离 `feed()` 输入、自行拥有解码字节的承诺请求元数据（`:method`、`:scheme`、`:authority`、`:path` 与常规字段）；只可移动构造。服务端提交的承诺请求则以 [`http_request_head_view`](#api-http-request-head-view-h) 描述。接受承诺不建立传输权威：驱动在消费推送响应前，必须用其已认证连接校验 URI 源站。
- `http2_push_promise_event`：客户端收到的 PUSH_PROMISE，经 `http2_event::push_promise()` 取得；`associated_stream_id_` 为关联请求流，`promised_stream_id_` 为承诺流，其响应随后以普通响应事件在承诺流上发布。两个流号字段没有默认初始化值。
- `http2_push_submit_error`（`http2_connection::submit_push_request()` 的失败分支）：

| 值 | 含义 |
| --- | --- |
| `invalid_state` | 无法在此连接或关联流上推送：非服务端、尚未收到对端 SETTINGS、连接已不处于打开状态（如已失败或终止）、已收到对端 GOAWAY，或关联流不存在/已放弃 |
| `push_disabled` | 对端（客户端）`SETTINGS_ENABLE_PUSH` 为 0 |
| `stream_limit` | 已打开的推送流达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS`（含为 0），或本端无法再分配承诺流；不发送 PUSH_PROMISE |
| `invalid_request` | 承诺请求不是带 authority、无内容的常规 GET 或 HEAD |

### 11.4 `"http2_connection.h"`
<a id="api-http2-connection-h"></a>

符合 RFC 9113 / RFC 7541 / RFC 8441 的 Sans-I/O HTTP/2 客户端与服务端连接状态机，以及 Akamai HTTP/2 指纹采集器。仅 `RUVIA_ENABLE_HTTP2`。每个连接持有一个 `nghttp2` 会话：分帧、HPACK、流状态、流量控制及 SETTINGS/PING/GOAWAY 处理由 `nghttp2` 完成；本头负责 HTTP 消息语义（字段校验、内容长度、CONNECT/隧道阶段）、请求租约、流控凭证与防滥用预算。公开 include [`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http2_frame.h"`](#api-http2-frame-h)、[`"http2_push.h"`](#api-http2-push-h)、[`"http_client_response_head.h"`](#api-http-client-response-head-h)、[`"http_connection_advertisement.h"`](#api-http-connection-advertisement-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)、[`"http_priority.h"`](#api-http-priority-h)、[`"http_request.h"`](#api-http-request-h)、[`"http_request_expectation.h"`](#api-http-request-expectation-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)、[`"http_response.h"`](#api-http-response-h)、[`"http_response_stream.h"`](#api-http-response-stream-h)、[`"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)、[`"websocket_handshake.h"`](#api-websocket-handshake-h)。源码：[`ruvia-http/include/http2_connection.h`](../ruvia-http/include/http2_connection.h)

#### 11.4.1 状态、设置与输出类型

```cpp
enum class http2_stream_receive_status : std::uint8_t {
    open,
    ended,
    closed,
};

enum class http2_request_pseudo_header : std::uint8_t {
    method,
    scheme,
    authority,
    path,
    protocol,
};

struct http2_server_request_view final {
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view path_{};
    std::string_view authority_{};
    std::string_view protocol_{};
    std::span<const http2_request_pseudo_header> pseudo_header_order_{};
};

struct http2_setting_entry final {
    http2_setting_id id_{http2_setting_id::header_table_size};
    std::uint32_t value_{0};
};

class http2_received_settings final {
public:
    static constexpr std::size_t max_frame_entries = 32;
    static constexpr std::size_t max_entries = max_frame_entries + 1;
    http2_received_settings() = default;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] http2_setting_entry at(std::size_t index) const;
};

struct http2_received_window_update final {
    std::uint32_t stream_id_{0};
    std::uint32_t increment_{0};
};

struct http2_received_priority final {
    std::uint32_t stream_id_{0};
    std::uint32_t stream_dependency_{0};
    std::uint16_t weight_{0};
    bool exclusive_{false};
};

struct http2_peer_settings final {
    bool enable_connect_protocol_{false};
    bool enable_push_{true};
    bool no_rfc7540_priorities_{false};
    std::uint32_t max_concurrent_streams_{0xffffffffU};
    std::uint32_t max_frame_size_{16384};
};

struct http2_send_window_state final {
    std::int32_t connection_window_{0};
    std::int32_t stream_window_{0};
    std::uint32_t available_{0};
};

enum class http2_feed_status : std::uint8_t {
    events_pending,
    accepted,
    protocol_failure,
};

enum class http2_end_stream : std::uint8_t {
    keep_open,
    end_stream,
};

[[nodiscard]] constexpr bool http2_ends_stream(http2_end_stream value) noexcept;

enum class http2_output_batch_status : std::uint8_t { taken, empty };

struct http2_output_batch_result final {
    http2_output_batch_status status_{http2_output_batch_status::empty};
    std::size_t bytes_{0};
};
using http2_data_output_observer_type = void (*)(void*, std::uint32_t, std::size_t) noexcept;

enum class http2_request_content_release_status : std::uint8_t {
    released,
    not_pending,
    closed,
};

enum class http2_server_request_release_status : std::uint8_t { released, closed };
```

`http2_stream_receive_status`（`stream_receive_status()` 的只读接收侧状态，不暴露流存储）：

| 值 | 含义 |
| --- | --- |
| `open` | 接收半边仍打开 |
| `ended` | 对端已以 END_STREAM 正常结束发送 |
| `closed` | 流已关闭、复位或不存在 |

`http2_request_pseudo_header`（请求伪首部，用于记录线上顺序）：

| 值 | 含义 |
| --- | --- |
| `method` | `:method` |
| `scheme` | `:scheme` |
| `authority` | `:authority` |
| `path` | `:path` |
| `protocol` | `:protocol`（RFC 8441） |

- `http2_server_request_view`：对端请求伪首部的借用视图，经 `http2_request_head_event::request_view()` 取得，存储由该请求租约持有；连接对象被移动不会使已有租约的视图失效。`pseudo_header_order_` 只包含线上实际出现的伪首部，按解码顺序（RFC 9113 §8.3）；不补入由 `Host` 推导的 authority；本地合成的推送请求为空。
- `http2_setting_entry`：一项 SETTINGS 参数（标识与值）。
- `http2_received_settings`：`received_settings` 事件的载荷，即一个已应用的非 ACK SETTINGS 帧中经 `nghttp2` 解码的条目。重复的已知 ID 合并为一项（取最后的值，位置为首次出现处），未知 ID 按线上顺序保留；同一帧先降低后升高 `HEADER_TABLE_SIZE` 时，末尾另附一项携带其中的最小值（RFC 7541 §4.2 要求编码器先确认该最小值）。超过 `max_frame_entries`（32）项的 SETTINGS 帧本身以 `GOAWAY(ENHANCE_YOUR_CALM)` 结束连接，因此事件以 `max_entries`（33）项定长数组内联保存，不分配内存，也不借用 `feed()` 输入。`size()` 返回条目数；`at(index)` 越界抛 `std::out_of_range`。
- `http2_received_window_update`：`received_window_update` 事件载荷，连接实际应用的 WINDOW_UPDATE（stream 0 或存活流）。
- `http2_received_priority`：带 PRIORITY 标志的 HEADERS 帧中 RFC 9113 §5.3.2 的原始依赖、权重（1..256）与独占位，仅为元数据，不代表调度策略。在 `http2_frame_metadata::priority_` 中也用于独立 PRIORITY 帧。
- `http2_peer_settings`：对端最近一个 SETTINGS 帧之后生效、约束本端提交的值（RFC 9113 §6.5.2）。`enable_connect_protocol_` 对应 RFC 8441 §3；`enable_push_` 只有客户端发送的值约束服务端；`no_rfc7540_priorities_` 对应 RFC 9218 §2.1；默认值为协议初始值。
- `http2_send_window_state`：存活流的发送流控只读观察；`available_` 为连接窗口与流窗口的较小者，下限截断为 0。
- `http2_feed_status`（`feed()` 对每段输入采取全有或全无的所有权）：

| 值 | 含义 |
| --- | --- |
| `events_pending` | 事件队列尚未取空；本段输入一个字节也未消费，调用方先 `next_event()` 取空后重新提交同一段 |
| `accepted` | 整段输入已消费；不完整的尾帧由 `nghttp2` 自行缓存，所以也覆盖部分帧输入，不存在“需要更多输入”结果 |
| `protocol_failure` | 连接处于致命错误：调用前已失败（此时直接返回），或本段输入导致失败；`connection_error()` 报告错误码 |

- `http2_end_stream`：`keep_open` 发送后保持流打开，`end_stream` 在本次输入全部消费后发送 END_STREAM；`http2_ends_stream(value)` 在值为 `end_stream` 时返回 `true`。
- `http2_output_batch_status` / `http2_output_batch_result`：`taken` 表示本次向 `output` 追加了 `bytes_` 字节，`empty` 表示无待写输出（`bytes_` 为 0）。
- `http2_data_output_observer_type`：`take_output_batch()` 的 DATA 观察回调 `void(void* context, std::uint32_t stream_id, std::size_t payload_bytes) noexcept`，对本批取出的每段 DATA 载荷报告流号与字节数，用于 socket 写完成记账。
- `http2_request_content_release_status`（`release_request_content()`）：`released` 已放行等待 `100 (Continue)` 的请求内容；`not_pending` 该流没有处于等待的请求内容；`closed` 流不存在。
- `http2_server_request_release_status`（`release()`）：`released` 已释放仍存活流的租约；`closed` 流已关闭，租约仅被消耗。

#### 11.4.2 帧与字段观察

```cpp
struct http2_frame_metadata final {
    std::uint64_t observation_sequence_{0};
    std::uint32_t payload_length_{0};
    std::uint32_t stream_id_{0};
    http2_frame_type type_{http2_frame_type::data};
    std::uint8_t flags_{0};
    std::array<http2_setting_entry, http2_received_settings::max_frame_entries> settings_{};
    std::size_t settings_count_{0};
    bool settings_complete_{true};
    std::optional<std::uint32_t> window_increment_{};
    std::optional<http2_received_priority> priority_{};

    [[nodiscard]] std::span<const http2_setting_entry> settings() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http2_setting_entry> settings() const&& = delete;
};

enum class http2_header_observation_phase : std::uint8_t { begin, field, end };

struct http2_header_metadata final {
    std::uint64_t first_frame_observation_sequence_{0};
    std::uint64_t frame_observation_sequence_{0};
    std::uint32_t stream_id_{0};
    std::size_t field_index_{0};
    http2_header_observation_phase phase_{http2_header_observation_phase::begin};
    std::string_view name_{};
    std::string_view value_{};
};

using http2_frame_observer_type = void (*)(void*, const http2_frame_metadata&) noexcept;
using http2_header_observer_type = void (*)(void*, const http2_header_metadata&) noexcept;
```

观察器提供同步、只读、无分配的协议解析事实，独立于 `receive_frame_metadata_` 的已应用事件，包含规范化事件刻意省略的原始事实。由 `http2_connection_options` 的 `frame_observer_`、`header_observer_`、`observer_context_` 配置（11.4.4）。

| 记录 / 字段 | 可观察事实 |
| --- | --- |
| `http2_frame_metadata` | 一个完整到达的帧，在其最后一段载荷被处理之前交付 |
| `observation_sequence_` | 从前言之后的 0 开始，按线上顺序计数 `nghttp2` 暴露的帧头（含 ACK、未知扩展、PRIORITY、CONTINUATION）；在 `nghttp2` begin-frame 回调前就被丢弃的帧（如已复位流上的 DATA）不暴露、不计数；不计传输读取与前言，因而不是绝对线上帧号 |
| `payload_length_` / `stream_id_` / `type_` | 载荷长度、流号与帧类型 |
| `flags_` | `nghttp2` 暴露的协议 flags；未定义/保留位可能已被解析器剔除，不是线上字节的二次解释 |
| `settings_` / `settings_count_` / `settings()` | 原始 SETTINGS 条目，按线上顺序，含未知与重复 ID；ACK SETTINGS 无条目。`settings()` 借用本记录的内联数组，右值记录上被删除 |
| `settings_complete_` | 固定观察预算（32 项）无法容纳载荷，或载荷长度不是 6 的倍数时为 `false`；超大 SETTINGS 仍由协议核心拒绝 |
| `window_increment_` | WINDOW_UPDATE 增量 |
| `priority_` | 带 PRIORITY 的 HEADERS 或独立 PRIORITY 帧的依赖/独占/权重；不是调度策略 |
| `http2_header_metadata` | HPACK 解码器事实，不是重建的请求字段 |
| `first_frame_observation_sequence_` / `frame_observation_sequence_` | 字段节首帧与当前帧的观察序号；HEADERS/PUSH_PROMISE 开始字段节，CONTINUATION 不另起字段节 |
| `stream_id_` / `field_index_` | 流号与字段下标；保留伪首部与普通字段的实际输出次序 |
| `phase_` / `name_` / `value_` | 阶段；`name_`/`value_` 仅在 `field` 阶段非空，借用解码器存储，只在本次回调内有效 |

`http2_header_observation_phase`：

| 值 | 含义 |
| --- | --- |
| `begin` | 字段节开始，先于 HTTP 字段校验交付 |
| `field` | HPACK 输出一个字段之后、HTTP 校验之前交付，导致拒绝的字段也会交付 |
| `end` | HPACK 完成该字段节（不表示 HTTP 语义已接受）；被拒绝的字段节可能没有 `end`，`nghttp2` 也可能抑制其余字段回调 |

- 观察不表示协议接受：协议校验与强制拒绝照常在其后运行。
- 回调只在 `feed()` 内同步调用，从不在 `next_event()` 中调用；回调返回前不得修改、重入、移动或销毁该连接。
- `observer_context_` 被借用至连接销毁。观察器为 `nullptr` 时不保留输入，没有额外分配或分帧工作；开启时复用同一 `nghttp2` 分帧与 HPACK 主链，不另建解析器。

#### 11.4.3 事件类型

```cpp
using http2_informational_head_event = http_stream_informational_head_event<std::uint32_t>;
using http2_response_head_event = http_stream_response_head_event<std::uint32_t>;
using http2_data_event = http_stream_data_event<std::uint32_t, http_stream_data_kind::message>;
using http2_tunnel_data_event = http_stream_data_event<std::uint32_t, http_stream_data_kind::tunnel>;
using http2_message_end_event = http_stream_message_end_event<std::uint32_t>;
using http2_stream_closed_event = http_stream_closed_event<std::uint32_t, http2_error_code>;
using http2_request_unprocessed_event = http_stream_request_unprocessed_event<std::uint32_t>;

struct http2_server_request_snapshot final {
    http_request_content_indication content_{http_request_content_indication::no_content};
    std::optional<std::size_t> declared_content_length_{};
    std::optional<http_priority_fields> priority_update_{};
    bool body_open_{false};
    bool connect_pending_{false};
    bool initial_end_stream_{false};
};

class http2_request_head_event final {
public:
    ~http2_request_head_event();
    http2_request_head_event(const http2_request_head_event&) = delete;
    http2_request_head_event& operator=(const http2_request_head_event&) = delete;
    http2_request_head_event(http2_request_head_event&& other) noexcept;
    http2_request_head_event& operator=(http2_request_head_event&&) = delete;
    [[nodiscard]] std::uint32_t stream_id() const noexcept;
    [[nodiscard]] const http2_server_request_snapshot& snapshot() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_server_request_snapshot& snapshot() const&& = delete;
    [[nodiscard]] http2_server_request_view request_view() const& noexcept RUVIA_LIFETIMEBOUND;
    http2_server_request_view request_view() const&& = delete;
    [[nodiscard]] http_request build_request(borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(borrowed_text body = {}) const&& = delete;
    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy policy) const noexcept;
};

class http2_tunnel_end_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept;
};

class http2_goaway_event final {
public:
    [[nodiscard]] std::uint32_t last_stream_id() const noexcept;
    [[nodiscard]] http2_error_code error() const noexcept;
};

class http2_event final {
public:
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;

    [[nodiscard]] http2_informational_head_event* informational_head() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http2_informational_head_event* informational_head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_informational_head_event* informational_head() const&& = delete;
    [[nodiscard]] http2_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http2_request_head_event* request_head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_request_head_event* request_head() const&& = delete;
    [[nodiscard]] const http2_response_head_event* response_head() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http2_response_head_event* response_head() & noexcept RUVIA_LIFETIMEBOUND;
    const http2_response_head_event* response_head() const&& = delete;
    [[nodiscard]] http2_data_event* data() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http2_data_event* data() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_data_event* data() const&& = delete;
    [[nodiscard]] const http2_message_end_event* message_end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http2_message_end_event* message_end() & noexcept RUVIA_LIFETIMEBOUND;
    const http2_message_end_event* message_end() const&& = delete;
    [[nodiscard]] http2_tunnel_data_event* tunnel_data() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http2_tunnel_data_event* tunnel_data() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_tunnel_data_event* tunnel_data() const&& = delete;
    [[nodiscard]] const http2_tunnel_end_event* tunnel_end() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_tunnel_end_event* tunnel_end() const&& = delete;
    [[nodiscard]] const http2_stream_closed_event* stream_closed() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_stream_closed_event* stream_closed() const&& = delete;
    [[nodiscard]] const http2_request_unprocessed_event* request_unprocessed() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_request_unprocessed_event* request_unprocessed() const&& = delete;
    [[nodiscard]] const http2_goaway_event* goaway() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_goaway_event* goaway() const&& = delete;
    [[nodiscard]] http_origin_advertisement* origin_advertisement() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_origin_advertisement* origin_advertisement() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_origin_advertisement* origin_advertisement() const&& = delete;
    [[nodiscard]] http_alternative_service_advertisement* alternative_service_advertisement() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service_advertisement() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_alternative_service_advertisement* alternative_service_advertisement() const&& = delete;
    [[nodiscard]] const http_priority_update* priority_update() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_priority_update* priority_update() const&& = delete;
    [[nodiscard]] const http2_received_settings* received_settings() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_received_settings* received_settings() const&& = delete;
    [[nodiscard]] const http2_received_window_update* received_window_update() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_received_window_update* received_window_update() const&& = delete;
    [[nodiscard]] const http2_received_priority* received_priority() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_received_priority* received_priority() const&& = delete;
    [[nodiscard]] const http2_push_promise_event* push_promise() const& noexcept RUVIA_LIFETIMEBOUND;
    const http2_push_promise_event* push_promise() const&& = delete;
};
```

共享事件别名的语义见 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)；隧道 DATA 与消息 DATA 是同一形状、不同类型。HTTP/2 专有事件：

- `http2_server_request_snapshot`：服务端请求头的接收状态快照，在取出事件时生成。`content_` 反映此时核心的接收状态，同一输入批次中已到达全部内容时读作 `no_content`（RFC 9110 §10.1.1 据此省略无意义的 `100 Continue`）；`declared_content_length_` 为允许的已知 `Content-Length`；`priority_update_` 为此时该流最近一次收到的合法 PRIORITY_UPDATE 字段（含流打开前针对空闲流收到的值；格式错误的 PRIORITY_UPDATE 不生效）；`body_open_` 在发布时固定：HEADERS 帧使对端发送半边保持打开、其后会有消息内容事件（无论核心是否已解析它们）；`connect_pending_` 表示 CONNECT 或 Extended CONNECT 仍在等待本端接受或拒绝；`initial_end_stream_` 保存初始 HEADERS 的 END_STREAM 位（包含 CONNECT），不随后续 DATA、Trailer 或复位改变。
- `http2_request_head_event`：服务端请求头事件，同时是 `request_view()`、`build_request()` 所有视图的流存储租约；只可移动构造。处理完毕后用 `http2_connection::release(std::move(event))` 一次性消耗事件并使这些视图失效；未释放直接销毁事件同样归还租约（释放失败时由后续调用重试）。
  - `stream_id()`、`snapshot()`：流号与快照。
  - `request_view()`：伪首部借用视图（[`http2_server_request_view`](#api-http2-connection-h)）。
  - `build_request(body)`：构造语义请求 [`http_request`](#api-http-request-h)；首部字段借用本租约，`body` 借用调用方存储（[`borrowed_text`](#api-borrowed-text-h) 在编译期拒绝临时拥有型字符串），二者都必须覆盖返回请求的使用期。
  - `expectation_plan(policy)`：按 [`"http_request_expectation.h"`](#api-http-request-expectation-h) 的 `server_plan()` 规则，结合请求内容指示与对未知期望的策略，给出 `100-continue` / `417` / 忽略计划。
  - 所有 `const&&` 重载被删除：拒绝从右值租约取视图。
- `http2_tunnel_end_event`：服务端已建立隧道的接收方向结束（对端 END_STREAM）；在 2xx 接受之后、所有先到达的 `tunnel_data` 之后发布。
- `http2_goaway_event`：对端 GOAWAY（RFC 9113 §6.8）：`last_stream_id()` 为其已处理或可能仍处理的最后流号，`error()` 为错误码；编号更高的请求流随后以 request-unprocessed 事件报告。
- `http2_event`：`next_event()` 产出的单一事件，只可移动构造。每个访问器在事件为该种类时返回指向内部值的指针，否则返回 `nullptr`；指针借用事件对象。部分访问器有非 const `&` 重载以便移出或取凭证（如 `data()->take_credit()`、`std::move(*ev->response_head()).take_head()`、`std::move(*ev->request_head())` 交给 `release()`）。`const&&` 重载均被删除。

| 访问器 | 事件 |
| --- | --- |
| `informational_head()` | 客户端 1xx 响应头 |
| `request_head()` | 服务端请求头与租约（含服务端推送请求以外的全部入站请求） |
| `response_head()` | 客户端最终响应头（含推送流的响应） |
| `data()` | 消息 DATA（带接收凭证） |
| `message_end()` | 消息正常结束与 Trailer |
| `tunnel_data()` | 隧道 DATA（带接收凭证） |
| `tunnel_end()` | 隧道接收方向结束 |
| `stream_closed()` | 流被放弃（错误码与来源恒有值） |
| `request_unprocessed()` | 客户端请求未被处理，可重试 |
| `goaway()` | 对端 GOAWAY |
| `origin_advertisement()` | 客户端收到 ORIGIN 帧（需 `receive_origin_advertisements_`），类型见 [`"http_connection_advertisement.h"`](#api-http-connection-advertisement-h) |
| `alternative_service_advertisement()` | 收到 ALTSVC 帧 |
| `priority_update()` | 服务端收到 PRIORITY_UPDATE（[`http_priority_update`](#api-http-priority-h)） |
| `received_settings()` | 已应用的非 ACK SETTINGS（需 `receive_frame_metadata_`；ACK 不产生） |
| `received_window_update()` | 已应用的 WINDOW_UPDATE（需 `receive_frame_metadata_`）；溢出的增量或关闭/未知流上的增量不产生；零增量在任何流上都是 `PROTOCOL_ERROR` 连接错误 |
| `received_priority()` | 被接受的 HEADERS 帧的优先级字段（需 `receive_frame_metadata_`），排在该帧产生的全部事件（如请求头）之前；PRIORITY 帧不产生 |
| `push_promise()` | 客户端收到的 PUSH_PROMISE（[`http2_push_promise_event`](#api-http2-push-h)） |

帧元数据事件按线上顺序与其他事件交错排队；每个已提交的帧至多产生一个此类事件，以连接错误拒绝的帧不产生。

#### 11.4.4 `struct http2_connection_options`

```cpp
struct http2_connection_options final {
    bool enable_push_{false};
    bool receive_origin_advertisements_{false};
    bool receive_frame_metadata_{false};
    http2_frame_observer_type frame_observer_{nullptr};
    http2_header_observer_type header_observer_{nullptr};
    void* observer_context_{nullptr};
    std::uint32_t max_non_progress_frames_{1000};
};
```

| 字段 | 默认 | 语义 |
| --- | --- | --- |
| `enable_push_` | `false` | 仅客户端：通告接受服务端推送；传给 `server()` 时抛 `std::invalid_argument` |
| `receive_origin_advertisements_` | `false` | 仅客户端（同上）：发布 ORIGIN 事件。只应在已认证的 h2 连接直连源站时开启；RFC 8336 禁止在 h2c 或经显式代理时使用 ORIGIN |
| `receive_frame_metadata_` | `false` | 发布 `received_settings`、`received_window_update`、`received_priority`（仅 HEADERS 优先级字段）事件，按线上顺序与其他事件交错（见 11.4.3）；关闭时不产生也不做任何元数据工作。请求伪首部顺序始终由请求头事件的 `request_view()` 提供 |
| `frame_observer_` | `nullptr` | 同步帧观察器（11.4.2） |
| `header_observer_` | `nullptr` | 同步 HPACK 字段观察器（11.4.2） |
| `observer_context_` | `nullptr` | 两个观察器共用的上下文，借用至连接销毁 |
| `max_non_progress_frames_` | `1000` | 非推进帧预算（见 11.4.6）；为 0 时 `server()` / `client()` 抛 `std::invalid_argument` |

#### 11.4.5 `class http2_connection`

```cpp
class http2_connection final {
public:
    [[nodiscard]] static http2_connection server(http2_connection_options options = {});
    [[nodiscard]] static http2_connection client(http2_connection_options options = {});
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;
    http2_connection(http2_connection&&) noexcept;
    http2_connection& operator=(http2_connection&&) noexcept;

    [[nodiscard]] http_connection_role role() const noexcept;
    [[nodiscard]] const std::optional<http2_peer_settings>& peer_settings() const& noexcept RUVIA_LIFETIMEBOUND;
    const std::optional<http2_peer_settings>& peer_settings() const&& = delete;
    [[nodiscard]] bool header_block_in_progress() const noexcept;
    [[nodiscard]] std::uint64_t received_stream_progress() const noexcept;
    [[nodiscard]] http2_feed_status feed(std::string_view input);
    template </* 未指定约束：拥有型 std::basic_string 的右值 */ typename input_type>
    http2_feed_status feed(input_type&&) = delete;
    [[nodiscard]] std::optional<http2_event> next_event() &;
    std::optional<http2_event> next_event() && = delete;
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes, std::string& output, http2_data_output_observer_type observer = nullptr, void* observer_context = nullptr);
    [[nodiscard]] bool wants_write() const noexcept;

    [[nodiscard]] std::size_t available_request_streams(std::size_t local_limit) const noexcept;
    [[nodiscard]] std::variant<std::uint32_t, http_submit_status> submit_request_head(const http_request_head_view& request);
    [[nodiscard]] std::variant<std::uint32_t, http_submit_status> submit_websocket_request_head(const websocket_client_negotiation& negotiation, std::string_view scheme, std::string_view authority, std::string_view target, borrowed_http_headers headers = {}, http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] http_data_submit_result submit_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream, std::size_t max_payload_bytes);
    [[nodiscard]] http_finish_status finish_request(std::uint32_t stream_id, std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_request_content_release_status release_request_content(std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::variant<http2_request_head_event, http2_push_submit_error> submit_push_request(std::uint32_t associated_stream_id, const http_request_head_view& request);

    [[nodiscard]] http_submit_status submit_interim_response_head(std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] http_submit_status submit_connect_response_head(std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(std::uint32_t stream_id, const http_request& request, websocket_handshake_validation_options options = {}) const noexcept;
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http_response_head_error> submit_websocket_handshake(std::uint32_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options = {});
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http_response_head_error> submit_response_head(std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan);
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http_response_head_error> submit_streaming_response_head(std::uint32_t stream_id, http_response response, http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);
    [[nodiscard]] http_finish_status finish_response(std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http_finish_status finish_stream(std::uint32_t stream_id);
    [[nodiscard]] http_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id, std::string_view origin, std::string_view field_value);
    [[nodiscard]] http_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http_submit_status submit_ping();
    [[nodiscard]] http_submit_status submit_max_concurrent_streams(std::uint32_t limit);
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] http2_stream_receive_status stream_receive_status(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] http2_server_request_release_status release(http2_request_head_event&& request);
    [[nodiscard]] http_submit_status begin_drain();
    [[nodiscard]] http_submit_status finish_drain();
    void terminate(http2_error_code error);
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] bool drain_final() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept;
};
```

稳定的 Sans-I/O HTTP/2 驱动：喂入传输字节，取出类型化事件，写出 `take_output_batch()` 的结果。连接对象只可移动；单线程使用，不可在其回调内重入。

构造与查询：

| 成员 | 语义 |
| --- | --- |
| `server(options)` / `client(options)` | 创建对应角色的连接并排队本端前言/SETTINGS。`server()` 在 `enable_push_` 或 `receive_origin_advertisements_` 为真时抛 `std::invalid_argument`；两者在 `max_non_progress_frames_ == 0` 时抛 `std::invalid_argument` |
| `role()` | 连接角色 |
| `peer_settings()` | 收到对端首个 SETTINGS 之前为 `std::nullopt`；之后为当前生效值，每个后续 SETTINGS 刷新。服务端忽略收到的 `SETTINGS_ENABLE_CONNECT_PROTOCOL` 0 或 1（RFC 8441 §3：对服务端“没有任何影响”）。右值连接上被删除 |
| `header_block_in_progress()` | CONTINUATION 序列未完成时为真；供传输层超时决策的只读观察 |
| `received_stream_progress()` | 单调计数推进流的接收步骤：完整字段块、DATA 内容分片、END_STREAM。所有者只在其变化时续期入站无活动时钟，使非推进帧无法让空闲连接保持打开；`ruvia-web` 的续期规则见 [ruvia-web](ruvia-web.md) |

输入、事件与输出：

- `feed(input)`：见 `http2_feed_status`（11.4.1）。被接受的输入，其背后存储必须在该次调用产生的全部事件被取出、且所有派生自事件的字节视图失效之前保持存活且不变；再次 `feed()` 可能使这些视图失效。拥有型字符串右值（`std::string` 临时对象等）在编译期被删除重载拒绝，避免视图借用临时存储。
- `next_event()`：按顺序取出下一个事件，队列空时返回 `std::nullopt`。事件队列容量几何增长并保留复用。
- `take_output_batch(max_bytes, output, observer, observer_context)`：向 `output` 追加整帧边界的输出（第一帧可超过 `max_bytes`；`max_bytes` 不受限时取出全部待写帧）；客户端前言视为一个不可分割段。完整未消费批次在 `output` 为空时交换存储，其余情况复制所选字节。复制失败时输出游标与观察器均不变；游标总停在帧边界，并在 DATA 观察之前提交。观察器可以在回调中提交或取出更多输出，那些帧与本批无关。
- `wants_write()`：存在待写输出时为真。

客户端请求：

- `available_request_streams(local_limit)`：某一时刻可用的客户端请求流数，不预留；把 `local_limit` 限制在协议核心与对端 SETTINGS 上限之内；服务端或不可用的客户端连接返回 0。
- `submit_request_head(request)`：打开请求流，由 HTTP/2 分配并返回流号。`request.kind()`（见 [`"http_request_head_view.h"`](#api-http-request-head-view-h)）选择请求形态（RFC 9113 §8.3、§8.5，RFC 8441）：常规请求需要 `scheme_` 与 `target_`（`authority_` 可缺省）；CONNECT 只需 `authority_`；Extended CONNECT 需要 `protocol_`、`scheme_`、`authority_`、`target_`；`content_` 与 `expectation_` 只属于常规请求。字段在返回前编码并复制，调用方可随即释放输入。成功的提交租给所有者直到其终结事件或 `submit_reset()`。失败分支从不是 `accepted`：服务端为 `invalid_state`；连接不再接纳新请求为 `closed`；无空位为 `peer_stream_limit_reached` / `local_stream_capacity_reached`；对端 SETTINGS 未启用 Extended CONNECT 为 `peer_capability_unavailable`；HTTP/2 无法承载的首部为 `invalid_message`。
- `submit_websocket_request_head(negotiation, scheme, authority, target, headers, source)`：RFC 8441 `:protocol websocket` 的 Extended CONNECT，等价于以协商中的 `Sec-WebSocket-*` 请求字段加应用首部调用 `submit_request_head()`，结果相同。最终响应头用 `validate_extended_connect_websocket_client_response()`（[`"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)）校验。
- `finish_request(stream_id, trailers)`：所有源字节都已被消费后调用；Trailer 字段在返回前编码。无 Trailer 时以空 DATA 帧承载 END_STREAM，窗口规则同 `submit_data()`，窗口不足返回 `backpressured` 且不改变状态。内容短于声明的 `Content-Length`，或 Trailer 字段非法、不允许出现在请求 Trailer（RFC 9110 §6.5.1）或超出首部列表上限时返回 `invalid_message`，不改变状态；请求内容未打开的流抛 `std::logic_error`。
- `release_request_content(stream_id)`：在未收到 `100 (Continue)` 时放行以 `Expect: 100-continue` 暂扣的请求内容，结果见 11.4.1。流式客户端请求中，响应 END_STREAM 只关闭接收半边；即使已收到 `message_end`，也必须结束或显式复位请求发送半边。

DATA 提交（双角色）：

- `submit_data(stream_id, bytes, end_stream, max_payload_bytes)`：至多序列化 `min(bytes.size(), max_payload_bytes, 可用发送窗口)` 字节，帧头开销另计；只有返回的 `consumed_bytes_` 推进 `Content-Length` 计数。不保留输入视图或未消费后缀：调用方保存后缀，在获得输出预算和/或 WINDOW_UPDATE、SETTINGS 变化后显式重试（窗口更新不会自动产生 DATA）。`complete` 表示本输入已被消费，不表示帧已到达 socket。
  - **输出拥有化**：实际接受的正文前缀在调用返回前直接复制进最终 DATA 输出段，不经 `nghttp2` 临时 DATA 帧中转。
  - 长度与终结：先对完整输入及其终结意图校验 `Content-Length`，超出声明长度或终结时不足均返回 `{0, invalid_message}`，不提交任何前缀；流的内容未打开时抛 `std::logic_error`。
  - END_STREAM 只在本次输入全部消费后发出；结束后不能再次终结。空的终结输入仍是一个 DATA 帧：RFC 9113 §6.9.1 允许（MAY）但不要求在零窗口发送空 END_STREAM DATA，`nghttp2` 后端等待连接与流窗口都为正，这是实现限制；任一窗口不为正时返回 `{0, backpressured}` 且不改变状态，调用方待 `send_window_state()` 报告 `available_ != 0` 后重试。
  - `expectation_pending` 与 `closed` 均消费 0 字节；前者等待 `100 Continue` 或 `release_request_content()`，后者停止发送。
- `pending_data_output_bytes(stream_id)`：已序列化、仍在核心输出缓冲中的 DATA 载荷字节；与输出批次观察器一起用于已编码 DATA 与 socket 写完成记账，不能代替应用层输出预算。
- `send_window_state(stream_id)`：响应写入者判断等待发送窗口能否推进的只读状态；不存在或已关闭的流返回 `std::nullopt`。

服务端响应与推送：

- `submit_interim_response_head(stream_id, response)`：提交 1xx 临时响应（[`http_interim_response_head`](#api-http-response-h)）。
- `submit_connect_response_head(stream_id, response)`：接受（2xx）或拒绝 CONNECT。服务端在接受/拒绝之前收到的 DATA（RFC 9113 §8.5；按 RFC 9298 §5 及 RFC 9931 的更新属乐观数据）以 `tunnel_data` 事件发布，其凭证由所有者持有到接受后消费这些字节；2xx 接受（经此方法或 `submit_websocket_handshake()`）后，`tunnel_end` 只在这些事件之后发布。非 2xx 响应之后到达的 DATA 被丢弃并归还其额度，流以 `RST_STREAM(NO_ERROR)` 结束。RFC 8441 `:protocol websocket` 的提前 DATA 同样处理（RFC 6455 §4.1 的等待规则只约束客户端）。已建立隧道上的未知扩展帧被丢弃（RFC 9113 §5.5），ALTSVC 与 ORIGIN 也被忽略（RFC 7838 §4、RFC 8336 §2.1）。
- `validate_websocket_handshake(stream_id, request, options)`：依据已解码的流状态与语义请求校验 RFC 8441 Extended CONNECT WebSocket 引导；`request` 须在调用期间存活。流不存在，或不是等待接受的 Extended CONNECT WebSocket 流时，返回 `websocket_handshake_error::invalid_request`。结果类型见 [`"websocket_handshake.h"`](#api-websocket-handshake-h)。
- `submit_websocket_handshake(stream_id, request, validation, options)`：以校验结果提交 2xx 握手响应，成功返回 `websocket_extended_connect_negotiation`，失败返回 `http_response_head_error`。
- `submit_response_head(stream_id, response, write_plan)`：只提交缓冲响应的最终头；返回已提交的 [`http_buffered_response_write_plan`](#api-http-response-body-h)，告知剩余多少表示需要以 DATA 或文件输出。
- `submit_streaming_response_head(stream_id, response, trailer_intent)`：提交最终通用响应头，并为后续 `submit_data()` 保持合格的响应正文打开，返回 [`http_response_stream_commit_plan`](#api-http-response-stream-h)。不自动生成 `Content-Length`；显式值存在时约束提交的 DATA 总字节。
  - 两种最终头提交都以 `invalid_message` 拒绝连接专用字段（RFC 9113 §8.2.2），唯一例外是值恰为 `chunked` 的 `Transfer-Encoding`（忽略 OWS 与 ASCII 大小写）：它是 HTTP/1 也会替换的分帧提示，HTTP/2 写入器将其省略。重复的 `Priority` 字段合成一个结构化字段字典，合法的响应参数覆盖对应的请求调度值。
- `finish_response(stream_id, trailers)`：全部正文已被消费后调用；返回前编码 Trailer，此后字段存储可释放。内容未达 `Content-Length` 返回 `invalid_message`；无 Trailer 时按 `finish_request()` 的窗口规则可能返回 `backpressured`。
- `finish_stream(stream_id)`：任一角色不带 Trailer 地结束打开的发送半边（请求内容、响应内容或已建立隧道），结果同无 Trailer 的 `finish_request()`；没有打开发送半边的流抛 `std::logic_error`。
- `submit_push_request(associated_stream_id, request)`：服务端推送（RFC 9113 §8.4）；`request` 必须是带 authority、无内容的常规 GET 或 HEAD。已打开的推送流达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS`（包括为 0）时返回 `stream_limit` 且不发送 PUSH_PROMISE；预留状态的承诺不计入（§5.1.2），之后其响应头找不到空位时得到 `http_response_head_error::peer_stream_limit_reached`。成功时提交承诺并返回用于产生其响应的服务端请求/存储租约，与入站请求一样释放或放弃。
- `release(std::move(request))`：在全部派生视图失效后消耗请求事件，结果见 11.4.1；事件已被消耗（流号为 0 或无连接）或由其他连接签发时抛 `std::logic_error`。

控制帧与通告：

- `submit_origin_advertisement(origins)` / `submit_alternative_service_advertisement(stream_id, origin, field_value)`：以 `nghttp2` 扩展帧发送 ORIGIN / ALTSVC；载荷上限为对端 `SETTINGS_MAX_FRAME_SIZE` 与 16384 字节中的较小者，超出返回 `invalid_message`。
- `submit_priority_update(stream_id, fields)`：仅客户端；服务端未发送 `SETTINGS_NO_RFC7540_PRIORITIES=1`（RFC 9218 §2.1）时返回 `peer_capability_unavailable`，不发送帧。
- `submit_reset(stream_id, error)`：复位流。不归还连接发送额度，也不重新打开其他流的发送窗口；复位关联请求会取消其上仍在进行的 PUSH_PROMISE 字段块（承诺流以 CANCEL 复位，不发布事件）。`nghttp2` 已不再跟踪的流（不保留已关闭流历史，RFC 9113 §5.1）返回 `closed`。双方 END_STREAM 已关闭流后，只接受针对畸形消息的 `PROTOCOL_ERROR`，仅在本地退役该流、不发送帧。
- `submit_ping()`：排队一个非 ACK PING，载荷不同于优雅排空的 PING。存活证据是收到的任何帧（含其 ACK）；ACK 不产生事件。一个探测未确认期间再次调用返回 `accepted` 但不再排队新帧。连接错误后返回 `closed`。
- `submit_max_concurrent_streams(limit)`：经协议会话排队 `SETTINGS_MAX_CONCURRENT_STREAMS`，使确认与接纳上限保持同步；可收紧或恢复固定的本地容量（128），更大值返回 `invalid_message`，连接失败返回 `closed`。
- `stream_aborted(stream_id)`：流已被放弃时为真；不存在的流视为已放弃，因此在拆除期间查询也安全。
- `stream_receive_status(stream_id)`：只读接收状态（11.4.1）。

排空与终止（排空查询词汇的跨协议定义见 [11.1.5](#api-http-multiplexed-stream-h)）：

- `begin_drain()`：优雅排空（RFC 9113 §6.8）。服务端先发送 `GOAWAY(2^31-1, NO_ERROR)` 与一个 PING，在收到该 PING 的 ACK 或调用 `finish_drain()` 之前继续接纳对端流；随后发送携带最后对端流号的 `GOAWAY(NO_ERROR)`。高于该边界的流被丢弃、不作应答（对端从 GOAWAY 得知）。往返期间对端再开超过 128 条流（产品上限）时，超出的那条收到 `RST_STREAM(REFUSED_STREAM)` 并立即发出最终 GOAWAY。不必等待 ACK：所有者用 `finish_drain()` 为等待设定时间上限。客户端立即发送最终 GOAWAY；客户端发出 GOAWAY 后到达的 PUSH_PROMISE 被丢弃。幂等：返回 `accepted`，连接错误后返回 `closed`（终止已排队其 GOAWAY）。
- `finish_drain()`：立即使排空成为最终：发送带最后对端流号的 `GOAWAY(NO_ERROR)` 并拒绝之后的对端流；未在排空中时直接以最终边界进入排空。适用于往返等待达到所有者时限，或对端输入已结束、不能再开流。合法的对端 GOAWAY 自行完成这一步。排空已最终时返回 `accepted` 且无效果；连接错误后返回 `closed`。
- `terminate(error)`：立即关闭（RFC 9113 §6.8）：排队携带 `error` 与最后已处理对端流号的 GOAWAY，丢弃之后全部入站帧，`connection_error()` 报告 `error`（`no_error` 亦然）；仍打开的流不再有协议进展，后续提交返回 `closed`；也结束进行中的优雅排空。连接已出错时为空操作并保留原错误码。所有者在写完输出后关闭传输。
- `draining()`：自 `begin_drain()`、`finish_drain()` 或合法对端 GOAWAY 起为真。
- `drain_final()`：最终 GOAWAY 已排队，不会再接纳对端流，所有者可在剩余流结束后关闭；合法对端 GOAWAY 同样使其为真。
- `drained()`：`drain_final()` 且连接不再跟踪任何流（两个方向的流都已关闭，且所有者租约已释放）；对端 GOAWAY 后剩余流结束时同样变为真。
- `connection_error()`：连接级错误码；无错误时为 `std::nullopt`。

#### 11.4.6 协议契约

流控与凭证：

- `data()` 与 `tunnel_data()` 事件携带 [`http_received_data_credit`](#api-http-multiplexed-stream-h)。`nghttp2` 的自动窗口更新被关闭：凭证归还时才把字节计入接收窗口，并按需发送连接级与流级 WINDOW_UPDATE。本端通告 1 MiB 初始流窗口，在对端确认本端 SETTINGS 之前对端只能使用 RFC 默认的 65535 字节（RFC 9113 §6.9.2）。
- 归还失败的租约或凭证由下一次 `feed()`、`next_event()`、输出取出、`submit_data()`、`finish_request()` 或 `submit_reset()` 重试；const 查询从不改变状态。
- 致命连接错误拒绝新的流输出；请求租约与 DATA 凭证仍可释放而不再产生帧。本端或对端的优雅 GOAWAY 仍允许已建立且仍打开的流继续输出。

字段与消息语义：

- **出站 HPACK**：复用连接唯一的 `nghttp2` 编码器，动态表容量取 `4096` 字节与对端 `SETTINGS_HEADER_TABLE_SIZE` 的较小值；对端扩大设置不会突破本端上限。
- **压缩内存**：表容量按 RFC 7541 §4.1 的条目大小计算，不含库元数据；压缩状态不跨连接共享。
- **凭据保护**：`authorization`、`proxy-authorization`、`cookie`、`set-cookie` 以 never-indexed 发送，不进入出站动态表。
- **入站同步**：字段被判定非法时以流错误复位该流，`nghttp2` 继续解码剩余字段块以保持动态表同步；HPACK 解码失败是 `COMPRESSION_ERROR` 连接错误。
- 接收字段文本增长时先为整个名/值对预留空间再复制；请求字段容量包含合成的 `Host` 与合并的 `Cookie`。无法放入公开请求视图的首部在发布前复位其流。
- 1xx 信息头不结束接收半边，没有固定数量上限，每个字段节受解码上限约束；收到的 1xx 字段作为有序元数据保留，`Content-Length` 与 `Trailer` 不为临时或最终响应分帧；1xx 上的 END_STREAM 为畸形。
- 收到的 `Trailer` 声明在请求与响应中都是不透明元数据，不授权被禁止的实际 Trailer 字段，也不选择消息分帧。
- 通用字节合法的 `Expect` 成员按所有者既有的 417/忽略策略分类，不因成员语法被判为畸形。
- 收到的请求 `TE` 允许空列表并忽略空成员，每个非空成员必须是 `trailers`（不区分大小写）；响应 `TE` 即使为空也是畸形。
- 入站 OPTIONS 正文在准入时不要求 `Content-Type`（RFC 9110 §9.3.7 把该要求放在发送方）。TRACE 接受零 `Content-Length` 与空 DATA 结束，但拒绝内容。

异常与失败：

- `nghttp2` 内部或缓存其输出时的分配失败，以及 `nghttp2` 执行回调期间抛出的异常，使连接进入致命错误（仍可排队时发送 `GOAWAY(INTERNAL_ERROR)`）并向调用方传播（分配失败为 `std::bad_alloc`）；没有事务回滚，不支持资源恢复后重试同一操作。

防滥用预算（核心没有时钟，`nghttp2` 基于时钟的复位与 glitch 限速器被关闭，改用计数）：

- 非推进帧预算 `max_non_progress_frames_`：不推进任何流的入站帧各计 1 单位：非 ACK 的 PING 与 SETTINGS、不应答本端所发 PING 的 PING ACK、WINDOW_UPDATE、PRIORITY、PRIORITY_UPDATE、GOAWAY、不带 END_STREAM 的空 DATA，以及每个扩展帧。每个完整接收的字段块、携带内容或 END_STREAM 的接收 DATA、以及每个发出的 HEADERS 或 DATA 帧返还 16 单位，并按 DATA 载荷每 KiB 再返还 1，不低于 0。未偿单位超过上限的那一帧以 `GOAWAY(ENHANCE_YOUR_CALM)` 结束连接。
- 对端诱发输出：输出未取空期间，PING ACK、SETTINGS ACK 与因对端流错误发出的 RST_STREAM 合计不超过 1000，输出完全取空后清零；连接生命周期内因对端流错误发出的 RST_STREAM 总数上限 1024，不回补。所有者主动的 `submit_reset()` 不计入。超限以 `GOAWAY(ENHANCE_YOUR_CALM)` 进入连接错误。
- Rapid Reset：对端复位流数与已完成响应数的净差上限与 HTTP/3 共用 [`"http_protocol.h"`](#api-http-protocol-h) 的 `max_unprocessed_stream_resets`（1000）。
- 超过 32 项的 SETTINGS 帧，或单个字段块超过 1024 个 CONTINUATION 帧，以 `GOAWAY(ENHANCE_YOUR_CALM)` 结束连接。
- 本端 SETTINGS 未获 ACK 时不设 `SETTINGS_TIMEOUT`（RFC 9113 §6.5.3 为 MAY）；新设置在 ACK 前不生效，连接由所有者的超时约束。

由 `nghttp2` 决定的错误类别：

- 以下为 `PROTOCOL_ERROR` 连接错误（RFC 9113 §5.4.1 允许把流错误升级）：任意流上的零增量 WINDOW_UPDATE、长度不足以容纳填充或优先级字段的 HEADERS、依赖自身的 HEADERS 优先级、任一角色收到 0 与 1 以外的 `SETTINGS_ENABLE_CONNECT_PROTOCOL`、对端在本端 SETTINGS 已确认后超出 `SETTINGS_MAX_CONCURRENT_STREAMS` 的新流，以及非法的客户端前言。确认之前超出通告上限的流收到 `RST_STREAM(REFUSED_STREAM)`。客户端仍校验服务端的 `SETTINGS_ENABLE_CONNECT_PROTOCOL` 1→0 回退。
- 已关闭流不被记住：`nghttp2` 已退役流上的帧（以及其无法解析的超长 PRIORITY_UPDATE）在任何回调前被忽略，既不计入 `max_non_progress_frames_`，也不推进 `received_stream_progress()`，由所有者的无活动时钟约束。关联流已不存在时到达的 PUSH_PROMISE，其承诺流以 CANCEL 复位；本端 GOAWAY 之后到达的 PUSH_PROMISE 被丢弃。已建立隧道上的 PUSH_PROMISE 先完成 HPACK 解码，再以 `RST_STREAM(PROTOCOL_ERROR)` 复位关联流、以 `RST_STREAM(CANCEL)` 取消承诺流。PRIORITY 帧被读取后丢弃（RFC 9113 §5.3.2 已弃用）。

客户端特有：

- 客户端在收到任何响应头（含 1xx）之前收到 `RST_STREAM(REFUSED_STREAM)` 时发布 `request_unprocessed()` 而非 `stream_closed()`（RFC 9113 §8.7，可安全重试），连接保持可用。
- 客户端在承诺的推送响应超出本端已确认的并发上限时按连接错误处理；在 PUSH_PROMISE 字段块结束前复位其关联流会一并取消该承诺。

`ruvia-web` 如何基于本连接调度正文队列、DATA 写入与信用记账属于 Web 层行为，见 [ruvia-web](ruvia-web.md)。

示例（服务端最小循环）：

```cpp
#include "http2_connection.h"

auto conn = ruvia::http2_connection::server({});

// 1. 喂入从网络收到的原始字节；events_pending 时先取空事件再重新提交同一段。
auto feed_res = conn.feed(inbound_bytes);

// 2. 消费协议事件：
while (auto ev = conn.next_event()) {
    if (auto* req_head = ev->request_head()) {
        std::uint32_t stream_id = req_head->stream_id();
        ruvia::http_response resp;
        resp.status(ruvia::http_status::no_content);
        // 无正文响应由 HEADERS 一次结束；有正文时保留请求租约并按 submit_data() 契约驱动 DATA。
        (void)conn.submit_streaming_response_head(stream_id, std::move(resp));
        (void)conn.release(std::move(*req_head));
    }
}

// 3. 取出待发往网络的编码字节（按整帧边界分批）：
std::string out;
while (conn.wants_write()) {
    out.clear();
    (void)conn.take_output_batch(16384, out);
    // 写出 out
}
```

#### 11.4.7 `class http2_akamai_fingerprint_collector`

```cpp
class http2_akamai_fingerprint_collector final {
public:
    explicit http2_akamai_fingerprint_collector(http_fingerprint_options options = {});

    void observe_frame(const http2_frame_metadata& frame) noexcept;
    void observe_header(const http2_header_metadata& header) noexcept;
    void reset() noexcept;

    static void on_frame(void* context, const http2_frame_metadata& frame) noexcept;
    static void on_header(void* context, const http2_header_metadata& header) noexcept;

    [[nodiscard]] constexpr bool complete() const noexcept;
    [[nodiscard]] constexpr bool failed() const noexcept;
    [[nodiscard]] std::string_view fingerprint() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view fingerprint() const&& = delete;
};
```

收集截至客户端第一个请求字段块结束的帧与 HPACK 观察，构建标准 Akamai HTTP/2 指纹字符串 `SETTINGS|WINDOW_UPDATE|PRIORITY|PSEUDO_HEADER_ORDER`。

- 接入：把 `on_frame` / `on_header` 作为 `http2_connection_options::frame_observer_` / `header_observer_`，`observer_context_` 指向采集器（采集器须比连接存活更久）。
- 状态：初始为收集中；第一个请求字段块 `end` 时变为完成（`complete()`），超出 [`http_fingerprint_options`](#api-http-multiplexed-stream-h) 预算、SETTINGS 不完整或位于非零流、在流 0 上观察到字段、首个请求没有伪首部，或内部分配失败时变为失败（`failed()`）。完成或失败后忽略后续观察；`reset()` 清空回到收集中。
- 各段：SETTINGS 为各非 ACK SETTINGS 条目 `id:value` 以 `;` 连接；WINDOW_UPDATE 为最后一个流 0 增量，无则 `00`；PRIORITY 为独立 PRIORITY 帧及首个请求流 HEADERS 优先级的 `stream:exclusive:dependency:weight` 以 `,` 连接，无则 `0`；PSEUDO_HEADER_ORDER 为首个常规字段之前伪首部的首字母（`m`、`a`、`s`、`p`，其他取名称第二字符）以 `,` 连接。
- `fingerprint()`：完成时返回借用采集器内部字符串的视图，否则为空视图；右值采集器上被删除。

### 11.5 `"http2_hpack.h"`
<a id="api-http2-hpack-h"></a>

独立于连接状态机的 RFC 7541 HPACK 解码器与字段级编码器，基于 `nghttp2` 的 inflater/deflater。仅 `RUVIA_ENABLE_HTTP2`。公开 include [`"checked_integer.h"`](#api-checked-integer-h)、[`"http_status.h"`](#api-http-status-h)。源码：[`ruvia-http/include/http2_hpack.h`](../ruvia-http/include/http2_hpack.h)

#### 11.5.1 类型

```cpp
class hpack_table_size final : public checked_integer<hpack_table_size, std::uint32_t, /* 未指定实现类型 */> {
public:
    using checked_integer::checked_integer;
};

enum class hpack_decode_error : std::uint8_t {
    compression_error,
    callback_rejected,
};
```

- `hpack_table_size`：32 位 HPACK 动态表字节容量（RFC 7541 §6.3）。构造、`try_from_value()`、`value()` 与比较等操作继承自 [`checked_integer`](#api-checked-integer-h)：在窄化前检查 `0..4294967295`，负数或更宽的整数不会回绕成合法值，越界时抛 `std::invalid_argument`（无异常探测用 `try_from_value()`）；可从整数隐式构造，默认值为 0；不接受浮点或枚举，不要预先窄化输入。可平凡复制，大小等于 `std::uint32_t`。

| `hpack_decode_error` 值 | 含义 |
| --- | --- |
| `compression_error` | 字段块格式错误或截断、容量更新违规，或解码器已中毒；`nghttp2` 把所有格式错误报告为同一种解压失败（RFC 7541 §3.2 将其规定为连接错误），因此不细分原因 |
| `callback_rejected` | 回调返回了 `false`；块已完整解码，动态表保持同步 |

#### 11.5.2 `class hpack_decoder`

```cpp
class hpack_decoder final {
public:
    hpack_decoder();
    ~hpack_decoder();
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;
    hpack_decoder(hpack_decoder&&) noexcept;
    hpack_decoder& operator=(hpack_decoder&&) noexcept;

    void set_max_dynamic_table_size(hpack_table_size bytes);

    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, callback&& callback_value);
};
```

有状态的 RFC 7541 解码器；只可移动。

- `decode(block, callback)`：消费一个完整字段块，按顺序对每个字段调用 `callback(name, value)`，成功返回 `std::nullopt`。名称与值视图只在该次回调期间有效，需保留时复制到自有存储。回调必须让本解码器的实现存活到 `decode()` 返回。
  - 回调返回 `false` 时停止后续回调，但仍解码完整个块以保持动态表同步，返回 `callback_rejected`，解码器仍可用。
  - 回调抛出异常时同样停止回调并解码完该块，再重抛该异常（即使继续解码本身也失败，回调异常仍优先传播）。
  - 同一实现上的嵌套 `decode()`（在回调中）抛 `std::logic_error`；不同解码器可以嵌套使用。这是库的状态独占契约，不是 RFC 的额外输入限制。
  - 压缩错误或分配失败使动态表与对端失步（RFC 7541 §2.2 无恢复手段）：解码器中毒，此后每次 `decode()` 都报告 `compression_error`，只能丢弃。
  - 块首的动态表容量更新按线上顺序生效；出现在字段之后或超过所设上限的容量更新是压缩错误。
- `set_max_dynamic_table_size(bytes)`：只能在两次 `decode()` 之间调用；在字段回调期间调用抛 `std::logic_error`，且不改变允许值或动态表。把上限降到动态表当前容量以下会立即驱逐条目，下一个块必须以不大于此后所设最小上限的容量更新开头（RFC 7541 §4.2），否则为压缩错误；调高上限不要求容量更新。

#### 11.5.3 `class hpack_encoder`

```cpp
class hpack_encoder final {
public:
    static void encode_header(std::string& output, std::string_view name, std::string_view value);
    static void encode_status(std::string& output, http_status_code status);
};
```

经 `nghttp2` deflater 的字段级编码，动态表容量为零，因而调用之间不依赖也不改变编码器状态：完全命中静态表的字段用索引表示，其余字段用不索引的字面量（`authorization`、`proxy-authorization`、`cookie`、`set-cookie` 用 never-indexed，RFC 7541 §7.1.3），较短时使用 Huffman 编码。

- `encode_header(output, name, value)`：把一个字段的表示追加到 `output`。
- `encode_status(output, status)`：追加 `:status` 伪首部（[`http_status_code`](#api-http-status-h)）。
- 名称与值可以借用 `output` 自身；别名输入会在追加可能使其失效前被复制。分配失败时 `output` 已有字节不变（异常传播）。

## 12. HTTP/3 与 QUIC

本章三个头仅在 CMake 选项 `RUVIA_ENABLE_HTTP3` 开启时安装，并以 `PUBLIC` 编译定义 `RUVIA_ENABLE_HTTP3` 传播给使用方；关闭时不编译其源码、不安装这些头，也不引入 nghttp3 / ngtcp2 依赖。`RUVIA_ENABLE_HTTP2` 与 `RUVIA_ENABLE_HTTP3` 互不要求对方启用。`ruvia::http` 本身只链接 nghttp3 与 ngtcp2 core；QUIC-TLS 适配（如 `ngtcp2_crypto_ossl`）与 OpenSSL 依赖属于 [ruvia-web](ruvia-web.md)。

两层关系：

- [`"http3_connection.h"`](#api-http3-connection-h) 是 Sans-I/O 的 HTTP/3 + QPACK 编解码核心（基于 nghttp3），不出现任何 ngtcp2 类型。自行驱动 QUIC 的调用方可只使用这一层。
- [`"http3_quic_connection.h"`](#api-http3-quic-connection-h) 在其上用 ngtcp2 承担 QUIC v1 包处理、丢包恢复、流量控制、流 ID 与计时，公开签名直接使用 ngtcp2 core 类型（`ngtcp2_cid`、`ngtcp2_path`、`ngtcp2_tstamp` 等）。
- [`"http3_request_head.h"`](#api-http3-request-head-h) 是服务端发布的、已验证的请求头投影。

HTTP/2 与 HTTP/3 共用的角色、提交状态、流事件模板、`http_received_data_credit` 与指纹预算等词汇定义在 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)，本章只说明 HTTP/3 特有的实例化与差异。`ruvia::web` 基于这两层提供的 HTTP/3 服务（UDP 端点与连接 ID 分流、连接会话、生命周期、0-RTT 准入与迁移策略）见 [ruvia-web](ruvia-web.md)。

### 12.1 `"http3_connection.h"`
<a id="api-http3-connection-h"></a>

HTTP/3 流编解码核心：QUIC 拥有流 ID、重传、流控与包 I/O，本头拥有 QPACK 与 HTTP/3 控制流/请求流分帧（经 nghttp3）。仅 `RUVIA_ENABLE_HTTP3`。公开 include [`"http3_request_head.h"`](#api-http3-request-head-h)、[`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)、[`"http_client_response_head.h"`](#api-http-client-response-head-h)、[`"http_connection_advertisement.h"`](#api-http-connection-advertisement-h)、[`"http_priority.h"`](#api-http-priority-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)、[`"http_response.h"`](#api-http-response-h)、[`"http_response_body.h"`](#api-http-response-body-h)、[`"http_response_stream.h"`](#api-http-response-stream-h)、[`"websocket_handshake.h"`](#api-websocket-handshake-h)、[`"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_request.h"`](#api-http-request-h)。源码：[`ruvia-http/include/http3_connection.h`](../ruvia-http/include/http3_connection.h)

#### 12.1.1 错误码、帧类型与传输结果

```cpp
namespace ruvia {

class http3_quic_connection;

enum class http3_error_code : std::uint64_t {
    no_error = 0x0100,
    general_protocol_error = 0x0101,
    internal_error = 0x0102,
    stream_creation_error = 0x0103,
    closed_critical_stream = 0x0104,
    frame_unexpected = 0x0105,
    frame_error = 0x0106,
    excessive_load = 0x0107,
    id_error = 0x0108,
    settings_error = 0x0109,
    missing_settings = 0x010a,
    request_rejected = 0x010b,
    request_cancelled = 0x010c,
    request_incomplete = 0x010d,
    message_error = 0x010e,
    connect_error = 0x010f,
    version_fallback = 0x0110,
    qpack_decompression_failed = 0x0200,
    qpack_encoder_stream_error = 0x0201,
    qpack_decoder_stream_error = 0x0202,
    datagram_error = 0x33,
};

enum class http3_frame_type : std::uint64_t {
    data = 0x0,
    headers = 0x1,
    cancel_push = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    goaway = 0x7,
    origin = 0xc,
    max_push_id = 0xd,
    priority_update_request = 0xf0700,
    priority_update_push = 0xf0701,
};

enum class http3_transport_status : std::uint8_t {
    accepted,
    would_block,
    events_pending,
    closed,
    invalid_state,
    protocol_error,
};

struct http3_transport_result final {
    http3_transport_status status_{http3_transport_status::accepted};
    std::optional<http3_error_code> error_code_{};
    std::size_t credit_bytes_{0};
};

}  // namespace ruvia
```

`http3_error_code`（底层 `std::uint64_t`）覆盖 RFC 9114 §8.1 的全部 H3 码、RFC 9204 §6 的 QPACK 码与 RFC 9297 §5.2 的 `H3_DATAGRAM_ERROR`，由 RESET_STREAM、STOP_SENDING 与 CONNECTION_CLOSE 携带。所有 reset、close、事件与 `connection_error()` 都收发这一强类型，公开契约不暴露 nghttp3 宏或库错误码。收到的未知码（含保留值 `0x1f * N + 0x21`）以原值保存在枚举变量中；按 RFC 9114 §9，未知码等价于 `H3_NO_ERROR`。

| 枚举值 | 数值 | 含义 |
| --- | --- | --- |
| `no_error` | `0x0100` | H3_NO_ERROR：无错误，用于优雅关闭或不再需要的流 |
| `general_protocol_error` | `0x0101` | H3_GENERAL_PROTOCOL_ERROR：无更具体码的协议违规 |
| `internal_error` | `0x0102` | H3_INTERNAL_ERROR：实现内部错误 |
| `stream_creation_error` | `0x0103` | H3_STREAM_CREATION_ERROR：对端创建了不可接受的流 |
| `closed_critical_stream` | `0x0104` | H3_CLOSED_CRITICAL_STREAM：控制流或 QPACK 流被关闭或重置 |
| `frame_unexpected` | `0x0105` | H3_FRAME_UNEXPECTED：帧在当前状态不允许 |
| `frame_error` | `0x0106` | H3_FRAME_ERROR：帧布局或大小不合法 |
| `excessive_load` | `0x0107` | H3_EXCESSIVE_LOAD：对端行为造成过量负载 |
| `id_error` | `0x0108` | H3_ID_ERROR：流 ID 或推送 ID 使用不当 |
| `settings_error` | `0x0109` | H3_SETTINGS_ERROR：SETTINGS 载荷错误 |
| `missing_settings` | `0x010a` | H3_MISSING_SETTINGS：控制流首帧不是 SETTINGS |
| `request_rejected` | `0x010b` | H3_REQUEST_REJECTED：服务端未处理该请求，可安全重试 |
| `request_cancelled` | `0x010c` | H3_REQUEST_CANCELLED：请求或响应被取消 |
| `request_incomplete` | `0x010d` | H3_REQUEST_INCOMPLETE：客户端流在请求完整前终止 |
| `message_error` | `0x010e` | H3_MESSAGE_ERROR：消息畸形（RFC 9114 §4.1.2） |
| `connect_error` | `0x010f` | H3_CONNECT_ERROR：CONNECT 隧道的 TCP 连接被重置或异常关闭 |
| `version_fallback` | `0x0110` | H3_VERSION_FALLBACK：请求应改用 HTTP/1.1 |
| `qpack_decompression_failed` | `0x0200` | QPACK_DECOMPRESSION_FAILED：字段节无法解压 |
| `qpack_encoder_stream_error` | `0x0201` | QPACK_ENCODER_STREAM_ERROR：编码器流指令错误 |
| `qpack_decoder_stream_error` | `0x0202` | QPACK_DECODER_STREAM_ERROR：解码器流指令错误 |
| `datagram_error` | `0x33` | H3_DATAGRAM_ERROR（RFC 9297 §5.2）：HTTP Datagram 畸形，或未协商时收到 |

`http3_frame_type`（底层 `std::uint64_t`）用于帧观察（见 12.1.5）；列表外的类型（含 RFC 9114 §7.2.8 保留类型）以原值保存。

| 枚举值 | 数值 | 含义 |
| --- | --- | --- |
| `data` | `0x0` | DATA（RFC 9114 §7.2.1） |
| `headers` | `0x1` | HEADERS（§7.2.2） |
| `cancel_push` | `0x3` | CANCEL_PUSH（§7.2.3） |
| `settings` | `0x4` | SETTINGS（§7.2.4） |
| `push_promise` | `0x5` | PUSH_PROMISE（§7.2.5） |
| `goaway` | `0x7` | GOAWAY（§7.2.6） |
| `origin` | `0xc` | ORIGIN（RFC 9412 §2） |
| `max_push_id` | `0xd` | MAX_PUSH_ID（§7.2.7） |
| `priority_update_request` | `0xf0700` | 针对请求流的 PRIORITY_UPDATE（RFC 9218 §7.2） |
| `priority_update_push` | `0xf0701` | 针对推送流的 PRIORITY_UPDATE（RFC 9218 §7.2） |

`http3_transport_status`（底层 `std::uint8_t`）是 QUIC 拥有者驱动编解码器输入输出（`read_stream()`、`read_datagram()`、`peek_output()` 及流记账调用）的结果：

| 枚举值 | 含义 |
| --- | --- |
| `accepted` | 调用已被接受并生效 |
| `would_block` | 当前没有可输出的数据 |
| `events_pending` | 输入须等待 `next_event()` 取尽上一次输入产生的事件后再提交；本次输入未被消费 |
| `closed` | 连接已失败，或该流已不存在 |
| `invalid_state` | 调用不符合角色、流状态或已绑定的控制流 |
| `protocol_error` | 本次调用使连接失败；`connection_error()` 随后给出 CONNECTION_CLOSE 应携带的码 |

`http3_transport_result` 字段：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `status_` | `accepted` | 上表状态 |
| `error_code_` | 空 | `protocol_error` 时、以及连接错误后的 `closed` 时设置，为 QUIC CONNECTION_CLOSE 应携带的 HTTP/3 码 |
| `credit_bytes_` | `0` | 仅 `read_stream()`：编解码器自行消费的分帧与 QPACK 字节，QUIC 拥有者应立即作为接收流控额度归还（RFC 9000 §4.1）；DATA 载荷不在其中，而经 `http_received_data_credit` 归还 |

nghttp3 在帧层识别的格式错误（伪首部缺失或非法、连接专属字段等）由 `read_stream()` 以 `protocol_error` + `H3_MESSAGE_ERROR` 报告为连接错误，这是 RFC 9114 §8 允许的将流错误升级为连接错误。

#### 12.1.2 事件类型

```cpp
namespace ruvia {

class http3_request_head_event final {
public:
    http3_request_head_event(http3_request_head_event&&) noexcept = default;
    http3_request_head_event& operator=(http3_request_head_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept;
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] http3_request_head take_head() && noexcept;
};

using http3_informational_head_event = http_stream_informational_head_event<std::int64_t>;
using http3_response_head_event = http_stream_response_head_event<std::int64_t>;
using http3_data_event = http_stream_data_event<std::int64_t, http_stream_data_kind::message>;
using http3_message_end_event = http_stream_message_end_event<std::int64_t>;
using http3_stream_closed_event = http_stream_closed_event<std::int64_t, http3_error_code>;
using http3_request_unprocessed_event = http_stream_request_unprocessed_event<std::int64_t>;

enum class http3_transport_action : std::uint8_t {
    stop_sending,
    reset_stream,
};

enum class http3_transport_action_origin : std::uint8_t {
    local,
    peer,
};

class http3_transport_action_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept;
    [[nodiscard]] http3_transport_action action() const noexcept;
    [[nodiscard]] http3_error_code error() const noexcept;
    [[nodiscard]] http3_transport_action_origin origin() const noexcept;
};

class http3_deferred_consume_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
};

class http3_goaway_event final {
public:
    [[nodiscard]] std::optional<std::int64_t> id() const noexcept;
};

class http3_datagram_event final {
public:
    http3_datagram_event(http3_datagram_event&&) noexcept = default;
    http3_datagram_event& operator=(http3_datagram_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept;
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view payload() const&& = delete;
    [[nodiscard]] std::string take_payload() && noexcept;
};

class http3_event final {
public:
    http3_event(const http3_event&) = delete;
    http3_event& operator=(const http3_event&) = delete;
    http3_event(http3_event&&) noexcept = default;
    http3_event& operator=(http3_event&&) = delete;

    [[nodiscard]] http3_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_request_head_event* request_head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_request_head_event* request_head() const&& = delete;
    [[nodiscard]] http3_informational_head_event* informational_head() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_informational_head_event* informational_head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_informational_head_event* informational_head() const&& = delete;
    [[nodiscard]] http3_response_head_event* response_head() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_response_head_event* response_head() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_response_head_event* response_head() const&& = delete;
    [[nodiscard]] http3_data_event* data() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_data_event* data() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_data_event* data() const&& = delete;
    [[nodiscard]] http3_message_end_event* message_end() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_message_end_event* message_end() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_message_end_event* message_end() const&& = delete;
    [[nodiscard]] const http3_transport_action_event* transport_action() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_transport_action_event* transport_action() const&& = delete;
    [[nodiscard]] const http3_deferred_consume_event* deferred_consume() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_deferred_consume_event* deferred_consume() const&& = delete;
    [[nodiscard]] const http3_stream_closed_event* stream_closed() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_stream_closed_event* stream_closed() const&& = delete;
    [[nodiscard]] const http3_request_unprocessed_event* request_unprocessed() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_request_unprocessed_event* request_unprocessed() const&& = delete;
    [[nodiscard]] const http3_goaway_event* goaway() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_goaway_event* goaway() const&& = delete;
    [[nodiscard]] http3_datagram_event* datagram() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http3_datagram_event* datagram() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_datagram_event* datagram() const&& = delete;
    [[nodiscard]] http_origin_advertisement* origin_advertisement() & noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_origin_advertisement* origin_advertisement() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_origin_advertisement* origin_advertisement() const&& = delete;
};

}  // namespace ruvia
```

所有事件只由编解码器构造（构造函数私有），调用方只能通过访问器读取；拥有型内容经右值 `take_*()` 交出。

- `http3_event`：`next_event()` 返回的只可移动事件，与 HTTP/2 的 `http2_event` 同形；恰好一个访问器返回非空指针。各访问器的 `const&&` 重载被 `= delete`，禁止从临时事件取指针（指针借用事件自身存储，事件移动或销毁后失效）。`transport_action()`、`deferred_consume()`、`stream_closed()`、`request_unprocessed()`、`goaway()` 只有 const 重载，因为它们不交出拥有内容。
- `http3_request_head_event`（仅服务端）：已通过验证的请求头（见 [`"http3_request_head.h"`](#api-http3-request-head-h)）。`stream_id()` 等于 `head().stream_id()`；`head()` 借用事件，`const&&` 重载删除；`take_head() &&` 移出 `http3_request_head`。仅可移动构造，不可赋值。
- 六个流事件别名是 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h) 中流事件模板以 `std::int64_t` 流 ID 的实例化，完整访问器与借用契约在那里定义。HTTP/3 特有约定：
  - `http3_informational_head_event` / `http3_response_head_event`（客户端）：`head()` / `take_head()` 交出 [`http_client_response_head`](#api-http-client-response-head-h)；`request_content_signal()` 对带 `Expect: 100-continue` 的请求在首个 100 给出 `continue_`，在仍等待 100 时到达的最终头给出 `exchange_complete`；`stream_ended()` 表示最终 HEADERS 已结束流。600–999 按 5xx 处理，101 被拒绝；官方 nghttp3 在应用回调前即拒绝 000–099 的状态码。
  - `http3_data_event`：开启 `own_data_events_` 时拥有字节，否则借用 `read_stream()` 的输入（见 12.1.4）。隧道字节同样以 DATA 事件交付。`take_credit()` 交出 `http_received_data_credit`，其字节析构时归还到 `received_data_credit_sink_`；消费方应持有到真正读完正文。
  - `http3_message_end_event`：对端发送半边正常结束时发布，隧道接收方向结束同样如此。`trailers()` 借用事件自身存储；无 Trailer 时为空。客户端收到的 Trailer 已验证并丢弃仅违反发送方 trailer 限制的字段；服务端收到解码后的普通 trailer 字段（nghttp3 拒绝 trailer 中的伪首部，RFC 9114 §4.3），交 `http_request_trailers::append_received()` 吸收。
  - `http3_stream_closed_event`：QUIC 退役该流后发布一次，错误类型为 `http3_error_code`；此后该流 ID 不得再用于任何提交。
  - `http3_request_unprocessed_event`（客户端）：对 GOAWAY 标识及以上仍开启的请求流（在 `http3_goaway_event` 之后按流号升序），以及在最终响应头之前被 `H3_REQUEST_REJECTED` 重置或停止发送的流发布（RFC 9114 §4.1.1、§5.2），请求可在其他连接重试。编解码器随后只为该流仍开启的方向排队本地 `H3_REQUEST_CANCELLED` 传输动作，此后不再有任何事件（包括 `http3_stream_closed_event`）提及该流。
- `http3_transport_action` / `http3_transport_action_origin` / `http3_transport_action_event`：

  | 枚举值 | 含义 |
  | --- | --- |
  | `http3_transport_action::stop_sending` | STOP_SENDING：终止接收半边 |
  | `http3_transport_action::reset_stream` | RESET_STREAM：终止发送半边 |
  | `http3_transport_action_origin::local` | 编解码器要求 QUIC 拥有者发送此帧（须执行）；`http3_quic_connection` 在发布事件前已执行 |
  | `http3_transport_action_origin::peer` | 对端的帧，已应用于编解码器；禁止回显 |

  `stream_id()`、`action()`、`error()`（携带的 `http3_error_code`）、`origin()` 为只读访问器。畸形消息（RFC 9114 §4.1.2）由编解码器以本地 STOP_SENDING 与 RESET_STREAM（`H3_MESSAGE_ERROR`）回应，此后没有消息事件提及该流。
- `http3_deferred_consume_event`：`read_stream()` 返回后 nghttp3 自行消费的流字节——被阻塞字段节解码后释放的分帧与 QPACK 输入，以及编解码器丢弃的 DATA（nghttp3 deferred_consume 语义）。无人持有，QUIC 拥有者应像 `credit_bytes_` 一样立即归还，不转为凭证。`http3_quic_connection` 自行处理该事件，从不发布它。
- `http3_goaway_event`：对端发送 GOAWAY（RFC 9114 §5.2）。客户端收到时 `id()` 是服务端不会处理的首个请求流 ID；服务端收到时是客户端不再接受的首个推送 ID。`std::nullopt` 表示其标识不限定任何流的 shutdown notice（2^62-4 或更大）。
- `http3_datagram_event`：本编解码器已解码请求头的请求流上的 HTTP/3 Datagram（RFC 9297 §2.1）。`payload()` 为 Quarter Stream ID 之后的载荷，借用事件，`const&&` 删除；`take_payload() &&` 移出 `std::string`。仅可移动构造，不可赋值。
- `origin_advertisement()`：对端 ORIGIN 帧（RFC 9412），类型 `http_origin_advertisement` 见 [`"http_connection_advertisement.h"`](#api-http-connection-advertisement-h)。

#### 12.1.3 设置、输出、观察与选项类型

```cpp
namespace ruvia {

struct http3_peer_settings final {
    bool enable_connect_protocol_{false};
    bool h3_datagram_{false};
    std::uint64_t max_field_section_size_{UINT64_MAX};
    std::uint64_t qpack_max_dtable_capacity_{0};
    std::uint64_t qpack_blocked_streams_{0};
};

struct http3_datagram_prefix final {
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const std::byte> bytes() const&& = delete;

    std::array<std::byte, 8> bytes_{};
    std::size_t size_{0};
};

struct http3_stream_output final {
    [[nodiscard]] std::size_t size() const noexcept;

    std::int64_t stream_id_{-1};
    std::array<std::span<const std::byte>, 16> buffers_{};
    std::size_t buffer_count_{0};
    bool fin_{false};
};

struct http3_frame_metadata final {
    std::uint64_t observation_sequence_{0};
    std::int64_t stream_id_{-1};
    http3_frame_type type_{http3_frame_type::data};
    std::uint64_t payload_length_{0};
};

enum class http3_field_section_kind : std::uint8_t { header,
    trailer };
enum class http3_field_observation_phase : std::uint8_t { begin,
    field,
    end };

struct http3_field_metadata final {
    std::int64_t stream_id_{-1};
    http3_field_section_kind section_{http3_field_section_kind::header};
    std::size_t field_index_{0};
    http3_field_observation_phase phase_{http3_field_observation_phase::begin};
    std::string_view name_{};
    std::string_view value_{};
};

using http3_frame_observer_type = void (*)(void*, const http3_frame_metadata&) noexcept;
using http3_field_observer_type = void (*)(void*, const http3_field_metadata&) noexcept;
using http3_received_data_credit_sink_type = void (*)(void* context, std::int64_t stream_id, std::size_t bytes) noexcept;

struct http3_connection_options final {
    std::uint64_t max_field_section_size_{64 * 1024};
    std::size_t max_concurrent_streams_{100};
    std::uint64_t qpack_max_dtable_capacity_{4096};
    std::uint64_t qpack_blocked_streams_{16};
    std::size_t max_stream_output_bytes_{256 * 1024};
    bool enable_connect_protocol_{false};
    bool enable_h3_datagram_{false};
    bool own_data_events_{false};
    borrowed_span<std::string_view> origins_{};
    http3_received_data_credit_sink_type received_data_credit_sink_{nullptr};
    void* received_data_credit_context_{nullptr};
    http3_frame_observer_type frame_observer_{nullptr};
    http3_field_observer_type field_observer_{nullptr};
    void* observer_context_{nullptr};
};

}  // namespace ruvia
```

`http3_peer_settings`：对端 SETTINGS（RFC 9114 §7.2.4）中选择可选功能的部分；由 `peer_settings()` 返回，也用于 `http3_quic_connection::start_early_http3()` 恢复 0-RTT 记忆值。

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `enable_connect_protocol_` | `false` | SETTINGS_ENABLE_CONNECT_PROTOCOL（RFC 9220 §3） |
| `h3_datagram_` | `false` | SETTINGS_H3_DATAGRAM（RFC 9297 §2.1.1） |
| `max_field_section_size_` | `UINT64_MAX` | SETTINGS_MAX_FIELD_SECTION_SIZE；未发送时为无限 |
| `qpack_max_dtable_capacity_` | `0` | SETTINGS_QPACK_MAX_TABLE_CAPACITY |
| `qpack_blocked_streams_` | `0` | SETTINGS_QPACK_BLOCKED_STREAMS |

`http3_datagram_prefix`：一条请求流全部 HTTP/3 Datagram 前缀的 Quarter Stream ID（RFC 9297 §2.1），以 QUIC 变长整数编码在 `bytes_` 的前 `size_` 字节。`bytes()` 返回借用该对象的视图，`const&&` 重载删除以拒绝临时对象。

`http3_stream_output`：`peek_output()` 填写的一段借用输出。`stream_id_` 为所属 QUIC 流（默认 `-1`）；`buffers_[0, buffer_count_)` 为至多 16 个借用缓冲；`fin_` 表示这些字节之后流结束；`size()` 返回各缓冲字节总和。

帧与字段观察（与 HTTP/2 帧观察对齐，见 [`"http2_connection.h"`](#api-http2-connection-h)）：

- `http3_frame_metadata`：编解码器在请求流或对端控制流上读完的帧头（RFC 9114 §7.1），在载荷处理前交付。`observation_sequence_` 为连接内从 0 起按 `read_stream()` 接收顺序计数的帧头序号；`stream_id_`、`type_`（未知与保留类型保留原值）、`payload_length_`。
- `http3_field_section_kind`：`header` 为头部字段节，`trailer` 为 trailer 字段节。
- `http3_field_observation_phase`：`begin` 在首个字段前；`field` 为 QPACK 解码后、HTTP 校验前的一个字段（包括导致拒绝的字段）；`end` 表示字段节已完整解码，不表示 HTTP 语义接受。
- `http3_field_metadata`：一个接收字段节的 QPACK 解码事实（RFC 9204），不是已验证的字段。`stream_id_`、`section_`、`field_index_`（节内下标）、`phase_`、`name_` / `value_`（仅在回调期间借用解码器存储；`begin` 与 `end` 时为空）。被阻塞的字段节（RFC 9204 §2.1.2）在动态表引用解出后才观察。
- `http3_frame_observer_type` / `http3_field_observer_type`：观察回调函数指针类型，`noexcept`。只在 `read_stream()` 内同步调用，从不在 `next_event()` 中调用；返回前不得修改、重入、移动或销毁该连接。观察不意味着接受：校验与强制错误处理照常执行。QPACK 编解码流与其他单向流类型不分帧，不被观察。空指针关闭观察器，不保留输入、不额外分配；开启帧观察时每条接收流只保存一个定长帧头扫描状态。
- `http3_received_data_credit_sink_type`：接收归还的 DATA 额度——应用已消费 `stream_id` 载荷中的 `bytes` 字节，QUIC 拥有者可据此扩展流与连接接收窗口（RFC 9000 §4.1）。在额度对象析构时同步调用，不得抛出或重入编解码器。

`http3_connection_options`：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `max_field_section_size_` | `64 * 1024` | 本端接受的字段节上限（通告为 SETTINGS_MAX_FIELD_SECTION_SIZE）；出站字段节同时受本地与已知对端上限约束，按每个名称、值加 32 字节计 |
| `max_concurrent_streams_` | `100` | 并发请求流上限；为 0 时构造抛 `std::invalid_argument` |
| `qpack_max_dtable_capacity_` | `4096` | 本端 QPACK 解码器动态表容量；超过 `std::size_t` 范围时抛 `std::invalid_argument` |
| `qpack_blocked_streams_` | `16` | 允许被 QPACK 阻塞的流数；超过 `std::size_t` 范围时抛 `std::invalid_argument` |
| `max_stream_output_bytes_` | `256 * 1024` | 每条流从 `submit_data()` 到对端确认之间持有的 DATA 载荷上限；`submit_data()` 只接受放得下的前缀，停滞的对端不能使输出无界排队。为 0 时抛 `std::invalid_argument` |
| `enable_connect_protocol_` | `false` | 服务端通告 SETTINGS_ENABLE_CONNECT_PROTOCOL（RFC 9220 §3）；关闭时带 `:protocol` 的请求是畸形请求 |
| `enable_h3_datagram_` | `false` | 通告 SETTINGS_H3_DATAGRAM（RFC 9297 §2.1.1），`read_datagram()` 才接受 HTTP/3 Datagram；QUIC 拥有者还须通告非零 `max_datagram_frame_size` 传输参数（`http3_quic_connection` 在此项开启时自动完成） |
| `own_data_events_` | `false` | 把 DATA 复制进事件自有存储；`http3_quic_connection` 强制开启，因为 ngtcp2 解密回调存储在 `receive_datagram()` 返回前即失效 |
| `origins_` | 空 | 仅服务端：序列化 origin（RFC 9412 §2），在控制流 SETTINGS 之后立即以 ORIGIN 帧发送。借用至构造完成。客户端非空或内容不合法（载荷超 16384 字节等）时构造抛 `std::invalid_argument` |
| `received_data_credit_sink_` | `nullptr` | 归还 DATA 额度的去向；为空时归还被丢弃。`http3_quic_connection` 安装自己的回收器，并拒绝调用方在此设置 |
| `received_data_credit_context_` | `nullptr` | 回收器上下文 |
| `frame_observer_` | `nullptr` | 帧观察器 |
| `field_observer_` | `nullptr` | QPACK 字段观察器 |
| `observer_context_` | `nullptr` | 两个观察器共用的上下文，借用至连接销毁 |

#### 12.1.4 `class http3_connection`

```cpp
namespace ruvia {

class http3_connection final {
public:
    [[nodiscard]] static http3_connection client(http3_connection_options options = {});
    [[nodiscard]] static http3_connection server(http3_connection_options options = {});

    ~http3_connection();
    http3_connection(const http3_connection&) = delete;
    http3_connection& operator=(const http3_connection&) = delete;
    http3_connection(http3_connection&&) noexcept;
    http3_connection& operator=(http3_connection&&) noexcept;

    [[nodiscard]] http_connection_role role() const noexcept;

    [[nodiscard]] http3_transport_status bind_control_streams(std::int64_t control_stream_id, std::int64_t qpack_encoder_stream_id, std::int64_t qpack_decoder_stream_id);

    [[nodiscard]] http3_transport_result read_stream(std::int64_t stream_id, std::span<const std::byte> bytes, bool fin, std::uint64_t ts_nanoseconds);
    template </* 临时拥有型 std::byte 连续容器 */ typename bytes_type>
        requires(!std::same_as<std::remove_cvref_t<bytes_type>, borrowed_span<std::byte>>)
    http3_transport_result read_stream(std::int64_t, bytes_type&&, bool, std::uint64_t) = delete;
    http3_transport_result read_stream(std::int64_t, std::initializer_list<std::byte>, bool, std::uint64_t) = delete;
    [[nodiscard]] std::optional<http3_event> next_event() &;
    std::optional<http3_event> next_event() && = delete;

    [[nodiscard]] http3_transport_result peek_output(std::size_t max_bytes, http3_stream_output& output);
    [[nodiscard]] http3_transport_status commit_output(std::int64_t stream_id, std::size_t bytes) noexcept;
    [[nodiscard]] http3_transport_status acknowledge_output(std::int64_t stream_id, std::uint64_t bytes) noexcept;
    [[nodiscard]] http3_transport_status block_stream(std::int64_t stream_id) noexcept;
    [[nodiscard]] http3_transport_status unblock_stream(std::int64_t stream_id) noexcept;

    [[nodiscard]] http3_transport_status shutdown_stream_read(std::int64_t stream_id, std::optional<http3_error_code> peer_error = std::nullopt) noexcept;
    [[nodiscard]] http3_transport_status shutdown_stream_write(std::int64_t stream_id, std::optional<http3_error_code> peer_error = std::nullopt) noexcept;
    void set_max_client_streams_bidi(std::uint64_t max_streams) noexcept;

    [[nodiscard]] http_submit_status submit_request_head(std::int64_t stream_id, const http_request_head_view& request);
    [[nodiscard]] http_submit_status submit_websocket_request_head(std::int64_t stream_id, const websocket_client_negotiation& negotiation, std::string_view scheme, std::string_view authority, std::string_view target, borrowed_http_headers headers = {}, http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] http_data_submit_result submit_data(std::int64_t stream_id, std::string_view bytes);
    [[nodiscard]] std::size_t unacknowledged_data_bytes(std::int64_t stream_id) const noexcept;
    [[nodiscard]] bool output_flushed(std::int64_t stream_id) const noexcept;
    [[nodiscard]] std::size_t available_output_bytes(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http_finish_status finish_stream(std::int64_t stream_id);
    [[nodiscard]] http_finish_status finish_request(std::int64_t stream_id, std::span<const http_header_view> trailers = {});

    [[nodiscard]] std::variant<http_buffered_response_write_plan, http_response_head_error> submit_response_head(std::int64_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan);
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http_response_head_error> submit_streaming_response_head(std::int64_t stream_id, http_response head, http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);
    [[nodiscard]] http_submit_status submit_interim_response_head(std::int64_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] http_finish_status finish_response(std::int64_t stream_id, const http_response_trailer_section& trailers);

    [[nodiscard]] http_submit_status submit_connect_response_head(std::int64_t stream_id, const http_response& response);
    [[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(std::int64_t stream_id, const http_request& request, websocket_handshake_validation_options options = {}) const noexcept;
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http_response_head_error> submit_websocket_handshake(std::int64_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options = {});

    [[nodiscard]] http3_transport_result read_datagram(std::span<const std::byte> payload);
    [[nodiscard]] std::optional<http3_datagram_prefix> datagram_prefix(std::int64_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http3_peer_settings> peer_settings() const noexcept;
    [[nodiscard]] bool header_block_in_progress() const noexcept;

    [[nodiscard]] http_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http_submit_status submit_priority_update(std::int64_t stream_id, http_priority_fields fields);
    [[nodiscard]] std::optional<http_priority> stream_priority(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http3_transport_status close_stream(std::int64_t stream_id, std::optional<http3_error_code> receive_error, std::optional<http3_error_code> send_error) noexcept;
    [[nodiscard]] http_submit_status begin_drain() noexcept;
    [[nodiscard]] http_submit_status finish_drain() noexcept;
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] bool drain_final() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::optional<http3_error_code> connection_error() const noexcept;
};

}  // namespace ruvia
```

`http_connection_role`、`http_submit_status`、`http_finish_status`、`http_data_submit_result` 的取值与语义见 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)；响应计划与错误类型见 [`"http_response_body.h"`](#api-http-response-body-h)、[`"http_response_stream.h"`](#api-http-response-stream-h)、[`"http_response.h"`](#api-http-response-h)；WebSocket 类型见 [`"websocket_handshake.h"`](#api-websocket-handshake-h) 与 [`"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)。

构造与生命周期：

| 成员 | 说明 |
| --- | --- |
| `client(options)` / `server(options)` | 按角色创建连接。选项非法时抛 `std::invalid_argument`（条件见 12.1.3 选项表）。 |
| 析构、移动构造/赋值 | 仅可移动（`noexcept`），不可复制。移动后源对象只能析构或被赋值。 |
| `role()` | 返回 `http_connection_role::client` 或 `server`。 |
| `bind_control_streams(control, qpack_encoder, qpack_decoder)` | 绑定本端打开的三条单向 QUIC 流（控制流、QPACK 编码器流、QPACK 解码器流），整个连接只能调用一次；重复或角色/流 ID 不符返回 `invalid_state`。 |

输入与事件：

| 成员 | 说明 |
| --- | --- |
| `read_stream(stream_id, bytes, fin, ts_nanoseconds)` | 交付一段 QUIC 流数据；`fin` 表示该流接收方向在这些字节后结束。`ts_nanoseconds` 必须来自同一单调时钟且不递减。上一次输入的事件未取尽时返回 `events_pending` 且不消费。`credit_bytes_` 为应立即归还的分帧/QPACK 字节。`own_data_events_` 关闭时，输入中的 DATA 视图须保持有效，直到 `next_event()` 取尽该次调用产生的全部事件；显式构造的视图/span 不延长底层所有者寿命。 |
| `read_stream(..., bytes_type&&, ...) = delete` | 拒绝直接传入临时拥有型 `std::byte` 连续容器（例如右值 `std::vector<std::byte>`），否则借用模式下 DATA 事件会悬垂；`borrowed_span<std::byte>` 显式借用视图不受此限制。命名左值容器可正常使用。 |
| `read_stream(..., std::initializer_list<std::byte>, ...) = delete` | 拒绝 `initializer_list` 临时数组，理由同上。 |
| `next_event()` | 取出下一个事件；无事件时返回 `std::nullopt`。传输动作事件以 `origin()` 区分须执行的本地命令与已应用的对端通知。 |

输出与流控反馈（独立驱动直接使用；`http3_quic_connection` 内部完成）：

| 成员 | 说明 |
| --- | --- |
| `peek_output(max_bytes, output)` | 借用某一条 HTTP/3 流至多 `max_bytes` 字节，不推进 nghttp3。缓冲在 `commit_output()` 之前保持有效，QUIC 须保持底层字节存活直到 `acknowledge_output()`；正文块与动态 ORIGIN 帧在确认前地址稳定。无输出返回 `would_block`。 |
| `commit_output(stream_id, bytes)` | 报告 QUIC 实际接受的前缀字节数（必须恰为接受的前缀）。 |
| `acknowledge_output(stream_id, bytes)` | 报告对端已确认的字节数，释放对应借用存储与 `max_stream_output_bytes_` 预算。 |
| `block_stream(id)` / `unblock_stream(id)` | QUIC 流信用耗尽时暂停该流本地发送半边的调度，获得信用后恢复；两者都不改变信用、不提交字节、不恢复应用正文源。连接错误后返回 `closed`。 |
| `shutdown_stream_read(id, peer_error)` | 报告本地接收半边终止。带对端错误码时发布 `peer` 来源的 RESET_STREAM 通知；客户端在最终响应头之前收到 `H3_REQUEST_REJECTED` 时改为发布 `http3_request_unprocessed_event`。`std::nullopt` 应用本地终止且不发布通知。关键接收流（控制/QPACK 流）终止可返回 `protocol_error`，码见 `connection_error()`。 |
| `shutdown_stream_write(id, peer_error)` | 报告本地发送半边终止，带码时发布 `peer` 来源的 STOP_SENDING 通知；其余规则同上。两个 `shutdown_stream_*` 都不发送 QUIC 帧，也不关闭相反半边；最终退役仍须调用 `close_stream()`。连接错误后返回 `closed`。 |
| `set_max_client_streams_bidi(max_streams)` | 仅服务端：QUIC 授予的客户端双向流累计绝对上限（不是增量或并发流数），由传输层单调递增提供；客户端调用无效果。 |
| `close_stream(id, receive_error, send_error)` | QUIC 流最终退役，附带终止两个半边的 RESET_STREAM/STOP_SENDING 应用错误码（如有）。之后发布 `http3_stream_closed_event`。 |

请求提交（客户端）：

| 成员 | 说明 |
| --- | --- |
| `submit_request_head(stream_id, request)` | 在 QUIC 已分配的客户端双向流上打开请求。`request.kind()` 选择形态（RFC 9114 §4.3.1、§4.4，RFC 9220）：普通请求需要 `scheme_` 与 `target_`（`authority_` 可缺省）；CONNECT 只需 `authority_`；extended CONNECT 需要 `protocol_`、`scheme_`、`authority_`、`target_`；`content_` 与 `expectation_` 只属于普通请求。整个头在发布任何内容前按本地与已知对端字段节上限整体校验；伪首部与 Content-Length 等分帧字段由核心生成，凭据字段始终 never-index；公开 API 不提供原始字段节提交。流保持开启时安装拉取源，`submit_data()` 与结束调用恢复它，不保留调用方视图。返回：非客户端、非客户端双向流或重复提交为 `invalid_state`；无法以 HTTP/3 表示为 `invalid_message`；extended CONNECT 在对端 SETTINGS（或 0-RTT 记忆值）到达前为 `peer_settings_pending`，对端未启用时为 `peer_capability_unavailable`。 |
| `submit_websocket_request_head(stream_id, negotiation, scheme, authority, target, headers, source)` | RFC 9220 `:protocol websocket` 的 extended CONNECT：先写入协商产生的 `Sec-WebSocket-*` 请求字段，再追加应用首部 `headers`；与 `http2_connection::submit_websocket_request_head()` 同名、共用同一字段合成。返回值规则同 `submit_request_head()`。最终响应头用 `validate_extended_connect_websocket_client_response()`（见 [`"websocket_client_handshake.h"`](#api-websocket-client-handshake-h)）校验。 |
| `finish_request(stream_id, trailers)` | 客户端结束请求内容；`trailers` 非空时发送 trailer 字段节，字段必须是发送方可放入请求 trailer 的合法字段（RFC 9110 §6.5.1）且在字段节上限内。内容未开启时抛 `std::logic_error`。 |

正文提交（两种角色）：

| 成员 | 说明 |
| --- | --- |
| `submit_data(stream_id, bytes)` | 复制放得进该流输出预算的前缀；空输入视为完成且无效果。若头部声明了内容长度，超出部分返回 `invalid_message` 且不复制任何字节。流内容未开启（无头、头已结束流、内容已结束）时抛 `std::logic_error`。 |
| `unacknowledged_data_bytes(stream_id)` | 已被 `submit_data()` 接受、尚未经 `acknowledge_output()` 确认的 DATA 载荷字节；无论 QUIC 是否已发送，均计入 `max_stream_output_bytes_`。 |
| `output_flushed(stream_id)` | 当前排队的 HEADERS/DATA/trailer 是否已全部交给 QUIC；不表示对端已确认，也不禁止继续提交正文。已移除的流返回 `true`。 |
| `available_output_bytes(stream_id)` | 剩余 DATA 容量，是该值的唯一权威；不可用或已结束的流返回 0。 |
| `finish_stream(stream_id)` | 结束任一角色的内容（包括隧道）；内容未开启时抛 `std::logic_error`。 |

响应提交（服务端，RFC 9114 §4.1–§4.3）：

| 成员 | 说明 |
| --- | --- |
| `submit_response_head(stream_id, response, write_plan)` | 提交最终头：`:status`、小写名称的响应字段（§4.2）、响应缺 Date 时生成 Date、计划推导的 Content-Length；连接专属字段被拒绝。计划需要发送内容时流保持开启，`submit_data()` 总量必须等于计划长度后才能 `finish_stream()`。成功返回计划，失败返回 `http_response_head_error`。 |
| `submit_streaming_response_head(stream_id, head, trailer_intent)` | 流式最终头：应用给出的 Content-Length 成为精确内容总量，否则内容无界。HEAD 请求、204、304 等抑制正文的响应随头结束流。请求方法使用该流收到的 `:method`，不由调用方传入。 |
| `submit_interim_response_head(stream_id, response)` | 最终头之前的 1xx 响应（§4.1）；字段节超出本地或对端上限返回 `invalid_message`。 |
| `finish_response(stream_id, trailers)` | 结束流式响应：空字段节仅结束内容，否则作为尾随 HEADERS 帧发送；内容未开启时抛 `std::logic_error`。 |
| `submit_connect_response_head(stream_id, response)` | 以不分帧内容的 2xx 头（无 Content-Length、无 Transfer-Encoding、无正文）接受 CONNECT（§4.4）或 extended CONNECT（RFC 9220）；之后流双向开启，隧道字节作为 DATA 传输（`submit_data()`、`finish_stream()`）。其他应答用 `submit_response_head()`。该流未收到 CONNECT 头或已应答返回 `invalid_state`；头不能接受隧道返回 `invalid_message`。 |
| `validate_websocket_handshake(stream_id, request, options)` | 校验 RFC 9220 extended CONNECT WebSocket 引导：该流收到了 `:protocol websocket` 的 CONNECT 头、流保持开启且未声明内容，且 `request` 带可接受的 `Sec-WebSocket-*` 字段。`request` 只需在调用期间存活。 |
| `submit_websocket_handshake(stream_id, request, validation, options)` | 接受经 `validate_websocket_handshake()` 接受的引导：200 头携带协商的子协议与 permessage-deflate 参数，流随后像 `submit_connect_response_head()` 一样成为隧道。成功返回 `websocket_extended_connect_negotiation`。 |

数据报、设置与扩展帧：

| 成员 | 说明 |
| --- | --- |
| `read_datagram(payload)` | 一个 QUIC DATAGRAM 帧（RFC 9221）载荷，内含 HTTP/3 Datagram。目标流的头尚未解码或已关闭时丢弃（RFC 9297 §2.1）；畸形数据报，或 `enable_h3_datagram_` 关闭时收到任意数据报，均为 `H3_DATAGRAM_ERROR` 连接错误。成功时发布 `http3_datagram_event`。 |
| `datagram_prefix(stream_id)` | 该流 HTTP/3 Datagram 的 Quarter Stream ID 前缀；仅当双方均启用（RFC 9297 §2.1.1）且该流是本编解码器仍在跟踪的客户端双向流时有值。 |
| `peer_settings()` | 对端 SETTINGS 到达前为 `std::nullopt`。 |
| `header_block_in_progress()` | 是否有接收字段节已开始 QPACK 解码（`begin`）而尚未完成或被放弃；实现为活动接收字段节计数非零。 |
| `submit_origin_advertisement(origins)` | 仅服务端：在已绑定的控制流上排队一个 RFC 9412 ORIGIN 帧。 |
| `submit_priority_update(stream_id, fields)` | 仅客户端：为开启中的客户端双向请求流在控制流上排队 RFC 9218 PRIORITY_UPDATE 帧；`http_priority_fields` 见 [`"http_priority.h"`](#api-http-priority-h)。服务端的响应优先级通过响应 `Priority` 字段表达。 |
| `stream_priority(stream_id)` | 仅服务端：开启中请求流当前的 RFC 9218 优先级（综合请求 `Priority` 字段与 PRIORITY_UPDATE）。 |

排空与连接错误（RFC 9114 §5.2）：

| 成员 | 说明 |
| --- | --- |
| `begin_drain()` | 服务端优雅关闭第一步：发送仍接纳在途请求的 shutdown-notice GOAWAY。连接错误后返回 `closed`。 |
| `finish_drain()` | 至少一个往返后调用：发送最终 GOAWAY，指明不会被处理的首个请求流。连接错误后返回 `closed`。 |
| `draining()` | 自一次被接受的 `begin_drain()` / `finish_drain()`，或收到对端 GOAWAY 起为 `true`。 |
| `drain_final()` | 本端 `finish_drain()` 被接受后为 `true`。与 HTTP/2 不同，对端 GOAWAY 不会使本地排空成为最终。 |
| `drained()` | `drain_final()` 且低于最终 GOAWAY 标识的所有客户端请求流已关闭（nghttp3_conn_is_drained2）。只跟随本端 `finish_drain()`；仅收到对端 GOAWAY 时保持 `false`，拥有者在自己的请求完成后自行结束连接。 |
| `connection_error()` | 连接失败时的 HTTP/3 错误码，供 CONNECTION_CLOSE 使用；未失败为 `std::nullopt`。 |

服务端对每个收到的请求头、客户端对每个响应头与 trailer 字段节进行校验；畸形消息是 `H3_MESSAGE_ERROR` 流错误（RFC 9114 §4.1.2），编解码器以本地 STOP_SENDING 与 RESET_STREAM 传输动作回应，驱动无需自行重置。核心校验 Content-Length 与实际字节数及终结的一致性。

#### 12.1.5 `class http3_fingerprint_collector`

```cpp
namespace ruvia {

class http3_fingerprint_collector final {
public:
    explicit http3_fingerprint_collector(http_fingerprint_options options = {});

    void observe_frame(const http3_frame_metadata& frame) noexcept;
    void observe_field(const http3_field_metadata& field) noexcept;
    void reset() noexcept;

    static void on_frame(void* context, const http3_frame_metadata& frame) noexcept;
    static void on_field(void* context, const http3_field_metadata& field) noexcept;

    [[nodiscard]] constexpr bool complete() const noexcept;
    [[nodiscard]] constexpr bool failed() const noexcept;
    [[nodiscard]] std::string_view fingerprint() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view fingerprint() const&& = delete;
};

}  // namespace ruvia
```

收集直到客户端首个请求头块结束为止的 HTTP/3 帧与 QPACK 字段观察，生成确定性指纹字符串 `CONTROL_FRAMES|FIRST_STREAM_ID|PSEUDO_HEADER_ORDER`。

| 成员 | 说明 |
| --- | --- |
| 构造函数 | `http_fingerprint_options`（见 [`"http_multiplexed_stream.h"`](#api-http-multiplexed-stream-h)，默认 `max_frames_ = 32`、`max_bytes_ = 16384`）限定首个请求头块完成前可观察的帧数与帧/字段字节数，超出即失败。 |
| `observe_frame(frame)` | 记录客户端控制流（流 ID 模 4 余 2）上的帧，格式为 `类型十进制:载荷长度`，多帧以 `;` 分隔。 |
| `observe_field(field)` | 只处理 `header` 字段节：首个客户端双向流（流 ID 模 4 余 0）的伪首部顺序，`:method`→`m`、`:authority`→`a`、`:scheme`→`s`、`:path`→`p`、`:protocol`→`pr`，其余伪首部取名称首字母，以 `,` 分隔；伪首部出现在普通字段之后、首个字段节位于非客户端双向流、或节结束时无伪首部均失败。该节 `end` 时生成指纹并进入完成态。 |
| `reset()` | 清空全部状态，回到收集态（保留预算）。 |
| `on_frame` / `on_field` | 可直接用作 `http3_connection_options::frame_observer_` / `field_observer_` 的静态适配函数，`context` 必须指向该收集器（设为 `observer_context_`）。 |
| `complete()` / `failed()` | 是否已生成指纹 / 是否因预算或格式失败（失败后不再收集，直到 `reset()`）。内部分配失败也转为失败态，从不抛出。 |
| `fingerprint()` | 完成时返回借用收集器的指纹，否则为空；`const&&` 删除以拒绝临时收集器。 |

```cpp
ruvia::http3_fingerprint_collector collector;
ruvia::http3_connection_options options;
options.frame_observer_ = &ruvia::http3_fingerprint_collector::on_frame;
options.field_observer_ = &ruvia::http3_fingerprint_collector::on_field;
options.observer_context_ = &collector;  // collector 须比连接活得久
auto connection = ruvia::http3_connection::server(options);
// ... read_stream() 之后：
if (collector.complete()) {
    std::string_view fp = collector.fingerprint();  // 形如 "控制流帧|首个请求流 ID|伪首部顺序"
}
```

### 12.2 `"http3_request_head.h"`
<a id="api-http3-request-head-h"></a>

服务端 `http3_connection` 以 `http3_request_head_event` 发布的、已验证的 HTTP/3 请求头（RFC 9114 §4.3.1）。仅 `RUVIA_ENABLE_HTTP3`。公开 include [`"http_request.h"`](#api-http-request-h)、[`"http_request_expectation.h"`](#api-http-request-expectation-h)、[`"http_header.h"`](#api-http-header-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http3_request_head.h`](../ruvia-http/include/http3_request_head.h)

#### 12.2.1 `class http3_request_head`

```cpp
namespace ruvia {

class http3_connection;

class http3_request_head final {
public:
    http3_request_head(http3_request_head&&) noexcept = default;
    http3_request_head& operator=(http3_request_head&&) = delete;
    http3_request_head(const http3_request_head&) = delete;
    http3_request_head& operator=(const http3_request_head&) = delete;

    [[nodiscard]] std::int64_t stream_id() const noexcept;
    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view scheme() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view scheme() const&& = delete;
    [[nodiscard]] std::string_view authority() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view authority() const&& = delete;
    [[nodiscard]] std::string_view path() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view path() const&& = delete;
    [[nodiscard]] std::string_view protocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view protocol() const&& = delete;
    [[nodiscard]] bool connect() const noexcept;
    [[nodiscard]] bool stream_ended() const noexcept;
    [[nodiscard]] bool early_data() const noexcept;
    [[nodiscard]] http_request_content_indication content() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> declared_content_length() const noexcept;
    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy policy) const noexcept;

    [[nodiscard]] http_request build_request(borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(borrowed_text body = {}) const&& = delete;
};

}  // namespace ruvia
```

验证分两层：nghttp3 拒绝 RFC 9114 §4.1.2、§4.3.1 的帧层违规（大写名称，伪首部错位、重复或缺失，连接专属字段，TE 非 `trailers`，畸形 Content-Length）；连接再施加 ruvia-http 各 HTTP 版本共享的请求语义（目标语法、`:authority` 与 `Host` 一致、接收方字段规则、Expect、Cookie 合并）。无法表示为 `http_request` 的头是畸形请求，由连接自行以 `H3_MESSAGE_ERROR` 回应（§4.1.2），不会发布该头。构造函数私有，只由 `http3_connection` 创建。

| 成员 | 说明 |
| --- | --- |
| 移动构造 | 唯一可用的特殊成员：不可复制、不可复制/移动赋值。`std::optional<http3_request_head>` 持有点须用 `emplace` 重建。 |
| `stream_id()` | 请求所在 QUIC 双向流 ID。 |
| `method()` / `scheme()` / `authority()` / `path()` / `protocol()` | 对应伪首部 `:method`、`:scheme`、`:authority`、`:path`、`:protocol` 的值，缺省时为空视图；借用本头，`const&&` 重载删除以拒绝临时头。 |
| `connect()` | 方法为 CONNECT（无论是否带 `:protocol`，RFC 9114 §4.4、RFC 9220）。 |
| `stream_ended()` | HEADERS 帧已结束请求流，没有内容跟随。 |
| `early_data()` | 请求头来自 0-RTT 早期数据（实际传输来源）。TLS 恢复并接受早期数据不保证某请求属于 0-RTT（发送节拍或重传可能使请求在握手完成后才到达），应用应读取该请求的此标记而非连接的 TLS 协商结果；也不能由请求字段替代。 |
| `content()` | `stream_ended()` 或声明长度为 0 时为 `http_request_content_indication::no_content`，否则为 `will_follow`（见 [`"http_request_expectation.h"`](#api-http-request-expectation-h)）。 |
| `declared_content_length()` | 声明的 Content-Length；未声明为 `std::nullopt`。 |
| `expectation_plan(policy)` | 按本请求的 Expect 与 `content()` 及给定的未支持期望策略生成服务端期望计划（同 [`"http_request_expectation.h"`](#api-http-request-expectation-h) 的 `server_plan`）。 |
| `build_request(body)` | 构造 [`http_request`](#api-http-request-h)；其中视图借用本头，本头必须比请求活得久。`body` 为借用正文。`const&&` 重载删除，拒绝在临时头上构造悬垂请求。 |

实现特性：普通字段索引在首次使用时按剩余字段上界一次预留，仅含伪首部与 Cookie 的请求不分配该索引；多个 Cookie 字段时，首个非空碎片直接借用已拥有字段，第二个非空碎片出现时才分配合并缓冲，空碎片不参与合并。

```cpp
while (auto event = connection.next_event()) {
    if (auto* head_event = event->request_head()) {
        ruvia::http3_request_head head = std::move(*head_event).take_head();
        ruvia::http_request request = head.build_request();  // head 须比 request 活得久
        // ...
    }
}
```

### 12.3 `"http3_quic_connection.h"`
<a id="api-http3-quic-connection-h"></a>

一条 QUIC v1 + HTTP/3 连接：ngtcp2 负责包处理、丢包恢复与流量控制，内部的 [`http3_connection`](#api-http3-connection-h) 负责 QPACK 与 HTTP 流分帧；调用方拥有 UDP socket 与 TLS。仅 `RUVIA_ENABLE_HTTP3`，并公开依赖 ngtcp2 core 头 `<ngtcp2/ngtcp2.h>`。公开 include [`"http3_connection.h"`](#api-http3-connection-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http3_quic_connection.h`](../ruvia-http/include/http3_quic_connection.h)

#### 12.3.1 状态、结果与回调结构

```cpp
namespace ruvia {

enum class http3_quic_status : std::uint8_t {
    accepted,
    would_block,
    closed,
    invalid_state,
    transport_error,
    application_error,
};

struct http3_quic_result final {
    http3_quic_status status_{http3_quic_status::accepted};
    std::optional<std::uint64_t> transport_error_code_{};
    std::optional<http3_error_code> application_error_code_{};
};

using http3_quic_stateless_reset_token = std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN>;

struct http3_quic_crypto_callbacks final {
    ngtcp2_client_initial client_initial_{nullptr};
    ngtcp2_recv_client_initial recv_client_initial_{nullptr};
    ngtcp2_recv_crypto_data recv_crypto_data_{nullptr};
    ngtcp2_encrypt encrypt_{nullptr};
    ngtcp2_decrypt decrypt_{nullptr};
    ngtcp2_hp_mask hp_mask_{nullptr};
    ngtcp2_recv_retry recv_retry_{nullptr};
    ngtcp2_update_key update_key_{nullptr};
    ngtcp2_delete_crypto_aead_ctx delete_crypto_aead_ctx_{nullptr};
    ngtcp2_delete_crypto_cipher_ctx delete_crypto_cipher_ctx_{nullptr};
    ngtcp2_get_path_challenge_data2 get_path_challenge_data_{nullptr};
    ngtcp2_version_negotiation version_negotiation_{nullptr};
    ngtcp2_rand rand_{nullptr};
};

struct http3_quic_owner_callbacks final {
    ngtcp2_get_new_connection_id2 get_new_connection_id_{nullptr};
    ngtcp2_remove_connection_id remove_connection_id_{nullptr};
    ngtcp2_handshake_completed handshake_completed_{nullptr};
    ngtcp2_handshake_confirmed handshake_confirmed_{nullptr};
    ngtcp2_recv_stateless_reset2 recv_stateless_reset_{nullptr};
    ngtcp2_tls_early_data_rejected tls_early_data_rejected_{nullptr};
};

}  // namespace ruvia
```

`http3_quic_status`（底层 `std::uint8_t`）：

| 枚举值 | 含义 |
| --- | --- |
| `accepted` | 调用成功 |
| `would_block` | 暂无可写包，或数据报队列已满 |
| `closed` | 连接已进入关闭或排空期（RFC 9000 §10.2）：已发送或收到 CONNECTION_CLOSE，或空闲超时。不再处理任何包；拥有者保留连接只为在 `close_period()` 内吸收迟到的对端包 |
| `invalid_state` | 调用不符合角色或当前状态（如不支持/超长的数据报） |
| `transport_error` | 连接因 QUIC 传输错误失败 |
| `application_error` | 连接因 HTTP/3 应用错误失败 |

`http3_quic_result`：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `status_` | `accepted` | 上表状态 |
| `transport_error_code_` | 空 | `transport_error` 或传输层关闭时：结束连接的 CONNECTION_CLOSE（本端发送或收到，RFC 9000 §19.19）携带的 QUIC 传输错误码（§20.1；TLS alert 为 0x0100–0x01ff） |
| `application_error_code_` | 空 | `application_error` 或应用层关闭时：CONNECTION_CLOSE 携带的 HTTP/3 码 |

连接开启期间两者都为空；空闲超时或无状态重置结束的连接也都为空。

`http3_quic_stateless_reset_token`：长度为 `NGTCP2_STATELESS_RESET_TOKENLEN`（16）的无状态重置令牌（RFC 9000 §10.3）。

`http3_quic_crypto_callbacks`：QUIC-TLS 适配器回调（例如 `ngtcp2_crypto_ossl` 的 `ngtcp2_crypto_*_cb`）。连接原样安装，回调收到连接自己的 user data（适配器不读取它）。`client_initial_` 与 `recv_retry_` 仅客户端需要，`recv_client_initial_` 仅服务端需要，其余字段两种角色都必须设置。每个字段原样赋给内部 `ngtcp2_callbacks` 的同名成员（去掉尾部 `_`；`get_path_challenge_data_` 赋给 `get_path_challenge_data2`）。所有流、确认与关闭回调由连接独占，无法传入。

`http3_quic_owner_callbacks`：拥有者生命周期通知；设置时均收到 `http3_quic_connection_options::callback_context_` 作为 user data，返回非零值按 ngtcp2 对该回调的文档使连接失败。

| 字段 | 含义 |
| --- | --- |
| `get_new_connection_id_` | 签发新连接 ID 及其无状态重置令牌（RFC 9000 §5.1.1） |
| `remove_connection_id_` | 连接 ID 退役 |
| `handshake_completed_` | 握手完成 |
| `handshake_confirmed_` | 握手确认 |
| `recv_stateless_reset_` | 收到无状态重置 |
| `tls_early_data_rejected_` | 在连接丢弃被拒绝的 0-RTT HTTP/3 流之后调用 |

#### 12.3.2 选项、路径与数据报

```cpp
namespace ruvia {

struct http3_quic_connection_options final {
    http3_quic_crypto_callbacks crypto_callbacks_{};
    http3_quic_owner_callbacks owner_callbacks_{};
    ngtcp2_cid source_connection_id_{};
    ngtcp2_cid destination_connection_id_{};
    std::optional<ngtcp2_cid> original_destination_connection_id_{};
    std::optional<ngtcp2_cid> retry_source_connection_id_{};
    borrowed_span<std::uint8_t> retry_token_{};
    std::optional<http3_quic_stateless_reset_token> stateless_reset_token_{};
    bool disable_active_migration_{false};
    ngtcp2_path initial_path_{};
    std::uint32_t version_{NGTCP2_PROTO_VER_V1};
    ngtcp2_tstamp initial_timestamp_{0};
    ngtcp2_duration handshake_timeout_{30 * NGTCP2_SECONDS};
    ngtcp2_duration idle_timeout_{30 * NGTCP2_SECONDS};
    std::uint64_t initial_max_data_{16 * 1024 * 1024};
    std::uint64_t initial_max_stream_data_{1024 * 1024};
    std::uint64_t initial_max_streams_bidi_{100};
    std::uint64_t initial_max_streams_uni_{16};
    std::uint64_t active_connection_id_limit_{4};
    std::size_t max_udp_payload_size_{1350};
    std::size_t max_pending_datagram_bytes_{64 * 1024};
    void* callback_context_{nullptr};
    http3_connection_options http3_{};
};

struct http3_quic_path final {
    std::span<const std::byte> local_;
    std::span<const std::byte> remote_;
};

struct http3_quic_datagram final {
    http3_quic_datagram() noexcept;
    http3_quic_datagram(const http3_quic_datagram&) = delete;
    http3_quic_datagram& operator=(const http3_quic_datagram&) = delete;
    http3_quic_datagram(http3_quic_datagram&&) = delete;
    http3_quic_datagram& operator=(http3_quic_datagram&&) = delete;

    ngtcp2_path_storage path_{};
    std::uint32_t ecn_{NGTCP2_ECN_NOT_ECT};
    std::vector<std::byte> bytes_;
};

}  // namespace ruvia
```

`http3_quic_connection_options`：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `crypto_callbacks_` | 全空 | QUIC-TLS 适配器回调（12.3.1） |
| `owner_callbacks_` | 全空 | 拥有者回调（12.3.1） |
| `source_connection_id_` | 空 | 本端 Source Connection ID；必须非空 |
| `destination_connection_id_` | 空 | 目标 Connection ID；客户端必须非空 |
| `original_destination_connection_id_` | 空 | 仅服务端且必需（RFC 9000 §7.3）：客户端首个 Initial 的 Destination Connection ID（使用 Retry 时从 Retry 令牌中取出） |
| `retry_source_connection_id_` | 空 | 仅服务端：Retry 包中发送、客户端在第二个 Initial 中回显为 Destination Connection ID 的 Source Connection ID（RFC 9000 §7.3、§18.2）；与非空 `retry_token_` 必须同时出现且本身非空 |
| `retry_token_` | 空 | 仅服务端：来自客户端第二个 Initial 的已验证 Retry 令牌（RFC 9000 §8.1.2），解除 ngtcp2 的抗放大限制；借用至构造完成 |
| `stateless_reset_token_` | 空 | 仅服务端：`source_connection_id_` 的无状态重置令牌（RFC 9000 §10.3），作为传输参数发送 |
| `disable_active_migration_` | `false` | 仅服务端：禁止客户端连接迁移（RFC 9000 §9）。UDP 拥有者跨路径按连接 ID 路由时保持 `false` |
| `initial_path_` | 空 | 初始路径；本地与远端地址都必须设置 |
| `version_` | `NGTCP2_PROTO_VER_V1` | QUIC 版本 |
| `initial_timestamp_` | `0` | 创建时刻（ngtcp2 时间戳） |
| `handshake_timeout_` | `30 * NGTCP2_SECONDS` | 握手超时 |
| `idle_timeout_` | `30 * NGTCP2_SECONDS` | 空闲超时 |
| `initial_max_data_` | `16 * 1024 * 1024` | 连接级初始接收窗口 |
| `initial_max_stream_data_` | `1024 * 1024` | 流级初始接收窗口 |
| `initial_max_streams_bidi_` | `100` | 对端可打开的双向流初始上限 |
| `initial_max_streams_uni_` | `16` | 对端可打开的单向流初始上限 |
| `active_connection_id_limit_` | `4` | 活动连接 ID 上限，须不小于 2；为当前路径与旧路径验证并存时的新候选预留 CID（RFC 9000 §5.1.1） |
| `max_udp_payload_size_` | `1350` | 发送 UDP 载荷上限；须非零 |
| `max_pending_datagram_bytes_` | `64 * 1024` | 在 ngtcp2 接受前保留的不可靠输出（HTTP Datagram）字节上限；须非零 |
| `callback_context_` | `nullptr` | 拥有者回调上下文，`callback_context()` 返回 |
| `http3_` | 默认 | 内部 [`http3_connection_options`](#api-http3-connection-h)。连接安装自己的 `received_data_credit_sink_`（归还的 `http_received_data_credit` 扩展流与连接接收窗口，RFC 9000 §4.1，DATA 额度只在应用消费后归还，分帧字节立即归还）；在此设置回收器会被拒绝。`own_data_events_` 被强制开启，`enable_h3_datagram_` 开启时自动通告非零 `max_datagram_frame_size` |

上述“须”条件不满足、或服务端专属字段与角色不匹配（服务端缺 `original_destination_connection_id_`，客户端设置了任一服务端专属字段），工厂函数抛 `std::invalid_argument`；ngtcp2 拒绝选项时同样抛 `std::invalid_argument`。

`http3_quic_path`：一条 QUIC 路径（RFC 9000 §9）的本地与远端套接字地址，按平台 `sockaddr` 布局的字节视图。

`http3_quic_datagram`：`next_datagram()` 的输出槽，不可复制也不可移动，由调用方持有并在多次调用间复用。默认构造以 `ngtcp2_path_storage_zero` 初始化 `path_`。`path_` 为该包应发送的路径（迁移期间可能与当前路径不同）、`ecn_` 为 ECN 标记（默认 `NGTCP2_ECN_NOT_ECT`）、`bytes_` 为拥有的包字节。

#### 12.3.3 `class http3_quic_connection`

```cpp
namespace ruvia {

class http3_quic_connection final {
public:
    [[nodiscard]] static http3_quic_connection client(const http3_quic_connection_options& options);
    [[nodiscard]] static http3_quic_connection server(const http3_quic_connection_options& options);

    ~http3_quic_connection();
    http3_quic_connection(const http3_quic_connection&) = delete;
    http3_quic_connection& operator=(const http3_quic_connection&) = delete;
    http3_quic_connection(http3_quic_connection&&) noexcept;
    http3_quic_connection& operator=(http3_quic_connection&&) noexcept;

    [[nodiscard]] http_connection_role role() const noexcept;
    [[nodiscard]] ngtcp2_conn* native_handle() noexcept;
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept;
    [[nodiscard]] void* callback_context() const noexcept;
    void set_tls_native_handle(void* tls_native_handle) noexcept;
    [[nodiscard]] bool handshake_completed() const noexcept;
    [[nodiscard]] bool after_retry() const noexcept;
    [[nodiscard]] bool stateless_reset_received() const noexcept;

    [[nodiscard]] http3_quic_status start_http3();
    [[nodiscard]] bool http3_started() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> open_request_stream();

    [[nodiscard]] http3_quic_status start_early_http3(std::span<const std::byte> transport_parameters, http3_peer_settings settings);
    [[nodiscard]] http3_quic_status reject_early_data();
    [[nodiscard]] bool early_data_rejected() const noexcept;
    [[nodiscard]] std::optional<std::size_t> encode_early_transport_parameters(std::span<std::byte> output) const noexcept;
    [[nodiscard]] http3_quic_path current_path() const& noexcept RUVIA_LIFETIMEBOUND;
    http3_quic_path current_path() const&& = delete;
    [[nodiscard]] http3_quic_result migrate(const ngtcp2_path& path, ngtcp2_tstamp timestamp);
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);
    [[nodiscard]] std::size_t max_datagram_payload_size(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http3_quic_result receive_datagram(const ngtcp2_path& path, std::uint32_t ecn, std::span<const std::byte> bytes, ngtcp2_tstamp timestamp);
    [[nodiscard]] http3_quic_result next_datagram(std::size_t capacity, ngtcp2_tstamp timestamp, http3_quic_datagram& output);
    [[nodiscard]] http3_quic_result expire_timers(ngtcp2_tstamp timestamp);
    [[nodiscard]] ngtcp2_tstamp expiry() const noexcept;

    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    void reset_send_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    void stop_receiving(std::int64_t stream_id, http3_error_code error) noexcept;
    void close(http3_error_code error) noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] ngtcp2_duration close_period() const noexcept;

    [[nodiscard]] std::optional<http3_event> next_event() &;
    std::optional<http3_event> next_event() && = delete;
    [[nodiscard]] http3_connection& http3() noexcept;
    [[nodiscard]] const http3_connection& http3() const noexcept;
    [[nodiscard]] std::optional<http3_error_code> application_error() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> transport_error() const noexcept;
};

[[nodiscard]] std::size_t write_http3_quic_stateless_reset(const http3_quic_stateless_reset_token& token, std::span<const std::uint8_t> random_bytes, std::span<std::byte> output, std::size_t max_packet_bytes) noexcept;

}  // namespace ruvia
```

驱动约定：输入数据报必须保持存活，直到 `next_event()` 取尽其产生的事件。写出过程中也可能发布事件（例如确认使流关闭），因此拥有者应在每次 `receive_datagram()`、`next_datagram()` 与 `expire_timers()` 之后取尽 `next_event()`。DATA 在 `receive_datagram()` 返回前已复制到事件自有存储（`own_data_events_` 强制开启）。`http3_transport_action_event` 中 `local` 来源的动作在发布前已执行，`http3_deferred_consume_event` 在内部处理、不会发布。ngtcp2/nghttp3 回调中捕获的异常（包括内存分配失败）由触发它的成员调用重新抛出。

基本访问与 TLS 接入：

| 成员 | 说明 |
| --- | --- |
| `client(options)` / `server(options)` | 创建连接；选项非法抛 `std::invalid_argument`（见 12.3.2）。 |
| 析构、移动构造/赋值 | 仅可移动（`noexcept`），不可复制。 |
| `role()` | 连接角色。 |
| `native_handle()` | 底层 `ngtcp2_conn*`，仅供 QUIC-TLS 适配器的 `ngtcp2_crypto_conn_ref::get_conn` 使用；其他能力都有对应成员。 |
| `callback_context()` | 返回选项中的 `callback_context_`。 |
| `set_tls_native_handle(handle)` | 设置交给 ngtcp2 的 TLS 原生句柄（如 OpenSSL `SSL*`）。 |
| `handshake_completed()` / `after_retry()` / `stateless_reset_received()` | 握手是否完成 / 客户端是否经历了 Retry / 是否收到无状态重置。 |

HTTP/3 启动与 0-RTT：

| 成员 | 说明 |
| --- | --- |
| `start_http3()` | TLS 建立应用密钥后，打开并绑定本端 HTTP/3 控制流与两条 QPACK 流；仅在返回 `would_block` 时可安全重试。 |
| `http3_started()` | HTTP/3 是否已启动。 |
| `open_request_stream()` | 客户端打开一条新的双向请求流并返回其 ID；当前不可打开时为 `std::nullopt`。随后经 `http3().submit_request_head()` 提交请求头。 |
| `start_early_http3(transport_parameters, settings)` | 打开 0-RTT 流之前，从票据恢复经认证保存的传输参数与对端 SETTINGS；TLS 也必须启用早期数据。 |
| `reject_early_data()` | TLS 拒绝早期数据时，丢弃被拒绝的 HTTP/3 流；从不重放请求，是否在 1-RTT 中重试由调用方显式决定。 |
| `early_data_rejected()` | 客户端：TLS 已拒绝早期数据，无论经 `reject_early_data()` 报告还是由 QUIC-TLS 适配器在握手中报告。 |
| `encode_early_transport_parameters(output)` | 编码 0-RTT 依赖的传输参数（RFC 9000 §7.4.1）。客户端取收到的服务端参数，与 TLS 会话票据一同保存并供 `start_early_http3()` 恢复；服务端取本端参数，供票据的恢复检查绑定。返回编码长度；客户端尚未收到或 `output` 过小时为 `std::nullopt`。 |

服务端判断某请求是否来自 0-RTT，应使用 [`http3_request_head::early_data()`](#api-http3-request-head-h)。

路径、迁移与 UDP I/O：

| 成员 | 说明 |
| --- | --- |
| `current_path()` | 当前发送路径（含已验证的迁移，RFC 9000 §9），视图借用连接存储，直到下一次处理输入、写出输出或迁移的调用；`const&&` 删除。 |
| `migrate(path, timestamp)` | 仅客户端、QUIC v1：先验证新路径再切换。调用方必须按输出数据报的 `path_` 发送，并在验证期间保持新旧路径都在接收。 |
| `receive_datagram(path, ecn, bytes, timestamp)` | 输入一个从 `path` 收到的 UDP 数据报及其 ECN 标记。 |
| `next_datagram(capacity, timestamp, output)` | 连接开启时写出下一个包（至多 `capacity` 字节）到 `output`，无可写时返回 `would_block`；本地关闭后只写一次 CONNECTION_CLOSE 包，之后返回 `closed`。 |
| `expire_timers(timestamp)` / `expiry()` | 处理到期的定时器 / 下一次到期时刻；拥有者在 `expiry()` 设定计时器。 |

HTTP Datagram：

| 成员 | 说明 |
| --- | --- |
| `submit_datagram(stream_id, payload)` | 载荷位于 HTTP Datagram Quarter Stream ID 之后；连接复制并有界保存（`max_pending_datagram_bytes_`）。不支持或超长返回 `invalid_state`，队列满返回 `would_block`。 |
| `max_datagram_payload_size(stream_id)` | 该流当前可发送的最大载荷；不支持时为 0。 |

流与连接终止：

| 成员 | 说明 |
| --- | --- |
| `reset_stream(id, error)` | 以 RESET_STREAM 与 STOP_SENDING 携带 `error` 放弃请求流的两个方向（RFC 9114 §4.1.1）。 |
| `reset_send_stream(id, error)` | 只以 RESET_STREAM 结束发送半边，接收半边继续交付（RFC 9000 §3.1、RFC 9114 §4.1）。 |
| `stop_receiving(id, error)` | 停止读取不再需要其剩余内容的请求（响应已完整时 RFC 9114 §4.1.2 允许 `H3_NO_ERROR`）。 |
| `close(error)` | 请求有序关闭：下一次 `next_datagram()` 写出携带 `error` 的 CONNECTION_CLOSE（RFC 9000 §10.2；HTTP/3 优雅关闭用 `H3_NO_ERROR`，RFC 9114 §5.2）。 |
| `closed()` | 已进入关闭或排空期（同 `http3_quic_status::closed`）。 |
| `close_period()` | 关闭后吸收迟到包的时长：当前 PTO 的三倍（RFC 9000 §10.2）。 |

事件与错误：

| 成员 | 说明 |
| --- | --- |
| `next_event()` | 取出下一个 [`http3_event`](#api-http3-connection-h)。 |
| `http3()` | 内部 `http3_connection` 的引用，用于提交请求/响应、正文、排空等全部 HTTP/3 操作；引用借用本对象。 |
| `application_error()` | 本连接发送或收到的 CONNECTION_CLOSE、或其失败所因的编解码错误的 HTTP/3 码。 |
| `transport_error()` | 本连接发送或收到的 CONNECTION_CLOSE 的 QUIC 传输错误码（RFC 9000 §20.1）。 |

`write_http3_quic_stateless_reset(token, random_bytes, output, max_packet_bytes)`：向 `output` 写出一个 QUIC Stateless Reset 包（RFC 9000 §10.3），至多 `max_packet_bytes` 字节；应传入 `触发数据报长度 - 1`，使其严格小于触发它的数据报（§10.3.3）。返回写入字节数；`output`、`max_packet_bytes` 或 `random_bytes` 不足以构成合法 Stateless Reset 时返回 0。

最小驱动循环（示意）：

```cpp
// options: http3_quic_connection_options；path: ngtcp2_path；ecn: std::uint32_t；
// bytes: std::span<const std::byte>；now: ngtcp2_tstamp；send_to 为调用方的 UDP 发送函数。
auto connection = ruvia::http3_quic_connection::client(options);
ruvia::http3_quic_datagram out;  // 复用，不可移动
// 收到 UDP 数据报：
ruvia::http3_quic_result result = connection.receive_datagram(path, ecn, bytes, now);
while (auto event = connection.next_event()) { /* 处理事件 */ }
// TLS 应用密钥就绪后：
if (!connection.http3_started()) { (void)connection.start_http3(); }
// 发送：
while (connection.next_datagram(options.max_udp_payload_size_, now, out).status_ == ruvia::http3_quic_status::accepted) {
    send_to(out.path_, out.bytes_);
    while (auto event = connection.next_event()) { /* 处理事件 */ }
}
// 计时：在 connection.expiry() 到期时调用 expire_timers(now)，随后同样取尽事件。
```

## 13. WebSocket、Capsule 与 CONNECT-UDP

本章覆盖 RFC 6455 / RFC 7692 WebSocket 的握手与 Sans-I/O 帧状态机，以及 RFC 9297 Capsule Protocol / HTTP Datagram 与 RFC 9298 CONNECT-UDP 的纯协议值类型。它们不持有 socket、不调度 I/O；Web 层的 `ruvia::websocket`、隧道与路由配置见 [ruvia-web](ruvia-web.md)。HTTP/2（RFC 8441）与 HTTP/3（RFC 9220）Extended CONNECT 的握手成员函数属于各自连接类型，见 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http3_connection.h"`](#api-http3-connection-h)。

### 13.1 `"websocket_handshake.h"`
<a id="api-websocket-handshake-h"></a>

服务端 WebSocket 打开握手：校验 HTTP/1.1 Upgrade 请求、协商子协议与 `permessage-deflate`，并生成拥有型 101 响应计划；同时定义 HTTP/1.1 与 Extended CONNECT 共用的服务端握手选项、错误枚举与 Extended CONNECT 协商结果。无条件安装；公开 include [`"websocket_connection.h"`](#api-websocket-connection-h)、[`"http1_server_request.h"`](#api-http1-server-request-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_response.h"`](#api-http-response-h)、[`"http_protocol.h"`](#api-http-protocol-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/websocket_handshake.h`](../ruvia-http/include/websocket_handshake.h)

#### 13.1.1 常量与 `class websocket_subprotocol_set`

```cpp
namespace ruvia {
inline constexpr std::size_t max_websocket_subprotocols = 64;

class websocket_subprotocol_set final {
public:
    [[nodiscard]] bool append(borrowed_text protocol) noexcept;
    [[nodiscard]] bool append_list(borrowed_text value) noexcept;
    [[nodiscard]] bool append_offered_list(borrowed_text value) noexcept;
    [[nodiscard]] bool contains(std::string_view protocol) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
};
}
```

`websocket_subprotocol_set` 是无分配、已校验的 RFC 6455 子协议集合，内联保存最多 `max_websocket_subprotocols`（64）个不同协议的 `std::string_view`。集合只借用：每个元素借用传入 `borrowed_text` 指向的存储（`borrowed_text` 拒绝右值拥有型字符串，见 [`"borrowed_text.h"`](#api-borrowed-text-h)），集合存活期间该存储必须保持有效且不变。默认构造为空集合，可复制。

| 成员 | 语义 |
| --- | --- |
| `append(protocol)` | 严格追加一个协议：必须是合法 HTTP token（`is_valid_http_header_name`）、集合未满且尚未出现；任一不满足返回 `false` 且集合不变。用于配置与客户端发出的提议（RFC 6455 §4.1 要求唯一） |
| `append_list(value)` | `append()` 的逗号列表形式：逐元素严格追加，空列表元素被忽略；任一元素非法、重复或超出容量返回 `false`。失败前已追加的元素保留 |
| `append_offered_list(value)` | 接收到的 `Sec-WebSocket-Protocol` 提议：唯一性只约束发送方（RFC 6455 §4.1），§4.2.1 不要求接收方检查，因此重复协议被跳过而非报错；非法 token 或超过 64 个不同协议返回 `false`（超量提议按非法握手拒绝，不截断） |
| `contains(protocol)` | 精确（大小写敏感）比较是否含该协议 |
| `empty()` | 集合是否为空 |

#### 13.1.2 服务端握手选项、错误与结果

```cpp
namespace ruvia {
struct websocket_server_handshake_options final {
    borrowed_span<std::string_view> supported_subprotocols_{};
    borrowed_http_headers response_headers_{};
    websocket_server_deflate_options deflate_{};
};

struct websocket_handshake_validation_options final {
    borrowed_span<std::string_view> supported_subprotocols_{};
    bool require_subprotocol_{false};
};

enum class websocket_handshake_error : std::uint8_t {
    invalid_request,
    unsupported_version,
    unsupported_subprotocol,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(websocket_handshake_error error) noexcept;
void apply_websocket_handshake_error_headers(websocket_handshake_error error, http_response& response);

using websocket_handshake_validation_result = std::variant<std::monostate, websocket_handshake_error>;
}
```

`websocket_server_handshake_options` 是 HTTP/1.1 Upgrade 与 RFC 8441 / RFC 9220 Extended CONNECT 共用的服务端握手策略：

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `supported_subprotocols_` | 空 | 服务端偏好顺序的子协议列表。每项必须是非空、唯一的 HTTP token；按此顺序选择客户端提议中第一个出现者。列表本身无效（非 token 或重复）时不选择任何子协议 |
| `response_headers_` | 空 | 附加响应字段，在返回响应计划前复制。名称与值须是合法字段（值不得以 SP/HTAB 开头或结尾）；分帧、连接相关字段与任何 `Sec-WebSocket-*` 字段由握手自身生成，应用提供时抛 `std::invalid_argument`；整个响应头（含 101 状态行、`Sec-WebSocket-Accept`、协商字段与空行）超过字段节上限时抛 `std::length_error` |
| `deflate_` | 见 [`websocket_server_deflate_options`](#api-websocket-connection-h) | 服务端 `permessage-deflate` 策略；其 `deflater_` 经 `validate_websocket_zlib_options()` 校验，越界抛 `std::invalid_argument`。客户端在多行 `Sec-WebSocket-Extensions` 中的扩展列表整体校验，按顺序采用第一个可接受的 `permessage-deflate` 提议；列表非法时不启用压缩 |

`websocket_handshake_validation_options` 只影响校验：

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `supported_subprotocols_` | 空 | 与 `require_subprotocol_` 联用的服务端支持列表 |
| `require_subprotocol_` | `false` | RFC 6455 §4.2.2 允许只讲特定子协议的服务端拒绝未提议其中任何一个的客户端。为 `true` 时，提议与 `supported_subprotocols_` 无交集的请求返回 `unsupported_subprotocol`；为 `false` 时无交集也接受，响应不带 `Sec-WebSocket-Protocol` |

| `websocket_handshake_error` | 含义 | `make_http_protocol_error()` 结果 |
| --- | --- | --- |
| `invalid_request` | 握手请求不合法：方法、版本、`Connection: Upgrade`、`Upgrade: websocket`、`Sec-WebSocket-Key`（恰好一个、标准 base64 编码的 16 字节随机数，即 24 字符）、扩展或子协议字段语法、需消费的请求正文等任一不满足 | `400`，代码 `invalid_websocket_handshake` |
| `unsupported_version` | `Sec-WebSocket-Version` 不是 `13` | `400`，代码 `unsupported_websocket_version` |
| `unsupported_subprotocol` | `require_subprotocol_` 为真且无共同子协议 | `400`，代码 `unsupported_websocket_subprotocol` |

- `make_http_protocol_error(error)`：将拒绝原因映射为 [`http_protocol_error`](#api-http-protocol-h)，供错误响应使用。
- `apply_websocket_handshake_error_headers(error, response)`：RFC 6455 §4.4 要求拒绝不支持的版本时列出全部支持版本；对 `unsupported_version` 向 `response` 写入 `Sec-WebSocket-Version: 13`，其他错误不修改。应在任何应用错误处理器之后调用，确保该字段不被覆盖。
- `websocket_handshake_validation_result`：一次性校验结果，`std::monostate` 表示接受，否则为拒绝原因。

#### 13.1.3 `class websocket_extended_connect_negotiation`

```cpp
namespace ruvia {
class websocket_extended_connect_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept;
};
}
```

RFC 8441（HTTP/2）或 RFC 9220（HTTP/3）Extended CONNECT 的 2xx 响应在线上实际提交的协商结果。只能由 [`http2_connection`](#api-http2-connection-h) / [`http3_connection`](#api-http3-connection-h) 的 WebSocket 握手成员构造，无公开构造函数；对象拥有选中子协议的副本。运行时应据此值配置 [`websocket_connection_options`](#api-websocket-connection-h)，不要另行重算压缩/子协议组合。

- `subprotocol()`：选中的子协议，未选中时为空视图；视图借用本对象，只允许左值调用（右值重载 `= delete`，防止借用即将销毁的临时对象）。
- `compression()`：协商得到的 [`websocket_compression`](#api-websocket-connection-h)。

#### 13.1.4 HTTP/1.1 校验与 `class http1_websocket_handshake_plan`

```cpp
namespace ruvia {
[[nodiscard]] websocket_handshake_validation_result validate_http1_websocket_handshake(const http_request& request, const http1_request_body_plan& body_plan, websocket_handshake_validation_options options = {}) noexcept;

class http1_websocket_handshake_plan final {
public:
    http1_websocket_handshake_plan(const http1_websocket_handshake_plan&) = delete;
    http1_websocket_handshake_plan& operator=(const http1_websocket_handshake_plan&) = delete;
    http1_websocket_handshake_plan(http1_websocket_handshake_plan&&) noexcept = default;
    http1_websocket_handshake_plan& operator=(http1_websocket_handshake_plan&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] constexpr websocket_compression compression() const noexcept;

    template <typename visitor_type>
    void for_each_response_part(visitor_type&& visitor) const&;
    template <typename visitor_type>
    void for_each_response_part(visitor_type&&) const&& = delete;
};

[[nodiscard]] http1_websocket_handshake_plan prepare_http1_websocket_handshake(const http_request& request, websocket_server_handshake_options options = {});
}
```

`validate_http1_websocket_handshake(request, body_plan, options)` 校验 RFC 6455 HTTP/1.1 打开握手，不创建也不写响应：

- 方法必须为 `GET`、版本为 HTTP/1.1；`body_plan`（解析器给出的 [`http1_request_body_plan`](#api-http1-server-request-h)）的分帧不得需要消费正文。RFC 6455 不禁止 Upgrade 请求带 `Content-Length`，`Content-Length: 0` 被接受，正长度或 chunked 仍有字节须先消费，返回 `invalid_request`。
- `Connection` 须含 `upgrade` 选项，`Upgrade` 须含无版本的 `websocket`（大小写不敏感）；`Sec-WebSocket-Key` 恰好一个且为 24 字符规范 base64（解码为 16 字节）；`Sec-WebSocket-Protocol`、`Sec-WebSocket-Extensions` 语法合法；然后检查版本 `13` 与 `options.require_subprotocol_`。
- 返回 `std::monostate` 或 [`websocket_handshake_error`](#api-websocket-handshake-h)；`noexcept`。

`prepare_http1_websocket_handshake(request, options)` 必须在校验成功后调用：计算 `Sec-WebSocket-Accept`，按 `options` 选择子协议并协商压缩，复制选中子协议与附加响应字段，返回不借用请求或服务端偏好存储的 `http1_websocket_handshake_plan`。异常：附加字段非法或属握手专有字段抛 `std::invalid_argument`；响应头超出字段节上限抛 `std::length_error`；`deflate_.deflater_` 越界抛 `std::invalid_argument`；分配失败传播。

`http1_websocket_handshake_plan` 是拥有型 HTTP/1.1 `101 Switching Protocols` 响应计划，只能移动构造（复制与任何赋值均删除）：

| 成员 | 语义 |
| --- | --- |
| `subprotocol()` | 选中的子协议（未选中为空），借用计划；右值重载删除 |
| `compression()` | 编码进本响应的确切 `permessage-deflate` 协商，用于配置随后的 `websocket_connection`，不要另行重算 |
| `for_each_response_part(visitor)` | 依次以 `std::string_view` 调用 `visitor`，产出完整响应头：固定前缀 `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: `、accept 值、CRLF；有子协议时 `Sec-WebSocket-Protocol: ` 行；启用压缩时 `Sec-WebSocket-Extensions: ` 行（`permessage-deflate` 及 `server_no_context_takeover`、`client_no_context_takeover`、`server_max_window_bits=N`、`client_max_window_bits=N` 参数）；各附加字段 `name: value` 行；终止空行。各视图借用计划自身或静态存储，适合 scatter-gather 写入；计划须存活且不被移动，直到写入完成。右值重载删除 |

HTTP/2 与 HTTP/3 的服务端握手（`validate_websocket_handshake` / `submit_websocket_handshake`）使用同一 `websocket_server_handshake_options` 与 `websocket_handshake_error`，签名与状态规则见 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http3_connection.h"`](#api-http3-connection-h)。

```cpp
// 解析器已给出 request 与 body_plan
const std::array<std::string_view, 2> protocols{"chat.v1", "json.v1"};
const auto checked = ruvia::validate_http1_websocket_handshake(request, body_plan, {.supported_subprotocols_ = protocols, .require_subprotocol_ = true});
if (const auto* error = std::get_if<ruvia::websocket_handshake_error>(&checked)) {
    // 以 make_http_protocol_error(*error) 生成 400，再 apply_websocket_handshake_error_headers(*error, response)
} else {
    auto plan = ruvia::prepare_http1_websocket_handshake(request, {.supported_subprotocols_ = protocols});
    plan.for_each_response_part([&](std::string_view part) { /* 收集 iovec */ });
    ruvia::websocket_connection ws({.compression_ = plan.compression()});
}
```

### 13.2 `"websocket_client_handshake.h"`
<a id="api-websocket-client-handshake-h"></a>

客户端 WebSocket 打开握手：与 HTTP 版本无关的子协议 / `permessage-deflate` 提议拥有者、HTTP/1.1 Upgrade 请求生成与 101 响应校验、Extended CONNECT（RFC 8441 / RFC 9220）2xx 响应校验，以及统一的客户端握手错误枚举。无条件安装；公开 include [`"websocket_connection.h"`](#api-websocket-connection-h)、[`"http1_client_request.h"`](#api-http1-client-request-h)、[`"http1_client_response.h"`](#api-http1-client-response-h)、[`"http_client_response_head.h"`](#api-http-client-response-head-h)、[`"http_authority.h"`](#api-http-authority-h)、[`"http_header.h"`](#api-http-header-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)。源码：[`ruvia-http/include/websocket_client_handshake.h`](../ruvia-http/include/websocket_client_handshake.h)

#### 13.2.1 类型与错误

```cpp
namespace ruvia {
struct websocket_client_negotiation_config_view final {
    borrowed_span<std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};

enum class websocket_client_handshake_error : std::uint8_t {
    response_status,
    stream_closed,
    accept,
    upgrade,
    connection,
    subprotocol,
    extensions,
    forbidden_field,
};

struct websocket_client_negotiation_result_view final {
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};
}
```

`websocket_client_negotiation_config_view` 是借用型配置：`subprotocols_` 为按偏好排列的提议子协议（默认空），`deflate_` 为 [`websocket_client_deflate_offer`](#api-websocket-connection-h)（默认不提议压缩）。两者在 `websocket_client_negotiation` 构造时复制一次。

`websocket_client_handshake_error` 是 HTTP/1.1 Upgrade（RFC 6455 §4.1）与 Extended CONNECT 共用的唯一失败形状：

| 值 | 含义 |
| --- | --- |
| `response_status` | HTTP/1.1 下状态不是 `101`；Extended CONNECT 下不是 2xx |
| `stream_closed` | Extended CONNECT：2xx HEADERS 已结束该流 |
| `accept` | HTTP/1.1：`Sec-WebSocket-Accept` 缺失、重复或与密钥不符 |
| `upgrade` | HTTP/1.1：缺少 `Upgrade: websocket` |
| `connection` | HTTP/1.1：缺少 `Connection: Upgrade` |
| `subprotocol` | 响应带多个 `Sec-WebSocket-Protocol`，或所选值为空、不在本端提议列表中 |
| `extensions` | `Sec-WebSocket-Extensions` 含多个扩展、含非本端提议可接受的 `permessage-deflate` 响应参数的项，或字段存在但只含空列表元素 |
| `forbidden_field` | Extended CONNECT：响应带连接专用字段（RFC 9113 §8.2.2、RFC 9114 §4.2） |

`websocket_client_negotiation_result_view` 是成功结果：`selected_subprotocol_` 为服务端选中的子协议（未选中为空视图），借用传入的响应字段 / 响应头；`compression_` 为服务端确认的 [`websocket_compression`](#api-websocket-connection-h)，用于配置客户端 `websocket_connection`。

#### 13.2.2 `class websocket_client_negotiation`

```cpp
namespace ruvia {
class websocket_client_negotiation final {
public:
    static void validate_configuration(websocket_client_negotiation_config_view config);
    explicit websocket_client_negotiation(websocket_client_negotiation_config_view config);
    websocket_client_negotiation(websocket_client_negotiation&&) noexcept = default;
    websocket_client_negotiation& operator=(websocket_client_negotiation&&) = delete;
    websocket_client_negotiation(const websocket_client_negotiation&) = delete;
    websocket_client_negotiation& operator=(const websocket_client_negotiation&) = delete;

    [[nodiscard]] std::span<const http_header> request_fields() const& noexcept RUVIA_LIFETIMEBOUND;
    std::span<const http_header> request_fields() const&& = delete;

    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_fields(std::span<const http_header> fields) const;
    template </* 临时拥有型 http_header 范围 */ typename fields_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_fields(fields_type&&) const = delete;
    template </* http_header */ typename element_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_fields(std::initializer_list<element_type>) const = delete;
};
}
```

与 HTTP 版本无关的 RFC 6455 / RFC 7692 客户端协商拥有者，只能移动构造。配置在构造时复制一次；它为 HTTP/1.1 Upgrade 或 Extended CONNECT 提供请求字段，并校验服务端的确切选择。所有提议视图随本对象失效；响应结果视图随响应头失效。

| 成员 | 语义 |
| --- | --- |
| `validate_configuration(config)` | 只校验不构造：每个子协议须是唯一合法 token，组成的 `Sec-WebSocket-Protocol` 值不超过字段上限；`deflate_.compression_` 中窗口位须在 `8..15`；设置 `client_max_window_bits_` 时 `offer_client_max_window_bits_` 必须为 `true`；`deflate_.deflater_` 经 `validate_websocket_zlib_options()` 校验；生成字段须放入字段节预算。失败抛 `std::invalid_argument` |
| 构造函数 | 执行同样的校验（失败抛 `std::invalid_argument`），然后生成最多 3 个请求字段：`Sec-WebSocket-Version: 13`、有提议时 `Sec-WebSocket-Protocol`、启用压缩时 `Sec-WebSocket-Extensions` |
| `request_fields()` | 上述请求字段，借用本对象；右值重载删除 |
| `validate_fields(fields)` | 只校验 `fields` 中服务端的 `Sec-WebSocket-Protocol` 与 `Sec-WebSocket-Extensions` 选择，不检查状态码或连接专用字段；成功视图借用 `fields`。版本专用的响应校验函数在自己的状态检查后执行同一选择校验 |
| 已删除的 `validate_fields` 重载 | 拒绝临时拥有型容器及字段初始化列表，包括具名列表；结果视图借用字段存储，不能比该存储活得久 |

#### 13.2.3 自由函数

```cpp
namespace ruvia {
void validate_websocket_client_handshake_headers(std::span<const http_header_view> headers);

[[nodiscard]] http1_client_request_prepare_result prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, borrowed_http_headers headers, std::span<char> head_buffer, http_request_field_source source = http_request_field_source::generated);

[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_http1_websocket_client_response(std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response RUVIA_LIFETIMEBOUND);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_http1_websocket_client_response(std::span<const char, 24>, const websocket_client_negotiation&, const http1_parsed_client_response_head&&) = delete;

[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_extended_connect_websocket_client_response(const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_extended_connect_websocket_client_response(const websocket_client_negotiation&, const http_client_response_head&&, bool) = delete;
}
```

HTTP/1.1 Upgrade 交换期间，调用方拥有 base64 编码的 16 字节随机数 `encoded_key`（恰好 24 字符，须每次握手新生成）、`websocket_client_negotiation` 与应用首部，直到响应校验完成。

| 函数 | 语义 |
| --- | --- |
| `validate_websocket_client_handshake_headers(headers)` | 拒绝握手或请求写入器自行生成的应用首部（`Host`、分帧字段、`Expect`、`Upgrade` / `Connection` 及任何 `Sec-WebSocket-*`），以及请求写入器会拒绝的任何首部；失败抛 `std::invalid_argument`。与 HTTP 版本无关，可在确定交换版本前调用 |
| `prepare_http1_websocket_client_request(...)` | 在 `head_buffer` 中写出 `GET target HTTP/1.1` Upgrade 请求头：`Host` 由 `origin` 生成，附加 `Upgrade: websocket`、`Connection: Upgrade`、`Sec-WebSocket-Key: <encoded_key>`、协商字段与 `headers`。返回 [`http1_client_request_prepare_result`](#api-http1-client-request-h)（缓冲不足等按该结果报告）；`headers` 含保留或非法首部时抛 `std::invalid_argument`。`source` 含义同 [`http_request_field_source`](#api-http1-client-request-h) |
| `validate_http1_websocket_client_response(key, negotiation, response)` | 先按密钥校验 101 响应（状态、`Upgrade`、`Connection`、`Sec-WebSocket-Accept`），再按协商校验服务端选择。结果视图借用 `response`；右值响应头重载删除 |
| `validate_extended_connect_websocket_client_response(negotiation, head, stream_open)` | Extended CONNECT（HTTP/2、HTTP/3）：校验 2xx 最终响应、`stream_open` 为真（否则 `stream_closed`）、无连接专用字段，再校验选择。结果视图借用 `head`；右值响应头重载删除 |

Extended CONNECT 请求头由连接成员 `submit_websocket_request_head` 发起，见 [`"http2_connection.h"`](#api-http2-connection-h) 与 [`"http3_connection.h"`](#api-http3-connection-h)。

### 13.3 `"websocket_connection.h"`
<a id="api-websocket-connection-h"></a>

已升级连接上的 Sans-I/O RFC 6455 帧状态机 `websocket_connection` 及其全部协议值类型：关闭码、opcode、`permessage-deflate`（RFC 7692）协商与 zlib 参数、事件、输出计划、终止原因与接收存储预算。它供服务端与客户端驱动共用，自身不做 I/O。无条件安装；公开 include [`"http_io_buffer.h"`](#api-http-io-buffer-h)、[`"http_protocol.h"`](#api-http-protocol-h)（`protocol_byte_limit`、`default_max_websocket_message_bytes`）、[`"checked_integer.h"`](#api-checked-integer-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/websocket_connection.h`](../ruvia-http/include/websocket_connection.h)

#### 13.3.1 常量、关闭码与 opcode

```cpp
namespace ruvia {
inline constexpr std::size_t websocket_retained_buffer_bytes = std::size_t{16} * 1024;
inline constexpr std::size_t websocket_idle_read_bytes = 512;

class websocket_close_code final : public checked_integer<websocket_close_code, std::uint16_t, /* 未指定策略类型 */> {
public:
    using checked_integer::checked_integer;
};

namespace websocket_close_status {
inline constexpr auto normal_closure = websocket_close_code(1000);
inline constexpr auto going_away = websocket_close_code(1001);
inline constexpr auto internal_error = websocket_close_code(1011);
}

enum class websocket_opcode : std::uint8_t {
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA
};
}
```

- `websocket_retained_buffer_bytes`（16 KiB）：每个连接缓冲（传输输入、待发输出、压缩出站负载、解压入站消息）在释放后保留的容量；更大的增长在用完后归还。
- `websocket_idle_read_bytes`（512）：服务端连接最近帧都较小时，等待下一帧的传输读取大小，用于限制空闲连接持有的读存储；更大的帧按需扩展缓冲。

`websocket_close_code` 是 RFC 6455 §7.4 与 IANA WebSocket Close Code 注册表中受支持的关闭状态码，基于 [`checked_integer`](#api-checked-integer-h)：大小与 `std::uint16_t` 相同且可平凡复制；不允许隐式整数转换，`explicit` 构造在原始整数宽度上校验，非法值抛 `std::invalid_argument`（消息 `invalid WebSocket Close status code`），`try_from_value(input)` 以 `std::nullopt` 报告非法，`value()` 取出整数；比较、哈希等其余操作由 `checked_integer` 提供。合法集合为 `1000..1003`、`1007..1014`、`3000..4999`；本地哨兵码 `1005` / `1006` 以及其他保留码不能上网。不要先转换成 `std::uint16_t`：`66536`、`-64536` 不会截断成 `1000`，而是被拒绝。角色限制由连接执行：`1010`（客户端报告服务端未协商所需扩展）只允许客户端发送，服务端 `submit_close` 返回 `invalid_code`。

`namespace websocket_close_status` 常量：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `normal_closure` | 1000 | 正常关闭 |
| `going_away` | 1001 | 端点离开（如服务端停机） |
| `internal_error` | 1011 | 端点遇到意外条件 |

其他受支持值用受检构造得到，如 `websocket_close_code(4000)`。

| `websocket_opcode` | 值 | 含义 |
| --- | --- | --- |
| `text` | `0x1` | 文本消息（UTF-8） |
| `binary` | `0x2` | 二进制消息 |
| `close` | `0x8` | Close 控制帧 |
| `ping` | `0x9` | Ping 控制帧 |
| `pong` | `0xA` | Pong 控制帧 |

续帧 opcode `0x0` 不作为枚举值公开，由连接在分片时自动生成。

#### 13.3.2 `permessage-deflate` 与 zlib 选项

```cpp
namespace ruvia {
struct websocket_compression final {
    bool enabled_{false};
    bool server_no_context_takeover_{true};
    bool client_no_context_takeover_{true};
    std::optional<int> server_max_window_bits_{};
    std::optional<int> client_max_window_bits_{};
    bool operator==(const websocket_compression&) const = default;
};

struct websocket_zlib_options final {
    int compression_level_{6};
    int mem_level_{8};
};

inline void validate_websocket_zlib_options(websocket_zlib_options config);

struct websocket_server_deflate_options final {
    bool enabled_{true};
    bool context_takeover_{false};
    websocket_zlib_options deflater_{};
};

struct websocket_client_deflate_offer final {
    websocket_compression compression_{};
    bool offer_client_max_window_bits_{true};
    websocket_zlib_options deflater_{};
};

enum class websocket_inflate_result : std::uint8_t {
    ok,
    error,
    too_large,
};

class websocket_deflate_codecs final {
public:
    websocket_deflate_codecs() noexcept = default;
    ~websocket_deflate_codecs();
    websocket_deflate_codecs(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs& operator=(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs(websocket_deflate_codecs&&) = delete;
    websocket_deflate_codecs& operator=(websocket_deflate_codecs&&) = delete;

    [[nodiscard]] bool deflate_message(std::string_view payload, std::string& out, int compression_level, int window_bits);
    [[nodiscard]] websocket_inflate_result inflate_message(std::string_view payload, std::string& out, protocol_byte_limit message_limit);
};
}
```

`websocket_compression` 是 RFC 7692 的确切协商结果，两个方向的字典与窗口相互独立；可用 `==` 比较。

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `enabled_` | `false` | 是否协商了 `permessage-deflate` |
| `server_no_context_takeover_` | `true` | 服务端发送方向每条消息从空字典开始 |
| `client_no_context_takeover_` | `true` | 客户端发送方向每条消息从空字典开始 |
| `server_max_window_bits_` | 空 | 服务端发送窗口（`8..15`）；省略即 15。是否存在同时决定协商扩展线上参数是否出现 |
| `client_max_window_bits_` | 空 | 客户端发送窗口（`8..15`），省略即 15，同上 |

`websocket_zlib_options` 是本端 deflater 的 zlib 参数，RFC 7692 将其留给发送方：

| 字段 | 默认值 | 合法范围 | 语义 |
| --- | --- | --- | --- |
| `compression_level_` | 6 | `0..9` | zlib 压缩等级 |
| `mem_level_` | 8 | `1..9` | zlib `memLevel`。哈希表与待发缓冲占 `2^(memLevel + 9)` 字节，另加 15 位窗口的 128 KiB 窗口与历史：8 时 128 KiB，4 时 8 KiB（每个 takeover 连接省 120 KiB，压缩率较低） |

只有以 context takeover 协商的方向保留私有 deflater，在该方向第一条压缩消息时创建；无 takeover 的方向（默认）共享 `websocket_deflate_codecs`，使用 zlib 默认 `memLevel`，不为每个连接分配编解码状态。

`validate_websocket_zlib_options(config)` 是该结构唯一的范围检查：等级不在 `0..9` 或 `memLevel` 不在 `1..9` 时抛 `std::invalid_argument`。

`websocket_server_deflate_options`（服务端策略）：

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `enabled_` | `true` | 是否接受客户端的 `permessage-deflate` 提议 |
| `context_takeover_` | `false` | 显式开启才允许跨消息保留字典。不要在同一字典中混合机密与攻击者可控内容；即使开启，对端请求 no-context-takeover 时仍按独立消息处理 |
| `deflater_` | `{}` | 服务端私有 deflater 的 zlib 参数 |

`websocket_client_deflate_offer`（客户端提议）：

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `compression_` | `{}`（不提议） | 提议的扩展参数（RFC 7692 §7.1）；`enabled_` 为真才发送提议 |
| `offer_client_max_window_bits_` | `true` | 追加无值的 `client_max_window_bits`，允许服务端选择客户端窗口（§7.1.2.2）；设置了 `compression_.client_max_window_bits_` 时必须为 `true` |
| `deflater_` | `{}` | 客户端私有 deflater 的 zlib 参数 |

`websocket_deflate_codecs` 是 RFC 7692 无 context takeover 方向的共享编解码器集合：这些方向每条消息从空字典开始，因此一个拥有者（例如一个 worker）可用一套编解码器服务任意多个连接。不可复制、不可移动，地址对借用者稳定；非线程安全。每次调用处理一整条消息，返回前重置所用编解码器；因异常遗留的编解码器在下次使用时丢弃重建，任何消息（含失败消息）都不影响其他消息。raw deflater 按（压缩等级, 窗口位）组合首次使用时创建；一个 15 位窗口的 raw inflater 服务所有接收窗口。

- `deflate_message(payload, out, compression_level, window_bits)`：将压缩后的消息负载追加到 `out`，不含最终 flush 的尾部 `00 00 FF FF`（RFC 7692 §7.2.1）。等级须 `0..9`、窗口须 `8..15`，否则抛 `std::invalid_argument`；返回 `false` 表示编解码器失败；分配异常以原类型传播。
- `inflate_message(payload, out, message_limit)`：将解压结果追加到 `out`，本次追加总量受 `message_limit` 约束（RFC 7692 §7.2.2）。

| `websocket_inflate_result` | 含义 |
| --- | --- |
| `ok` | 解压成功 |
| `error` | 压缩数据非法或编解码器失败 |
| `too_large` | 解压输出超过 `message_limit` |

#### 13.3.3 消息、事件与输出计划

```cpp
namespace ruvia {
class websocket_message final {
public:
    [[nodiscard]] static constexpr websocket_message borrow(websocket_opcode opcode, borrowed_text payload_value) noexcept;
    websocket_message(const websocket_message&) noexcept = default;
    websocket_message& operator=(const websocket_message&) noexcept = default;
    websocket_message(websocket_message&&) noexcept = default;
    websocket_message& operator=(websocket_message&&) noexcept = default;
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept;
    [[nodiscard]] constexpr std::string_view payload() const noexcept;
    [[nodiscard]] bool text() const noexcept;
    [[nodiscard]] bool binary() const noexcept;
};

enum class websocket_transport_disposition : std::uint8_t { keep_open, end_transport };

class websocket_output_plan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept;
    [[nodiscard]] constexpr std::string_view borrowed_bytes() const noexcept;
    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr websocket_transport_disposition disposition() const noexcept;
};

class websocket_ping_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept;
};
class websocket_pong_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept;
};
class websocket_close_event final {
public:
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept;
    [[nodiscard]] constexpr std::string_view reason() const noexcept;
};
class websocket_protocol_error_event final {
public:
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept;
};
class websocket_transport_end_event final {};

class websocket_event final {
public:
    websocket_event(const websocket_event&) = default;
    websocket_event(websocket_event&&) noexcept = default;
    websocket_event& operator=(const websocket_event&) = delete;
    websocket_event& operator=(websocket_event&&) = delete;
    [[nodiscard]] const websocket_message* message() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_message* message() const&& = delete;
    [[nodiscard]] const websocket_ping_event* ping() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_ping_event* ping() const&& = delete;
    [[nodiscard]] const websocket_pong_event* pong() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_pong_event* pong() const&& = delete;
    [[nodiscard]] const websocket_close_event* close() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_close_event* close() const&& = delete;
    [[nodiscard]] const websocket_protocol_error_event* protocol_error() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_protocol_error_event* protocol_error() const&& = delete;
    [[nodiscard]] const websocket_transport_end_event* transport_end() const& noexcept RUVIA_LIFETIMEBOUND;
    const websocket_transport_end_event* transport_end() const&& = delete;
};
}
```

`websocket_message` 是 opcode 与负载视图的零复制组合，可复制，不拥有负载。`borrow(opcode, payload)` 在调用方存储上构造消息，经 `borrowed_text` 拒绝直接传入的右值拥有型字符串；调用方须保持存储存活且不变，直到视图或借用它的发送操作不再使用。`opcode()` / `payload()` 访问；`text()` / `binary()` 判断 opcode 是否为 `text` / `binary`。由连接事件得到的消息，其 `payload()` 借用连接的接收存储，见下文事件借用规则；需要更长寿命时先复制。

`websocket_output_plan` 是连接待发送的传输输出，按 `bytes()` 然后 `borrowed_bytes()` 的顺序写出：

| 成员 | 语义 |
| --- | --- |
| `bytes()` | 连接自有输出缓冲中的待发字节 |
| `borrowed_bytes()` | 以 `websocket_borrowed_payload` 提交、按引用排队的负载剩余部分（位于其帧头之后）；否则为空 |
| `size()` | 两者总长，即全部写出后应传给 `consume_output()` 的字节数 |
| `disposition()` | 写完后对传输的处置 |

| `websocket_transport_disposition` | 含义 |
| --- | --- |
| `keep_open` | 写完后保持传输打开 |
| `end_transport` | 写完后结束传输（服务端关闭握手完成、EOF 或终止后）；结束后调用 `commit_transport_end()` |

`websocket_event` 是两种公开驱动与 Sans-I/O 核心共享的协议事件值，恰为以下之一，通过对应访问器取得指针（不匹配返回 `nullptr`）：

| 访问器 | 事件 | 语义 |
| --- | --- | --- |
| `message()` | `websocket_message` | 完整（已重组、已解压、文本已验证 UTF-8）的数据消息 |
| `ping()` | `websocket_ping_event` | 收到 Ping；`payload()` 为其负载。连接已自动排队 Pong |
| `pong()` | `websocket_pong_event` | 收到 Pong；`payload()` 为其负载 |
| `close()` | `websocket_close_event` | 收到对端 Close；`close_code()` 为整数状态码，对端未带状态码时为本地哨兵 `1005`（RFC 6455 §7.1.5，§7.4.1 禁止其上网），因此不能未经 `websocket_close_code` 检查直接用于发送；`reason()` 为 UTF-8 原因 |
| `protocol_error()` | `websocket_protocol_error_event` | 输入违反协议；`close_code()` 为本端已排队发送的 Close 码（`1002`、`1007` 或 `1009`，对应 `termination()`） |
| `transport_end()` | `websocket_transport_end_event` | 连接已到传输结束阶段（关闭握手完成、EOF 或终止），驱动应按 `output_plan()` 收尾 |

访问器返回的指针借用事件对象，只允许左值调用（右值重载删除）。事件可复制、移动构造，不可赋值。负载与原因视图借用驱动的输入或消息重组存储：下一次 `next_event()`、`feed()` 或向借用输入缓冲追加数据都可能使其失效。

#### 13.3.4 状态枚举与回调类型

```cpp
namespace ruvia {
enum class websocket_liveness_mode : std::uint8_t { open, awaiting_peer_close, awaiting_peer_eof, inactive };
enum class websocket_frame_submit_status : std::uint8_t {
    accepted, not_open, invalid_opcode, message_too_large, invalid_text_payload, control_frame_too_large, message_in_progress
};
enum class websocket_close_submit_status : std::uint8_t { accepted, already_closing, closed, invalid_code, invalid_reason, reason_too_large };
enum class websocket_abort_disposition : std::uint8_t { abort_transport, no_transport_action };
enum class websocket_termination : std::uint8_t {
    none, peer_close, local_close, eof, protocol_error, invalid_payload_data, message_too_large, timeout, aborted, io_error,
};
enum class websocket_output_consume_status : std::uint8_t { pending, drained, out_of_range };
enum class websocket_connection_role : std::uint8_t { server, client };
enum class websocket_feed_status : std::uint8_t { accepted, inactive, backpressured };

using websocket_mask_key_type = std::array<char, 4>;
using websocket_mask_key_generator_type = bool (*)(void*, websocket_mask_key_type&) noexcept;
}
```

| `websocket_liveness_mode` | 含义 |
| --- | --- |
| `open` | 连接打开，可收发 |
| `awaiting_peer_close` | 本端 Close 已写出，等待对端 Close |
| `awaiting_peer_eof` | 客户端已完成关闭握手，按 RFC 6455 §7.1.1 由服务端先关闭传输：驱动继续读取，`feed()` 忽略输入，`next_event()` 清空输入并释放超过 16 KiB 的输入缓冲，直到 `notify_transport_eof()` 进入传输结束；若长时间无 EOF，客户端可经 `abort()` 先关闭 |
| `inactive` | 连接不再活动（已结束、已终止或编解码器重置失败） |

| `websocket_frame_submit_status` | 含义 |
| --- | --- |
| `accepted` | 帧已排入输出 |
| `not_open` | 连接已不在打开阶段（已发起或收到 Close、已终止） |
| `invalid_opcode` | opcode 不是 `text` / `binary` / `ping` / `pong`，或控制帧带 `final_ = false`（控制帧不可分片，RFC 6455 §5.5） |
| `message_too_large` | 消息累计（未压缩）长度超过 `message_limit_` |
| `invalid_text_payload` | 文本消息不是合法 UTF-8（跨分片校验） |
| `control_frame_too_large` | Ping / Pong 负载超过 125 字节 |
| `message_in_progress` | 分片数据消息尚未结束，只能提交控制帧或同 opcode 的后续分片 |

| `websocket_close_submit_status` | 含义 |
| --- | --- |
| `accepted` | Close 帧已排入输出，进入关闭阶段 |
| `already_closing` | 关闭握手已在进行 |
| `closed` | 连接已结束 |
| `invalid_code` | 角色不允许该状态码（服务端发送 `1010`） |
| `invalid_reason` | 原因不是合法 UTF-8 |
| `reason_too_large` | 状态码与原因超过 125 字节控制帧负载 |

| `websocket_abort_disposition` | 含义 |
| --- | --- |
| `abort_transport` | 调用方须立即中止传输 |
| `no_transport_action` | 传输已结束，无需动作 |

`websocket_termination` 是本端观察到的会话结束原因，由 `websocket_connection` 单点记录，先到者获胜：之后的 Close、EOF 或 `abort()` 不覆盖。对端 Close 的状态码属于 close 事件本身。

| 值 | 含义 |
| --- | --- |
| `none` | 会话仍活动 |
| `peer_close` | 对端发起关闭握手 |
| `local_close` | 对端应答了本端发起的关闭握手 |
| `eof` | 传输未经 Close 帧结束（RFC 6455 §7.1.5，对应 1006），含截断帧 |
| `protocol_error` | 帧或消息非法（如未掩码 / 多余掩码、保留 opcode 或位、非法分片或控制帧），本端已发 Close `1002` |
| `invalid_payload_data` | 文本消息或 Close 原因不是 UTF-8，已发 `1007` |
| `message_too_large` | 帧声明、分片累计、解压后消息或接收存储预算超过接收上限，已发 `1009` |
| `timeout` | 驱动期限：心跳 Pong、关闭握手或操作超时（由驱动经 `abort(timeout)` 记录） |
| `aborted` | 本地结束：`abort()`、本地故障或会话拆除 |
| `io_error` | 传输读写失败（由驱动经 `abort(io_error)` 记录） |

| `websocket_output_consume_status` | 含义 |
| --- | --- |
| `pending` | 仍有待写字节 |
| `drained` | 输出已全部确认 |
| `out_of_range` | 确认字节数超过待发总量，状态不变 |

| `websocket_connection_role` | 含义 |
| --- | --- |
| `server` | 要求入站帧带掩码，出站不掩码 |
| `client` | 要求入站帧不带掩码，出站（含自动 Pong / Close）掩码；需要掩码键生成器 |

| `websocket_feed_status` | 含义 |
| --- | --- |
| `accepted` | 输入已接受（`awaiting_peer_eof` 期间输入被丢弃也返回此值） |
| `inactive` | 连接不再活动，未消费输入 |
| `backpressured` | 待 `next_event()` 处理的字节将超过 `max_buffered_input_bytes_`，未消费任何输入；先取事件或分小块重试 |

`websocket_mask_key_generator_type`：客户端每次调用必须用密码学安全随机数填充一个新的 4 字节掩码键并返回 `true`；`context` 被借用，须比连接存活更久。返回 `false` 对传输是致命的：相关操作可能抛异常（`std::runtime_error`），调用方必须中止连接。

#### 13.3.5 帧选项、借用负载与接收存储预算

```cpp
namespace ruvia {
struct websocket_frame_options final {
    bool final_{true};
    bool compress_{true};
};

class websocket_borrowed_payload final {
public:
    explicit constexpr websocket_borrowed_payload(borrowed_text bytes) noexcept;
    [[nodiscard]] constexpr std::string_view bytes() const noexcept;
};

struct websocket_receive_storage_budget final {
    bool (*resize_)(void* context, std::size_t bytes) noexcept {nullptr};
    void* context_{nullptr};
};
}
```

- `websocket_frame_options::final_`（默认 `true`）：`false` 发送文本或二进制消息的非最终分片。`compress_`（默认 `true`）：在已协商 `permessage-deflate` 时为消息首帧选择压缩；首帧决定整条消息是否压缩。
- `websocket_borrowed_payload`：`submit_frame()` 按引用排队而不复制的帧负载；经 `borrowed_text` 构造，拒绝右值拥有型字符串（其字节会在传输写出前失效）。`bytes()` 返回借用视图。
- `websocket_receive_storage_budget`：为连接在输入缓冲之外持有的接收存储计费——分片消息重组与 `permessage-deflate` 输出，各自可达接收消息上限——使拥有者把它们与其他入站内存一起限额。`resize_(context, bytes)` 收到该存储的总字节数：在增长之前调用，返回 `false` 拒绝增长；在增长或释放之后再次以实际容量调用，增长后返回 `false` 会丢弃新增存储。被拒绝的增长使该消息按超限失败（Close `1009`，`termination()` 为 `message_too_large`）。回调不得抛异常，只在 `next_event()` 内调用。`resize_` 为空表示不计费。

#### 13.3.6 `struct websocket_connection_options` 与 `class websocket_connection`

```cpp
namespace ruvia {
struct websocket_connection_options final {
    protocol_byte_limit message_limit_{protocol_byte_limit::limited(default_max_websocket_message_bytes)};
    protocol_byte_limit receive_message_limit_{};
    websocket_compression compression_{(websocket_compression{})};
    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};
    websocket_zlib_options deflater_{};
    websocket_deflate_codecs* shared_deflate_{nullptr};
    std::size_t max_buffered_input_bytes_{default_max_websocket_message_bytes + 14};
    websocket_receive_storage_budget receive_storage_{};
};

class websocket_connection final {
public:
    explicit websocket_connection(websocket_connection_options options = {});
    explicit websocket_connection(http_io_buffer& input, websocket_connection_options options = {});
    ~websocket_connection();
    websocket_connection(const websocket_connection&) = delete;
    websocket_connection& operator=(const websocket_connection&) = delete;
    websocket_connection(websocket_connection&&) noexcept;
    websocket_connection& operator=(websocket_connection&&) noexcept;

    [[nodiscard]] websocket_feed_status feed(std::string_view input);
    template </* 临时拥有型字符串 */ typename input_type>
    websocket_feed_status feed(input_type&&) = delete;
    [[nodiscard]] std::optional<websocket_event> next_event() & RUVIA_LIFETIMEBOUND;
    std::optional<websocket_event> next_event() && = delete;
    [[nodiscard]] std::size_t needed_input_bytes() const noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t bytes) noexcept;
    void commit_transport_end() noexcept;
    void notify_transport_eof() noexcept;
    [[nodiscard]] websocket_abort_disposition abort(websocket_termination cause = websocket_termination::aborted) noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    [[nodiscard]] websocket_termination termination() const noexcept;
    [[nodiscard]] bool message_pending() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload, websocket_frame_options options = {});
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, websocket_borrowed_payload payload, websocket_frame_options options = {});
    [[nodiscard]] websocket_close_submit_status submit_close(websocket_close_code code, std::string_view reason);
};
}
```

`websocket_connection_options`：

| 字段 | 默认值 | 语义 |
| --- | --- | --- |
| `message_limit_` | `limited(default_max_websocket_message_bytes)`（16 MiB） | 收发两个方向的整条消息上限（发送按未压缩长度，接收含重组与解压） |
| `receive_message_limit_` | 默认构造的 `protocol_byte_limit` | 额外的仅接收上限，覆盖消息聚合与解压；输入取它与 `message_limit_` 的较小者，不改变发送上限 |
| `compression_` | `websocket_compression{}`（未启用） | 握手协商结果，取自 [`http1_websocket_handshake_plan::compression()`](#api-websocket-handshake-h)、[`websocket_extended_connect_negotiation`](#api-websocket-handshake-h) 或 [`websocket_client_negotiation_result_view`](#api-websocket-client-handshake-h)；窗口位不在 `8..15` 时构造抛 `std::invalid_argument` |
| `role_` | `server` | 固定入站掩码校验与出站掩码；非法枚举值构造抛 `std::invalid_argument` |
| `mask_key_generator_` | `nullptr` | 客户端掩码键生成器；`client` 角色为空时构造抛 `std::invalid_argument` |
| `mask_key_context_` | `nullptr` | 传给生成器的借用上下文 |
| `deflater_` | `{}` | 以 context takeover 协商方向的私有 deflater 参数 |
| `shared_deflate_` | `nullptr` | 借用的 `websocket_deflate_codecs`，用于所有无 context takeover 的压缩方向；须比连接存活更久，只在连接调用内同步使用。为空时这些方向也使用私有编解码器。私有编解码器在该方向第一条压缩消息时创建 |
| `max_buffered_input_bytes_` | `default_max_websocket_message_bytes + 14` | `feed()` 后等待 `next_event()` 的字节独立上限（不含已返回帧占用的部分）；为 0 时构造抛 `std::invalid_argument`；超出时 `feed()` 返回 `backpressured` 且不消费 |
| `receive_storage_` | `{}` | 接收存储预算，见上节；未设置不计费 |

`websocket_connection` 是已升级连接的 Sans-I/O RFC 6455 驱动，只能移动（移动构造与移动赋值 `noexcept`）。

- 输入：默认构造的连接拥有自己的输入缓冲，经 `feed()` 输入；以 `http_io_buffer& input` 构造时从调用方缓冲读取，调用方也可在 `next_event()` 前直接向其追加收到的字节；借用缓冲必须比连接存活更久。
- 关闭：关闭握手后服务端在输出刷空时规划 `end_transport`；客户端保持传输打开（`awaiting_peer_eof`，期间 `feed()` 与 `next_event()` 丢弃输入），直到 `notify_transport_eof()`，因为服务端先关闭（RFC 6455 §7.1.1）。
- 借用：事件视图在下一次 `feed()`、`next_event()` 或向借用输入缓冲追加之前有效。输出字节在下一次 `next_event()`、`submit_frame()`、`submit_close()`、`consume_output()` 或析构之前有效。输入、EOF 与 `abort()` 不使在途写入失效（EOF / abort 后 `output_plan()` 不再暴露未发字节，但其存储保留至析构）。
- 线程：单个连接不可并发调用；回调（掩码生成、存储预算）在调用线程同步执行。

| 成员 | 语义 |
| --- | --- |
| 构造函数 | 校验选项：`max_buffered_input_bytes_` 为 0、`role_` 不是合法枚举值、`compression_` 窗口位越界、`client` 角色缺少 `mask_key_generator_` 时抛 `std::invalid_argument` |
| `feed(input)` | 追加输入。`input` 可以借用上一事件的负载：回收已消费输入前先取必要快照。返回 [`websocket_feed_status`](#api-websocket-connection-h)。右值拥有型字符串重载删除（避免在复制前就已失效的临时对象被误当作稳定输入） |
| `next_event()` | 解析下一事件：完整消息、Ping、Pong、Close、协议错误或传输结束；输入不足返回 `std::nullopt` 并更新 `needed_input_bytes()`。自动排队 Pong 与 Close 应答；协议违规时排队对应 Close 并返回 `protocol_error()` 事件。只能对左值调用，事件借用连接（`RUVIA_LIFETIMEBOUND`） |
| `needed_input_bytes()` | 每次 `next_event()` 开始时清零；该调用因输入不足返回 `std::nullopt` 时，设为解析当前帧还缺少的字节数。因此 0 表示上次 `next_event()` 不是因输入不足而返回 |
| `output_plan()` | 当前待写输出，见 `websocket_output_plan`；只能对左值调用 |
| `consume_output(bytes)` | 确认传输已写出 `bytes` 字节（先 `bytes()` 后 `borrowed_bytes()`），推进输出游标并计入 `bytes_sent()`；写完全部自有字节时释放超过 16 KiB 保留量的存储。全部输出确认后返回 `drained` 并推进关闭阶段：本端 Close 已排队则进入 `awaiting_peer_close`；最终输出（关闭握手后服务端的输出）写完则进入传输结束（`output_plan().disposition()` 为 `end_transport`）；客户端关闭握手应答写完则进入 `awaiting_peer_eof`。传输结束或已关闭后确认 0 字节返回 `drained`。超出待发总量返回 `out_of_range` 且不变 |
| `commit_transport_end()` | 驱动已按 `end_transport` 结束传输后调用，连接进入已关闭状态 |
| `notify_transport_eof()` | 传输读到 EOF：未关闭时进入传输结束阶段，并在尚无原因时记录 `eof` |
| `abort(cause)` | 本端中止：记录 `cause`（传 `none` 记为 `aborted`；已有原因保留），返回是否需要中止传输 |
| `liveness_mode()` / `termination()` | 当前存活阶段与首个结束原因 |
| `message_pending()` | 有消息输入未完成：分片消息尚未结束，或缓冲中有 `next_event()` 尚未返回的帧字节；驱动以此为单条消息计完成时限 |
| `bytes_received()` / `bytes_sent()` | RFC 6455 整帧线上字节（帧头、掩码键与负载，`permessage-deflate` 下为压缩后负载，数据帧与控制帧均计）。接收计 `next_event()` 解析的每一帧（被拒绝的帧不计），发送计 `consume_output()` 确认的输出；均饱和于 `std::uint64_t`，不含升级握手与下层分帧开销 |
| `submit_frame(opcode, std::string_view, options)` | 提交一个文本 / 二进制消息帧或 Ping / Pong，返回前把负载复制进连接输出 |
| `submit_frame(opcode, websocket_borrowed_payload, options)` | 同一提交，但按引用排队未压缩、未掩码的负载：`output_plan()` 在帧头之后以 `borrowed_bytes()` 暴露它；其字节须保持存活不变，直到 `consume_output()` 确认、`abort()` 或析构。掩码（客户端）帧、压缩帧或与待发输出重叠的负载仍被复制 |
| `submit_close(code, reason)` | 排队 Close 帧并进入关闭阶段，返回 [`websocket_close_submit_status`](#api-websocket-connection-h)；`code` 已由 `websocket_close_code` 保证在受支持集合内 |

`submit_frame` 的分片规则（RFC 6455 §5.4）：数据帧是其消息的一个分片；不延续任何未结束消息的最终帧是整条消息。`final_ = false` 时首帧以 `opcode` 开启消息（`compress_` 选择压缩时带 RSV1），之后同 opcode 的帧作为续帧（opcode 0）发送，最终帧结束消息。Ping、Pong、Close 可插在分片之间；另一 opcode 的数据帧在最终分片前返回 `message_in_progress`，非最终控制帧返回 `invalid_opcode`。整条消息在 deflate 不能缩小时保持不压缩；压缩的分片消息是一条跨分片的 DEFLATE 流，每个分片以 sync flush 结束（RFC 7692 §7.2.1）。文本跨分片边界校验 UTF-8，消息上限覆盖整条消息。被拒绝的帧使已开启消息保持开启。失败的提交保留已排队输出，并丢弃尚未发送的压缩历史；可以重试，除非重置编解码器也失败，此时 `liveness_mode()` 报告 `inactive`。分配失败与掩码键生成失败以异常传播，已排队输出保留。

```cpp
ruvia::websocket_connection ws({.compression_ = plan.compression()});
if (ws.feed(received) == ruvia::websocket_feed_status::backpressured) { /* 先取事件 */ }
while (auto event = ws.next_event()) {
    if (const auto* message = event->message()) {
        (void)ws.submit_frame(message->opcode(), message->payload());  // 回显：复制负载
    } else if (event->transport_end()) {
        break;
    }
}
const auto out = ws.output_plan();
// 写出 out.bytes() 与 out.borrowed_bytes() 后：
(void)ws.consume_output(out.size());
```

### 13.4 `"http_capsule.h"`
<a id="api-http-capsule-h"></a>

RFC 9297 Capsule Protocol 的增量帧解码、Capsule 头编码、`Capsule-Protocol` 字段解析，以及 HTTP Datagram（RFC 9297 §3.5 DATAGRAM Capsule）与 RFC 9298 UDP 负载的收发规划。无条件安装；公开 include [`"decode_step.h"`](#api-decode-step-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。源码：[`ruvia-http/include/http_capsule.h`](../ruvia-http/include/http_capsule.h)

#### 13.4.1 编码与 UDP Datagram 工具

```cpp
namespace ruvia {
enum class http_capsule_codec_error : std::uint8_t {
    value_out_of_range,
    output_too_small,
};
[[nodiscard]] std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept;

inline constexpr std::uint64_t http_datagram_capsule_type = 0;
struct http_udp_datagram_view final {
    std::uint64_t context_id_{0};
    std::span<const char> payload_{};
};
[[nodiscard]] std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept;
template </* 临时拥有型 char 范围 */ typename input_type>
std::optional<http_udp_datagram_view> decode_http_udp_datagram(input_type&&) noexcept = delete;
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id = 0) noexcept;
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept;
}
```

| `http_capsule_codec_error` | 含义 |
| --- | --- |
| `value_out_of_range` | 值超过 QUIC 变长整数上限（2^62 - 1） |
| `output_too_small` | 输出缓冲放不下编码 |

- `parse_http_capsule_protocol(value)`：按 RFC 9297 将 `Capsule-Protocol` 解析为 Structured Field Boolean Item（允许参数，前后只允许 SP）；返回 `true` / `false`，非 Boolean 或语法错误返回 `std::nullopt`。
- `http_datagram_capsule_type`：DATAGRAM Capsule 类型值 `0`。
- `http_udp_datagram_view`：`context_id_` 为 RFC 9298 Context ID，`payload_` 为其后的字节。
- `decode_http_udp_datagram(input)`：从 HTTP Datagram 负载读出 Context ID 变长整数；变长整数不完整返回 `std::nullopt`。`payload_` 借用 `input`。RFC 9298：Context ID 0 承载 UDP 负载，未知 Context ID 由调用方丢弃。
- `encode_http_udp_datagram_prefix(output, context_id = 0)`：把 Context ID 编码到 `output` 开头，返回写入字节数或错误。
- `encode_http_capsule_header(output, type, length)`：写出 Capsule 头（类型与长度两个 QUIC 变长整数），返回写入字节数；任一值越界返回 `value_out_of_range`，空间不足返回 `output_too_small`，失败时不写入。

#### 13.4.2 `class http_capsule_decoder`

```cpp
namespace ruvia {
struct http_capsule_event final {
    std::uint64_t type_{0};
    std::span<const char> payload_{};
    bool end_capsule_{false};
};
using http_capsule_callback_type = void (*)(void*, http_capsule_event);
enum class http_capsule_decode_error : std::uint8_t {
    truncated,
    limit_exceeded,
};
using http_capsule_decode_failure = decode_failure<http_capsule_decode_error>;
struct http_capsule_boundary final {};
using http_capsule_feed_result = decode_step<decode_need_input, http_capsule_boundary, decode_complete, http_capsule_decode_failure>;
struct http_capsule_options final {
    std::uint64_t max_capsule_length_{16 * 1024 * 1024};
};
enum class http_capsule_retention : std::uint8_t {
    all,
    datagrams,
    udp_payloads,
};

class http_capsule_decoder final {
public:
    explicit http_capsule_decoder(http_capsule_options config = {});
    [[nodiscard]] decoder_state state() const noexcept;
    void reset() noexcept;
    [[nodiscard]] http_capsule_feed_result feed(std::span<const char> input, bool fin, http_capsule_callback_type callback, void* context, http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed(std::span<const char> input, bool fin, visitor_type&& visitor, http_capsule_retention retention = http_capsule_retention::all);
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback, void* context, http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin, visitor_type&& visitor, http_capsule_retention retention = http_capsule_retention::all);
};
}
```

`http_capsule_event` 是一次回调交付的片段：`type_` 为 Capsule 类型，`payload_` 为负载片段，`end_capsule_` 为真表示该 Capsule 到此结束（空负载 Capsule 以单次 `{type, {}, true}` 交付）。片段只在回调期间有效：借用 `feed` 输入，或在 `udp_payloads` 模式下首片段（Context ID 字节）借用解码器内部存储。`http_capsule_callback_type` 为 C 风格回调，第一个参数是调用方上下文。

| `http_capsule_decode_error` | 含义 |
| --- | --- |
| `truncated` | 流在 Capsule 头或负载中途结束；回调抛异常后的失败状态也记为此值 |
| `limit_exceeded` | 保留的 Capsule 超过 `max_capsule_length_` |

`http_capsule_feed_result` 是 [`decode_step`](#api-decode-step-h) 实例，`consumed_bytes()` 为本次消费字节数，结果为以下之一：`decode_need_input`（需要更多输入）、`http_capsule_boundary`（仅 `feed_one()`：一个保留 Capsule 在已消费输入内结束）、`decode_complete`（流在 FIN 处干净结束）、`http_capsule_decode_failure`（失败，见 [通用契约](#2-通用契约)）。

`http_capsule_options::max_capsule_length_`（默认 16 MiB）限制被保留的 Capsule 长度；构造时超过 QUIC 变长整数上限抛 `std::invalid_argument`。被丢弃的 Capsule 无论多长都被跳过。

| `http_capsule_retention` | 含义 |
| --- | --- |
| `all` | 保留全部 Capsule，供通用 Capsule Protocol 读取方处理未知类型 |
| `datagrams` | 只保留 DATAGRAM Capsule（RFC 9297 §3.5） |
| `udp_payloads` | 只保留携带 RFC 9298 Context ID 0（UDP 负载）的 DATAGRAM；其他 Context ID 为未知并被丢弃。先检查 Context ID 再应用长度上限；短到放不下 Context ID 的 DATAGRAM 仍被保留，以便读取方按畸形拒绝 |

按 RFC 9297 §3.2，接收方静默丢弃并跳过不使用的 Capsule：被丢弃者不回调、不缓冲、不受长度上限约束。某个 Capsule 的保留与否由完成其头部（`udp_payloads` 下还包括 Context ID）的那次 feed 调用的 `retention` 决定。

`http_capsule_decoder` 是 HTTP/1 隧道的增量解码器：头部状态内联保存，不创建拥有型负载或事件队列。

| 成员 | 语义 |
| --- | --- |
| 构造函数 | 保存配置；`max_capsule_length_` 越界抛 `std::invalid_argument` |
| `state()` | [`decoder_state`](#api-decode-step-h)：活动、已完成或已失败 |
| `reset()` | 以构造时配置开始新的 Capsule 流 |
| `feed(input, fin, callback, context, retention)` | 消费全部 `input`（保留 Capsule 超限时除外），对保留 Capsule 的每个片段调用回调。`fin` 为真且全部消费时：处于 Capsule 中途返回 `truncated` 失败，否则进入完成状态返回 `decode_complete`；否则返回 `decode_need_input` |
| `feed_one(...)` | 在第一个完整的保留 Capsule 处停止，返回 `http_capsule_boundary`，未消费后缀留给调用方；FIN 只在全部输入消费后才提交，因此恰好在 FIN 处结束的 Capsule 报告边界且解码器已完成。便于拉取式适配器，无需缓冲完整 Capsule 队列 |
| 模板 `feed` / `feed_one`（visitor） | 将 `visitor` 以左值引用包装为回调转发到上面的重载；`visitor` 只在调用期间被借用 |

失败状态与完成状态：失败使解码器失败，完成（FIN）使其完成；此后每次调用都重复该结果，`consumed_bytes()` 为 0，不消费输入，直到 `reset()`。失败结果的 `consumed_bytes()` 为失败前已消费的字节。回调中再次调用同一解码器的 `feed` / `feed_one` 抛 `std::logic_error`；回调抛出的异常传播，解码器进入失败状态（`truncated`）。非线程安全。

#### 13.4.3 HTTP Datagram 会话与写计划

```cpp
namespace ruvia {
enum class http_datagram_error : std::uint8_t {
    send_closed,
    payload_too_large,
    malformed,
};

class http_datagram_write_plan final {
public:
    [[nodiscard]] constexpr std::string_view prefix() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view prefix() const&& = delete;
    [[nodiscard]] constexpr std::span<const char> payload() const noexcept;
};

enum class http_datagram_mode : std::uint8_t {
    raw,
    connect_udp,
};

class http_datagram_session final {
public:
    http_datagram_session() noexcept = default;
    void close_send() noexcept;
    void close_receive() noexcept;
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(http_datagram_mode mode, std::span<const char> payload) const noexcept;
    template </* 临时拥有型 char 范围 */ typename payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(http_datagram_mode, payload_type&&) const noexcept = delete;
    [[nodiscard]] std::variant<std::optional<std::span<const char>>, http_datagram_error> receive_datagram(http_datagram_mode mode, std::span<const char> input) const noexcept;
    template </* 临时拥有型 char 范围 */ typename input_type>
    std::variant<std::optional<std::span<const char>>, http_datagram_error> receive_datagram(http_datagram_mode, input_type&&) const noexcept = delete;
};
}
```

| `http_datagram_error` | 含义 |
| --- | --- |
| `send_closed` | 发送方向已经 `close_send()` |
| `payload_too_large` | 负载超出上限：`connect_udp` 为 65527 字节（65535 减 8 字节 UDP 头），`raw` 为 Capsule 长度可编码上限 |
| `malformed` | `connect_udp` 接收时 Context ID 变长整数不完整 |

| `http_datagram_mode` | 含义 |
| --- | --- |
| `raw` | DATAGRAM Capsule 值即 HTTP Datagram 负载原样，包含任何协议专用 Context ID |
| `connect_udp` | RFC 9298 隧道的一个 UDP 包：发送时加 Context ID 0 前缀并限制负载 65527 字节；接收时剥离 Context ID 0，静默丢弃其他 Context ID |

`http_datagram_write_plan` 是会话产出的有效只读输出快照，无公开构造函数（不可默认构造），不暴露可写前缀或长度成员。按 `prefix()` 然后 `payload()` 组成 scatter-gather 写入一个完整 DATAGRAM Capsule：

- `prefix()`：Capsule 头（类型 0 与长度）及 `connect_udp` 下的 Context ID，借用计划自身的内联存储，只允许左值调用（右值重载删除）；计划必须存活且不变，直到传输消费完前缀。
- `payload()`：仍借用调用方原始输入，寿命独立于计划；调用方须保留输入直到写入完成。
- 复制 / 移动计划得到独立的内联前缀与相同的负载借用，不分配、不复制负载。

`http_datagram_session` 表示一个已接受的 HTTP Datagram 通道：在请求协商后构造（客户端可在响应前乐观发送），不拥有内存或传输；默认收发均打开。`prepare_datagram` / `receive_datagram` 是唯一收发入口，返回的负载借用调用方输入。

**借用约束**：`decode_http_udp_datagram`、`prepare_datagram` 和 `receive_datagram` 拒绝临时拥有型字符容器及字符初始化列表；空 `{}` 仍表示空 span。

| 成员 | 语义 |
| --- | --- |
| `close_send()` / `close_receive()` | 关闭发送 / 接收方向（不可重新打开） |
| `prepare_datagram(mode, payload)` | 发送已关闭返回 `send_closed`；超限返回 `payload_too_large`；否则返回写计划 |
| `receive_datagram(mode, input)` | `input` 是完整 DATAGRAM Capsule 值（不含类型与长度）。返回 `input` 内的负载；空 `optional` 表示静默丢弃（接收已关闭，或 `connect_udp` 遇到未知 Context ID）。只有 `connect_udp` 报错：Context ID 畸形为 `malformed`，负载超过 65527 为 `payload_too_large` |

```cpp
ruvia::http_datagram_session session;
const std::span<const char> udp_payload = packet;  // 调用方存储
auto planned = session.prepare_datagram(ruvia::http_datagram_mode::connect_udp, udp_payload);
if (const auto* plan = std::get_if<ruvia::http_datagram_write_plan>(&planned)) {
    // 依次写出 plan->prefix() 与 plan->payload()；写完前保留 plan 与 packet
}
```

Web 层隧道（`http_tunnel`、`http_capsule_stream`）如何使用这些类型见 [ruvia-web](ruvia-web.md)。

### 13.5 `"http_connect_udp.h"`
<a id="api-http-connect-udp-h"></a>

RFC 9298 CONNECT-UDP 的目标值类型、默认 URI 模板路径编解码、请求与响应头校验，以及服务端响应规划（HTTP/1.1 GET Upgrade 与 HTTP/2、HTTP/3 Extended CONNECT）。无条件安装；公开 include [`"http_header.h"`](#api-http-header-h)、[`"http_message.h"`](#api-http-message-h)、[`"http_request_head_view.h"`](#api-http-request-head-view-h)、[`"http_response.h"`](#api-http-response-h)、[`"ip_address.h"`](#api-ip-address-h)、[`"borrowed_text.h"`](#api-borrowed-text-h)。Capsule 与 Datagram 帧见 [`"http_capsule.h"`](#api-http-capsule-h)。源码：[`ruvia-http/include/http_connect_udp.h`](../ruvia-http/include/http_connect_udp.h)

#### 13.5.1 目标类型与错误

```cpp
namespace ruvia {
struct http_connect_udp_target_view final {
    borrowed_text host_{};
    port_number port_{};
};
struct http_connect_udp_target final {
    http_connect_udp_target() noexcept = default;
    http_connect_udp_target(std::string host, port_number port = {}) noexcept;
    http_connect_udp_target(const http_connect_udp_target&) = default;
    http_connect_udp_target(http_connect_udp_target&&) noexcept = default;
    http_connect_udp_target& operator=(const http_connect_udp_target&) = default;
    http_connect_udp_target& operator=(http_connect_udp_target&&) noexcept = default;

    std::string host_;
    std::uint16_t port_{0};
};
enum class http_connect_udp_error : std::uint8_t {
    invalid_target,
    invalid_path,
    invalid_request,
    invalid_response,
    invalid_capsule_protocol,
};
}
```

- `http_connect_udp_target_view`：借用型目标，`host_` 为主机（reg-name 或不带方括号与 zone ID 的 IPv6 字面量），`port_` 为 [`port_number`](#api-ip-address-h)（默认 0）。
- `http_connect_udp_target`：拥有型目标。用 `http_connect_udp_target{std::string(host), port}` 构造，`port` 经 `port_number` 检查（整数可隐式转换，越界在转换处抛 `std::invalid_argument`）后存为 `std::uint16_t`；`host_`、`port_` 为公开成员。可复制、移动。

| `http_connect_udp_error` | 含义 |
| --- | --- |
| `invalid_target` | 主机为空、含 `%`、非法 reg-name 或 IPv6，路径中含未转义的 `:`，或端口为 0 / 非法 |
| `invalid_path` | 路径不符合 `/.well-known/masque/udp/{host}/{port}/` 结构 |
| `invalid_request` | CONNECT-UDP 请求头不合法 |
| `invalid_response` | CONNECT-UDP 响应不合法 |
| `invalid_capsule_protocol` | 发送方要求的 `Capsule-Protocol: ?1` 缺失、非 Boolean、为 `?0` 或重复 |

#### 13.5.2 路径与请求 / 响应校验

```cpp
namespace ruvia {
[[nodiscard]] std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target);
[[nodiscard]] std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path);

[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_protocol_version version, const http_request_head_view& request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(http_protocol_version version, const http_request_head_view& request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_response(http_protocol_version version, std::uint16_t status, const http_header_range_view& headers) noexcept;
[[nodiscard]] bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept;
[[nodiscard]] std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version);
[[nodiscard]] http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response);
}
```

| 函数 | 语义 |
| --- | --- |
| `encode_http_connect_udp_path(target)` | 按 RFC 9298 默认 URI 模板生成 `/.well-known/masque/udp/{host}/{port}/`：主机中所有非 unreserved 字节百分号编码（IPv6 的 `:` 编码为 `%3A`），端口为十进制。主机非法或端口为 0 返回 `invalid_target`；分配失败抛异常 |
| `parse_http_connect_udp_path(path)` | 逆操作：前缀、两段与结尾 `/` 不符返回 `invalid_path`；主机段含未转义 `:`、端口非 `1..65535` 十进制、解码后主机非法返回 `invalid_target`；成功返回拥有型目标 |
| `validate_http_connect_udp_request(version, request)` | 接收方检查。HTTP/1.1：方法 `GET`，`Upgrade` 含 `connect-udp`、`Connection` 含 `upgrade`，有 `Host`；HTTP/2、HTTP/3：方法 `CONNECT`、`protocol_` 为 `connect-udp`、`scheme_` 为 `http` / `https`，不带连接专用字段。两者都要求非空 `authority_`（HTTP/1.1 为 Host，且与 Host 一致）与 origin-form `target_`；HTTP/1.0 拒绝。`connect-udp` token 本身选择 Capsule Protocol，因此 `Capsule-Protocol` 可选，非 Boolean、`?0` 或重复的字段按缺失处理（RFC 9297 §3.4）。失败返回 `invalid_request` |
| `validate_http_connect_udp_request_to_send(version, request)` | 本端发出的请求：接收方规则之外，必须恰好一个 `Capsule-Protocol: ?1`（RFC 9297 §3.4 的 SHOULD，对 Ruvia 发送方强制），否则 `invalid_capsule_protocol` |
| `validate_http_connect_udp_response(version, status, headers)` | 客户端校验响应：HTTP/1.1 须为 `101` 且带 `Upgrade` / `Connection: Upgrade`；HTTP/2、HTTP/3 须为 2xx 且不是 204/205/206（RFC 9297 §3.2 视为畸形），不带 `Upgrade` / `Connection` 或连接专用字段；HTTP/1.0 拒绝。失败返回 `invalid_response` |
| `is_http_connect_udp_upgrade_request(request)` | 只按 HTTP/1.1 `GET` 与 `Upgrade` token 分类是否为 CONNECT-UDP 升级请求；完整校验仍须调用 |
| `validate_http_connect_udp_request(request)` | 从已解析的 [`http_request`](#api-http-request-h) 取出方法、协议、scheme、authority（缺失时用 Host）、路径与首部，执行上面的接收方检查；absolute-form 目标的查询串非法时返回 `invalid_request` |
| `prepare_http_connect_udp_response(response, version)` | 接收应用响应元数据并校验：须为 2xx 且不是 204/205/206，无正文或文件正文，不带 `Upgrade` / `Connection` 等受控字段，否则 `invalid_response`；应用自带的 `Capsule-Protocol` 必须是单个 `?1`，否则 `invalid_capsule_protocol`。缺失时补 `Capsule-Protocol: ?1`；HTTP/1.1 下把状态改为 `101` 并补 `Connection: Upgrade`、`Upgrade: connect-udp`。HTTP/1 的 101 只能经此专用边界产生 |
| `prepare_http1_connect_udp_response_head(response)` | 对 `prepare_http_connect_udp_response()` 产出的 HTTP/1.1 响应生成 [`http1_response_head_plan`](#api-http1-server-response-h)（连接按关闭分帧，后续字节属于隧道）；其他响应抛 `http1_response_head_error` |

```cpp
auto path = ruvia::encode_http_connect_udp_path({.host_ = "2001:db8::1", .port_ = 443});
// std::get<std::string>(path) == "/.well-known/masque/udp/2001%3Adb8%3A%3A1/443/"
auto target = ruvia::parse_http_connect_udp_path(std::get<std::string>(path));
```

Web 层 CONNECT-UDP 路由与隧道驱动见 [ruvia-web](ruvia-web.md)。
