# `ruvia-core` 运行时与 `ruvia-http` 协议层 API 手册

本文档提供 `ruvia::core`（独立协程与事件循环运行时底座）和 `ruvia::http`（独立 Sans-I/O HTTP/1.0、HTTP/1.1、HTTP/2、HPACK、WebSocket 与内容编解码协议层）的公开头文件与核心 API 参考。两库均可脱离 `ruvia::web` 独立使用。所有公开符号位于 `namespace ruvia`（状态码常量位于 `namespace ruvia::http_status`）。

---

## 第一部分：`ruvia::core` 运行时 API

### 1. 协程、任务域与 Asio 桥接

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/task.h"` | `template <typename T> class task` | 结构化惰性协程（仅支持移动构造与右值 `co_await`）。未启动（cold）或已完成（done）可安全析构；**销毁已启动但仍挂起的 `task` 直接触发 `std::terminate()`** |
| `"ruvia/core/root_task.h"` | `template <typename T> class root_task final` | 由 `event_loop::start(task<T>)` 返回的跨线程等待句柄。提供 `valid()`、`wait()`、`get()`；析构时若未调用 `get()` 则转为后台托管直至完成，**禁止在所属 `event_loop` 自身线程调用 `wait()` / `get()`** |
| `"ruvia/core/task_scope.h"` | `class task_scope final`、`struct task_scope_options` | 单 Worker 亲和的结构化并发任务域。提供 `spawn(task<void>)`、`request_stop()`、`close_and_join()`、`active_count()`、`take_error()`；析构前若曾 `spawn` 过任务，必须完成 `co_await scope.close_and_join()` |
| `"ruvia/core/scoped_operation.h"` | `class operation_scope final`、`class scoped_capability_registration final`、`make_scoped_operation(...)` | 将借用型能力（capability）与惰性协程操作 `task<T>` 绑定到同一 Worker 作用域，`close_and_join()` 先回收冷协程帧并等待已启动操作完成后再失效 capability |
| `"ruvia/core/async.h"` | `template <typename T> asio::awaitable<T> as_awaitable(task<T> task_value)`、`template <typename Result = void, typename Initiate> auto async_asio(Initiate&& initiate)` | 将 `ruvia::task<T>` 适配为 `asio::awaitable<T>`，或将基于完成回调的 Asio 异步启动操作适配为 Ruvia 协程可直接 `co_await` 的 awaiter |

---

### 2. 停止令牌、超时、截止期与定时器

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/stop_token.h"` | `class stop_source final`、`class stop_token final`、`class stop_registration final`、`combine_stop_tokens(first, second)` | 零堆分配侵入式回调注册的协作式取消原语。单个 `stop_token` 最多合并观察 `max_stop_token_sources`（4）个源；`stop_registration` 析构时自动注销并同步等待正在执行的回调返回 |
| `"ruvia/core/operation_timeout.h"` | `struct operation_options final`、`validate_operation_options`、`merge_operation_options`、`class operation_timeout final`、`template <typename Kind = std::monostate> class operation_deadline final` | 通用操作选项（`timeout_` 与 `stop_token_`）、跨异步阶段共享的 `steady_clock` 截止时间计算器与零分配三态（inactive / active / expired）截止期状态机 |
| `"ruvia/core/worker_timer.h"` | `class worker_timer_registration final`、`class worker_timer_cancellation final`、`enum class worker_timer_outcome { expired, cancelled }` | 单 Worker 侵入式定时器注册句柄，配合 `worker_handle::schedule_timer` 使用；提供 `cancel()`（静默取消）与 `cancel_and_notify()` |
| `"ruvia/core/timer.h"` | `enum class timer_sleep_result { elapsed, stop_requested }`、`sleep_for(const worker_handle&, duration, [stop_token])` | 在绑定 Worker 上挂起当前协程指定时长；若传入的 `stop_token` 触发或 Worker 停止定时器，返回 `stop_requested` 而不抛异常 |

---

### 3. 事件循环、Worker 运行时与阻塞卸载池

