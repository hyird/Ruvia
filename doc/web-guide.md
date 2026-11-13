# Web 服务端使用与 API 手册

本文档涵盖 `ruvia::web` 服务端的核心模块：应用配置与生命周期、控制器与路由声明、请求上下文 `context`、中间件、声明式模型 `RUVIA_MODEL` 与校验、静态文件服务、运维探针以及内存级测试框架。所有公开符号均位于 `namespace ruvia`。

---

## 1. 应用配置与生命周期

### 1.1 应用单例与启动主链（`"ruvia/web/app.h"`）

框架模式使用进程级单例 `ruvia::app()`（返回 `ruvia::application&`）完成配置，最后调用 `run()` 启动服务；嵌入其他项目的独立入站宿主见 [1.9](#19-独立入站宿主ruviawebhttp_serverh)。

```cpp
#include "ruvia/web/app.h"

int main() {
    ruvia::app()
        .load_dotenv()
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .server({
            .worker_count_ = 4,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
        })
        .run();
}
```

#### `ruvia::application` 核心方法

| 方法签名 | 说明 |
| --- | --- |
| `application& server(server_config config)` | 配置业务 worker 数量、超时、连接数/请求数上限、入站缓冲预算及响应压缩与 CORS 策略 |
| `application& listen(listen_config config)` | 注册监听端点（支持可多次调用监听多个地址/端口，重复端点会报错拒绝；TLS 证书在调用时立即加载校验；`inherited_sockets_` 接管已绑定的 socket） |
| `task<void> replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config)` | 异步替换 HTTPS 监听器及其 HTTP/3 服务的身份集合，之后的新握手使用新集合，见 [1.2](#ruvialisten_config-与-ruviatls_config) |
| `task<void> add_listener(worker_ref caller, listen_config config)` | 异步新增监听器：全部 worker 准备成功后统一开放接入，任一失败全部回滚，见 [1.2](#ruvialisten_config-与-ruviatls_config) |
| `task<prepared_listener> prepare_listener(worker_ref caller, listen_config config)` / `prepare_listeners(caller, span<const listen_config>)` | 先准备单项或整批监听，再由调用方开放接入，见下方“监听准备与激活” |
| `task<void> remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options = {})` | 停止新准入；`close_connections_` 可同时断开该监听器的既有连接 |
| `task<listener_sockets> duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const` | 仅 POSIX：异步复制全部 worker 的监听 socket，供 exec 热升级交接 |
| `task<bool> close_connection(worker_ref caller, std::uint64_t id)` | 异步按 ID 请求断开整条连接，返回值契约见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `on_connection_admission(...)` / `on_connection_open(...)` / `on_connection_close(...)` | 配置传输准入、生命周期与连接附加状态，见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `on_http2_frame(...)` / `on_http2_headers(...)` / `on_http3_frame(...)` / `on_http3_field(...)` | 启用 HTTP/2 结构化帧与 HPACK 字段观察（需 `RUVIA_ENABLE_HTTP2`）及 HTTP/3 帧与 QPACK 字段观察（需 `RUVIA_ENABLE_HTTP3`），见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `template <middleware M, typename... Args> application& use(Args&&... args)` | 注册全局中间件（每个 worker 独立构造一份实例） |
| `template <middleware M, typename... Args> application& use_at(middleware_scope_options options, Args&&... args)` | 注册路径前缀作用域中间件（如 `{.prefix_ = "/api"}`，按完整路径段匹配；前缀 `"/"` 抛出 `std::invalid_argument`，全局请用 `use`） |
| `template <typename S, typename... Args> application& use_worker_state(Args&&... args)` | 注册 per-worker 状态类型 `S`，在每个 worker 启动时独立构造一份实例 |
| `application& load_dotenv(dotenv_options options = {})` | 从可执行文件旁 `.env`（或指定 `std::filesystem::path`）加载实例环境变量 |
| `const ruvia::env& env() const noexcept` | 获取应用环境变量只读视图 |
| `application& document_root(document_root_config config)` / `document_root(std::nullptr_t)` | 启用或禁用后台定时刷新的静态文档根目录服务 |
| `application& trusted_proxies(trusted_proxy_config config)` / `trusted_proxies(std::nullptr_t)` | 配置可信反向代理 CIDR 列表与 `X-Forwarded-Proto` 信任策略 |
| `application& rate_limit(rate_limit_config config)` / `rate_limit(std::nullptr_t)` | 启用或禁用全局按客户端 IP 限流 |
| `application& blocking_pool(blocking_pool_options config)` / `blocking_pool(std::nullptr_t)` | 配置各 worker 事件循环借用的常驻阻塞卸载线程池（默认启用）；注册的数据库与 Redis 客户端也在其上执行 DNS 查询。禁用后 `run_blocking` 返回 `blocking_status::unavailable`，文件响应在响应头前以 `503` 回答（Windows 明文 TCP 上大于 16 KiB 的单范围仍走 `TransmitFile`），配置了文档根或主机不是 IP 字面量的注册数据库或 Redis 客户端会使 `run()` 启动失败 |
| `application& database(db_registration_config config)` / `database(std::nullptr_t)` | 注册 per-worker SQL 数据库连接池（需 `RUVIA_ENABLE_DATABASE`） |
| `application& redis(redis_registration_config config)` / `redis(std::nullptr_t)` | 注册 per-worker Redis 连接池（需 `RUVIA_ENABLE_REDIS`） |
| `application& client(http_client_config config)` / `client(std::nullptr_t)` | 按 alias 与 `base_url_` 注册或清除 per-worker 出站 HTTP 客户端；链式调用见 [客户端手册](streaming-websocket.md#5-出站-http-客户端) |
| `application& on_error(http_error_handler handler)` | 注册全局错误处理器，重复注册抛出 `std::invalid_argument`（另有 `scoped_error_handler_options` 重载支持按路径前缀注册） |
| `application& on_not_found(http_not_found_handler handler)` | 注册全局 404 处理器，重复注册抛出 `std::invalid_argument`（另有 `scoped_not_found_handler_options` 重载支持按路径前缀注册） |
| `application& on_access(access_log_callback callback)` | 注册请求访问日志回调（`noexcept`） |
| `application& on_connection_failure(connection_failure_callback callback)` | 注册连接/握手层异常回调（`noexcept`） |
| `application& on_tls_client_hello(tls_client_hello_callback callback)` / `on_tls_client_hello(std::nullptr_t)` | 注册或清空 TLS ClientHello 观察回调（`noexcept`），见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `application& on_http2_input(http2_input_callback callback)` / `on_http2_input(std::nullptr_t)` | 注册或清空 HTTP/2 连接入站字节观察回调（`noexcept`），见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `application& on_connection(connection_callback callback)` / `on_connection(std::nullptr_t)` | 注册或清空 `listener_dispatch::connection` 监听器的原始连接处理器；存在此类监听器而未注册时 `run()` 抛 `std::invalid_argument`，见 [1.7](#17-原始连接分发ruviawebserver_connectionh) |
| `application& on_request_stream(request_stream_callback callback)` / `on_request_stream(std::nullptr_t)` | 注册或清空 `listener_dispatch::request_stream` 监听器的请求流处理器；存在此类监听器而未注册时 `run()` 抛 `std::invalid_argument`，见 [1.8](#18-请求流分发ruviawebrequest_streamh) |
| `application& on_start(app_hook hook)` / `on_stop(app_hook hook)` | 注册启动后（accept 开始前）与停机后的生命周期钩子（在 `run()` 调用线程上执行） |
| `void run()` | 启动所有业务 worker 并阻塞，直到调用 `stop()` 或收到停止信号；允许无监听启动，再动态准备监听；全部 worker 退役后返回 |
| `void stop()` | 线程安全地请求停机，直接断连、不设排空阶段：停止接入（取消待决 accept 与 QUIC 接收）→ 立即关闭全部连接（HTTP/1、HTTP/2、WebSocket、隧道不发送响应、`GOAWAY` 或关闭握手；HTTP/3 立即发送 `CONNECTION_CLOSE(H3_NO_ERROR)`），在途处理器观察到 `c.stop_token()` → 先停 I/O 再 join 后台操作 |
| `http_server_stats stats() const` | 线程安全地汇总所有 worker 的连接与故障统计 |
| `blocking_pool_stats blocking_pool_stats() const` | 线程安全地读取阻塞线程池统计 |
| `std::vector<web_worker_ref> workers() const` | 获取全部业务 worker 的跨线程派发引用 |
| `web_worker_ref worker_for(std::uint64_t key) const` / `worker_for(std::string_view key) const` | 按哈希键确定性选择单个业务 worker 引用 |

应用公开回调统一使用 `move_only_function`，签名中的 `noexcept` 在构造期受编译器约束；所有权与退役规则见 [core 可调用包装器](core-and-http.md)。

`application::on_access()`、`on_connection_failure()`、`on_tls_client_hello()`、`on_http2_input()`、`on_connection()` 与 `on_request_stream()` 在配置锁外构造地址固定的回调所有者，锁内只交换拥有指针并发布借用视图，解锁后析构旧目标；`on_start(nullptr)` / `on_stop(nullptr)` 清空钩子列表、`on_error(nullptr)` / `on_not_found(nullptr)` 清空全局处理器时同样在解锁后析构被清空的目标。用户目标的移动与析构可查询应用状态，不会因重新进入配置锁而死锁。运行期间仍拒绝修改这些回调。

---

### 1.2 服务器、监听、TLS、压缩与 CORS 配置（`"ruvia/web/http_server.h"`）

#### `ruvia::server_config`

时长输入、取整与显式解包见 [Core 超时契约](core-and-http.md)。

| 字段名 | 类型与默认值 | 说明 |
| --- | --- | --- |
| `worker_count_` | `std::size_t`（默认 `max(1u, hardware_concurrency())`） | 业务 worker 线程数 |
| `process_signal_handlers_` | `process_signal_handler_policy`（默认 `external_owner`） | 设为 `install` 时自动安装 `SIGINT` / `SIGTERM` 信号处理以触发 `stop()` |
| `worker_queue_capacity_` | `std::size_t`（默认 `1024`） | 每个 worker 跨线程任务投递队列容量 |
| `idle_timeout_` | `std::optional<timeout_duration>`（默认 `75s`） | 空闲连接超时时间。HTTP/1 为等待下一个请求时的无活动时长，也是处理器既不读也不写时的兜底。HTTP/2 没有任何流时从最后一条流结束（或连接就绪）起计时，`PING`、`SETTINGS`、`WINDOW_UPDATE` 等不开启流的帧以及服务端自身的写出都不续期（与 Pingora `h2_idle_timeout` 一致），到期按下文连接级 `GOAWAY` 排空结束；有在途流时与 HTTP/1 处理器兜底相同 |
| `connection_scan_interval_` | `timeout_duration`（默认 `1s`） | 连接超时扫描周期。worker 没有任何连接且空闲修剪已执行后扫描停止，下一个连接到来时恢复 |
| `request_header_timeout_` | `std::optional<timeout_duration>`（默认 `60s`） | 请求头读取超时；HTTP/2 连接收到客户端连接前言与 `SETTINGS` 之前、以及头块（`CONTINUATION`）未完成时同样受此约束 |
| `request_body_timeout_` | `std::optional<timeout_duration>`（默认 `60s`） | 请求体单次读取超时。HTTP/2 按流计时：某条流超过该时长没有收到请求体 `DATA` 时，只以 `RST_STREAM(CANCEL)` 重置该流（不发送响应），连接与其他流不受影响，其他流或控制帧不续期 |
| `header_completion_timeout_` | `std::optional<timeout_duration>`（默认 `30s`） | 请求头整体完成截止时间；HTTP/2 连接就绪（连接前言与 `SETTINGS`）同样受此截止期约束 |
| `body_completion_timeout_` | `std::optional<timeout_duration>`（默认 `120s`） | 请求体整体完成截止时间。HTTP/2 按流计时：自该流请求头到达起计算，到期只重置该流 |
| `write_timeout_` | `std::optional<timeout_duration>`（默认 `60s`） | 写入无进展超时：每次 socket 写入期间独立计时，只有写入进展续期（读到的数据不续期），不是整体截止期。覆盖 HTTP/1 响应、HTTP/1 升级连接（WebSocket 消息、控制帧与关闭帧，`CONNECT` 隧道数据，与是否启用 WebSocket 心跳无关）以及 HTTP/2 连接的全部帧输出（含 RFC 8441 WebSocket 与 `CONNECT` 隧道），与读方向当前处于空闲、读取请求体还是长连接阶段无关：对端停止读取时关闭连接。HTTP/2 流等待对端流控窗口（发送窗口为 0）也受此限制：连续等待超过该时长的流以 `RST_STREAM(CANCEL)` 重置，只影响该流（与 Pingora 以写超时约束 h2 流控等待一致）。阻塞池上的文件正文读取与 `c.file()` / `c.static_file()` 的打开等待同样以此为界，超时返回 `503 file_read_unavailable`（见 §6.1） |
| `max_connections_per_worker_` | `std::optional<std::size_t>`（默认 `1024`） | 单个 worker 允许的最大并发连接数。达到上限时，新连接接管“请求间空闲最久”的连接的槽位（等待下一个请求的 HTTP/1 keep-alive 连接，或没有任何流的 HTTP/2 连接），后者的 socket 立即关闭（与 nginx 在 `worker_connections` 用尽时复用 keep-alive 连接的机制一致）；被淘汰连接的会话退出前 `active_connections_` 可能短暂超过上限，但打开的 socket 数不超过。没有空闲连接时，新 socket 在 TLS 与协议识别之前直接关闭（不伪造响应），并计入 `connections_refused_` |
| `max_requests_per_connection_` | `std::optional<std::size_t>`（默认 `1000`） | 单条连接最大处理请求数；HTTP/2 达到上限时按下文连接级两阶段 `GOAWAY` 排空，往返期间在途的流仍被处理（产品上限：最多再接纳 128 条，往返最长 1 秒）；HTTP/3 同理 |
| `max_buffered_body_bytes_` | `std::size_t`（默认 `default_max_buffered_body_bytes`） | 缓冲式路由默认允许的最大请求体字节数 |
| `max_inbound_buffer_bytes_per_worker_` | `std::size_t`（默认 `256 MiB`） | 单 worker 入站缓冲总字节上限 |
| `max_inbound_buffer_bytes_per_connection_` | `std::size_t`（默认 `64 MiB`） | 单连接入站缓冲字节上限 |
| `max_stream_body_bytes_` | `std::optional<std::size_t>`（默认无上限） | 流式路由请求体总字节上限 |
| `max_websocket_message_bytes_` | `std::size_t`（默认 `default_max_websocket_message_bytes`） | WebSocket 单条消息最大字节数 |
| `compression_` | `std::optional<compression_config>`（默认不启用） | 宿主级响应内容协商压缩（gzip / deflate / br / zstd）与预压缩静态变体协商，见下文“压缩、CORS 与可信代理配置” |
| `cors_` | `std::optional<http_cors_policy_config>`（默认不启用） | 宿主级 CORS 策略（复用 `ruvia::http_cors_policy`），含路由引擎 `OPTIONS` 预检回答 |

连接关闭遵循 [RFC 9112 §9.6/§9.8](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6)：

- 服务器主动结束、而对端仍可能发来数据时采用逗留关闭：HTTP/1 发送要求关闭连接的最终响应（`Connection: close`、请求被拒绝）后，以及 HTTP/2 以 `GOAWAY` 结束会话（连接级排空完成、空闲超时、连接错误）后，TLS 先发送 `close_notify`，再半关闭 TCP 发送方向，读取并丢弃对端剩余输入，直到对端结束输入、累计 2 秒或丢弃 4 MiB 先发生为止，然后关闭 TCP。这样未读输入不会使 TCP 以 RST 结束连接而冲掉最后的响应。2 秒与 4 MiB 是固定的产品上限。
- 等待请求头时发生空闲超时或请求头超时，以及对端正常结束输入时，TLS 发送 `close_notify` 后关闭。HTTP/2 对端结束输入（FIN 或 `close_notify`）时先发送最终 `GOAWAY(NO_ERROR)` 再关闭；该连接上仍在处理的流被取消，不再完成其响应。
- HTTP/2 连接级排空（RFC 9113 §6.8，仅由 `max_requests_per_connection_` 或连接超时触发，与停机无关）：先发 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳客户端已在途的流；收到 `PING` ACK、往返等待达到 1 秒（产品上限，不必等 ACK）或客户端此间新开超过 128 条流时，再发携带最后流 ID 的最终 `GOAWAY`。连接超时（写入超时除外）触发的排空不再获得第二个完整超时周期：最终 `GOAWAY` 发出时取消仍未结束的流，并以逗留关闭结束连接；若届时仍有写入未完成（对端不读取），自超时起 2 秒（产品上限）后直接关闭，不发送关闭告警。HTTP/2 请求体超时与流控窗口等待超时只以 `RST_STREAM(CANCEL)` 重置该流。
- 写超时（HTTP/1 响应、WebSocket 与 `CONNECT` 隧道的写入，以及 HTTP/2 连接的写入）、HTTP/1 读取请求体超时与处理器无读写超过 `idle_timeout_`、传输错误以及 `stop()` 立即关闭连接，不发送关闭告警。逗留中的连接计入 `active_connections_`，`stop()` 时同样立即关闭。
- HTTP/1 路由未读取请求体就给出响应（未命中路由的 `404`/`405`/`OPTIONS`/文档根、处理器或中间件未读请求体即返回，以及限流的 `429`）时，若整个请求体（`Content-Length` 或完整的 `chunked` 帧）已随请求头一并收到，框架不经任何 I/O 直接消费它，连接保持 keep-alive；请求体仍有未到达部分时，响应后按上文逗留关闭。其他由框架生成的拒绝（`400`、`413`、`417`、`421` 等）一律关闭连接。

#### `ruvia::listen_config` 与 `ruvia::tls_config`

```cpp
ruvia::app().listen({
    .address_ = "0.0.0.0",
    .http_ = 8080,
    .https_ = 8443,
    .http3_ = 8443,
    .tls_ = {
        .identity_ = {
            .certificate_chain_file_ = "certs/server.crt",
            .private_key_file_ = "certs/server.key",
        },
        .client_certificates_ = {
            .ca_file_ = "certs/ca.crt",
            .requirement_ = ruvia::tls_client_certificate_requirement::optional,
        },
        .sni_ = {
            {
                .host_ = "api.example.com",
                .identity_ = {
                    .certificate_chain_file_ = "certs/api.crt",
                    .private_key_file_ = "certs/api.key",
                },
            },
            {
                .host_ = "*.example.com",
                .identity_ = {
                    .certificate_chain_pem_ = wildcard_chain_pem,  // 内存 PEM
                    .private_key_pem_ = wildcard_key_pem,
                },
            },
        },
        .local_addresses_ = {
            {
                .address_ = "192.0.2.10",
                .identity_ = {
                    .certificate_chain_file_ = "certs/ip.crt",
                    .private_key_file_ = "certs/ip.key",
                },
            },
        },
    },
    .auto_https_redirect_ = true,
});
```

- **服务选择**：`http_` 与 `https_` 至少指定一个；同时指定且 `auto_https_redirect_ = true` 时，仅明文请求自动重定向至 HTTPS。
- **明文与 TLS 同端口**：`http_ == https_` 创建一个物理 TCP 监听服务，按首字节选择明文或 TLS；TLS 握手失败不回退明文。两种传输共享服务限额，在各分发模式既有限制内支持动态监听、外部 `adopt()` 与 HTTP/3 配对。
- **IPv6 监听**：`listen_config::ipv6_only_` 显式设置 IPv6 TCP/QUIC socket 的 `IPV6_V6ONLY`；`nullopt` 保留平台既有默认。继承 socket 必须与指定值一致，否则准备失败，不静默改写外部 socket。
- `dispatch_`（`listener_dispatch`，默认 `routes`）决定该配置的 `http_`、`https_` 与 `http3_` 服务如何处理已接受的连接：`routes` 走路由、中间件与控制器；`request_stream` 把每个 HTTP 请求（HTTP/1.1、HTTP/2、HTTP/3）交给 `on_request_stream()`，见 [1.8](#18-请求流分发ruviawebrequest_streamh)；`connection` 把每个连接（TLS 监听器在握手完成后）交给 `on_connection()`，服务器不解析 HTTP，见 [1.7](#17-原始连接分发ruviawebserver_connectionh)。`auto_https_redirect_` 下的明文 `http_` 服务只做重定向；`connection` 模式下设置 `http3_` 或 `auto_https_redirect_` 时 `listen()` 抛 `std::invalid_argument`。
- `http3_` 在该地址的 UDP 端口上提供 HTTP/3（RFC 9114，QUIC v1），要求 `https_` 与启用 `RUVIA_ENABLE_HTTP3` 的构建（否则 `listen()` 报错）；UDP 与 TCP 端口互不冲突，可与 `https_` 同号。它复用 `tls_` 的证书、SNI 与客户端证书策略，只协商 TLS 1.3 与 ALPN `h3`。HTTPS 监听器的每个 HTTP/1.1 与 HTTP/2 响应随之携带 `Alt-Svc: h3=":<端口>"; ma=<秒>`（RFC 7838 §3；`http3_alt_svc_max_age_` 默认 24 小时且须为正），应用已自行设置 `Alt-Svc` 的响应保持原值；HTTP/3 响应本身不加。`context::advertise_alternative_service()` 仍只发送 HTTP/2 ALTSVC 帧。
- HTTP/3 的 worker 模型：POSIX 上按 worker 下标顺序 `0..N-1` 以 `SO_REUSEADDR` + `SO_REUSEPORT` 绑定同一 UDP 端口；服务器签发的 20 字节连接 ID 用后 16 字节随机数与监听器密钥派生的掩码异或隐藏所属 worker 并携带 16 位认证标签（RFC 9000 §9.5 零明文前缀防关联）；Linux 多 worker 下通过 `SO_ATTACH_REUSEPORT_CBPF` 在内核态校验标签并将携带服务端 CID 的数据报（含连接迁移与 NAT 重绑定后的新四元组）直接投递给所属 worker（客户端 Initial 按连接 ID 密钥哈希选定 worker），其他 POSIX 平台或落到其他 worker 的数据报经有界队列（每 worker 至多 256 个在途，满则丢弃，由 QUIC 重传恢复）转交给所属 worker。Windows 由独立 Acceptor 线程以 `SO_EXCLUSIVEADDRUSE` 独占 UDP socket：按连接 ID 把数据报转交所属 worker，新连接的 Initial（客户端自选 ID）按监听器密钥加盐的哈希选定 worker（重传的 Initial 落到同一 worker，Acceptor 不保存连接状态）；业务 worker 的出站数据报经每 worker 至多 256 个在途的有界队列交回 Acceptor 发送，满则丢弃。业务 worker 在两种平台上同构，连接始终留在接受它的 worker 上；连接数与 TCP 共用 `max_connections_per_worker_` 计数与“淘汰最久空闲连接”策略。
- `http3_early_data_` 默认关闭；显式启用后接受可恢复会话的 QUIC 0-RTT。非安全方法自动返回 `425 Too Early`，不调用路由处理器；安全方法的处理器仍等待 TLS 握手认证完成后才启动（[RFC 8470 §3](https://www.rfc-editor.org/rfc/rfc8470.html#section-3)），不能仅凭方法名称推断业务无副作用。`c.early_data()` 保留该请求实际使用早期数据的来源标记，应用仍须评估重放风险。
- HTTP/3 支持客户端主动迁移与 NAT 重绑定：ngtcp2 完成路径验证，按 CID 将数据报转交原连接所属 worker，连接及处理器不跨线程迁移。支持普通 CONNECT（RFC 9114 §4.4）、WebSocket Extended CONNECT（RFC 9220）、CONNECT-UDP（RFC 9298）、HTTP Datagrams（RFC 9221 / RFC 9297）、RFC 9412 `ORIGIN` 帧（`c.advertise_origins()`）与 RFC 9218 优先级（`c.req().priority()`、`c.draft().priority()` 及客户端 `PRIORITY_UPDATE` 帧）。
- **迁移与 CID 容量**：依赖已升级至包含[备用 CID 退休修复](https://github.com/ngtcp2/ngtcp2/commit/a359eba92519ebed1339e1efbd309fc00c65a1d7)的 ngtcp2 1.26.0。默认 `active_connection_id_limit_ = 4`，为当前路径、旧路径探测和下一候选路径保留容量；这是实现选择，RFC 9000 §18.2 的最低合法值仍为 2。出站客户端在 CID 暂时不足时等待协议补充，等待与路径验证共用 `connect_timeout_` 整体截止期，不通过重连或重放请求伪装迁移成功。真实协议错误仍关闭连接。
- **Retry 与无状态重置**：`http3_retry_` 默认关闭；显式启用后对未携带 Token 的客户端 Initial 发送 QUIC Retry（RFC 9000 §8.1.2）完成无状态地址验证，伪造或过期的 Retry Token 以无状态 `CONNECTION_CLOSE(INVALID_TOKEN)` 拒绝。指向本 worker 已退役服务端连接 ID 的短包头数据报在关闭期结束后按速率限制发送严格小于触发包长的 Stateless Reset（RFC 9000 §10.3）。HTTP/3 不提供 Server Push（nghttp3 不实现 HTTP/3 推送）与 HTTP/2 专属的 `ALTSVC` 帧（RFC 7838 §4 仅定义于 HTTP/2；HTTP/3 使用 `Alt-Svc` 响应头）。`:authority` 与 `Host` 不一致等请求语义错误由 `http3_connection` 校验请求头时直接以 `H3_MESSAGE_ERROR` 只重置该流，不进入应用也不计入单连接请求数；nghttp3 在帧层识别的格式错误按 RFC 9114 §8 允许的方式升级为连接错误。`100 Continue` 与 `103 Early Hints` 等 1xx 照常发送。
- HTTP/3 的超时与连接级排空复用 HTTP/2 的语义：`idle_timeout_` 同时作为 QUIC `max_idle_timeout`（未设置则不启用），握手受 `request_header_timeout_` / `header_completion_timeout_` 约束；请求体超过 `request_body_timeout_` / `body_completion_timeout_`、或对端超过 `write_timeout_` 未确认输出的流以 `H3_REQUEST_CANCELLED` 单独重置；达到 `max_requests_per_connection_` 或连接超时时先发 shutdown-notice GOAWAY，经 `min(1 s, 3×PTO)` 后发最终 GOAWAY，所有已接纳流结束后以 `H3_NO_ERROR` 关闭连接；`stop()` 则立即以 `H3_NO_ERROR` 关闭。
- 服务端与数据库、Redis 客户端共用 `ruvia/web/http_client.h` 中的 `tls_identity_config`（证书链、私钥与可选口令）。每个身份恰好使用一种来源：两个 PEM 文件（`certificate_chain_file_` / `private_key_file_`），或内存 PEM 文本（`certificate_chain_pem_` 为叶证书后接其链，`private_key_pem_` 为可加密私钥）；混用或只给一半抛出 `std::invalid_argument`。路径为 `std::filesystem::path`，不得为空或含 NUL，以 UTF-8 交给 OpenSSL 与数据库驱动，Windows 原生路径不经过 ANSI 代码页。内存 PEM 仅供 HTTPS 监听器使用，数据库、Redis 与 HTTP 客户端读取身份文件并拒绝它。凭据解析后清除操作自身持有的口令与内存 PEM 副本；`application::listen()` 在启动前同步解析，独立宿主的 `listen()` 只暂存，解析推迟至异步启动。
- `tls_.client_certificates_.requirement_` 支持 `tls_client_certificate_requirement::optional` 或 `required`（双向 TLS / mTLS）。先用 `const auto connection = c.conn();` 保存元数据，再通过 `connection.tls()->client_certificate_subject()` 读取 optional 主题：无值表示未出示证书，有值空串表示已验证但 Subject DN 为空的证书（[RFC 5280 §4.1.2.6](https://www.rfc-editor.org/rfc/rfc5280.html#section-4.1.2.6)）；不能用文本是否为空判断证书存在性。主题提取失败终止该连接并报告，不降级为未认证身份。
- **固定身份选择**：未设置 `certificate_store_` 时，每次握手在处理 ClientHello 时选定一次身份并安装到该连接，优先级为精确 `sni_` 主机名 > 通配 `sni_` 主机名 > `local_addresses_` > 主身份 `identity_`；HelloRetryRequest 之后的第二个 ClientHello 沿用首次所选。
  - **SNI**：`sni_` 按 SNI 主机名（ASCII 不区分大小写）匹配；`host_` 可写作 `*.example.com`，通配符只能是完整的最左标签并恰好覆盖一个标签（RFC 6125 §6.4.3：匹配 `a.example.com`，不匹配 `example.com` 与 `a.b.example.com`）。
  - **本端地址**：`local_addresses_` 按连接到达的本端地址选择身份，`address_` 须为非未指定的数字 IP（IPv4 映射 IPv6 地址等同其 IPv4 形式），用于客户端以 IP 字面量连接的部署（RFC 6066 禁止在 SNI 中携带 IP）。TCP 与 HTTP/3 都使用实际目的地址，支持通配监听；QUIC 跨 worker 转交保留目的地址与接口，回复使用对应源地址。
  - **兜底**：客户端未发送 server_name 或名称与地址都不匹配时使用主证书完成握手。这是为单证书与通配符证书部署做出的产品选择，不发送 RFC 9325 §3.7 建议的 `unrecognized_name` 告警。
- **OCSP Stapling**：`tls_.ocsp_response_` 为主身份的 DER `OCSPResponse`；每个 `sni_` 与 `local_addresses_` 项各自设置 `ocsp_response_`，与证书一起选择，绝不借用主身份的响应。仅向请求 `status_request` 的客户端发送；空值不附送，TLS 1.2 使用 CertificateStatus，TLS 1.3（含 HTTP/3）使用叶证书的 CertificateEntry 扩展。
  - **输入与更新**：加载时拒绝非法 DER、BER 与尾随字节（`std::invalid_argument`），不与 `certificate_store_` 混用。不代为获取、刷新、验证签名或检查有效期；部署方负责提供相应证书的有效响应，使用 `replace_tls_identities()` 连同身份更新，新握手立即采用，已选身份的握手保留原证书与响应。
- **运行期替换**：`co_await app().replace_tls_identities(caller, {.address_ = "0.0.0.0", .port_ = 8443}, new_tls)` 替换该 HTTPS 端点及其 HTTP/3 服务的固定身份集合，或证书库与选择策略。`ruvia::listener_endpoint` 以 `address_`（默认 `"0.0.0.0"`，按 `listen_config::address_` 同一规则规范化）与 `port_`（该监听配置的 `http_` 或 `https_` 端口）指名一个 TCP 服务，HTTP/3 服务经其 HTTPS 端点指名。
  - **校验在前**：候选凭据的文件读取、PEM 与 CA 加载、TLS 上下文准备卸载至 `caller` 借用的有界 `blocking_pool`，不阻塞事件循环。未知端点、非 HTTPS 端点、非法设置，或 `client_certificates_` 与 `max_outgoing_records_` 与现值不同，抛出 `std::invalid_argument`；文件不可读、PEM 无效或证书私钥不匹配抛出系统错误。失败保留原集合。
  - **异步发布**：每个业务 worker 在自身线程采用新集合，调用协程挂起至全部完成。此后的 ClientHello 使用新集合；既有连接与已越过 ClientHello 的握手保留原身份。每 worker 每协议族的持久 `SSL_CTX`、会话缓存与票据密钥不替换，会话仍只在原 SNI 名称下恢复。
  - **线程与生命周期**：在显式 `caller` 所属 worker 上 `co_await`，允许业务 worker 发起；应用未运行（含启动钩子与停机期间）抛出 `std::logic_error`。与停机竞争时等待已投递控制及其负载退出，再传播失败，不提前销毁运行时；未成功提交的监听定义保留原集合。已采用新集合的 worker 只可能将其用于与停机竞争的握手，这些连接随停机关闭。
  - **持久化**：成功替换同时更新监听器定义，应用停止后再次 `run()` 继续使用新集合；停止期间仍用 `listen()` 整体替换监听配置。并发调用按发布顺序生效。
- **TCP TLS 版本**：入站服务、出站 HTTP/原始 TCP、Redis 与数据库最低为 TLS 1.1，仍拒绝 TLS 1.0、压缩与重协商。TLS 1.1 仅使用 ECDHE + AES-128/256-CBC-SHA；TLS 1.2 保持 ECDHE + AEAD（AES-128-GCM 优先），TLS 1.3 使用 OpenSSL 默认 AEAD 套件。
- **出站 TLS 下限**：`client_tls_config::minimum_version_` 接受 `tls_version::tls_1_1`、`tls_1_2`、`tls_1_3`，覆盖 HTTP、原始 TCP、Redis 与数据库客户端；默认保持 TLS 1.1 兼容行为。回源禁止旧版时显式设为 `tls_1_2`；协议自身的更高下限不被配置降低。
- **旧版兼容例外**：TLS 1.1 已被 [RFC 8996 §3](https://www.rfc-editor.org/rfc/rfc8996.html#section-3) 禁止协商，此处按明确兼容需求保留，并非现代安全基线。仅豁免该版本与其必需的旧握手签名；证书链、SAN、密钥强度及 TLS 1.2/1.3 的检查仍委托原 [OpenSSL 安全回调](https://docs.openssl.org/3.0/man3/SSL_CTX_set_security_level/)，不全局设置 `SECLEVEL=0`。
- **HTTP 协议下限**：HTTP/2 至少 TLS 1.2，HTTP/3 / QUIC 仅 TLS 1.3。`routes` 与 `request_stream` 的 ALPN 按 `h2`、`http/1.1`、`http/1.0` 选择，TLS 1.1 跳过 `h2`；没有共同协议时以 `no_application_protocol` 告警失败。原始连接的自定义 ALPN 同样不能在 TLS 1.1 选择 `h2`。
- `tls_.alpn_` 只用于 `listener_dispatch::connection` 的 HTTPS 服务：按服务器偏好顺序提供的 ALPN 协议名（每个 1..255 字节，为空时依次为 `h2`、`http/1.1`）；客户端提供的 ALPN 均不在列表中时以 `no_application_protocol` 告警失败（RFC 7301 §3.2），未提供 ALPN 时握手照常完成且不选协议。`routes` 与 `request_stream` 监听器设置非空列表、名称长度越界或无 `https_` 时 `listen()` 抛 `std::invalid_argument`。
- `tls_.max_outgoing_records_`（默认 `2^23`，范围 `1..2^23`）限制每条连接发出的全部 TLS 记录，计入握手与 KeyUpdate 且更新密钥不重置；到达上限时终止连接，以低于 RFC 9846 §5.5 的 AES-GCM 单密钥用量上限。
- 会话可在同一监听器的任意 worker 上恢复；TLS 1.1、1.2 与 1.3 会话都只在与建立时相同的 SNI 名称（ASCII 不区分大小写）下恢复（RFC 6066 §3；RFC 9846 §4.6.1 允许拒绝不同 SNI 的 PSK），否则完整握手。会话票据密钥每 2 小时（会话有效期）轮换一次，旧密钥再保留一个周期用于解密，随后销毁（RFC 9325 §3.4）；某个 worker 复制密钥失败时约 5 秒后重试，而不是等到下一次轮换。
- 目标源校验（RFC 9110 §7.4 / §15.5.20）：目标为 `https` 资源的请求（HTTPS 连接上的 origin-form / asterisk-form，或显式 `https` 绝对形式目标 / HTTP/2 `:scheme https`），只有当本连接握手出示的证书在 subjectAltName 的 DNS 或 IP 条目中覆盖目标主机（`:authority`，否则 `Host`；通配符只匹配完整的最左标签，不使用主题 CN）时才处理，否则回 `421 Misdirected Request`；HTTP/2 只拒绝该流，连接保留。证书匹配而非 SNI 匹配，因此证书覆盖的其他名称仍可复用连接（RFC 9113 §9.1.1）。明文监听器收到 `https` 目标同样回 `421`。`http` 目标、CONNECT 隧道目标与空 authority 的请求不受影响；来自 `trusted_proxy_config` 网段的对端豁免两项检查。每个连接缓存最近 4 个主机的判定，不对每个请求重复扫描证书。
- **监听 socket 接管（`inherited_sockets_`）**：`ruvia::listener_sockets` 以 `http_`、`https_`（监听中的 TCP socket）与 `http3_`（已绑定的 UDP socket）三组 `ruvia::native_socket`（POSIX 文件描述符，Windows `SOCKET`）代替绑定对应服务；空向量照常绑定。
  - **同端口句柄**：明文与 TLS 共用端口时，唯一 TCP 句柄组放在 `https_`；`http_` 必须为空，导出也使用 `https_`。
  - **校验**：TCP 句柄须处于监听（`SO_ACCEPTCONN`），UDP 句柄须已绑定，且本端地址族、地址与端口都等于本配置（规范化后比较，通配地址只匹配通配绑定）；非空向量要求其服务已启用。不符或同一句柄出现两次抛出 `std::invalid_argument`。
  - **所有权**：`listen()` / `add_listener()` / `prepare_listener()` / `prepare_listeners()` 从调用起拥有全部句柄并设置 close-on-exec（Windows 清除继承标志），失败或丢弃冷任务也关闭所接管句柄。下一次 `run()` 无论成败都消费启动配置中的句柄代替绑定，之后的 `run()` 照常绑定；被 `listen()` 替换而未消费的句柄随之关闭。
  - **数量与顺序**：POSIX 每组恰为 worker 数，否则 `run()` / `add_listener()` 抛出 `std::invalid_argument`；第 i 个交给第 i 个 worker。`http3_` 须按 `SO_REUSEPORT` 组内成员顺序给出（即 `duplicate_listener_sockets()` 的返回顺序），Linux 在该组上挂载 cBPF 分流；顺序不符时数据报经用户态转交仍到达所属 worker。Windows 每组至多 1 个，由 Acceptor 线程如自行绑定的 socket 一样使用（不再设置 `SO_EXCLUSIVEADDRUSE`）；已被其他进程关联 I/O 完成端口的 socket 无法接管，启动失败。
- **导出（仅 POSIX）**：`co_await duplicate_listener_sockets(caller, endpoint)` 按 worker 顺序返回运行中 TCP 服务各 worker 监听 socket 的 `dup()`（HTTPS 服务一并返回 HTTP/3 的 UDP socket），未设置 `FD_CLOEXEC`，归调用方所有；本应用继续在原 socket 上接入。exec 出的新进程把它们作为 `inherited_sockets_` 交给 `listen()` 并 `run()`，旧进程随后异步 `remove_listener(caller, endpoint)` 或 `stop()`：端口始终有人监听，旧进程未接受的连接留在共享队列中由新进程接受。QUIC 连接状态不随 socket 交接，旧进程的 HTTP/3 连接随其关闭而结束。Windows 的 Acceptor 每服务只独占一个 socket，不提供该接口。
- **运行期增删**：`ruvia::listener_endpoint` 指名服务的规则同运行期替换。
  - **新增**：`co_await add_listener(caller, config)` 校验配置并异步准备凭据；与运行中监听器重复的 TCP 或 HTTP/3 UDP 端点抛出 `std::invalid_argument`。各 worker（Windows 另含 Acceptor）绑定或接管 socket，并采用准备好的 TLS 上下文与 HTTP/3 端点；全部成功后统一开放接入，全部开始接入后恢复调用方。任一失败时关闭候选 socket、等待清理并重抛首个异常，既有监听器照常服务。新增 TLS 监听器与本次运行其他监听器同时轮换票据密钥。
  - **移除**：`remove_listener(..., options = {})` 停止 TCP 接入及配对 HTTP/3 的新 Initial；默认保留既有会话，最后一条会话结束后释放监听资源。`listener_remove_options{.close_connections_ = true}` 同时断开它的 TCP/TLS/QUIC 会话，包括 WebSocket 与隧道。按稳定监听身份匹配，不误关同地址重新添加的监听器；允许当前处理器移除自身监听，不等待自身退出。
  - **持久化与并发**：成功增删更新监听配置，供之后的 `run()` 使用；调用边界同运行期替换。运行期控制通过异步门串行提交，不持有互斥锁跨挂起，也不在 worker 上调用 `future::get()` 或等待条件变量。`run()` / `stop()` 与启动前配置仍由外部控制线程同步驱动。

#### 监听准备与激活

| 入口 / 边界 | 契约 |
| --- | --- |
| `prepare_listener(...)` / `prepare_listeners(..., span<const listen_config>)` | 返回冷任务前拥有配置与全部继承句柄；整批 TLS、QUIC、socket 和所有 worker 准备完成后交付同一个 `prepared_listener` |
| `co_await std::move(prepared).activate()` | 先发布业务配置，再消耗票据开放整批接入；准备期间 TCP 握手可能完成，但不执行连接或请求处理器 |
| `co_await std::move(prepared).cancel()` | 消耗票据并等待整批回滚；析构票据或丢弃消费它的冷任务也请求回滚，但不阻塞析构线程 |
| 失败与停机 | 任一项失败回滚整批，原监听继续服务；宿主关闭也取消未激活事务 |
| 串行控制 | 一张票据持有该宿主的监听事务；同一配置的多个端点须使用批量入口，不能持票再等待本宿主的另一监听控制操作 |
| 线程与寿命 | 票据可跨线程移动、析构；`activate()` / `cancel()` 在准备时的 caller worker 等待，相关循环须持续驱动至清理完成 |

`add_listener()` 仍是同一准备与激活主链的便利入口。`application`、`http_server_group` 显式传入 `caller`；单循环 `http_server` 使用绑定循环。

#### 增量证书库与选择策略（`"ruvia/web/http_client.h"`）

| 公开入口 | 契约 |
| --- | --- |
| `co_await tls_certificate_store::insert_or_assign(caller, id, identity)` | 非零 ID，异步解析并新增或替换单个身份；失败不改变库，清除操作自身持有的口令与 PEM 副本 |
| `co_await erase(caller, id)` | 异步删除并返回是否存在该 ID |
| `contains(id)` / `size()` | 同步查询；库可独立使用，全部方法线程安全 |
| `tls_config::certificate_store_` | 共享证书库；不能同时配置固定 `identity_`、`sni_` 或 `local_addresses_` |
| `tls_config::default_certificate_` | 未配置选择器时使用的证书 ID |
| `tls_config::select_certificate_` | `shared_ptr<const tls_certificate_selector>`；必须配合证书库，完全接管身份选择 |
| `tls_certificate_selection` | 连接 ID、真实本端/对端地址（`optional<ip_address>` / `ip_address`）及端口、SNI、QUIC 标志、legacy version，以及主机字节序 `span<const uint16_t>` 的 cipher suites、signature algorithms、supported versions/groups 与 `span<const string_view>` 的 ALPN 协议名借用视图 |
| `offers_signature_algorithm(code)` | 按 TLS 签名方案编号检查客户端能力，可用于 RSA / ECDSA 选择 |

- **发布与所有权**：每次变更原子发布不可变快照；握手在调用选择器前取得快照，期间替换或删除所选 ID 不影响该握手。已建立连接保留原证书引用，不重建上下文、会话缓存或票据密钥。
- **执行与取消**：在 `caller` 所属 worker 上等待变更；已有身份的快照复制、解析及删除卸载至其借用的有界 `blocking_pool`。缺池、满池、解析失败或调用方停止均不发布候选；已开始的后台作业仅持有自有输入，取消等待后不能偷偷提交。不存在的 ID 可直接返回 `false`，不占阻塞池。
- **拒绝规则**：选择器返回 `nullopt`、抛异常或返回快照中不存在的 ID 时握手失败；恢复握手也执行选择，不绕过拒绝策略。没有选择器时，缺失的默认 ID 同样拒绝握手。
- **回调边界**：TCP TLS 与 QUIC 共用选择入口，不要求启用观察回调；视图仅在选择期间有效，不暴露 OpenSSL 对象。不同 worker 可并发调用，目标自行同步可变状态；文件读取与 PEM 解析只发生在库更新时。


#### 压缩、CORS 与可信代理配置

- **单一配置位置**：`server_config::compression_` / `cors_` 随 `application::server()`、`test_app::server()`、`http_server` / `http_server_group` 构造参数整体校验发布；非法阈值或 CORS 字段使整次配置失败，原配置不变。
- **适用范围**：作用于宿主准备的每个 HTTP 响应，包括路由、`router::dispatch()` 与 `request_stream` 回调；隧道与 WebSocket 升级不选择响应表示。
- **`compression_config`**：`min_bytes_`（默认 `1024`）、`sync_bytes_`（默认 `64 KiB`）、`max_bytes_`（默认 `64 MiB`）。
  - **小正文**：`min_bytes_` 只控制压缩是否划算；请求以 `identity;q=0` 等方式拒绝 identity 时，小缓冲体同样压缩，结果不必小于原文，不因此返回 `406`。
  - **固定表示**：响应已有 `Content-Encoding`、`Content-Range`、`Cache-Control: no-transform` 或完整性字段 `Content-Digest` / `Repr-Digest` / `Content-MD5` 时不再压缩；RFC 9530 的摘要随内容编码而变。
  - **应用编码协商**：自行设置的 `Content-Encoding` 栈含自定义 token 时也按 `Accept-Encoding` 逐项校验，任一层被拒绝即回 `406`；其 `200` / `406` 结果依赖请求编码偏好，因此同样追加 `Vary: Accept-Encoding`。
  - **媒体白名单**：仅压缩 `text/*`、JSON / XML 及 `+json` / `+xml` 后缀、JavaScript / ECMAScript、`application/wasm`、`application/x-ndjson`、SVG、图标及未压缩字体 `font/ttf` / `font/otf` / `application/vnd.ms-fontobject`；静态文件预压缩共用此白名单。缺少 `Content-Type`、其他类型或类型名含 `zip` 时保持 identity。
  - **验证器与范围**：框架编码时弱化强 `ETag`、删除不是带引号 entity-tag 的 `ETag`，并删除不再适用于编码表示的 `Accept-Ranges`。
  - **阻塞卸载**：`sync_bytes_` 是事件循环内联编码上限；更大的缓冲体在启用阻塞池时卸载编码，未启用时保持 identity，拒绝 identity 则回 `406`，不在事件循环上编码。
  - **状态归属**：每种算法的缓冲上下文与流式空闲池归同一 Worker 所有；卸载任务使用独立编码器，不借用 Worker 状态。
  - **准备结果**：结果固定构造期类别；已完成响应与续处理状态不同时存在。
  - **提交失败**：缓冲压缩与 HEAD 编码元数据提交失败均保持原表示；成功响应允许 identity 时回退，拒绝时返回 `500 response_compression_failed`。
  - **流式阈值**：响应头声明 `Content-Length` 时，按与缓冲体相同的 `min_bytes_` / `max_bytes_` 规则决定是否编码。
  - **分片与 flush**：每次应用写入按 16 KiB 输入分片编码，仅在该次写入末尾 flush 一次；HTTP/1 与 HTTP/2 一致。
  - **无正文流**：初始头即完成的 HEAD / 304 等流式响应保留已选表示元数据，不取得编码器。
  - **原生窗口**：流式 br 使用 512 KiB 窗口（lgwin 19，同 Pingora）；zstd 使用压缩级别默认窗口，RFC 9659 的 8 MiB 仅作上限。
  - **流式租约**：编码器借用不超出租约寿命；归还或丢弃先解除绑定，再执行回收。
  - **流式归还**：编码器回池前先重置，空闲池保持有界；超出空闲上限、编码失败或重置失败的编码器直接释放，不回池。
  - **HEAD 回退**：`GET` 路由（含 `HEAD` 回退）、0 字节文件与重定向响应在 `HEAD` 下与 `GET` 一致发送 `Content-Length: 0`；拒绝 identity 时同样编码 0 字节体。
  - **显式 HEAD**：`RUVIA_HEAD` 返回空非文件响应体时不压缩该空体；按声明的 `Content-Length` 决定是否添加编码，支持逗号分隔的相同值，无声明时仅在拒绝 identity 时添加。添加编码后去掉未知的编码长度（RFC 9110 §8.6）；未声明长度的显式空 `HEAD` 响应省略 `Content-Length`。
  - **请求解码**：按 `Content-Encoding` 透明解码时，前 64 KiB 输出在事件循环内完成，更大的表示改在阻塞池上解码。无阻塞池或池拒绝时仍在事件循环内解码，请求必须得到内容；两条路径均受解码上限约束。
- `http_cors_policy_config`（`"ruvia/http/http_response.h"`）：
  - `allow_any_origin_`、`allowed_origins_`（精确 Origin、`*` 或尾缀域名模式）、`allowed_origin_suffixes_`、`allowed_methods_`、`allowed_headers_`、`reflect_request_headers_`、`expose_headers_`、`allow_credentials_`、`max_age_`。
  - 默认构造 `http_cors_policy_config{}` 允许任意 Origin（`*`）、任意合法方法并反射 `Access-Control-Request-Headers`；显式配置任一列表时仅匹配所列项。
  - **预检判定**：仅 `OPTIONS` 同时携带合法 `Origin` 与 `Access-Control-Request-Method` 时才进入预检分支；未固定 `allowed_methods_` 时自动提升路由生成的 `Allow` 响应头为 `Access-Control-Allow-Methods`。
  - **完整校验**：策略在新增授权头前校验 `Origin` 与 `OPTIONS` 的预检字段；请求首部列表即使采用固定授权模式也须完整合法。
  - **反射与优先级**：反射模式将通过全量校验的跨行 `Access-Control-Request-Headers` 合并为单一响应字段，不覆盖应用已设置的响应头。
- `trusted_proxy_config`：`cidrs_`（受信任代理网段列表，如 `{"127.0.0.1/32", "10.0.0.0/8"}`）、`forwarding_headers_`（优先级转发头规格列表，默认 `X-Forwarded-For` > `Forwarded`）、`trust_x_forwarded_proto_`（是否信任代理传入的 `X-Forwarded-Proto`）。底层统一由 `ruvia::http_client_ip_extractor` 执行。

---

### 1.3 `.env` 与环境变量（`"ruvia/web/app.h"`）

- `dotenv_options`：
  - `existing_variables_`：`dotenv_existing_variable_policy::preserve`（默认，保留已有环境变量）或 `override`。
  - `missing_file_`：`dotenv_missing_file_policy::ignore`（默认）或 `require`。
- 文件格式（整个文件先完整解析，任一行出错则抛出 `std::invalid_argument`（`invalid dotenv entry in <path>:<line>: <原因>`），不应用任何变量）：
  - 空行与以 `#` 开头的行忽略；首行 UTF-8 BOM 被剥离；可选 `export ` 前缀。
  - 变量名须匹配 `[A-Za-z_][A-Za-z0-9_]*`。同一文件内重复赋值同一变量名视为配置错误，在第二次赋值所在行报 `duplicate variable name`；`existing_variables_` 策略只裁决与此前加载结果的冲突，不影响该检查。
  - **值编码**：无引号、单引号与双引号值均须为合法 UTF-8；校验与词法扫描同步完成，非法字节直接报错，不猜测编码或替换字符。
  - 未加引号的值去除首尾空白，遇到行首或空白之后的 `#` 时截断为注释。
  - 单引号值按字面保留，不处理任何转义。
  - 双引号值仅定义 `\n`、`\r`、`\t`、`\"`、`\\` 五个转义；与 POSIX sh 双引号一致，反斜杠后跟其他任意字符时反斜杠与该字符一起按字面保留（`"\q"` 得到 `\q`，`"C:\Users"` 保持原样）。
  - 引号闭合后只允许空白或 `#` 注释；不支持多行值与变量展开。
- **独立加载**：`ruvia::env variables; variables.load_dotenv();` 不依赖 `application`。`load_dotenv(options)` 读取可执行文件旁 `.env`；`load_dotenv(path, options)` 读取指定文件，均返回左值 `env&`。
- **实例隔离**：加载仅修改当前 `env`，不改进程环境；成功加载可能使此前 `get()` 的借用视图失效。并发读取前完成加载，后续修改由调用方同步。
- **读取入口**：独立实例、`app().env()` 与 `c.env()` 使用同一契约：
  - `std::optional<std::string_view> get(std::string_view name) const& noexcept`
  - `template <typename T> std::optional<T> get(std::string_view name) const&`（支持 `std::string_view`、`bool`、整型、浮点型；变量存在但格式非法时抛出 `std::invalid_argument`）

---

### 1.4 错误处理与异常（`"ruvia/web/middleware.h"`, `"ruvia/web/app.h"`）

- **抛出或构造业务错误**：
  - 在路由或中间件中可抛出 `ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "bad_input", .message_ = "..."})`，或直接 `co_return c.error({...});`。`http_error` 是唯一的拥有型应用错误：status / code / message / status_text 以及 `.validation_issues_` 全部深拷贝持有，`info()` 返回携带 issues 的 `http_error_info`。
- **错误资源归属**：`http_error` 复制或移动赋值保留目标资源对象，包含嵌套诊断文本；不同资源对象即使 `is_equal()` 为真，也先在目标资源完整重建再提交，分配失败保留原值。
- **独立校验诊断**：`validation_issue` 复制构造沿用源 PMR，赋值保留目标 PMR；保留诊断副本时须同步保留资源所有者。
- **诊断赋值提交**：全部字段准备成功后才替换旧值；分配失败保留两端诊断，资源等价的移动不分配。
- **全局与前缀级错误/404 处理器**：
  - `on_error` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c, ruvia::http_error_info error)`。处理器内再次抛出 `http_error`（含校验 issues）会按该错误生成默认响应，不会变成 500；协议层错误的 `error.code()` 为具体机器码（如 `missing_host`、`conflicting_content_length`），而非按状态派生的 `bad_request`。内置中间件的拒绝同样经过 `on_error`：全局与路由级限流为 `429` / `too_many_requests`（处理器返回的响应之后会追加 `Retry-After` 与 `X-RateLimit-Limit` / `X-RateLimit-Remaining` / `X-RateLimit-Reset`），CSRF 校验失败为 `403` / `csrf_token_mismatch`，会话冲突为 `409` / `session_conflict`。
  - `on_not_found` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c)`。
  - 支持通过 `{.prefix_ = "/api", .handler_ = &api_error}` 注册前缀作用域处理器，按最长完整路径段匹配（例如 `"/api"` 匹配 `"/api/users"` 但不匹配 `"/apix"`）。等价前缀重复注册或前缀 `"/"`（全局只用无前缀形式）抛出 `std::invalid_argument`。
- **异常文本格式化**：
  - `std::string ruvia::exception_text(const std::exception_ptr& exception)`：将任意捕获的异常安全格式化为 UTF-8 描述文本。

---

### 1.5 Worker 状态与跨 Worker 任务投递（`"ruvia/web/middleware.h"`）

每个业务 worker 可绑定无锁局部状态，并支持从任意线程或控制器向指定 worker 投递异步任务：

```cpp
struct worker_metrics {
    std::uint64_t counter_{0};
};

// 启动前注册：
ruvia::app().use_worker_state<worker_metrics>();

// 在路由中无锁访问当前 worker 状态：
auto& metrics = c.worker_state<worker_metrics>();
++metrics.counter_;

// 跨 worker 投递任务：
for (const auto& worker : ruvia::app().workers()) {
    auto outcome = worker.post([](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
        ++ctx.worker_state<worker_metrics>().counter_;
        co_return;
    });
}
```

`ruvia::web_worker_context` 提供与控制器一致的 worker 级能力访问：`worker()`、`pool()`、`stop_token()`、`worker_state<T>()`、`run_blocking(...)`、`db()`、`redis()`、`client()` / `client(alias)`。

---

### 1.6 连接观察钩子（`"ruvia/web/http_server.h"`）

#### 1.6.1 原始与结构化协议观察

观察回调只读、不改变协议行为，`app.h` 已包含该头。回调在连接所属 worker 线程上同步调用；多个 worker 并发调用同一目标，目标须线程安全。记录及其全部视图只在回调期间有效。未注册回调时不安装观察分支、不构造记录。

```cpp
ruvia::app()
    .on_tls_client_hello([](const ruvia::tls_client_hello_record& hello) noexcept {
        // hello.message()[0] == std::byte{1}：完整 ClientHello 握手消息
        fingerprint(hello.connection_id(), hello.message());
    })
    .on_http2_input([](const ruvia::http2_input_record& input) noexcept {
        capture(input.connection_id(), input.bytes());
    });
```

| 记录方法 | 说明 |
| --- | --- |
| `tls_client_hello_record::connection_id()` / `http2_input_record::connection_id()` | 所属连接的 `conn_info::connection_id()`，用于与该连接上的请求 `c.conn().connection_id()` 关联 |
| `tls_client_hello_record::local()` / `remote()` | 连接接入时的真实本端与对端地址端口（`ip_endpoint`，地址为拥有字节的 `ip_address`，文本经 `to_string()` 获取） |
| `tls_client_hello_record::quic()` | HTTP/3（QUIC）连接为 `true`，TCP 上的 TLS 为 `false` |
| `tls_client_hello_record::message()` | 完整 ClientHello 握手消息：`msg_type`（1）、3 字节长度与正文，已完成记录层或 CRYPTO 帧重组（[RFC 9846 §4.1.2](https://www.rfc-editor.org/rfc/rfc9846.html#section-4.1.2)） |
| `http2_input_record::bytes()` | 一次读取的已解密 HTTP/2 连接入站字节 |
| `http2_frame_record::connection_id()` / `metadata()` | 所属连接与 HTTP/2 结构化帧事实；由 `on_http2_frame()` 注册 |
| `http2_headers_record::connection_id()` / `metadata()` | 所属连接与 HPACK 原始字段次序；由 `on_http2_headers()` 注册 |
| `http3_frame_record::connection_id()` / `metadata()` | 所属连接与 HTTP/3 结构化帧事实；由 `on_http3_frame()` 注册 |
| `http3_field_record::connection_id()` / `metadata()` | 所属连接与 QPACK 原始字段次序；由 `on_http3_field()` 注册 |

- **ClientHello 时机**：每个收到的 ClientHello 交付一次，先于 SNI 与证书选择；HelloRetryRequest 之后的第二个 ClientHello 再次交付。TCP TLS 与 HTTP/3 均覆盖。
- **HTTP/2 输入顺序**：同一连接的记录按线上顺序拼接，即从客户端连接前言（[RFC 9113 §3.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4)）开始的完整入站字节；TLS ALPN `h2` 与明文 prior-knowledge 两条路径一致。分块边界是读取边界，不是帧边界。
- **连接标识**：`c.conn().connection_id()` 在进程内跨 worker、宿主与重新启动保持唯一，退役 ID 永不复用；同一连接上的全部请求与观察记录共享。已服务连接永不为 0，`test_app` 请求无连接，恒为 0。
- **结构化 HTTP/2**：要求 `RUVIA_ENABLE_HTTP2`；帧与字段观察直接复用协议解析器，适合提取 Akamai 指纹所需事实，不重建请求字段顺序。字段、序号、规范化 flags 与无效输入的可观察范围见 [Sans-I/O 观察契约](core-and-http.md#421-帧与字段观察)；原始线上字节仍使用 `on_http2_input()`。

#### 1.6.2 准入、生命周期与主动断连（`"ruvia/web/http_server.h"`）

| 应用注册入口 | `http_server_callbacks` 字段 | 契约 |
| --- | --- | --- |
| `on_connection_admission(fn)` | `connection_admission_` | `bool(const connection_event&) noexcept`；TCP 在 TLS/HTTP 会话前、QUIC 在有效新 Initial 建立会话前调用，`false` 静默拒绝 |
| `on_connection_open(fn)` | `connection_open_` | `shared_ptr<void>(const connection_event&)`；建立连接附加状态，先于握手与请求分发 |
| `on_connection_close(fn)` | `connection_close_` | `void(const connection_event&, const shared_ptr<void>&) noexcept`；ID 退役后、释放附加状态前调用 |

- **事件事实**：`connection_event` 提供 `connection_id()`、`local()`、`remote()`、`quic()`；端点统一为拥有 `ip_address` 与端口的 `ip_endpoint`，是接入时的真实传输快照，不采用转发头；不暴露 Asio 地址类型。事件可复制，端点地址按值返回；借用事件端点的访问器禁止右值宿主。
- **配对与失败**：被准入回调拒绝的连接没有打开/关闭事件；获准连接覆盖握手失败、取消和冷任务销毁。打开回调抛异常时关闭一次、状态为空，并上报原异常。
- **状态共享**：`context::connection_state()`、`request_stream::connection_state()`、`server_connection::connection_state()` 返回附加对象的共享所有权，供同连接全部请求使用。关闭回调只借用该 `shared_ptr`，需要延长对象寿命时显式复制。
- **执行边界**：回调在所属 worker 同步执行且不持有控制锁，多 worker 可并发调用；QUIC 迁移不重复准入或打开，关闭仍恰好一次。
- **本端元数据**：`context::conn().local()` 与 `request_stream::conn().local()` 返回与接入事件一致的地址端口；无传输的测试请求本端为空、端口为 0。
- **请求内断连**：`context::abort_connection()` / `request_stream::abort_connection()` 立即中止整条连接，取消该连接上其他 HTTP/2、HTTP/3 流；它是无需选择响应的终态，但不吞掉随后抛出的实际异常。无传输的虚拟请求无操作；原始连接使用已有 `abort()`。

| 按 ID 控制入口 | 线程与生命周期 |
| --- | --- |
| `co_await application::close_connection(caller, id)` | 应用运行时，在显式 `caller` 所属 worker 上等待控制完成，可由业务 worker 发起 |
| `co_await http_server_group::close_connection(caller, id)` | 宿主组运行时异步发布，`caller` 可以是成员 worker |
| `http_server::close_connection(id)` | 单宿主运行时，在所属循环直接执行 |

- **返回值**：`true` 表示找到已登记连接并请求关闭，不表示任务已全部回收；未知或已退役 ID 返回 `false`。违反上述调用边界抛 `std::logic_error`，请求内自断连使用 `abort_connection()`。

### 1.7 原始连接分发（`"ruvia/web/http_server.h"`）

`listen_config::dispatch_ = ruvia::listener_dispatch::connection` 的监听器把每个已接受的连接交给 `on_connection()` 注册的处理器：明文监听器交付 socket 字节，HTTPS 监听器在握手完成后交付解密字节流，服务器不解析 HTTP。同一应用可同时拥有 `routes` 与 `connection` 监听器；`app.h` 已包含该头。

```cpp
ruvia::task<void> echo(ruvia::server_connection& connection) {
    while (auto chunk = co_await connection.read()) {
        co_await connection.write(*chunk);
    }
    co_await connection.finish();
}

ruvia::app()
    .listen({.address_ = "0.0.0.0", .http_ = 7000, .https_ = 7443,
        .tls_ = {.identity_ = {.certificate_chain_file_ = "certs/server.crt", .private_key_file_ = "certs/server.key"},
            .alpn_ = {"echo/1"}},
        .dispatch_ = ruvia::listener_dispatch::connection})
    .on_connection(&echo);
```

| `ruvia::server_connection` 方法 | 说明 |
| --- | --- |
| `connection_id()` | 连接标识，与 `conn_info::connection_id()` 及该连接 `on_tls_client_hello` 记录的 `connection_id()` 相同 |
| `local()` / `remote()` | 本端与对端地址端口（`ip_endpoint`，地址为拥有字节的 `ip_address`，文本经 `to_string()` 获取） |
| `tls()` | 明文监听器为 `nullptr`；否则为 `tls_connection_transport`（`alpn()`、`server_name()`、`client_certificate_subject()`） |
| `stop_token()` | 应用停机时请求停止 |
| `worker()` | 所属核心 `worker_ref`；可组合 `sleep_for`、`task_scope` 与 `run_blocking`，不会切换连接线程 |
| `connection_state()` | 连接打开回调返回的附加对象，见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `co_await read(operation = {})` | 下一段至多 4 KiB 的入站字节；视图有效到下一次 `read()` 开始；对端结束发送方向（TCP FIN、TLS `close_notify`，或 TLS 对端未发 `close_notify` 直接关闭 TCP）后返回 `std::nullopt`，此后重复返回 |
| `co_await write(bytes, operation = {})` | 发送字节；调用时即复制输入 |
| `co_await finish(operation = {})` | 半关闭发送方向：TCP `shutdown(send)`，TLS 发送 `close_notify`；之后仍可读取，再写抛 `std::logic_error`，重复调用无操作 |
| `abort()` | 立即关闭连接，仍在运行的操作失败 |

- **生命周期**：对象由服务器拥有、不可复制或移动，只在处理器任务运行期间、在该连接所属 worker 线程上有效；任务结束后不得继续访问对象。一个读操作与一个输出操作（`write` / `finish`）可并发运行，同方向重叠抛 `std::logic_error`。
- **逐次操作**：`operation`（`operation_options`）的超时或停止使 `read()` 只结束本次读取，抛 `std::system_error`（`timed_out` / `operation_canceled`），不丢输入、可继续读；使 `write()` / `finish()` 中止连接并抛同样的异常，令牌启动前已停止则不发出字节。服务器超时继续生效；非正超时抛 `std::invalid_argument`。
- **操作回收**：`server_connection` 与 `http_tunnel` 只用读写通道租约判定在途操作；处理器退出时先中止仍活跃的 I/O，再等待回收。未启动的冷任务直接失效，随后 `co_await` 抛 `std::logic_error`。
- **结束**：处理器任务正常返回且无在途操作时，服务器先结束仍打开的发送方向（TLS `close_notify`），再关闭 socket；任务抛出异常则立即断开，并经 `on_connection_failure()` 上报、计入 `connection_failures_`。对端复位、关闭及服务器取消的 I/O（超时、`stop()`、`abort()`）均不计故障；应用自行抛出的异常不按传输取消过滤。
- **服务器限制**：连接占用一个 `max_connections_per_worker_` 名额且不会为接纳新连接而被淘汰；读缓冲计入入站内存双预算（超限 `read()` 抛 `std::bad_alloc`）；`idle_timeout_` 在无读写完成时关闭连接，`write_timeout_` 在写入停滞时关闭连接；`stop()` 立即关闭全部连接并请求 `stop_token()`，`run()` 等待处理器任务退出后返回。连接被关闭后，在途与后续操作抛 `std::system_error`。
- **平台**：Windows 与 POSIX 语义一致，连接同样经 Acceptor 交给业务 worker 后处理。

### 1.8 请求流分发（`"ruvia/web/http_server.h"`）

`listen_config::dispatch_ = ruvia::listener_dispatch::request_stream` 的监听器把每个 HTTP 请求（HTTP/1.1、明文 prior-knowledge 与 TLS 上的 HTTP/2、配对的 HTTP/3）交给 `on_request_stream()` 注册的处理器，不经过路由。同一应用可同时拥有 `routes`、`request_stream` 与 `connection` 监听器；`app.h` 已包含该头。

```cpp
ruvia::task<void> proxy(ruvia::request_stream& stream) {
    if (stream.tunnel_requested()) {  // CONNECT、扩展 CONNECT 或 HTTP/1.1 Upgrade
        auto* tunnel = co_await stream.accept_tunnel(
            stream.request().known_method() == ruvia::http_known_method::connect ? ruvia::http_status::ok
                                                                                 : ruvia::http_status::switching_protocols);
        while (auto chunk = co_await tunnel->read()) {
            co_await tunnel->write(*chunk);
        }
        co_await tunnel->finish();
        co_return;
    }
    stream.draft().header("content-type", "application/octet-stream");
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        while (auto chunk = co_await stream.body().read()) {
            co_await stream.response().write(*chunk);
        }
        co_await stream.response().end();
    }
}

ruvia::app()
    .listen({.address_ = "0.0.0.0", .http_ = 8080, .https_ = 8443, .http3_ = 8443,
        .tls_ = {.identity_ = {.certificate_chain_file_ = "certs/server.crt", .private_key_file_ = "certs/server.key"}},
        .dispatch_ = ruvia::listener_dispatch::request_stream})
    .on_request_stream(&proxy);
```

| `ruvia::request_stream` 方法 | 说明 |
| --- | --- |
| `request()` | 请求方法、目标与首部字段（`http_request`）；正文经 `body()` 读取 |
| `conn()` | 连接元数据，`connection_id()` 与 routes 监听器的 `c.conn().connection_id()` 同一编号 |
| `connection_state()` / `abort_connection()` | 共享连接附加状态与整连接中止，见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `body()` | 流式请求正文（`body_reader`），首次读取时发送待发的 `100 Continue`；隧道请求抛 `std::logic_error`（首部之后的字节属于隧道） |
| `body_state()` | 初始正文分帧快照：`framing_`、可选 `content_length_` 与 `initial_end_stream_`；不必先读一次正文 |
| `trailers()` / `trailer(name)` | 正文读至 EOF 后读取请求尾部字段，与初始首部分开保存 |
| `co_await inform(head, operation = {})` | 最终响应提交前发送合法 1xx（如 `103 Early Hints`），字段在调用返回前拥有化；`operation` 同 `context::inform()` |
| `early_data()` | 实际请求是否来自 HTTP/3 0-RTT；可据此返回 `425` 或转发 `Early-Data: 1` |
| `push(request)` | 按对端设置与容量提交 HTTP/2 推送承诺 |
| `advertise_origins(origins)` / `advertise_alternative_service(value)` | 复用连接的 ORIGIN / ALTSVC 输出能力及协议限制 |
| `worker()` / `worker_state<T>()` | 所属核心 worker 与其注册状态，不依赖路由分发 |
| `run_blocking(...)` | 绑定请求取消的阻塞卸载；参数及结果契约同 `context::run_blocking()` |
| `client(alias)` / `db(alias)` / `redis(alias)` | 借用当前 worker 注册的出站能力（`router::dispatch()` 期间为该 router 借入的客户端）；支持无参默认别名，后两者受构建特性控制 |
| `env()` | 宿主环境只读视图 |
| `draft()` | 编辑下一次文件工厂、流式提交或隧道/WebSocket 接受使用的响应元数据 |
| `response()` | 流式响应（`response_stream_writer`）：提交、写正文、以可选 Trailer 结束 |
| `co_await file(options)` / `static_file(root, options)` | 复用 `context` 文件工厂，返回完整 `http_response`；选项在调用返回前拥有化，静态根借用至任务完成，禁止传入临时根 |
| `respond(http_response)` | 接管响应的状态、首部、Cookie 与正文/文件，在回调返回后发送；不合并尚未消费的 `draft()`，也不立即提交到线面 |
| `tunnel_requested()` | CONNECT / 扩展 CONNECT（RFC 9110 §9.3.6、RFC 8441、RFC 9220），或无正文且提供 `Upgrade` 的 HTTP/1.1 请求（RFC 9110 §7.8）时为 `true` |
| `co_await accept_tunnel(status = http_status::ok, config = {})` | 提交成功头并返回 `http_tunnel*`：CONNECT 须为 2xx，HTTP/1.1 Upgrade 须为 `101`；`101` 只能选择客户端提供的协议。非隧道请求或响应已提交时抛 `std::logic_error`。`config` 为 `http_tunnel_route_config`，复用路由隧道的协商校验 |
| `co_await accept_websocket(config = {})` | 对提供 WebSocket Upgrade / Extended CONNECT 的请求执行正常握手，返回 `websocket_accept_result`：`session()` 为借用的 `websocket*`；握手拒绝时 `rejection()` 给出所选错误响应的 `status_` 与失败检查 `code_`（如缺失或不支持版本为 `400` / `unsupported_websocket_version`），该响应在回调返回后发出并按此状态记入访问日志 |
| `stop_token()` | 应用停机时请求停止；非隧道请求还在客户端放弃时请求（HTTP/1 请求正文读完后连接断开，HTTP/2 / HTTP/3 流被重置） |
| `reset()` / `reset(http2_error_code)` / `reset(http3_error_code)` | 提交前即可终结当前流；HTTP/1 关闭连接，HTTP/2、HTTP/3 默认分别为 CANCEL、H3_REQUEST_CANCELLED |

- **保留的服务器语义**：分帧与目标校验、`421`（RFC 9110 §7.4）、不支持的 `Expect` 回 `417`、`max_stream_body_bytes_` 正文上限（声明长度超限回 `413`，读取超限时 `read()` 抛出）、流控信用、各阶段超时、请求数上限与 HTTP/3 0-RTT 的 `425` 照常生效；这些回答使用框架默认错误表示。
- **宿主表示策略**：`server_config::cors_` / `compression_` 同样作用于回调选择的响应，范围见 [1.2](#压缩cors-与可信代理配置)。
- **跳过的应用层**：不经过路由、中间件、`document_root`、应用级限流与 `on_error` / `on_not_found`。
- **路由隔离**：仅有 `connection` / `request_stream` 的应用不构造控制器或中间件、不编译或封存全局路由；首次动态添加 `routes` 监听器时才准备并采纳路由与路由限流，失败保留原服务。
- **生命周期**：对象由服务器拥有、不可复制或移动，只在处理器任务运行期间、在请求所属 worker 线程上有效；多个 worker 并发调用同一目标，目标须线程安全。
- **正文快照**：`request_body_framing` 区分 `none`、`known_length`、`chunked`、`multiplexed`。`initial_end_stream_` 在 HTTP/2 保存初始 HEADERS 的 END_STREAM，在 HTTP/3 保存初始首部结束时的 FIN；HTTP/1 与服务端推送为 `nullopt`，后续数据不改写快照。
- **只取消读取**：`body().read(token)` / `text(token)` 的取消与 `reset()` 独立，仍可发送拒绝响应；协议间的继续读取边界见 [body_reader](streaming-websocket.md#11-流式消费请求体body_reader-与-multipart_reader)。
- **只重置当前流**：重置后不会补发 `500`，也不影响同连接其他多路复用流；之后的处理器异常仍上报。显式错误码的协议类型与实际协议不符时抛 `std::invalid_argument`。
- **选择与提交**：处理器须选择 `respond()`、流式提交、`accept_tunnel()`、`accept_websocket()`、`reset()` 或主动断连之一；重复选择输出抛 `std::logic_error`。已提交但未 `end()` 的响应流在回调返回后正常结束。完整响应尚未写到线面时，回调异常仍可替换它并释放原正文/文件；线面提交后异常中止 HTTP/1 连接或重置 HTTP/2、HTTP/3 流，并上报。
- **隧道拒绝**：经 `response()` 立即提交并流式发送，不等待处理器返回，保留背压与协议允许的 Trailer；HTTP/1.1 随后关闭连接。上游拒绝正文可直接流式转发，提交次序见[出站 HTTP 客户端](streaming-websocket.md#5-出站-http-客户端)。
- **隧道接受**：CONNECT 的 2xx 只能由 `accept_tunnel()` 发送；接受后隧道在处理器返回前有效，返回后结束发送方向，按 `config.peer_transport_fin_timeout_` 等待对端结束。已到达的隧道字节仍受入站预算与流控约束。
- **Capsule 与 Datagram**：扩展 CONNECT / Upgrade 可通过 `config.datagrams_ = true` 声明 Capsule/HTTP Datagram 语义；普通 CONNECT 拒绝该配置。CONNECT-UDP 自动启用并执行现有 `Capsule-Protocol` 等握手校验；接受后可直接使用 `capsules()`、`datagrams()` 或 `udp()`，HTTP/3 支持原生 Datagram 与可靠 Capsule 回退。
- **WebSocket**：使用与路由相同的 `websocket_route_config`、子协议、压缩、心跳和关闭握手；会话只在回调期间有效，回调结束时复用正常会话收尾。文件工厂与接受操作的未启动冷任务随请求作用域失效，在途操作在销毁请求前取消并收束。
- **平台**：Windows 与 POSIX 语义一致，请求在接受连接（或 HTTP/3 连接）所属的业务 worker 上处理。

### 1.9 独立入站宿主（`"ruvia/web/http_server.h"`）

外部项目通过 `http_server(event_loop, http_server_callbacks, server_config = {})` 复用入站驱动，不创建 `application`、不接触全局控制器注册表、不接管进程信号。链接 `ruvia::web`；事件循环可来自 `event_loop_pool` 或 `attach_event_loop()`。

| 方法 | 契约 |
| --- | --- |
| `listen(config)` | 仅在 `start()` 前暂存配置，不读取凭据；接受 `request_stream` / `connection` 分发及 TLS、HTTP/3、继承 socket |
| `co_await add_listener(config)` | 运行期原子新增；候选准备失败完整回滚 |
| `co_await prepare_listener(config)` / `prepare_listeners(configs)` | 分阶段准备单项或整批监听；票据契约见“监听准备与激活” |
| `co_await add_connection_service(config)` | 启动前或运行期异步准备不绑定监听 socket 的 TCP/TLS 服务，供外部 Acceptor 交接 |
| `adopt(endpoint, native_socket, adopt_options = {})` | 接管已连接 socket，失败也关闭句柄；首次绑定当前循环，执行相同的 PROXY、TLS、HTTP、预算、超时与回收主链 |
| `trusted_proxies(config)` | 启动前原子替换可信代理集合，默认不信任转发头；非法 CIDR 不改变原配置 |
| `co_await start()` | 一次性准备全部服务后开放准入；失败先关闭并 join 已准备资源，再传播异常；允许先启动空宿主 |
| `remove_listener(endpoint, options = {})` | 立即停止新准入；是否断开既有会话由 `listener_remove_options` 决定，同端点可以重新添加 |
| `co_await replace_tls_identities(endpoint, config)` | 异步准备并替换 TLS 身份，保持既有客户端证书策略与记录上限 |
| `close_connection(id)` | 按 ID 请求整连接关闭，控制边界见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `duplicate_listener_sockets(endpoint)` | POSIX 上复制监听句柄，交接契约同应用入口 |
| `close()` / `co_await shutdown()` | 立即断开、不提供 GOAWAY 后等待在途请求的排空模式；前者任意线程请求关闭，后者在所属循环等待全部会话与后台操作回收，不停止循环或其他宿主 |
| `worker()` / `stats()` | 核心 worker 与宿主统计；统计可跨线程读取 |

```cpp
#include "ruvia/core/task_scope.h"
#include "ruvia/web/http_server.h"

ruvia::task<void> answer(ruvia::request_stream& stream) {
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(std::string_view("ready"));
        co_await stream.response().end();
    }
}

// 由调用方在其事件循环上启动，并用其停止令牌控制服务寿命。
ruvia::task<void> serve(ruvia::event_loop loop, ruvia::stop_token stop) {
    ruvia::http_server server(loop, {.request_stream_ = &answer});
    server.listen({.address_ = "127.0.0.1", .http_ = 8080,
        .dispatch_ = ruvia::listener_dispatch::request_stream});
    co_await server.start();
    (void)co_await ruvia::sleep_until(server.worker(),
        std::chrono::steady_clock::time_point::max(), stop);
    co_await server.shutdown();
}
```

- **线程归属**：除 `close()`、析构与 `stats()` 外，操作均在绑定循环执行；`server_config` 的调度字段由外部循环负责，其余限额作用于该宿主的单 worker。
- **原生接管**：外部服务不接收 HTTP/3、继承监听 socket 或自动重定向配置；Windows 的接管 socket 不得已绑定其他 IOCP，绑定监听仍复用独立原生 Acceptor。
- **销毁边界**：析构仅请求关闭；调用方须继续驱动循环，直到 `shutdown()` 完成或循环自身的停止回调完成 join。
- **出站与状态**：同循环的 `http_client`、`db_client`、`redis_client` 及 per-worker 状态由调用方拥有，经 `router` 的 `bind_*` 入口借给路由请求，使 `c.redis()` / `c.db()` / `c.client()` / `c.worker_state<T>()` 及 `session_middleware`、CSRF 等内置中间件可用，见 [1.11](#111-显式路由组合ruviawebrouterh)。
- **表示策略**：构造参数 `server_config` 的 `compression_` / `cors_` 生效，范围见 [1.2](#压缩cors-与可信代理配置)。
- **异步准备**：TLS 凭据、CA 与上下文准备需要所属循环显式借用 `blocking_pool`；缺池或拒绝准入时传播失败，不退回 worker 同步执行。纯明文服务不要求池。冷任务返回前拥有配置、敏感输入与继承 socket；丢弃未等待的任务也会释放它们。
- **Windows Acceptor**：启动、退役与关闭异步等待原生线程完成；退出等待不占阻塞池，因此池已满或已停止也不妨碍清理。

#### PROXY 前缀与已读字节交接

- **强制前缀**：`listen_config::proxy_protocol_ = true` 后，每条 TCP 连接先解析 HAProxy PROXY v1/v2，再做准入、TLS 与 HTTP；不猜测、不回退为普通 HTTP，缺失、非法及超长头直接关连。仅在可信前置代理可访问该端口时启用。
- **有界接收**：v1 最多 107 字节，v2 整头最多 4096 字节；完成期限取已设 `request_header_timeout_`、`header_completion_timeout_` 的较小值，均关闭时仍以 30 秒为绝对上限，涓流不延期。
- **唯一远端**：PROXY 源端点贯穿准入、连接事件、`conn_info`、访问日志、限流键与可信代理判定；v2 `LOCAL`、v1 `UNKNOWN` 及无 IP 源的头保留物理对端，socket 本地端点不变。HTTP/3 配置与 PROXY 策略互斥，配置时拒绝。
- **外部嗅探**：`adopt_options::prefix_` 放回调用方已读的原始字节，调用返回前复制，优先于 socket 剩余字节交给 PROXY、TLS、HTTP/1、HTTP/2 或原始连接回调；`remote_` 可覆盖源端点，服务自身解析到的 PROXY 源仍优先。`http_server_group::adopt()` 的冷任务同样立即拥有输入；`application` 不新增接管入口。
- **底层组合**：`"ruvia/http/http_request.h"` 提供无 I/O 的单遍增量 `proxy_protocol_parser`；结果只消费头字节，后续字节交还下一协议，TLV 视图借用解析器。

#### 多循环共享服务

`http_server_group(span<const event_loop>, callbacks, config = {})` 将同一服务部署到调用方提供的多个循环，复用上述监听、接管及协议能力，不创建业务线程。

| 入口 / 边界 | 契约 |
| --- | --- |
| `listen()` / `trusted_proxies()` | 启动前纯配置，可在循环尚未驱动时调用；运行后用异步新增入口 |
| `co_await start(caller)` / `co_await shutdown(caller)` | 在显式 `caller` 的 worker 上等待；允许成员 worker 发起，调用方须持续驱动全部存活循环 |
| `co_await add_listener(caller, config)` / `add_connection_service(caller, config)` | 前者运行期新增监听；后者启动前或运行期准备外部接管服务；失败整组回滚 |
| `co_await prepare_listener(caller, config)` / `prepare_listeners(caller, configs)` | 整组、整批准备成功后返回一张票据；先发布业务配置，再激活 |
| `co_await remove_listener(caller, endpoint, options = {})` / `replace_tls_identities(caller, endpoint, config)` / `duplicate_listener_sockets(caller, endpoint)` | 异步跨成员控制；POSIX 复制结果保持 worker 顺序 |
| `co_await adopt(caller, worker_index, endpoint, socket)` | 显式指定接管 worker，冷任务即拥有 socket，失败或丢弃也关闭句柄 |
| `co_await close_connection(caller, id)` | 按 ID 请求整连接关闭，返回值契约见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `worker_count()` / `worker(index)` / `stats()` | 查询实际成员与汇总统计，限额仍逐 worker 执行 |
| 共享状态 | 同一监听器的 TLS 票据密钥、QUIC secret 与 CID 目录服务级共享；Windows 共用一个原生 Acceptor |
| 关闭与退役 | `close()` 任意线程请求立即断连，不提供排空模式、不停止外部循环；任一成员退役或发生致命驱动故障时关闭整组，收束全部连接及在途操作 |

```cpp
#include <array>
#include <iostream>
#include "ruvia/core/event_loop.h"

int main() {
    ruvia::event_loop_pool runtime({.loop_count_ = 2});
    const std::array loops{runtime.loop(0), runtime.loop(1)};
    ruvia::http_server_group server(loops, {.request_stream_ = &answer});
    server.listen({.address_ = "127.0.0.1", .http_ = 8080,
        .dispatch_ = ruvia::listener_dispatch::request_stream});
    runtime.start();
    const auto startup = [&]() -> ruvia::task<void> {
        co_await server.start(loops[0].worker());
    };
    loops[0].start(startup()).get();
    std::cin.get();  // 仅外部控制线程阻塞；worker 不调用 get()。
    const auto shutdown = [&]() -> ruvia::task<void> {
        co_await server.shutdown(loops[0].worker());
    };
    loops[0].start(shutdown()).get();
    runtime.join();
}
```

### 1.10 原始 TCP/TLS 出站（`"ruvia/web/http_client.h"`）

`tcp_client(event_loop, const tcp_client_config&)` 是可独立使用的字节流拥有者，不解析 HTTP，也不创建线程；复用共享 DNS、Happy Eyeballs、TCP 策略与 SAN 校验的 TLS/mTLS 传输。

| 配置 / 方法 | 契约 |
| --- | --- |
| `host_` / `port_` | 拥有型主机与受检 `port_number`，拒绝零端口；DNS 解析要求循环借用 `blocking_pool` |
| `tls_` / `tcp_` | 沿用共享客户端配置；TLS 默认验证身份，明文须显式选择 `client_tls_mode::disabled` |
| `alpn_` / `alpn()` | 按偏好发送 RFC 7301 不透明协议名并读取协商结果；每项 1..255 字节、编码总长至多 65535，明文拒绝非空配置。结果借用连接，只在所属循环、连接成功后访问 |
| `connect_timeout_` / `read_timeout_` / `write_timeout_` | 连接与读写独立期限；`operation_options` 只能收紧配置期限并附加取消，从实际等待时起计 |
| `co_await connect(options = {})` | 在所属循环建立一次连接 |
| `co_await read(options = {})` | 至多 4 KiB；仅 `nullopt` 为 EOF，字节借用至下一次读取或关闭。EOF 不结束写方向 |
| `co_await write(bytes, options = {})` | 写完全部输入；返回冷任务前拥有化输入，`std::string&&` 右值零拷贝接管 |
| `close()` / `co_await shutdown()` | 前者任意线程取消整条连接，后者在所属循环等待回收；不是半关闭或 TLS 优雅关闭 |
| `worker()` | 借用所属核心 worker |

- **并发与失败**：读、写可并行，同方向重叠抛 `std::logic_error`。I/O 错误、超时或取消关闭整条流；`std::system_error` 分别携带底层错误、`timed_out` 或 `operation_canceled`。
- **寿命**：构造、析构与 `close()` 可在其他线程执行；冷任务保留拥有化输入及状态，`shutdown()` 和循环退出使未启动操作失效并收束在途操作。

### 1.11 显式路由组合（`"ruvia/web/context.h"`）

`router(worker_ref)` 直接复用路由编译与分发引擎，不依赖 `application`、控制器宏或全局注册表。

| 入口 | 契约 |
| --- | --- |
| `add<Modifiers...>(method, path, handler)` | 拥有缓冲响应处理器 `task<http_response>(context&)`，同步复制路径 |
| `stream<Modifiers...>(method, path, handler)` | 拥有流式响应处理器 `task<void>(context&)` |
| `websocket<Modifiers...>(path, handler, websocket_route_config = {})` | WebSocket 路由：HTTP/1.1 Upgrade 与扩展 CONNECT（RFC 8441 / RFC 9220），对应 `RUVIA_GET_WS` |
| `connect<Modifiers...>(authority, handler, http_tunnel_route_config = {})` | 普通 CONNECT 隧道：精确 `host:port` 或 `"*"`，对应 `RUVIA_CONNECT` |
| `connect_protocol<Modifiers...>(protocol, path, handler, config = {})` | 扩展 CONNECT 协议隧道；`"connect-udp"` 同时接受 HTTP/1.1 Upgrade，对应 `RUVIA_CONNECT_PROTOCOL` |
| `use<T>(args...)` / `use_at<T>(options, args...)` | 注册 worker-local 中间件；前缀按完整路径段匹配 |
| `rate_limit(rate_limit_config)` | 本 router 的按客户端地址限流规则，与 `application::rate_limit()` 同形，含 `capacity_per_worker_`；未命中路由的请求同样计数，路由级 `rate_limit<>` 叠加生效 |
| `bind_redis(alias, client)` / `bind_database(alias, client)` | 借入调用方拥有的同 worker `redis_client` / `db_client`，按别名供 `c.redis()` / `c.db()` 查找；受构建特性控制 |
| `bind_http_client(alias, client)` | 以 router 自有唯一别名借入同 worker 的 `http_client`；`c.client(alias)` 选择该对象的默认配置，`c.client()` 等价于别名 `"default"` |
| `bind_worker_state<T>(state)` | 借入调用方拥有的 per-worker 状态，供 `c.worker_state<T>()` 查找；每类型一份 |
| `on_error(...)` / `on_not_found(...)` | 注册全局或路径作用域的错误处理器 |
| `freeze()` | 原子编译并构造中间件；失败保留可编辑配置，成功后拒绝注册及再次封存 |
| `co_await dispatch(stream)` | 在 `request_stream` 回调中执行；每请求仅一次，且须早于其他输出选择 |

```cpp
#include "ruvia/web/context.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/middleware.h"

// 位于所属 loop 的服务协程内；server.shutdown() 完成前保持 redis 与 routes 存活。
ruvia::redis_client redis(loop, {.host_ = "127.0.0.1", .port_ = 6379,
    .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}});
co_await redis.connect();
ruvia::router routes(loop.worker());
routes.bind_redis("default", redis)
    .rate_limit({.rule_ = {.max_requests_ = 100, .window_ = std::chrono::seconds(1)}});
routes.use<ruvia::session_middleware>();
routes.add("GET", "/items/:id", [](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
    co_return c.text(c.req().param("id").value());
});
routes.websocket("/ws", [](ruvia::context& c) -> ruvia::task<void> {
    while (auto message = co_await c.websocket().read()) {
        co_await c.websocket().send(std::move(*message));
    }
});
routes.freeze();
ruvia::http_server server(loop,
    {.request_stream_ = [&](ruvia::request_stream& stream) { return routes.dispatch(stream); }},
    {.compression_ = ruvia::compression_config{}, .cors_ = ruvia::http_cors_policy_config{}});
server.listen({.address_ = "127.0.0.1", .http_ = 8080,
    .dispatch_ = ruvia::listener_dispatch::request_stream});
co_await server.start();
// 服务寿命与 shutdown() 的完整驱动见 1.9；redis.shutdown() 在 server.shutdown() 之后。
```

- **线程与寿命**：配置、封存和分发均在绑定 worker 上执行；多 worker 各自拥有 router。处理器捕获的引用须覆盖全部请求，冷 dispatch 保留路由状态但不能脱离请求作用域。
- **共享语义**：参数、HEAD 回退、OPTIONS／405／501、single-shot 中间件、限流、错误处理、URL 生成与缓冲子请求使用原主链；提交后的异常仍关闭连接或重置流。
- **升级路由**：中间件链之后的终端经 `request_stream::accept_websocket()` / `accept_tunnel()` 完成握手（CONNECT 回 `200`，HTTP/1.1 Upgrade 回 `101`），再以 `c.websocket()` / `c.tunnel()` 运行处理器；会话收尾、Capsule 与 Datagram 语义同 [1.8](#18-请求流分发ruviawebrequest_streamh)。
- **升级拒绝**：未提供对应升级的请求（普通 `GET`、携带正文的 Upgrade）经 `on_error` 回答 `400`；握手被拒绝时不运行处理器并回答 `400`，中间件后处理仍可见该响应。
- **借入能力**：借入任一客户端后，本 router 分发的客户端查找只在借入集合内解析；借入任一状态后，状态查找同理；未借入的类别沿用宿主注册。
- **借入约束**：客户端须绑定 router 所属 worker，否则抛 `std::invalid_argument`；同类别别名重复同样抛出。客户端与状态须覆盖全部分发，在服务该 router 的宿主 `shutdown()` 完成后才能关闭或销毁。
- **独立 Session**：借入 Redis 后 `session_middleware`、CSRF 等依赖 `c.redis()` 的内置中间件在 `http_server` 下直接可用，无需第二种会话来源。
- **正文策略**：`body_limit<N>` 由 `middleware.h` 提供，可与 `stream_body` 组合；只收紧宿主预算。协议 I/O、取消、正文与响应输出继续借用请求流服务。

---

## 2. 控制器与路由声明（`"ruvia/web/context.h"`）

控制器是不继承任何框架基类、可默认构造的 `final` 类。通过路由宏在类定义内声明并静态注册；首次准备 `routes` 监听器时才构造控制器、编译并封存路由计划。

### 2.1 路由组与控制器宏

| 宏 | 位置与说明 |
| --- | --- |
| `RUVIA_CONTROLLER_GROUP(prefix, ...)` | 位于类定义内部、`RUVIA_ROUTES_BEGIN` 之前；指定该控制器的公共路径前缀及控制器级中间件类型列表 |
| `RUVIA_ROUTES_BEGIN(controller_type)` | 开始声明 `controller_type` 的路由表 |
| `RUVIA_GROUP_BEGIN(prefix, ...)` / `RUVIA_GROUP_END` | 在路由表内部开启/结束嵌套路径前缀与组级中间件（支持多层嵌套） |
| `RUVIA_ROUTES_END` | 结束路由表声明 |

### 2.2 路由注册宏全表

末尾可变参数 `...` 均用于附加当前路由专属的中间件或策略类型（如 `ruvia::json_body<Model>`、`ruvia::body_limit<1024>`、`ruvia::rate_limit<10, 1000>`）：

| 类别 | 宏名称 | 处理器成员函数签名 | 说明 |
| --- | --- | --- | --- |
| 缓冲或流式响应路由 | `RUVIA_GET(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 自动提供 `HEAD` 回退并将其列入 `Allow`；仍运行同一处理器与中间件，`c.req().method()` 为 `"HEAD"`。缓冲响应省略正文；流式/SSE 必须先 `co_await writer.commit()`，返回 `completed` 时停止正文生产（HEAD/304 为正常完成，`aborted()` 为 false，不取消 token），返回 `body` 才可写入。请求体消费/排空与响应完成独立；流式/SSE 在请求体完整消费后监测客户端断开并停止 `c.stop_token()`，pipeline 后续请求之后的写半关闭不取消当前响应（见 [streaming-websocket.md](streaming-websocket.md#客户端断开即取消请求)）。WebSocket 路由不提供 HEAD 回退 |
| 缓冲或流式路由 | `RUVIA_POST(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 注册 `POST` 路由（附加 `ruvia::stream_body` 时流式消费请求体；返回 `task<void>` 时流式写出响应/SSE） |
| 缓冲或流式路由 | `RUVIA_PUT(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 注册 `PUT` 路由（可附加 `ruvia::stream_body` 流式消费请求体） |
| 缓冲或流式路由 | `RUVIA_DELETE(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 注册 `DELETE` 路由 |
| 缓冲或流式路由 | `RUVIA_PATCH(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 注册 `PATCH` 路由（可附加 `ruvia::stream_body` 流式消费请求体） |
| 缓冲请求路由 | `RUVIA_HEAD(path, handler, ...)` | `task<http_response> handler(context&)` | 注册显式 `HEAD` 路由 |
| 缓冲请求路由 | `RUVIA_OPTIONS(path, handler, ...)` | `task<http_response> handler(context&)` | 注册显式 `OPTIONS` 路由 |
| 缓冲请求路由 | `RUVIA_METHOD(token, path, handler, ...)` | `task<http_response> handler(context&)` | 注册自定义扩展方法（如 `"PROPFIND"`，要求静态路径）；405 与 `OPTIONS` 的 `Allow` 列出该路径注册的全部扩展方法 |
| 缓冲请求路由 | `RUVIA_ALL(path, handler, ...)` | `task<http_response> handler(context&)` | 同时注册 `GET`、`POST`、`PUT`、`PATCH`、`DELETE`、`OPTIONS` |
| 缓冲请求路由 | `RUVIA_ON(methods, paths, handler, ...)` | `task<http_response> handler(context&)` | 方法元组与路径元组的笛卡尔积批量注册 |
| WebSocket | `RUVIA_GET_WS(path, handler, ...)` | `task<void> handler(context&)` | WebSocket 路由（首参可传可选的 `websocket_route_config`，后接中间件） |
| HTTP 隧道 | `RUVIA_CONNECT(authority, handler, ...)` | `task<void> handler(context&)` | `CONNECT` 字节流隧道（`authority` 为 `"host:port"` 或 `"*"`，首参可传可选的 `http_tunnel_route_config`） |
| 扩展 CONNECT 隧道 | `RUVIA_CONNECT_PROTOCOL(proto, path, handler, ...)` | `task<void> handler(context&)` | Extended `CONNECT` 路由（如 `"connect-udp"`，首参可传可选的 `http_tunnel_route_config`） |

- **扩展方法索引**：方法标记与规范路径在启动期排序封存；请求期的方法识别、路由匹配和 `Allow` 路径分组采用零分配二分查找。

WebSocket 处理器通过 `c.websocket()` 获取连接（`"ruvia/web/streaming.h"`）。允许一个已启动的 `read()` 与一个写操作并行；第二个同方向操作在前一个完成前启动会抛出 `std::logic_error`。单个写者连续发送时，已等待的读者在完整消息边界获得协议轮询机会，并在下一条应用消息写入前处理已收到的 Ping/Close 及自动 Pong/Close 输出；不会中断正在发送的完整消息，也不会隐式创建后台读任务（RFC 6455 §5.5.1–5.5.2）。`send(websocket_message&&)` 借用消息的读取存储，按公开头约定额外占用读 lane，不能与另一个 `read()` 重叠。

### 2.3 路径模式与反向链接生成（`url_for`）

- **静态段**：`/api/hello`
- **命名参数段**：`/api/users/:id`（通过 `c.req().param("id")` 读取）
- **尾部通配符**：`/api/files/*`（匹配余下路径，通过 `c.req().param("*")` 读取）
- **路径等价**：请求路径与注册模式按同一规范形式比较：以字面 `/` 切分路径段，段内按百分号解码后的字节比较。因此 `/users/%61dmin`、`/users/adm%69n` 与 `/users/admin` 命中同一路由（十六进制大小写不影响匹配，如 `caf%c3%a9` 与 `caf%C3%A9`），而 `%2F` 始终留在所在段内，不作为路径分隔符。该形式与路径参数的解码结果、`use_at` 前缀作用域中间件以及前缀错误/404 处理器的匹配规则一致；比 RFC 3986 的等价规则更宽，`%3A` 与 `:` 这类保留字符的编码与字面形式也视为同一路径。同一方法下仅拼写不同的两个模式（如 `/a` 与 `/%61`）属于重复注册，启动时抛出 `std::invalid_argument`。重复判定逐段进行，参数段与通配符段只与同类段相同：以 `:` 开头的字面段或字面 `*` 只能编码书写（`%3Aid`、`%2A`），与同位置的 `:id` 参数或 `*` 通配符是不同路由，可同时注册。
- **反向路由生成**：`c.url_for("/api/users/:id", {"42"})` 返回经过百分号编码的 `std::string`（若传入未注册的路由模式则直接抛异常，防止生成死链）。

---

## 3. 请求上下文 `context` 与 `context_request`

头文件：`"ruvia/web/context.h"`。

### 3.1 读取请求信息（`c.req()` 与 `c.conn()`）

| 方法签名 | 说明 |
| --- | --- |
| `c.req().method()` / `c.req().known_method()` | 请求方法字符串视图（如 `"GET"`）与 `http_known_method` 枚举 |
| `c.req().path()` / `c.req().target()` / `c.req().route_path()` | 请求路径、原始请求目标与命中的注册路由模式 |
| `c.req().scheme()` / `c.req().authority()` / `c.req().protocol_version()` | 请求 scheme、authority 与协议版本（`http10` / `http11` / `http2`） |
| `c.req().param(name)` / `c.req().param<T>(name)` / `c.req().param_fields()` | 获取路径参数字符串视图、类型化解析标量值（整数或 `bool`，缺失返回 `std::nullopt`，格式非法自动抛 HTTP 400）或参数列表 |
| `c.req().query(name)` / `c.req().query<T>(name)` / `c.req().queries(name)` / `c.req().query_fields()` | 获取单个/类型化/多个同名 URL 查询参数（已自动 URL 解码） |
| `c.req().header(name)` / `c.req().header_fields()` | 大小写不敏感读取请求头 |
| `c.req().cookie(name)` / `c.req().signed_cookie({.name_ = ..., .secret_ = ...})` | 读取普通 Cookie 或验证 HMAC 签名并提取 Cookie 原值 |
| `c.req().cookie_fields()` | 返回保留重复项与请求顺序的只读 `request_name_value_list`；名称区分大小写，`get(name)` 取最后一个同名值 |
| `c.req().negotiate(type, supported)` | 在候选列表中按 `Accept*` 权重协商最佳匹配（单候选时 `has_value()` 即表示客户端接受该值） |
| `co_await c.req().text(operation = {})` / `co_await c.req().bytes(operation = {})` | 读取完整缓冲请求体视图（`std::string_view` / `std::span<const std::byte>`） |
| `co_await c.req().discard_body(operation = {})` | 丢弃剩余请求体字节 |
| `co_await c.req().multipart(operation = {})` | 解析缓冲式 `multipart/form-data`，返回 `std::vector<multipart_part>` |
| `c.req().validated<T>()` | 获取由校验中间件（`json_body<T>` 等）已完成解析和校验的模型常量引用 |
| `co_await c.req().json_if<T>(operation = {})` / `co_await c.req().form_if<T>(operation = {})` | 按需结构化解析并校验 JSON 或 Form 为 `std::optional<T>`（仅当 `Content-Type` 不匹配时返回 `std::nullopt`） |
| `c.conn().remote()` / `c.conn().client()` | 直连对端 `ip_endpoint`（`.address()` 返回拥有字节的 `ip_address`，`.port()` 返回端口）与经过 `trusted_proxies` 解析后的真实客户端文本地址 |
| `c.conn().via_trusted_proxy()` / `c.conn().scheme()` / `c.conn().tls()` | 是否经过可信代理、连接有效 scheme（`http` / `https`）及 TLS 传输信息指针（`client_certificate_subject()`、协商的 `alpn()` 与客户端 `server_name()`，未协商或未发送时为空） |
| `c.conn().connection_id()` | 传输连接标识（`std::uint64_t`），规则见 [1.6](#16-连接观察钩子ruviawebserver_observationh) |
| `c.client()` / `c.client(alias)` | 获取当前请求作用域内的出站 HTTP 客户端句柄；与 `c.conn().client()`（客户端地址）不同 |
| `c.early_data()` | 请求流是否实际携带 QUIC 0-RTT 数据；不会因为处理时握手已完成而清除此来源标记 |

HTTP/1 定长正文已完整预读且无需内容解码时，缓冲读取直接借用连接缓冲并同步完成该读取步骤，不创建额外的正文 I/O 协程；返回的视图仍只在当前请求内有效。

- **逐次等待**：缓冲读取的 `operation`（`operation_options`）约束等待正文到达的时间；超时或停止只结束本次调用，抛 `std::system_error`（`timed_out` / `operation_canceled`），已到达字节保留，再次调用继续并返回完整正文。启动前已停止抛 `operation_canceled`；非正超时抛 `std::invalid_argument`；服务器正文超时继续生效。HTTP/2、HTTP/3 路由的缓冲正文在分派前已收齐，不再等待。
- **视图句柄**：`c.req()` 返回的 `context_request` 是可复制句柄，操作借用 `context` 而非句柄本身，因此可直接在临时句柄上调用。
- **HTTP/1 惰性正文互斥**：原始正文读取与丢弃共用消费状态；底层操作在途时，再启动完整读取或丢弃抛 `std::logic_error`。
- **HTTP/1 惰性正文终态**：读取成功后复用缓冲，丢弃不清除已缓冲内容；完成丢弃后拒绝读取，原始正文消费失败后拒绝读取与丢弃，均抛 `std::logic_error`。

- **Cookie 缓存**：首次枚举时单遍解析全部 `Cookie` 首部；后续读取复用完整结果，分配失败不发布部分列表。
- **Cookie 借用**：描述符随 Context 销毁；名称和值借用请求首部，不得逃逸请求生命周期。

### 3.2 构造响应与设置响应元数据

```cpp
ruvia::task<ruvia::http_response> handler(ruvia::context& c) {
    c.draft().status(ruvia::http_status::created);
    c.draft().header("X-Custom", "value");
    c.draft().set_cookie({
        .name_ = "theme",
        .value_ = "dark",
        .attributes_ = {
            .path_ = "/",
            .http_only_ = ruvia::cookie_attribute_policy::emit,
        },
    });

    // 1. 静态字面量零拷贝响应：
    // co_return c.text(ruvia::static_text("created\n"));

    // 2. 构造动态字符串并零拷贝移动返回：
    std::string body("created\n");
    co_return c.text(std::move(body));
}
```

| `ruvia::context` 响应方法 | 说明 |
| --- | --- |
| `c.draft()` / `c.edit_response()` / `c.policy()` | 返回显式元数据 editor：分别编辑下一次构造的草稿、已选定的缓冲响应、跨替换的请求级响应策略 |
| `editor.status(code)` / `header(name, value, options = {})` / `remove_header(name)` / `priority(fields)` | 设置 `200..599` 状态、响应头或 RFC 9218 优先级；头默认替换，可指定 `http_response_header_mode::append` |
| `editor.set_cookie(options)` / `set_signed_cookie(options)` / `delete_cookie(options)` | 设置普通/HMAC 签名 Cookie 或用过去的 `Expires` 使其过期；`max_age_ = 0s` 同样使用过去的 `Expires`，并覆盖同时设置的 `expires_` |
| `http_response text(...)` / `html(...)` / `body(...)` | 构造 `text/plain; charset=utf-8`、`text/html; charset=utf-8` 或原始响应体（支持字符串字面量、`std::string_view`、`std::string&&`、`std::span<const std::byte>`、`ruvia::static_text`） |
| `template <typename T> http_response json(const T& value)` | 将 `RUVIA_MODEL` 或模型数组（`ruvia::array<T>` / `boxed_array<T>`）序列化为 `application/json` 响应 |
| `http_response redirect(redirect_response_options options)` | 构造重定向响应（默认 `302 Found`）；绝对或 `//` 开头的 Location authority 含 `\` 时抛 `std::invalid_argument`，避免浏览器把反斜杠当路径分隔符而改变主机 |
| `task<http_response> file(file_response_options options)` | 在阻塞池上打开磁盘文件并构造响应（`co_return co_await c.file(...)`），支持 Range 与条件请求；任务完成时状态、校验器、长度与打开的句柄均已确定 |
| `task<http_response> static_file(const static_root& root, static_file_response_options options)` | 从预构建的 `static_root` 快照构造响应（`co_await`）；文件型条目在阻塞池上于根目录之下打开 |
| `http_response error(http_error_info_options options) const` / `error(const http_error_info& info) const` | 构造标准 JSON 错误响应（传入 `http_error_info` 时自动保留 `validation_issues`） |
| `task<http_response> not_found()` | 调用当前匹配的 404 处理器生成响应 |
| `void respond(http_response&&)` / `const http_response* response() const noexcept` / `bool committed() const noexcept` | 选择或替换尚未提交的缓冲响应 / 只读观察选定响应（不包含尚待应用的 policy，不是最终线上响应头） / 查询响应头或流升级是否已提交 |

草稿字段只属于下一次成功的 `body/text/json/html/file/static_file/redirect` 构造；成功后草稿清空，构造失败则保留。`error()` 构造独立错误响应，不继承草稿。异常进入错误处理器前框架清空失败处理器的草稿，避免表示字段污染错误响应。

`respond()` 只接管传入响应，不合并草稿或被替换响应。`edit_response()` 明确修改当前已选响应；没有响应时抛 `std::logic_error`。需要跨替换保留的字段必须显式写入 `policy()`：它在缓冲响应交付或流式提交时应用一次，**也适用于错误响应**。默认不覆盖状态，显式 `policy().status(...)` 则连错误状态也覆盖；不要把表示相关的 `Content-Range`、`ETag` 等放入通用策略。安全头中间件以默认/覆盖策略保留配置的冲突规则，session 与 CSRF Cookie 使用明确的请求级策略，不依赖 `next()` 前后时机。所有 editor（包括此前保存的 editor）在流式提交后修改均抛 `std::logic_error`。

### 3.3 进程内子请求（`"ruvia/web/context.h"`）

在控制器或中间件内可通过 `co_await c.dispatch({...})` 在当前 worker 内直接重入路由表发起虚拟子请求（不经过网络 socket；`dispatch_options` 的 `method_`、`target_`、`body_` 使用 `borrowed_text`，拒绝临时拥有型字符串并在 `dispatch()` 返回前复制）：

```cpp
auto sub = co_await c.dispatch({
    .method_ = "GET",
    .target_ = "/api/internal/profile",
});
// sub.status(), sub.body(), sub.header("Content-Type")
// 子请求内可通过 c.is_subrequest() 判断当前是否为子请求
```

### 3.4 阻塞任务卸载（`c.run_blocking`）

阻塞任务在 worker 事件循环借用的应用阻塞池上执行（默认启用，可通过 `app().blocking_pool({...})` 调整；`blocking_pool(nullptr)` 禁用后返回 `blocking_status::unavailable`）。在请求中卸载耗时同步计算：

```cpp
std::string input_copy(c.req().query("data").value_or(""));
auto result = co_await c.run_blocking(
    [owned = std::move(input_copy)]() {
        return compute_expensive_digest(owned);
    },
    {.timeout_ = std::chrono::milliseconds(500)});

if (!result.completed()) {
    co_return c.error({
        .status_ = ruvia::http_status::service_unavailable,
        .code_ = "blocking_rejected",
        .message_ = ruvia::describe_blocking_status(result.status()),
    });
}
auto digest = std::move(result).take_value();
```

签名为 `run_blocking(fn, operation_options = {})`（`request_stream` 与 `web_worker_context` 相同）：`timeout_` 只限定等待，不中断已开始的调用；`stop_token_` 与 `c.stop_token()` 组合，二者任一触发即以 `cancelled` 结束等待。

---

## 4. 中间件与内置请求策略

### 4.1 自定义中间件（`"ruvia/web/middleware.h"`）

任何声明为 `final`、不继承基类，且提供 `ruvia::task<void> invoke(ruvia::context&, ruvia::next&)` 的类均满足 `ruvia::middleware` 概念；中间件自行产生响应时在返回前调用 `c.respond(...)`：

```cpp
class timing_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        c.policy().header("X-Policy", "retained");
        co_await next_value();
        // 仅对当前缓冲响应后处理；不能修改已经提交的流式响应。
        c.edit_response().header("X-Processed", "true");
    }
};
```

- **协程边界**：`next` 仅组合 `task<void>::operator co_await()` 的公开等待协议，不依赖 core 内部等待物类型。

#### 请求级类型化状态传递（`bind_request_state` / `request_state`）

中间件向后续路由传递鉴权身份或上下文数据时，使用 `c.bind_request_state(const T&)`。绑定對象在中间件协程帧上持有并跨越 `co_await next_value()`：

```cpp
struct current_user {
    std::string_view id_;
};

class auth_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        current_user user{.id_ = "u_100"};
        auto binding = c.bind_request_state(user);
        co_await next_value();
    }
};

// 在下游路由处理器中读取：
const auto& user = c.request_state<current_user>();
```

### 4.2 内置中间件与运维组件

| 组件 / 头文件 | 用法与说明 |
| --- | --- |
| `ruvia::body_limit<MaxBytes>` (`"ruvia/web/context.h"`) | 仅用于控制器或单条路由（如 `RUVIA_POST("/upload", upload, ruvia::body_limit<65536>)`），收紧该路由允许的最大请求体字节数 |
| `ruvia::rate_limit<MaxReqs, WindowMs>` / `ruvia::sliding_window_rate_limiter` (`"ruvia/web/middleware.h"`) | 控制器或路由级滑动窗口限流中间件（按 `c.conn().client().address()` 在当前 worker 内计数），以及可独立用于 `request_stream` / `server_connection` 或自定义维度的 `sliding_window_rate_limiter`、`rate_limit_decision`、`format_rate_limit_address_key`、`apply_rate_limit_headers` |
| `ruvia::security_headers_middleware` (`"ruvia/web/middleware.h"`) | 不依赖 Session 或 Redis；注入 `X-Content-Type-Options`、`X-Frame-Options`、`Content-Security-Policy`、`Referrer-Policy`、`Permissions-Policy` 等安全响应头，对 404 同样生效；`Strict-Transport-Security` 仅在 HTTPS 下发出 |
| `ruvia::csrf_protection` (`"ruvia/web/middleware.h"`) | 基于 Double-Submit Cookie（默认 Cookie `"XSRF-TOKEN"` 与请求头 `"X-XSRF-TOKEN"`）的无状态 CSRF 防护中间件；校验失败抛出 `http_error`（`403` / `csrf_token_mismatch`），经 `on_error` 应答 |
| `ruvia::session_middleware` (`"ruvia/web/middleware.h"`, 需 `RUVIA_ENABLE_REDIS`) | 基于 Redis 的服务端会话中间件；路由中通过 `c.session().data()`、`c.session().set(...)`、`c.session().regenerate()`、`c.session().clear()` 管理会话；`redis_alias_` 经 `c.redis(alias)` 解析，应用宿主取 `application::redis()` 注册，独立宿主取 `router::bind_redis()` 借入 |
| `ruvia::make_health_response` / `make_readiness_response` (`"ruvia/web/context.h"`) | 生成标准健康检查（`200 OK`）与就绪探针响应（`readiness_state::ready` 返回 `200`，`unavailable` 返回 `503`） |

两级限流使用相同的地址身份规则：IPv4-mapped IPv6 与对应 IPv4 共用配额；不带 scope 的 IPv6 按 `/64` 网络前缀计数；带 scope 的 IPv6 按完整 128 位地址及数值 scope 计数，压缩、展开和大小写形式不会产生独立配额。不同 scope 保持隔离。地址仍由可信代理配置决定，限流计数仍归当前 worker。

两级限流按 key 计算滑动窗口估计值（与 pingora-limits 的 `Rate`、Cloudflare 的做法相同）：每个 key 的窗口从它的首个请求开始、按 `window_` 首尾相接地滚动，分别记录当前窗口与上一窗口已放行的请求数；请求在 `上一窗口计数 × 当前窗口尚未经过的比例 + 当前窗口计数 < max_requests_` 时放行并计入当前窗口，被拒绝的请求不计数。窗口滚动时当前计数转为上一窗口计数；距当前窗口起点已过两个或更多窗口时两者都清零。上一窗口的计数随时间线性淡出而不是在边界处一次清空，因此不允许在一个窗口末尾用完配额、紧接着在下一窗口开头再用完一整份配额；相邻请求间隔（按毫秒计）都大于 `window_ / max_requests_` 的流量始终放行，恰好以该间隔对齐窗口起点到达的请求会因估计值等于上限而被拒绝。拒绝时 `X-RateLimit-Remaining` 为 `0`；`Retry-After` 为估计值再次放行的最早时刻（向上取整到秒，按它等待不会早于真实放行时刻）；`X-RateLimit-Reset` 为估计值降到零、整份配额恢复的时刻（向上取整到秒，不小于 `Retry-After`）。表满时按 `overflow_policy_ = deny` 拒绝的请求两者均为 `1`。估计值的比较使用 128 位精确乘积，延迟按无符号毫秒计算后再向上取整到秒，因此即使 `window_` 接近 `std::chrono::milliseconds::max()`、`max_requests_` 取到 `std::size_t` 上限也不会溢出，最长约两个窗口的 `X-RateLimit-Reset` 也不会被截短。

- **窗口时基**：保留首个请求的 `steady_clock` 原始时刻；先计算经过的时长再量化为毫秒，不截断窗口起点，避免亚毫秒部分导致 Reset 少报或槽位提前回收。

每个 worker 预分配 `capacity_per_worker_`（默认 `8192`）个滑动窗口计数槽。key 的起始槽位由每个 worker 启动时随机抽取密钥的 SipHash-1-3 计算，攻击者无法离线算出哪些 key 落入同一探测窗口；一次查找或插入只探测该起始槽位起的连续槽位（`min(capacity_per_worker_, 32)` 个），单次请求的开销与表容量无关；这些槽位全被其他 key 的未过期计数占用时，该请求按规则的 `overflow_policy_` 处理（默认 `deny`），即使表的其他部分仍有空位。槽位在其 key 当前窗口起点之后满两个窗口才过期、可被其他 key 复用；在此之前上一窗口计数仍参与估计。容量需要为活跃客户端数留出余量。

会话仅接受固定 48 位小写十六进制 ID，其他长度或字符按不存在的会话处理，不读取或修改其拼接的 Redis key。会话按 `key_prefix_ + ID` 存储。固定长度防止不同前缀通过变长 ID 穿透命名空间；使用同一 Redis 数据库和相同前缀则共享会话命名空间，隔离责任由配置方承担。

- **显式会话轮换**：`regenerate()` 仅重新安排已加载会话：非空数据轮换 ID，空数据清除旧 ID；未加载会话或已有待提交动作时保持原计划。

CSRF 的 `header_name_` 必须是前端脚本主动提交 token 的自定义字段，例如 `X-App-CSRF` 或 `CSRF-Token`。构造时拒绝 CORS-safelisted、浏览器保留及已知自动生成/代理字段，如 `Accept`、`Content-Type`、`Origin`、`Sec-*`、`X-Forwarded-*`，前后端须使用同一 token 字段。这里依赖 [Fetch 的自定义请求头与预检边界](https://fetch.spec.whatwg.org/#cors-safelisted-request-header)，不是给任意等值字段赋予认证意义。部署方须保证代理不注入所选 token 字段，且仅向可信 origin 授予携带凭据的 CORS 权限；该中间件不替代用户认证，也不隔离已经被应用信任的跨源脚本。

---

## 5. 声明式模型 `RUVIA_MODEL`、序列化与参数校验

头文件：`"ruvia/web/model.h"`。

### 5.1 模型定义与字段类型

使用 `RUVIA_MODEL(ModelName, ...)` 定义编译期反射模型。模型专用的字段类型位于 `namespace ruvia`：

- **标量类型**：`ruvia::bool_value`、`ruvia::int8`、`ruvia::uint8`、`ruvia::int16`、`ruvia::uint16`、`ruvia::int32`、`ruvia::uint32`、`ruvia::int64`、`ruvia::uint64`、`ruvia::float_value`、`ruvia::double_value`
- **字符串与字节**：`ruvia::string`（UTF-8 字符串）、`ruvia::bytes`（JSON 中以 Base64 编码的二进制数据）
- **复合与容器类型**：嵌套的另一个 `RUVIA_MODEL` 类型、`ruvia::array<T>`（连续数组）、`ruvia::boxed_array<T>`（支持前向声明的自引用递归模型数组）
- **容器字段重绑定**：`array` / `boxed_array` 的字符串及嵌套值先完整准备候选，再消费源；`set()` 从字段自身移动时同样保持失败原值，成功后统一提交。
- **容器插入**：先取得存储再构造拥有型值；分配失败不消费移动源或破坏已有元素，包括来自容器自身元素的输入。

`float_value` / `double_value` 的构造与模型 `set()` 在宽浮点数窄化前检查有限值范围：超出目标类型有限范围时抛 `std::out_of_range`，setter 保留原字段值或 null 状态。范围内仍按目标浮点类型舍入；显式 NaN/infinity 的既有行为不变，JSON 序列化为 `null`。

#### 字段声明与属性修饰宏

| 宏 | 说明 |
| --- | --- |
| `RUVIA_REQUIRED_FIELD(field, type, ...)` | 必填字段（缺失则校验失败） |
| `RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...)` | 指定外部序列化字段名 `wire_name` 的必填字段（`wire_name` 须为合法 UTF-8，否则编译失败；其 JSON 键在编译期完成转义） |
| `RUVIA_OPTIONAL_FIELD(field, type, ...)` | 可选字段（仅允许字段缺失；若需允许 JSON `null` 须额外加 `RUVIA_NULLABLE`） |
| `RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...)` | 指定外部序列化字段名 `wire_name` 的可选字段（约束同上） |
| `RUVIA_NULLABLE` | 允许字段值为 `null`（通过 `m.is_null<"field">()` 判断） |
| `RUVIA_DEFAULT(value)` | 反序列化时若字段缺失（非 `null`），自动填充默认值 `value`（仅用于可选字段；用于 `RUVIA_REQUIRED_FIELD` 时编译失败） |
| `RUVIA_INITIAL(value)` | 在 C++ 中默认构造模型实例时将该字段初始化为 `value` |
| `RUVIA_OMIT_EMPTY` / `RUVIA_EMIT_NULL` | 控制序列化时缺失或空值字段的输出行为 |

#### 声明式校验规则宏（附加在字段宏末尾）

- `RUVIA_MIN(limit, message)`：数值下限或字符串 UTF-8 字节长度下限
- `RUVIA_MAX(limit, message)`：数值上限或字符串 UTF-8 字节长度上限
- `RUVIA_EMAIL(message)`：电子邮件格式校验
- `RUVIA_ONE_OF(message, val1, val2, ...)`：枚举白名单校验
- `RUVIA_PATTERN(pattern, message)`：整串锚定正则模式校验（字符类在编译期折叠为字节集合，每个输入字节 O(1) 判定；回溯按固定步数预算有界，超出即判定不匹配）
- `RUVIA_CUSTOM(predicate, message)`：自定义谓词函数校验

```cpp
#include "ruvia/web/model.h"

RUVIA_MODEL(create_user_request,
    RUVIA_REQUIRED_FIELD(username, ruvia::string,
        RUVIA_MIN(3, "must be at least 3 bytes"),
        RUVIA_MAX(32, "must be at most 32 bytes")),
    RUVIA_REQUIRED_FIELD(email, ruvia::string, RUVIA_EMAIL("must be a valid email")),
    RUVIA_OPTIONAL_FIELD(role, ruvia::string,
        RUVIA_DEFAULT("member"),
        RUVIA_ONE_OF("invalid role", "member", "admin")),
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::string, RUVIA_NULLABLE)
);
```

### 5.2 模型实例访问与路由自动校验

#### 模型成员方法

- `m.get<"username">()`：获取只读字段值；非空必填字段返回引用，其余字段返回只读 `std::optional` 引用。`ruvia::string` 可调用 `.view()`，标量包装可读取 `.value_` 或隐式转换。
- `m.set<"username">("alice")`：设置字段值（支持链式调用）
- `m.set_null<"nickname">()`：将 `RUVIA_NULLABLE` 字段置为 `null`（等价于 `m.set<"nickname">(nullptr)`，支持链式调用）
- `m.has_value<"username">()`：查询该字段当前是否持有有效非空值（含默认值、初始值、输入解析值或应用 `set` 赋值），`null` 与缺失均返回 `false`；`m.is_present<"role">()` 查询原始输入中是否出现该字段，默认值及应用后续修改不改变输入来源记录；`m.is_null<"nickname">()` 查询当前字段是否为 `null`。模型不提供 `is_set`：`RUVIA_DB_ENTITY` / `RUVIA_REDIS_ENTITY` 的 `is_set` 表示“已赋值，`null` 也算”，与模型的 `has_value` 语义不同。
- `m.ensure<"nested">()`：确保可原地修改的字段已初始化并返回可变引用。`json_value` 字段不提供此入口：用 `get()` 读取，用 `set<"field">(token)` 替换；声明 `RUVIA_NULLABLE` 后可用 `set<"field">(nullptr)` 或 `set_null<"field">()` 设置 null。这样 token 替换与字段 null 状态始终经过同一个检查入口。
- `m.reset<"nickname">()`：将可选字段恢复为缺失状态并返回模型引用（支持链式调用）；必填字段不提供此入口，可空必填字段用 `set_null<"field">()` 或 `set(nullptr)` 设置 null。

#### 路由级多源绑定中间件

将以下校验中间件直接挂在路由宏末尾，框架会在进入路由处理器前自动解析并执行全部字段规则（校验失败自动返回 `400`/`422` 结构化错误响应，包含 `validation_issues`）：

请求校验中间件与 `json_if<T>()` / `form_if<T>()` 共用解析后校验路径；JSON/Form 媒体类型不匹配时，中间件拒绝请求，按需解析返回 `std::nullopt`。

| 校验中间件类型 | 数据源 | 路由内提取方式 |
| --- | --- | --- |
| `ruvia::json_body<T>` | JSON 请求体（`application/json`） | `c.req().validated<T>()` |
| `ruvia::form_body<T>` | URL 编码表单请求体（`application/x-www-form-urlencoded`） | `c.req().validated<T>()` |
| `ruvia::query_model<T>` | URL 查询参数（Query String） | `c.req().validated<T>()` |
| `ruvia::path_model<T>` | 路由路径参数（`:param`） | `c.req().validated<T>()` |
| `ruvia::header_model<T>` | HTTP 请求头 | `c.req().validated<T>()` |
| `ruvia::cookie_model<T>` | 请求 Cookie | `c.req().validated<T>()` |

```cpp
RUVIA_POST("/users", create_user, ruvia::json_body<create_user_request>);

ruvia::task<ruvia::http_response> create_user(ruvia::context& c) {
    const auto& body = c.req().validated<create_user_request>();
    std::string_view username = body.get<"username">().view();
    co_return c.text(username);
}
```

### 5.3 独立编解码、动态 JSON 与命令式校验器

不依赖 HTTP 请求上下文时，可直接使用以下工具：

- **JSON / Form 编解码**：
  - `T ruvia::from_json<T>(std::string_view json, model_parse_options options = {})`（解析失败抛 `std::invalid_argument`，模型字段校验失败抛携带 issues 的 `ruvia::http_error`）
  - `std::string ruvia::to_json(const T& value)`（无短转义形式的控制字符输出为小写 `\u00xx`）
  - `T ruvia::from_form<T>(std::string_view form, model_parse_options options = {})`（解析失败抛 `std::invalid_argument`，字段校验失败抛携带 issues 的 `ruvia::http_error`）
- **数值与布尔 JSON 输出**：`to_json()` 不按标量类型的长度上界预分配；短输出复用返回字符串的内联存储。
- **零拷贝动态 JSON 视图（`"ruvia/web/model.h"`）**：
  - `ruvia::json_object::parse(std::string_view body, model_parse_options options = {})` / `ruvia::json_value::parse(std::string_view body, model_parse_options options = {})`（直接返回值，语法非法抛 `std::invalid_argument`）
  - 支持 `obj.get<ruvia::string>("key")`、`obj.get<ruvia::int64>("count")`、`obj.for_each_field([](std::string_view key, const ruvia::json_value& val) { ... })`、`val.for_each_element([](const ruvia::json_value& item) { ... })`。`get<T>()` / `get<T>("key")` 的结果可能借用 token 字节，因此只能在左值 token 上调用，右值 token 上调用编译失败。
  - `get<T>()` 把整个 token 转换为 `T`，返回 `std::optional<T>`。`get<T>("key")` 是成员查找，返回 `ruvia::json_field_lookup<T>`：`status()` 为 `ruvia::unique_lookup_status::absent`（无此成员，非对象 token 一律为此）、`found`（恰好一个成员名按转义解码后等于 `key`）或 `repeated`（同名成员出现两次及以上；RFC 8259 §4 未定义重复名语义，因此不选取任何一次出现，与模型类型化解析拒绝重复字段一致，扫描在第二次出现处立即停止）。`value()` 仅在 `found` 且成员值能转换为 `T`（模型类型还须通过字段校验）时有值；`found` 而 `value()` 为空表示成员存在但不是合法的 `T`。左值查找结果的 `value()` 返回引用，右值查找结果的 `value()` 按值移出 `std::optional<T>`（如 `if (auto port = obj.get<ruvia::uint32>("port").value())`），值同样不能比被查询的 token 活得久。
  - 成功移动或右值字段转移后，源 `json_value` 恢复为 `null`，源 `json_object` 恢复为 `{}`，仍可查询、赋值或作为模型成员序列化；重置本身不分配。自移动赋值保持原值；移动后重新取得借用视图，不继续使用旧视图。
- **命令式校验器 `ruvia::validator`（`"ruvia/web/model.h"`）**：
  - 提供 `.required(opt, field)`、`.min_length(opt, field, n)`、`.max_length(opt, field, n)`、`.range(opt, field, low, high)`、`.one_of(opt, field, list)`、`.add(field, code, message)`，最后调用 `.throw_if_invalid(validation_failure_options = {})` 抛出携带 issues 的 `ruvia::http_error`（默认 400 / `validation_failed`）。

---

## 6. 静态文件服务与内存级测试

### 6.1 静态文件服务（`"ruvia/web/http_server.h"`）

Ruvia 提供三种静态文件服务方式：

1. **单文件响应**：在路由中调用 `co_return co_await c.file({.path_ = "/path/to/file.pdf", .content_type_ = "application/pdf"})`。
2. **不可变目录快照 `ruvia::static_root`**：启动前构造 `ruvia::static_root assets("public/", {.cache_control_ = "public, max-age=3600"})`，在路由中调用 `co_return co_await c.static_file(assets, {.relative_path_ = c.req().param("*").value_or("")})`。
3. **托管自动刷新的 `app().document_root({...})`**：后台按 `refresh_interval_` 重建并发布不可变目录快照。
   - **单一刷新者**：首个登记的 Worker 负责目录扫描、驻留读取与预压缩，其他 Worker 只采用发布快照。
   - **预压缩**：`server_config::compression_` 启用后，`precompress_gzip_` / `precompress_brotli_` / `precompress_zstd_` 控制启动及刷新时生成的表示；仅设置预压缩开关不会生成这些表示。
   - **失败保留**：重建或采用失败时继续服务上一份完整快照，`stats().document_root_refresh_failures_` 计数加一；原因经限流的未处理故障通道（`ruvia: document root refresh failed: ...`，写入 stderr）报告。
   - **等价跳过**：指纹只作预筛选；随后比较完整规范化策略、目录索引及文件元数据，等价时不发布替换快照。
   - **无锁切换**：启动期固定各 Worker 的邮箱；发布者通过无锁原子指针交换移交快照所有权，各 Worker 在请求间隙采用，不获取发布者互斥锁。
   - **请求租约**：请求通过代际租约保活旧快照，正常结束或停机取消时归还；不做请求级快照引用计数。
   - **邮箱回收**：未读取的邮箱节点被后续发布替换回收；邮箱宿主活过全部 Worker 的停机与 join。

三者均内置路径穿越防护、点文件策略（`static_dotfile_policy::deny` 默认拒绝隐藏文件）、`ETag` / `Last-Modified` 条件请求（`304 Not Modified`）与 `Range` 分段响应（`206 Partial Content`）。`c.static_file()` 的 `relative_path_` 是已解码的文件路径，`/` 与 `\` 均为段分隔符；含 `.` 或 `..` 段、空的中间段、绝对路径或 NUL 时一律返回 `403`，不做解析或夹取。命名参数（如 `/files/:name`）可在单个段内解码出 `%2F` / `%5C`，因此 `sub%2F..%2Fsecret` 这类拼写同样被拒绝。

- **目录索引**：规范化后的目录路径按 `index_file_` 查找快照；路径末尾有无分隔符不影响索引选择。

多个范围的请求返回 `multipart/byteranges`，各部分只带 `Content-Type` 与 `Content-Range`；选中预压缩变体（带 `Content-Encoding`）时多范围请求返回完整的 `200` 表示（单个范围仍是带 `Content-Encoding` 的单部分 `206`）。`If-Range` 匹配后的 `206` 只带必需字段（`ETag`、`Cache-Control`、`Vary`、`Content-Range` 或 multipart `Content-Type` 等）及描述所发字节的 `Content-Encoding`，不再重复 `Content-Type`、`Last-Modified`（RFC 9110 §15.3.7）。

非 resident 文件响应会验证索引或构造时记录的文件身份，零字节文件也不例外：空文件在索引后被改写或删除，不能以旧 `Content-Length: 0` 成功提交。`c.file()` 与 `c.static_file()` 返回 `task<http_response>`：文件只打开一次，打开（`c.file()` 的打开与 `fstat` 快照、`static_root` 在根目录之下的按条目打开）在 worker 的阻塞池上执行，任何平台上 worker 都不做文件系统调用；任务完成时状态（`200/206/304/404/403/412/416/500/503`）、`ETag`、`Content-Length`、Range 处理与打开的句柄均已确定，中间件与处理器可直接检查。不发送文件字节的响应（`HEAD`、`304`、`412`、`416`）在池上打开时同时校验索引身份，文件已变化则返回 `500 static_file_changed`。响应正文由同一句柄（读取时为其复制句柄）提供，传输层不再按路径重新解析。两者在返回任务前即完成输入拥有化：路径、`relative_path_` 与 `content_type_` 由调用本身复制到请求存储（`static_file()` 同时完成路径规范化、索引查找与表示选择），保存任务后释放这些来源再 `co_await` 也安全；无法表示文件的路径（`404`）、被拒绝的相对路径（`403`）、未索引的名称（`404`）与不可接受的编码（`406`）在调用时即抛出。只有 `static_file()` 的 `root` 仍是借用，必须活到返回的任务完成；文档根请求的内部分发走同一条池上打开路径，并在等待期间持有快照租约。

文件正文在所有平台、所有传输上走同一条读取链：打开与阻塞读取在阻塞池上执行，每轮最多预读 64 KiB，worker 不做同步文件读取；唯一例外是 Windows 明文 TCP 上大于 16 KiB 的单个范围，经内核 `TransmitFile` 按 64 KiB 分片发送，每片之后重新校验句柄身份，最后一个字节在校验通过后才发出。处理器内的打开与每次文件读取的等待都以 `server_config::write_timeout_` 与请求的 `stop_token()` 为界，超时后放弃等待（打开或读取在池线程上自行结束并释放句柄与缓冲），请求与连接不再被卡住的磁盘占用。

- **预读缓冲**：按需分配，以移动唯一所有权随读取状态转移，不复制缓冲字节。
- **读取租约**：HTTP/2、HTTP/3 文件读取准入成功即交付移动唯一租约，正常或异常退出均自动归还槽位。

响应头之前读取并校验首个文件范围（multipart 正文也一样，先于首个边界）。提交前失败时连接或流仍得到错误响应：文件被删除、替换或改写、读取出错返回 `500 file_read_failed`；读取未完成（阻塞池队列满、未配置阻塞池、超过 `write_timeout_`）返回 `503 file_read_unavailable`；HTTP/1 随后关闭连接，HTTP/2 只结束该流（响应头已发出后则以 `RST_STREAM(INTERNAL_ERROR)` 结束）。`c.file()` / `c.static_file()` 的打开未完成时同样以 `503 file_read_unavailable` 回答：`co_await` 抛出该 `http_error`，经错误处理器得到响应。真实故障（I/O 错误、句柄耗尽、打开或读取超时、缺少阻塞池）经连接故障通道报告（`on_connection_failure()` 回调；未注册时走限流的未处理故障报告）；文件变化、队列满（计入 `blocking_pool_stats().rejected_`）与停机不报告。提交后失败按当前 HTTP 连接或流终止。resident 和已经复制到响应内的字节仍属于独立快照，不因后续磁盘变化失效。

HTTP/2 文件正文在 `HEADERS` 之前只打开并校验、不预读；之后每块只在对端授予发送窗口后读取，至多读取窗口大小，每个连接同时最多 4 个流占用阻塞池读取或持有读取缓冲，等待窗口的流不持有缓冲。

Linux 上 `static_root` 与文档根的索引和每个请求都在保留的根目录句柄之下打开条目（`openat2` 的 `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`；内核不支持时逐段 `O_NOFOLLOW` 打开），任何一段路径被换成符号链接都会拒绝，不会索引或服务根目录之外的文件。Windows 按规范化后的绝对路径打开，以索引记录的卷序列号与 FileId 身份校验作为防线。

### 6.2 内存内无 Socket 测试（`"ruvia/web/app.h"`）

`ruvia::test_app` 在单线程内存内直接驱动已注册的控制器路由、模型校验、中间件、Worker 状态和错误处理器，无需绑定网络端口：

```cpp
#include "ruvia/web/app.h"

ruvia::test_app app;
app.use<request_id_middleware>();

auto res = app.request(
    ruvia::test_request::post("/api/users")
        .header("X-Admin-Token", "secret")
        .json(R"({"username":"ada","email":"ada@example.com"})")
);
// res.status() == ruvia::http_status::ok
// res.body(), res.header("X-Example")
```

`test_request::arrival(std::chrono::steady_clock::time_point)` 为请求固定到达时刻，全局与路由级限流都按该时刻计算窗口，测试可以把请求精确放在窗口边界前后，无需 `sleep`；未固定时与运行中的服务器一样读取 steady clock。同一客户端的到达时刻不得倒退。