```cpp
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/timer.h"

ruvia::event_loop_pool pool({.loop_count_ = 2, .queue_capacity_ = 1024});
pool.start();

ruvia::event_loop loop = pool.next_loop();
auto root = loop.start([](ruvia::worker_handle worker) -> ruvia::task<int> {
    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
    co_return 42;
}(loop.handle()));

int answer = root.get();
pool.join();
```

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/event_loop.h"` | `class event_loop final`、`class event_loop_stop_registration final`、`class event_loop_attachment final`、`attach_event_loop(asio::io_context&, options = {})` | 指向单个 Worker 运行时及其 `asio::io_context` 的轻量句柄（提供 `start(task<T>)`、`post(fn)`、`defer_cleanup(fn)`、`on_stop(fn)`、`handle()`、`io_context()`、`executor()`、`is_current()`），以及将 Ruvia Worker 挂载到外部自管 `asio::io_context` 的适配器 |
| `"ruvia/core/event_loop_pool.h"` | `class event_loop_pool final`、`struct event_loop_pool_options` | 管理固定数量独立事件循环线程的池。提供 `start()`、`stop()`、`join()`、`loop_count()`、`loop(index)`、`next_loop()`、`loop_for(key)` |
| `"ruvia/core/worker_handle.h"` | `class worker_handle`、`using worker_id_type = std::uint64_t`、`enum class post_status`、`template <typename Sig> class post_outcome final` | 跨线程安全的稳定派发端点与有界队列投递结果。提供 `valid()`、`accepting()`、`is_current()`、`id()`、`post(fn)`、`post_factory(factory)`、`schedule_timer(...)` |
| `"ruvia/core/worker_runtime.h"` | `class worker_runtime final`、`struct worker_runtime_options`、`struct worker_runtime_hooks`、`enum class runtime_lifecycle_phase` | 底层单 Worker 线程与两阶段启停状态机（`startup -> ready -> serve -> stop_admission -> finalize -> drain -> shutdown -> join`） |
| `"ruvia/core/worker_group.h"` | `class worker_group final` | 协调多个 `worker_runtime` 同步完成两阶段启动屏障（`start()` / `serve()`）与停机回收（`request_stop()` / `join()`） |
| `"ruvia/core/blocking_pool.h"` | `class blocking_pool final`、`template <typename T> class blocking_result final`、`run_blocking(pool, worker_handle, [timeout], [stop_token], fn)`、`run_blocking(worker_handle, operation_options, fn)` | 固定大小阻塞卸载线程池。`run_blocking` 在绑定 Worker 上 `co_await`，将 `fn` 提交至池线程并在完成后回到原 Worker 恢复协程；不显式传池的重载使用 core 拥有的进程级有界池（默认线程数与队列容量），用于独立运行时也必须可用的系统阻塞操作。超时、取消或 Worker 停机先释放等待者；已开始的调用继续完成，但只持有自身数据，迟到结果被丢弃 |

---

### 4. 跨线程通道、单次交付、信号与内存池

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/worker_wait_result.h"` | `enum class worker_wait_status { value, closed, worker_stopping, timed_out, cancelled }`、`template <typename T> class worker_wait_result final` | Worker 等待操作的统一返回包装（提供 `status()`、`has_value()`、`value()`、`take_value()`） |
| `"ruvia/core/channel.h"` | `make_channel<T>(worker_handle, channel_options)`、`class channel_sender<T>`、`class channel_receiver<T>` | 有界 MPSC 通道：`channel_sender` 可拷贝并支持多生产者跨线程 `send(value)`；`channel_receiver` 绑定单个目标 Worker 并支持 `receive()` / `receive_for(...)` |
| `"ruvia/core/one_shot.h"` | `make_one_shot<T>(worker_handle, one_shot_options = {})`、`class one_shot_completion<T>`、`class one_shot_receiver<T>` | 跨线程单次值交付原语：`complete(value)` 可在任意线程调用一次，`wait()` / `wait_for(...)` 在绑定 Worker 上等待 |
| `"ruvia/core/worker_signal.h"` | `class worker_signal final` | 同 Worker 内部零分配单等待者唤醒原语（`wait()` 与 `notify()` 均须在所属 Worker 线程调用） |
| `"ruvia/core/worker_notification.h"` | `class worker_notification final` | 跨线程闩锁合并通知原语（任意线程无锁 `notify()`，所属 Worker 上 `co_await notification.wait()`） |
| `"ruvia/core/pool_lease_scheduler.h"` | `class pool_lease_scheduler final`、`class pool_lease_acquire final` | 单 Worker 连接/槽位池租约调度器（空闲时同步返回槽位下标，满载时挂起排队并支持超时与取消） |
| `"ruvia/core/memory/memory_pool.h"` | `class worker_memory final`、`class request_memory final`、`struct memory_pool_config` | Worker 级可回收内存池与单次请求单调 Arena（支持 `allocator<T>()`、`resource()`、`fork()`、`trim(owner)`） |
| `"ruvia/core/memory/pmr_object.h"` | `std::pmr::memory_resource* process_resource() noexcept`、`pmr_resource_or_default(resource)`、`construct_pmr_object`、`destroy_pmr_object`、`make_pmr_object` | 进程级线程安全 mimalloc PMR 适配资源、回退辅助与 PMR 对象构造/销毁工具 |
| `"ruvia/core/move_only_function.h"` | `template <typename Sig> class move_only_function` | 带 `3 * sizeof(void*)` 小对象内联优化（SBO）的仅移动可调用包装器 |
| `"ruvia/core/local_ring_queue.h"` / `"ruvia/core/mpsc_ring_queue.h"` | `class local_ring_queue<T>`、`class mpsc_ring_queue<T>` | 预分配容量的单线程零分配环形队列与多生产者单消费者有界环形队列 |
| `"ruvia/core/bytes.h"` / `"ruvia/core/failure_report.h"` / `"ruvia/core/socket.h"` | `as_bytes`、`as_chars`、`error_code_text`、`system_error_text`、`parse_ip_address` | 字节/字符视图转换、跨平台 UTF-8 系统错误文本转换、严格 IPv4/IPv6 字面量解析（返回 `std::optional<asio::ip::address>`） |

框架默认内存由 mimalloc 承载：`process_resource()` 是存活至进程结束的线程安全 PMR 适配器，不再叠加进程级共享 pool；默认 Worker 内存、停止状态与协程帧使用该资源。Worker 的小对象 pool、大块缓存、协程帧缓存及 `trim(owner)` 契约保持不变。接受显式 PMR 的 API 始终优先使用调用方资源，调用方仍须保证资源覆盖所分配对象的生命周期。

非 Windows 构建启用 mimalloc 普通 `override`，在最终链接中保留静态库，对标准 `malloc` / `free` 与全局 `new` / `delete` 使用 mimalloc；不得叠加另一套全局 allocator replacement。Windows 保持原静态 CRT 与普通 mimalloc 依赖，仅通过显式 API 承载框架资源和协程帧，不替换宿主标准分配接口。两种接入都不替换 `std::pmr::get_default_resource()` 对象，也不改变显式 PMR 的优先级。独立 `ruvia::http` 不依赖 core 或 mimalloc，未显式指定 PMR 时仍遵循调用方的标准 PMR 默认资源。

---

## 第二部分：`ruvia::http` Sans-I/O 协议层 API

`ruvia::http` 不执行任何 socket I/O、定时器或线程调度，全部协议状态机均以内存字节流（`std::string_view` / `std::span` / `http_io_buffer`）与强类型事件/计划对象交互。

### 1. 基础类型、缓冲与编码工具

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/borrowed_text.h"` | `class borrowed_text final` | 只读字符串借用视图，拒绝直接绑定临时拥有型字符串，但不延长底层文本生命周期；集合 option 使用 `std::span<const T>` 借用连续存储，调用方必须保持存储有效直到消费入口完成读取或拥有化，`span` 本身不阻止悬垂 |
| `"ruvia/http/http_io_buffer.h"` | `class http_io_buffer final` | PMR 流式读写缓冲区：`prepare(n)`、`commit(n)`、`append(bytes)`、`view()`、`consume(n)`、`clear_retaining_small()` |
| `"ruvia/http/http_protocol_version.h"` / `"ruvia/http/http_known_method.h"` | `http_protocol_version`（`http10`, `http11`, `http2`）、`http_scheme`（`http`, `https`）、`http_known_method`、`classify_http_method`、`is_valid_http_method_token` | 协议版本、Scheme 与 HTTP 方法分类及幂等/安全性判断 |
| `"ruvia/http/http_status.h"` | `class http_status_code final`、`namespace ruvia::http_status`、`http_reason_phrase` | 强类型 HTTP 状态码（`100..599`）与标准状态码常量（`http_status::ok`、`not_found` 等） |
| `"ruvia/http/http_protocol_error.h"` | `class http_protocol_error final : public std::exception`、`enum class http_parse_error` | 零动态分配的协议错误异常（`http_protocol_error(status, code, message)`：携带 `http_status_code`、静态存储期的稳定 snake_case 机器码 `code()`（如 `missing_host`、`conflicting_content_length`）与定长诊断文本）与解析错误枚举 |
| `"ruvia/http/http_request_target.h"` | `http_authority_view`、`http_origin_view`、`parse_http_authority`、`is_valid_http_origin_form_target`、`is_valid_http_connect_authority`、`is_valid_http_serialized_origin` | RFC 9110 / RFC 3986 请求目标、Authority 与 Origin 语法校验和解析 |
| `"ruvia/http/http_target_origin.h"` | `decide_http_target_origin`、`http_target_origin`、`http_target_origin_connection`、`http_target_origin_action` | RFC 9110 §7.4 目标源判定：服务器提供连接事实（是否 TLS、对端是否可信网关），返回接受、需按证书校验的主机（DNS 名去掉末尾点 / IPv4 / IPv6 网络字节序）或 `421` 拒绝；明文连接上的 `https` 目标直接判为 misdirected，零分配 |
| `"ruvia/http/url_encoding.h"` / `"ruvia/http/http_hex.h"` / `"ruvia/http/http_base64.h"` | `decode_url_component`、`validate_url_encoding`、`visit_url_encoded_pairs`、`percent_encode_uri_reference`、`uri_paths_equivalent`、`compare_uri_paths`、`http_encode_lower_hex`、`http_base64_encode`、`http_base64_decode` | URL 百分号/表单编解码、RFC 3986 URI-reference 规范化编码、路由使用的路径规范形式比较（字面 `/` 分段、段内按百分号解码后的字节比较，`%2F` 不作分隔符）、十六进制转换、标准与 URL-safe Base64 编解码 |

---

### 2. HTTP 首部、缓存、条件请求、范围与 Cookie

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_header.h"` / `"ruvia/http/http_field_syntax.h"` / `"ruvia/http/http_protocol_managed_fields.h"` | `class http_header final`、`class http_header_view final`、`is_valid_http_header_name`、`is_valid_http_header_value`、`is_http_ows_char`、`is_http_protocol_managed_request_field` | PMR 拥有型首部键值对、非拥有型首部视图、RFC 9110 字段语法校验及分帧/连接专用请求字段判定 |
| `"ruvia/http/http_accept_match.h"` / `"ruvia/http/http_accept_encoding.h"` | `class http_accept_match final`、`http_response_coding_selection_for(request)` | `Accept` 媒体类型/语言权重匹配与 `Accept-Encoding` 响应压缩算法协商 |
| `"ruvia/http/http_date.h"` / `"ruvia/http/http_cache.h"` | `format_http_date`、`parse_http_date`、`class cache_control final`、`parse_cache_control` | IMF-fixdate 日期格式化/解析与 `Cache-Control` 指令解析 |
| `"ruvia/http/http_conditional_request.h"` / `"ruvia/http/http_byte_range.h"` / `"ruvia/http/http_representation_response_plan.h"` | `get_http_etag_preconditions`、`resolve_http_byte_range_set`、`plan_http_representation_response` | 条件请求（`If-Match` / `If-None-Match` / `If-Modified-Since` / `If-Unmodified-Since` / `If-Range`）与 `Range` 字节范围响应计划计算 |
| `"ruvia/http/cookies.h"` / `"ruvia/http/http_set_cookie.h"` | `struct cookie_options`、`parse_set_cookie`、`class set_cookie_plan final` | Cookie 属性校验（含 `__Secure-` / `__Host-` 前缀规则）、`Set-Cookie` 解析与序列化计划 |
| `"ruvia/http/http_priority.h"` | `struct http_priority`、`struct http_priority_fields`、`parse_http_priority`、`encode_http_priority` | RFC 9218 可扩展 HTTP 优先级首部与 HTTP/2 `PRIORITY_UPDATE` 帧编解码 |

---

### 3. 消息模型、内容压缩编解码、Multipart 与 SSE

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_request.h"` / `"ruvia/http/http_request_trailers.h"` | `class http_request final`、`make_parsed_http_request`、`class http_request_trailers final` | 请求头与请求体零拷贝视图描述符、大小写不敏感首部/Cookie 查找与请求 Trailer 校验收集 |
| `"ruvia/http/http_response.h"` / `"ruvia/http/http_interim_response.h"` | `class http_response final`、`class http_response_headers final`、`class http_interim_response_head final`、`plan_buffered_http_response_write` | 最终响应（`200..599`）与临时响应（`1xx`）模型，以及针对 `HEAD` / `204` / `205` / `304` 的响应体写计划。`head_metadata_body()` 将正文替换为描述 HEAD 所选表示的空正文；`has_head_metadata_body()` 查询该状态。显式 `Content-Length` 保留为 GET 表示的长度，缺失时不推断为零；重新设置正文会清除标记 |
| `"ruvia/http/http_content_coding.h"` / `"ruvia/http/http_content_codec.h"` / `"ruvia/http/http_content_encoder.h"` | `enum class http_content_coding { identity, gzip, deflate, brotli, zstd }`、`encode_http_content`、`decode_http_content`、`class http_content_encode_context final`、`class http_content_encode_error final`、`class http_content_encoder final` | 单次缓冲、可复用缓冲上下文与流式增量的 `gzip` / `deflate` / `br` / `zstd` 内容压缩，以及带解压上限保护的解码器 |
| `"ruvia/http/multipart_parser.h"` | `parse_multipart_boundary`、`parse_multipart_body`、`class multipart_parser final` | 一次性与流式增量（`feed` / `poll`）的 `multipart/form-data` 解析器 |
| `"ruvia/http/sse.h"` | `struct sse_message final`、`format_sse_message` | Server-Sent Events 消息格式化与校验 |

`encode_http_content` 的两个 overload 与 `http_content_encode_context::encode` 统一返回 `std::optional<std::pmr::string>`：有值表示完整编码成功，`std::nullopt` 只表示结果超过 `max_encoded_bytes_`；真实编码器故障抛出 `http_content_encode_error`，分配异常保留原始类型。预算为零只允许零字节表示；多 coding 按声明顺序编码，每个中间表示都受同一预算约束。输出使用 `options.resource_`；可复用上下文的状态使用构造时的资源，该资源必须覆盖上下文寿命，且上下文不可并发使用。成功、超预算或异常后上下文均可继续编码独立表示，不携带上一次的压缩历史。解码 API 和增量编码器保持各自的错误契约。

Web 缓冲响应用“小于原文”的预算尝试压缩，超预算是正常的“不划算”结果，仍回退 identity；请求拒绝 identity 时不设该预算，任何可接受的编码都优于 `406`。真实故障沿现有失败边界记录并翻译，不作为预算结果忽略。静态根预压缩同样只略过不划算的表示，真实编码异常使构建或刷新失败，不发布不完整的新快照。

---

### 4. HTTP/1.x 与 HTTP/2 Sans-I/O 状态机

#### 4.1 HTTP/1.0 & HTTP/1.1 状态机

- **服务端解析与序列化**：
  - `"ruvia/http/http1_server_request_parser.h"`：`http1_server_request_parse_state::parse_head(buffer, offset, resource)` 解析请求行、首部、连接复用计划（`http1_request_connection_plan`）与请求体分帧计划（`http1_request_body_plan`）。首行不包含 HTTP-version 词的明文输入（例如 TLS 记录、SSH banner）按非 HTTP 流量静默丢弃，不返回状态行；只有声明了版本号的非法请求行才回 400（RFC 9112 §2.2 的 400 为 SHOULD）。
  - `"ruvia/http/http1_chunked_body_decoder.h"`：`http1_chunked_body_decoder::decode(available)` 增量解码 `chunked` 请求体与 Trailer。Trailer 中的 obs-fold 由 `http1_chunked_body_decoder_config::obs_fold_` 决定：默认 `reject`（请求，服务端可回 `400`），`accept` 时按展开后的字段校验并原样发布字节，由使用方展开（RFC 9112 §5.2）。
  - `"ruvia/http/http1_server_semantics.h"` & `"ruvia/http/http1_response_head_plan.h"`：提交最终响应头与流式响应头分帧计划。
- **客户端写出与响应解析**：
  - `"ruvia/http/http1_client_request_writer.h"` & `"ruvia/http/http1_request_content_writer.h"`：生成客户端请求头与流式分块请求体，产出 `http1_client_exchange_state`。
  - `"ruvia/http/http1_client_response_parser.h"` & `"ruvia/http/http1_client_response_body_decoder.h"`：增量解析服务端 `1xx` 临时响应、最终响应头及定长/分块/EOF 定界响应体。客户端按接收方规则处理响应：首部与 Trailer 中的 obs-fold 均替换为空格（响应体解码器发布的 Trailer 块已展开，仅含折叠时从解码器的 PMR 分配一次）；`000..099` 与 `600..999` 状态码按 `500` 处理，`received_status()` 保留原值；HTTP/1.0 响应携带 `Transfer-Encoding` 时按分帧故障处理：忽略 transfer codings，回退到 `Content-Length` 或关闭定界，并在该响应后关闭连接（RFC 9112 §6.1，HEAD/204/304 同样关闭）；`1xx` 永不终止交换，其 `Connection` 选项不改变连接生命周期，最终响应才决定复用（RFC 9110 §15.2）；`Connection` 仅拒绝点名影响响应处理的字段（`Content-Length`、`Transfer-Encoding`、`Set-Cookie`），请求端接收方另拒绝 `Host`、`Authorization`、`Cookie`、`Origin`。发送方（客户端请求与服务端响应）不得发送点名 `Content-Length`、`Transfer-Encoding`、`Host` 等由协议层管理或面向全部接收方的字段的 `Connection` 选项（RFC 9110 §7.6.1），`TE`、`Upgrade`、`keep-alive` 与自定义逐跳字段仍可列出。

HTTP/1 请求头与响应头解析的字段描述符共用内部存储（`parser/http_header_descriptor_storage.h`）：前 32 项内联，超出后整体迁入传入的 PMR（缺省为默认资源）并按实际字段数增长。服务端 `parsed_request_header_block` 与客户端 `http1_client_parsed_response_head` 均不按首部字节上限预留线程栈数组，也不增加字段数量限制；64 KiB 首部字节上限仍然生效。服务端请求视图零拷贝借用输入缓冲区，客户端最终响应头拥有字段数据，原始输入可在解析完成后释放。PMR 分配失败以 `std::bad_alloc` 传出，此时不发布 head_ready 或成功状态；释放资源后可用同一缓冲区与偏移重试，但部分元数据可能残留，不保证回滚。PMR 须存活至其拥有的描述符存储释放：服务端请求为 `http_request::reset()` 或请求对象析构，客户端响应头为 `take_headers()` 返回的向量或响应头析构。

#### 4.2 HTTP/2 与 HPACK 状态机（`"ruvia/http/http2_connection.h"`, `"ruvia/http/hpack.h"`）

`ruvia::http2_connection` 实现了符合 RFC 9113 / RFC 7541 / RFC 8441 的完整客户端与服务端 Sans-I/O HTTP/2 连接状态机：

```cpp
#include "ruvia/http/http2_connection.h"

// 构造客户端或服务端状态机：
auto conn = ruvia::http2_connection::server({.resource_ = resource});

// 1. 喂入从网络收到的原始字节：
auto feed_res = conn.feed(inbound_bytes);

// 2. 轮询消费协议事件：
while (auto ev = conn.next_event()) {
    if (auto* req_head = ev->request_head()) {
        std::uint32_t stream_id = req_head->stream_id();
        ruvia::http_response resp(resource);
        resp.status(ruvia::http_status::ok);
        resp.body("hello h2");
        (void)conn.submit_buffered_response(stream_id, resp);
        (void)conn.release(std::move(*req_head));
    }
}

// 3. 取出待发往网络的编码字节（按整帧边界分批，游标始终停在帧边界）：
std::pmr::string out(resource);
while (conn.wants_write()) {
    out.clear();
    (void)conn.take_output_batch(16384, out);
    // 写出 out
}
```

- **流控凭证 `http2_received_data_credit`**：`message_body_chunk()` 与 `tunnel_data()` 事件携带移动专属的流控凭证；当上层消费完缓冲数据后，通过析构凭证或调用 `conn.acknowledge(std::move(credit))` 触发连接与流级 `WINDOW_UPDATE` 帧发送，实现精确背压。
- **服务端请求租约 `http2_request_head_event`**：持有服务端流请求头的底层内存租约；请求处理完毕后调用 `conn.release(std::move(event))` 归还存储。
- **优雅排空 `begin_drain()` / `finish_drain()` / `draining()` / `drain_final()`**：服务端按 RFC 9113 §6.8 两阶段排空——先发 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳在途的流；收到该 `PING` 的 ACK 或所有者调用 `finish_drain()` 后发出携带最后对端流 ID 的最终 `GOAWAY` 并拒绝之后的流（对端此间新开超过 128 条流时提前进入最终阶段，属产品上限）。RFC 只要求至少一个往返时间，不要求等到 ACK：所有者以 `finish_drain()` 为往返等待设定时间上限；未在排空时调用则直接以最终边界进入排空（适用于对端已结束输入、无法再开流）。收到对端合法 `GOAWAY` 时同样立即进入最终阶段（其发送方不得再开流），包括已在往返中的排空。`drain_final()` 为真后，所有者在剩余流结束时即可关闭连接。客户端角色一步发出最终 `GOAWAY`。
- **服务端 `CONNECT` 乐观数据**：接受/拒绝前收到的 `DATA` 以 `tunnel_data()` 事件发布，其流控凭证由所有者持有到接受隧道后消费；`submit_connect_response_head()` 接受后再发布 `tunnel_end()`（若请求半边已结束）。非 2xx 拒绝后到达的 `DATA` 丢弃并归还流控额度。RFC 8441 `:protocol = websocket` 的提前 `DATA` 同样保留（RFC 6455 §4.1 的等待规则只约束客户端）。非 2xx 拒绝响应后服务端以 `RST_STREAM(NO_ERROR)` 结束流。已建立隧道上的未知扩展帧被丢弃（RFC 9113 §5.5），`ALTSVC`/`ORIGIN` 帧也被忽略（RFC 7838 §4、RFC 8336 §2.1）。
- **客户端 `REFUSED_STREAM`**：客户端在收到任何响应头（含 `1xx` 临时响应与最终响应头）前收到 `RST_STREAM(REFUSED_STREAM)` 时发布 `request_unprocessed()` 而不是 `stream_closed()`（RFC 9113 §8.7：请求未被处理，可安全重试），连接保持可用；`http_client` 对无请求体的请求重试一次。
- **Server Push 并发**：`submit_push_request()` 在已打开的服务端推送流（预留状态不计入，RFC 9113 §5.1.2）达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 时返回 `http2_push_submit_error::stream_limit`，不发送 `PUSH_PROMISE`（RFC 9113 §8.4）。
- **`SETTINGS_ENABLE_CONNECT_PROTOCOL`**：服务端收到该参数不产生任何影响（RFC 8441 §3）；客户端仍校验服务端发送的取值与 1→0 回退。

---

### 5. WebSocket 与扩展隧道协议层

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/websocket_handshake.h"` / `"ruvia/http/websocket_client_negotiation.h"` | `validate_websocket_handshake`、`make_websocket_server_handshake`、`class websocket_client_negotiation final`、`class http1_websocket_client_handshake final` | WebSocket 服务端与客户端在 HTTP/1.1 Upgrade 及 HTTP/2 Extended CONNECT 下的握手校验、子协议选择与 `permessage-deflate` 扩展参数协商 |
| `"ruvia/http/websocket_connection.h"` / `"ruvia/http/websocket_protocol.h"` | `class websocket_connection final`、`struct websocket_event` | Sans-I/O WebSocket 帧状态机与协议类型：支持增量输入 `feed`、事件轮询 `next_event`（消息重组、Ping/Pong、Close、协议错误）、帧与关闭提交 `submit_frame` / `submit_close` 以及 RFC 7692 `permessage-deflate` 压缩编解码；关闭握手完成后服务端角色规划结束传输，客户端角色进入 `websocket_liveness_mode::awaiting_peer_eof`，丢弃后续输入并保持传输打开，直到 `notify_transport_eof()`（RFC 6455 §7.1.1 由服务端先关闭 TCP） |
| `"ruvia/http/http_connect_udp.h"` / `"ruvia/http/http_datagram.h"` | `encode_http_connect_udp_path`、`parse_http_connect_udp_path`、`validate_http_connect_udp_request`、`class http_capsule_decoder final`、`class http_datagram_session final` | RFC 9298 `CONNECT-UDP` 请求/目标校验与 RFC 9297 HTTP Capsule / Datagram 帧增量编解码 |
