# Web 服务端使用与 API 手册

本文档涵盖 `ruvia::web` 服务端的核心模块：应用配置与生命周期、控制器与路由声明、请求上下文 `context`、中间件、声明式模型 `RUVIA_MODEL` 与校验、静态文件服务、运维探针以及内存级测试框架。所有公开符号均位于 `namespace ruvia`。

---

## 1. 应用配置与生命周期

### 1.1 应用单例与启动主链（`"ruvia/web/app.h"`）

生产环境使用进程级单例 `ruvia::app()`（返回 `ruvia::application&`）完成全部配置，最后调用 `run()` 启动服务：

```cpp
#include "ruvia/web/app.h"

int main() {
    ruvia::app()
        .load_dotenv()
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .server({
            .worker_count_ = 4,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .drain_timeout_ = std::chrono::seconds(10),
        })
        .run();
}
```

#### `ruvia::application` 核心方法

| 方法签名 | 说明 |
| --- | --- |
| `application& server(server_config config)` | 配置业务 worker 数量、超时、连接数/请求数上限与内存池参数 |
| `application& listen(listen_config config)` | 注册监听端点（支持可多次调用监听多个地址/端口，重复端点会报错拒绝；TLS 证书在调用时立即加载校验） |
| `template <middleware M, typename... Args> application& use(Args&&... args)` | 注册全局中间件（每个 worker 独立构造一份实例） |
| `template <middleware M, typename... Args> application& use_at(middleware_scope_options options, Args&&... args)` | 注册路径前缀作用域中间件（如 `{.prefix_ = "/api"}`，按完整路径段匹配；前缀 `"/"` 抛出 `std::invalid_argument`，全局请用 `use`） |
| `template <typename S, typename... Args> application& use_worker_state(Args&&... args)` | 注册 per-worker 状态类型 `S`，在每个 worker 启动时独立构造一份实例 |
| `application& load_dotenv(dotenv_options options = {})` | 从当前目录 `.env`（或指定 `std::filesystem::path`）加载环境变量 |
| `const ruvia::env& env() const noexcept` | 获取进程环境变量只读视图 |
| `application& compression(compression_config config)` / `compression(std::nullptr_t)` | 启用或禁用响应内容协商压缩（gzip / deflate / br / zstd） |
| `application& cors(cors_config config)` / `cors(std::nullptr_t)` | 启用或禁用全局 CORS 策略与自动预检处理 |
| `application& document_root(document_root_config config)` / `document_root(std::nullptr_t)` | 启用或禁用后台定时刷新的静态文档根目录服务 |
| `application& trusted_proxies(trusted_proxy_config config)` / `trusted_proxies(std::nullptr_t)` | 配置可信反向代理 CIDR 列表与 `X-Forwarded-Proto` 信任策略 |
| `application& rate_limit(rate_limit_config config)` / `rate_limit(std::nullptr_t)` | 启用或禁用全局按客户端 IP 限流 |
| `application& blocking_pool(blocking_pool_options config)` / `blocking_pool(std::nullptr_t)` | 配置各 worker 事件循环借用的常驻阻塞卸载线程池（默认启用）；注册客户端也在其上执行 DNS 查询。禁用后 `run_blocking` 返回 `blocking_status::unavailable`，文件响应在响应头前以 `503` 回答（Windows 明文 TCP 上大于 16 KiB 的单范围仍走 `TransmitFile`），配置了文档根或主机不是 IP 字面量的注册客户端会使 `run()` 启动失败 |
| `application& http_client(http_client_registration_config config)` / `http_client(std::nullptr_t)` | 注册绑定到每个 worker 的出站 HTTP 客户端池（支持命名 `alias_`，默认 `"default"`） |
| `application& database(db_registration_config config)` / `database(std::nullptr_t)` | 注册 per-worker SQL 数据库连接池（需 `RUVIA_ENABLE_DATABASE`） |
| `application& redis(redis_registration_config config)` / `redis(std::nullptr_t)` | 注册 per-worker Redis 连接池（需 `RUVIA_ENABLE_REDIS`） |
| `application& on_error(http_error_handler handler)` | 注册全局错误处理器，重复注册抛出 `std::invalid_argument`（另有 `scoped_error_handler_options` 重载支持按路径前缀注册） |
| `application& on_not_found(http_not_found_handler handler)` | 注册全局 404 处理器，重复注册抛出 `std::invalid_argument`（另有 `scoped_not_found_handler_options` 重载支持按路径前缀注册） |
| `application& on_access(access_log_callback callback)` | 注册请求访问日志回调（`noexcept`） |
| `application& on_connection_failure(connection_failure_callback callback)` | 注册连接/握手层异常回调（`noexcept`） |
| `application& on_start(app_hook hook)` / `on_stop(app_hook hook)` | 注册启动后（accept 开始前）与停机后的生命周期钩子（在 `run()` 调用线程上执行） |
| `void run()` | 启动所有业务 worker 并阻塞，直到调用 `stop()` 或收到停止信号；排空完成后返回 |
| `void stop()` | 线程安全地请求停机：停止接入新连接 → 在 `drain_timeout_` 内排空已接纳请求（已完整收到请求头的 HTTP/1 请求仍会处理并以 `Connection: close` 应答）、发送 WebSocket `1001` 关闭帧并触发 `c.drain_token()` → 强制关闭超时连接 → 退役退出 |
| `http_server_stats http_stats() const` | 线程安全地汇总所有 worker 的连接与故障统计 |
| `blocking_pool_stats blocking_pool_stats() const` | 线程安全地读取阻塞线程池统计 |
| `std::vector<web_worker_handle> workers() const` | 获取全部业务 worker 的跨线程派发句柄 |
| `web_worker_handle worker_for(std::uint64_t key) const` / `worker_for(std::string_view key) const` | 按哈希键确定性选择单个业务 worker 句柄 |

应用回调（`app_hook`、访问/故障日志回调、错误/404 处理器）独占目标；空函数指针与 `nullptr` 一样构造空回调。清空、替换或析构时先将 owner 置空，再析构目标，因此目标的资源释放可以再次清空同一 owner。目标析构中的重入仅支持清空，不得向该 owner 安装新目标。

`application::on_access()` 与 `on_connection_failure()` 替换或清空配置时，先在锁内发布新回调及其内部视图，再解锁析构旧目标；`on_start(nullptr)` / `on_stop(nullptr)` 清空钩子列表、`on_error(nullptr)` / `on_not_found(nullptr)` 清空全局处理器时同样在解锁后析构被清空的目标。旧目标析构可以调用 `stop()` 或查询应用状态，不会因重新进入配置锁而死锁。运行期间仍拒绝修改这些回调。

---

### 1.2 服务器、监听、TLS、压缩与 CORS 配置（`"ruvia/web/server_config.h"`）

#### `ruvia::server_config`

| 字段名 | 类型与默认值 | 说明 |
| --- | --- | --- |
| `worker_count_` | `std::size_t`（默认 `max(1u, hardware_concurrency())`） | 业务 worker 线程数 |
| `process_signal_handlers_` | `process_signal_handler_policy`（默认 `external_owner`） | 设为 `install` 时自动安装 `SIGINT` / `SIGTERM` 信号处理以触发优雅停机 |
| `drain_timeout_` | `std::chrono::milliseconds`（默认 `10s`） | 停机排空阶段等待在途请求完成的最大时长。HTTP/2 连接按 RFC 9113 §6.8 两阶段排空：先发 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳客户端已在途的流；收到 `PING` ACK、往返等待达到 `min(1 秒, drain_timeout_ / 2)`（产品上限，不必等 ACK）或客户端此间新开超过 128 条流时，再发携带最后流 ID 的 `GOAWAY` 并拒绝之后的流。客户端已先发 `GOAWAY` 时直接发出最终 `GOAWAY`，不再走 `PING` 往返 |
| `worker_queue_capacity_` | `std::size_t`（默认 `1024`） | 每个 worker 跨线程任务投递队列容量 |
| `idle_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `75s`） | 空闲连接超时时间。HTTP/1 为等待下一个请求时的无活动时长，也是处理器既不读也不写时的兜底。HTTP/2 没有任何流时从最后一条流结束（或连接就绪）起计时，`PING`、`SETTINGS`、`WINDOW_UPDATE` 等不开启流的帧以及服务端自身的写出都不续期（与 Pingora `h2_idle_timeout` 一致），到期以 `GOAWAY` 排空；有在途流时与 HTTP/1 处理器兜底相同 |
| `connection_scan_interval_` | `std::chrono::milliseconds`（默认 `1s`） | 连接超时扫描周期。worker 没有任何连接且空闲修剪已执行后扫描停止，下一个连接到来时恢复 |
| `request_header_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 请求头读取超时；HTTP/2 连接收到客户端连接前言与 `SETTINGS` 之前、以及头块（`CONTINUATION`）未完成时同样受此约束 |
| `request_body_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 请求体单次读取超时。HTTP/2 按流计时：某条流超过该时长没有收到请求体 `DATA` 时，只以 `RST_STREAM(CANCEL)` 重置该流（不发送响应），连接与其他流不受影响，其他流或控制帧不续期 |
| `header_completion_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `30s`） | 请求头整体完成截止时间；HTTP/2 连接就绪（连接前言与 `SETTINGS`）同样受此截止期约束 |
| `body_completion_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `120s`） | 请求体整体完成截止时间。HTTP/2 按流计时：自该流请求头到达起计算，到期只重置该流 |
| `write_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 写入无进展超时：每次 socket 写入期间独立计时，只有写入进展续期（读到的数据不续期），不是整体截止期。覆盖 HTTP/1 响应、HTTP/1 升级连接（WebSocket 消息、控制帧与关闭帧，`CONNECT` 隧道数据，与是否启用 WebSocket 心跳无关）以及 HTTP/2 连接的全部帧输出（含 RFC 8441 WebSocket 与 `CONNECT` 隧道），与读方向当前处于空闲、读取请求体还是长连接阶段无关：对端停止读取时关闭连接。HTTP/2 流等待对端流控窗口（发送窗口为 0）也受此限制：连续等待超过该时长的流以 `RST_STREAM(CANCEL)` 重置，只影响该流（与 Pingora 以写超时约束 h2 流控等待一致）。阻塞池上的文件正文读取与 `c.file()` / `c.static_file()` 的打开等待同样以此为界，超时返回 `503 file_read_unavailable`（见 §6.1） |
| `max_connections_per_worker_` | `std::optional<std::size_t>`（默认 `1024`） | 单个 worker 允许的最大并发连接数。达到上限时，新连接接管“请求间空闲最久”的连接的槽位（等待下一个请求的 HTTP/1 keep-alive 连接，或没有任何流的 HTTP/2 连接），后者的 socket 立即关闭（与 nginx 在 `worker_connections` 用尽时复用 keep-alive 连接的机制一致）；被淘汰连接的会话退出前 `active_connections_` 可能短暂超过上限，但打开的 socket 数不超过。没有空闲连接时，新 socket 在 TLS 与协议识别之前直接关闭（不伪造响应），并计入 `connections_refused_` |
| `max_requests_per_connection_` | `std::optional<std::size_t>`（默认 `1000`） | 单条连接最大处理请求数；HTTP/2 达到上限时按上述两阶段 `GOAWAY` 排空，往返期间在途的流仍被处理（产品上限：最多再接纳 128 条，往返最长等待同上） |
| `max_buffered_body_bytes_` | `std::size_t`（默认 `default_max_buffered_body_bytes`） | 缓冲式路由默认允许的最大请求体字节数 |
| `max_inbound_buffer_bytes_per_worker_` | `std::size_t`（默认 `256 MiB`） | 单 worker 入站缓冲总字节上限 |
| `max_inbound_buffer_bytes_per_connection_` | `std::size_t`（默认 `64 MiB`） | 单连接入站缓冲字节上限 |
| `max_stream_body_bytes_` | `std::optional<std::size_t>`（默认无上限） | 流式路由请求体总字节上限 |
| `max_websocket_message_bytes_` | `std::size_t`（默认 `default_max_websocket_message_bytes`） | WebSocket 单条消息最大字节数 |
| `memory_pool_` | `memory_pool_config` | 请求 Arena 初始字节数（`request_initial_buffer_bytes_`）与 worker 保留块上限（`retained_block_bytes_`） |
| `http_client_result_budget_` | `http_client_result_budget_config` | 出站 HTTP 客户端保留字节预算 |

连接关闭遵循 [RFC 9112 §9.6/§9.8](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6)：

- 服务器主动结束、而对端仍可能发来数据时采用逗留关闭：HTTP/1 发送要求关闭连接的最终响应（`Connection: close`、请求被拒绝、排空中）后，以及 HTTP/2 以 `GOAWAY` 结束会话（排空完成、空闲超时、连接错误）后，TLS 先发送 `close_notify`，再半关闭 TCP 发送方向，读取并丢弃对端剩余输入，直到对端结束输入、累计 2 秒或丢弃 4 MiB 先发生为止，然后关闭 TCP。这样未读输入不会使 TCP 以 RST 结束连接而冲掉最后的响应。2 秒与 4 MiB 是固定的产品上限。
- 等待请求头时发生空闲超时、请求头超时或排空，以及对端正常结束输入时，TLS 发送 `close_notify` 后关闭。HTTP/2 对端结束输入（FIN 或 `close_notify`）时先发送最终 `GOAWAY(NO_ERROR)` 再关闭；该连接上仍在处理的流被取消，不再完成其响应。
- HTTP/2 连接超时（写入超时除外）先进入 `GOAWAY` 排空，但不再获得第二个完整超时周期：往返等待达到上述 `min(1 秒, drain_timeout_ / 2)` 时发出最终 `GOAWAY`，取消仍未结束的流，并以逗留关闭结束连接（TLS 发送 `close_notify`）。若届时仍有写入未完成（对端不读取），自超时起 2 秒（产品上限）后直接关闭连接，不发送关闭告警。HTTP/2 请求体超时与流控窗口等待超时属于单条流，只以 `RST_STREAM(CANCEL)` 重置该流，不结束连接。
- 写超时（HTTP/1 响应、WebSocket 与 `CONNECT` 隧道的写入，以及 HTTP/2 连接的写入）、HTTP/1 读取请求体超时与处理器无读写超过 `idle_timeout_`、传输错误以及 `drain_timeout_` 到期后的强制关闭立即关闭连接，不发送关闭告警（停滞的写入无法再携带 `GOAWAY` 或关闭告警）。逗留中的连接计入 `active_connections_`，受 `drain_timeout_` 约束。
- HTTP/1 路由未读取请求体就给出响应（未命中路由的 `404`/`405`/`OPTIONS`/文档根、处理器或中间件未读请求体即返回，以及限流的 `429`）时，若整个请求体（`Content-Length` 或完整的 `chunked` 帧）已随请求头一并收到，框架不经任何 I/O 直接消费它，连接保持 keep-alive；请求体仍有未到达部分时，响应后按上文逗留关闭。其他由框架生成的拒绝（`400`、`413`、`417`、`421` 等）一律关闭连接。

#### 优雅停机与 `c.drain_token()`

`app().stop()` 开始排空时，每个 worker 请求其 `drain_token`（`drain_timeout_ = 0` 跳过排空时，于 worker 停止时请求）。SSE、长轮询等会一直占住连接的响应应观察 `c.drain_token()` 并主动结束，使停机在 `drain_timeout_` 到期前完成，而不是到期后被强制切断。`c.stop_token()` 不受影响：`db()`、`redis()`、`http_client()` 与 `run_blocking()` 绑定的仍是 `stop_token()`，排空期间继续运行；内存内测试分发（`ruvia::testing`）得到的 `drain_token()` 永不触发。

```cpp
ruvia::task<void> events(ruvia::context& c) {
    auto writer = c.stream_sse();
    if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
        co_return;
    }
    const auto drain = c.drain_token();
    while (!drain.stop_requested()) {
        co_await writer.write({.data_ = "tick"});
        // 排空开始时立即醒来，随后结束流，连接随响应结束关闭。
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1), drain);
    }
}
```

#### `ruvia::listen_config` 与 `ruvia::tls_config`

```cpp
ruvia::app().listen({
    .address_ = "0.0.0.0",
    .http_ = 8080,
    .https_ = 8443,
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
        },
    },
    .auto_https_redirect_ = true,
});
```

- `http_` 与 `https_` 至少指定一个；同时指定且 `auto_https_redirect_ = true` 时，明文 `http_` 端口上的请求会自动重定向至 `https_` 端口。
- 服务端与客户端共用 `ruvia/web/tls_config.h` 中的 `tls_identity_config`（证书链、私钥与可选口令）。路径为 `std::filesystem::path`，不得为空或含 NUL，以 UTF-8 交给 OpenSSL 与数据库驱动，Windows 原生路径不经过 ANSI 代码页。
- `tls_.client_certificates_.requirement_` 支持 `tls_client_certificate_requirement::optional` 或 `required`（双向 TLS / mTLS）。先用 `const auto connection = c.conn();` 保存元数据，再通过 `connection.tls()->client_certificate_subject()` 读取 optional 主题：无值表示未出示证书，有值空串表示已验证但 Subject DN 为空的证书（[RFC 5280 §4.1.2.6](https://www.rfc-editor.org/rfc/rfc5280.html#section-4.1.2.6)）；不能用文本是否为空判断证书存在性。主题提取失败终止该连接并报告，不降级为未认证身份。
- `sni_` 按 SNI 主机名（ASCII 不区分大小写）选择证书；客户端未发送 server_name 或名称不匹配任何 `sni_` 项时使用主证书完成握手。这是为单证书与通配符证书部署做出的产品选择，不发送 RFC 9325 §3.7 建议的 `unrecognized_name` 告警。
- 协议策略固定：仅 TLS 1.2 / 1.3；TLS 1.2 只协商 ECDHE + AEAD 密码套件（AES-128-GCM 优先），由服务器偏好决定；拒绝重协商；ALPN 依次选择 `h2`、`http/1.1`、`http/1.0`，三者都未提供时以 `no_application_protocol` 告警失败。
- `tls_.max_outgoing_records_`（默认 `2^23`，范围 `1..2^23`）限制每条连接发出的全部 TLS 记录，计入握手与 KeyUpdate 且更新密钥不重置；到达上限时终止连接，以低于 RFC 9846 §5.5 的 AES-GCM 单密钥用量上限。
- 会话可在同一监听器的任意 worker 上恢复；TLS 1.2 与 TLS 1.3 会话都只在与建立时相同的 SNI 名称（ASCII 不区分大小写）下恢复（RFC 6066 §3；RFC 9846 §4.6.1 允许拒绝不同 SNI 的 PSK），否则完整握手。会话票据密钥每 2 小时（会话有效期）轮换一次，旧密钥再保留一个周期用于解密，随后销毁（RFC 9325 §3.4）；某个 worker 复制密钥失败时约 5 秒后重试，而不是等到下一次轮换。
- 目标源校验（RFC 9110 §7.4 / §15.5.20）：目标为 `https` 资源的请求（HTTPS 连接上的 origin-form / asterisk-form，或显式 `https` 绝对形式目标 / HTTP/2 `:scheme https`），只有当本连接握手出示的证书在 subjectAltName 的 DNS 或 IP 条目中覆盖目标主机（`:authority`，否则 `Host`；通配符只匹配完整的最左标签，不使用主题 CN）时才处理，否则回 `421 Misdirected Request`；HTTP/2 只拒绝该流，连接保留。证书匹配而非 SNI 匹配，因此证书覆盖的其他名称仍可复用连接（RFC 9113 §9.1.1）。明文监听器收到 `https` 目标同样回 `421`。`http` 目标、CONNECT 隧道目标与空 authority 的请求不受影响；来自 `trusted_proxy_config` 网段的对端豁免两项检查。每个连接缓存最近 4 个主机的判定，不对每个请求重复扫描证书。

#### 压缩、CORS 与可信代理配置

- `compression_config`：`min_bytes_`（默认 `1024`）、`sync_bytes_`（默认 `64 KiB`）、`max_bytes_`（默认 `64 MiB`）。`min_bytes_` 只是“压缩是否划算”的阈值：请求以 `identity;q=0` 等方式拒绝 identity 时，小于该值的缓冲响应同样压缩（且不要求结果小于原文），而不是回 `406`。响应已带 `Content-Encoding`、`Content-Range`、`Cache-Control: no-transform` 或完整性字段 `Content-Digest` / `Repr-Digest` / `Content-MD5` 时保持原表示不压缩（RFC 9530：摘要随内容编码而变）；应用自行设置的 `Content-Encoding` 编码栈（含自定义编码 token）仍按请求的 `Accept-Encoding` 逐项校验，任一层编码被拒绝时回 `406`。仅文本类媒体类型参与压缩（`text/*`、JSON / XML 及 `+json` / `+xml` 后缀、JavaScript / ECMAScript、`application/wasm`、`application/x-ndjson`、SVG、图标及未压缩字体 `font/ttf` / `font/otf` / `application/vnd.ms-fontobject`），缺少 `Content-Type`、其他类型或类型名含 `zip` 时保持 identity；静态文件预压缩使用同一白名单。应用自带 `Content-Encoding` 的响应不再编码，但其 `200` / `406` 结果取决于 `Accept-Encoding`，因此同样加 `Vary: Accept-Encoding`。框架编码响应时弱化强 `ETag`、删除不是带引号 entity-tag 的 `ETag`，并删除 `Accept-Ranges`（identity 字节范围不适用于编码后的表示）。`sync_bytes_` 是事件循环内联编码上限：更大的缓冲体在启用阻塞池时卸载编码，未启用时保持 identity（拒绝 identity 时回 `406`），不在事件循环上编码。流式响应的头部声明 `Content-Length` 时按与缓冲体相同的 `min_bytes_` / `max_bytes_` 规则决定是否编码；每次应用写入按 16 KiB 输入分片编码，仅在该次写入末尾 flush 一次（HTTP/1 与 HTTP/2 一致）；流式 br 使用 512 KiB 窗口（lgwin 19，同 Pingora），zstd 使用压缩级别默认窗口（RFC 9659 的 8 MiB 只作上限），编码失败的流式编码器直接释放、不回池复用。`GET` 路由（含 `HEAD` 回退）、0 字节文件与重定向响应在 `HEAD` 下与 `GET` 一致发送 `Content-Length: 0`（拒绝 identity 时同样编码 0 字节体）；显式 `RUVIA_HEAD` 路由返回空非文件响应体时不压缩该空体：按其声明的 `Content-Length`（支持逗号分隔的相同值；无声明时仅在拒绝 identity 时）决定是否加 `Content-Encoding`，加上时去掉 `Content-Length`（编码后长度未知，RFC 9110 §8.6 允许省略），未声明 `Content-Length` 的显式空 `HEAD` 响应省略 `Content-Length`。请求体按 `Content-Encoding` 透明解码时，前 64 KiB 解码输出在事件循环内完成，更大的表示改在阻塞池上解码；无阻塞池或池拒绝时仍在事件循环内解码（请求必须得到内容），两者都受解码上限约束。
- `cors_config`：
  - `origin_`：`cors_origin_config{.mode_ = cors_origin_mode::any | exact | credentialed_exact, .value_ = "..."}`
  - `request_headers_`：`cors_request_headers_config{.mode_ = cors_request_headers_mode::reflect | fixed, .names_ = {...}}`
  - `expose_headers_`：`std::vector<std::string>`
  - `max_age_`：`std::optional<std::chrono::seconds>`
- `trusted_proxy_config`：`cidrs_`（受信任代理网段列表，如 `{"127.0.0.1/32", "10.0.0.0/8"}`）、`trust_x_forwarded_proto_`（是否信任代理传入的 `X-Forwarded-Proto`）。

---

### 1.3 `.env` 与环境变量（`"ruvia/web/dotenv.h"`）

- `dotenv_options`：
  - `existing_variables_`：`dotenv_existing_variable_policy::preserve`（默认，保留已有环境变量）或 `override`。
  - `missing_file_`：`dotenv_missing_file_policy::ignore`（默认）或 `require`。
- 文件格式（整个文件先完整解析，任一行出错则抛出 `std::invalid_argument`（`invalid dotenv entry in <path>:<line>: <原因>`），不应用任何变量）：
  - 空行与以 `#` 开头的行忽略；首行 UTF-8 BOM 被剥离；可选 `export ` 前缀。
  - 变量名须匹配 `[A-Za-z_][A-Za-z0-9_]*`。同一文件内重复赋值同一变量名视为配置错误，在第二次赋值所在行报 `duplicate variable name`；`existing_variables_` 策略只裁决与此前加载结果的冲突，不影响该检查。
  - 未加引号的值去除首尾空白，遇到行首或空白之后的 `#` 时截断为注释。
  - 单引号值按字面保留，不处理任何转义。
  - 双引号值仅定义 `\n`、`\r`、`\t`、`\"`、`\\` 五个转义；与 POSIX sh 双引号一致，反斜杠后跟其他任意字符时反斜杠与该字符一起按字面保留（`"\q"` 得到 `\q`，`"C:\Users"` 保持原样）。
  - 引号闭合后只允许空白或 `#` 注释；不支持多行值与变量展开。
- `ruvia::env`（通过 `app().env()` 或 `c.env()` 访问）：
  - `std::optional<std::string_view> get(std::string_view name) const& noexcept`
  - `template <typename T> std::optional<T> get(std::string_view name) const&`（支持 `std::string_view`、`bool`、整型、浮点型；变量存在但格式非法时抛出 `std::invalid_argument`）

---

### 1.4 错误处理与异常（`"ruvia/web/error.h"`, `"ruvia/web/app.h"`）

- **抛出或构造业务错误**：
  - 在路由或中间件中可抛出 `ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "bad_input", .message_ = "..."})`，或直接 `co_return c.error({...});`。`http_error` 是唯一的拥有型应用错误：status / code / message / status_text 以及 `.validation_issues_` 全部深拷贝持有，`info()` 返回携带 issues 的 `http_error_info`。
- **全局与前缀级错误/404 处理器**：
  - `on_error` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c, ruvia::http_error_info error)`。处理器内再次抛出 `http_error`（含校验 issues）会按该错误生成默认响应，不会变成 500；协议层错误的 `error.code()` 为具体机器码（如 `missing_host`、`conflicting_content_length`），而非按状态派生的 `bad_request`。内置中间件的拒绝同样经过 `on_error`：全局与路由级限流为 `429` / `too_many_requests`（处理器返回的响应之后会追加 `Retry-After` 与 `X-RateLimit-Limit` / `X-RateLimit-Remaining` / `X-RateLimit-Reset`），CSRF 校验失败为 `403` / `csrf_token_mismatch`，会话冲突为 `409` / `session_conflict`。
  - `on_not_found` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c)`。
  - 支持通过 `{.prefix_ = "/api", .handler_ = &api_error}` 注册前缀作用域处理器，按最长完整路径段匹配（例如 `"/api"` 匹配 `"/api/users"` 但不匹配 `"/apix"`）。等价前缀重复注册或前缀 `"/"`（全局只用无前缀形式）抛出 `std::invalid_argument`。
- **异常文本格式化**：
  - `std::string ruvia::exception_text(const std::exception_ptr& exception)`：将任意捕获的异常安全格式化为 UTF-8 描述文本。

---

### 1.5 Worker 状态与跨 Worker 任务投递（`"ruvia/web/web_worker.h"`）

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

`ruvia::web_worker_context` 提供与控制器一致的 worker 级能力访问：`worker()`、`pool()`、`stop_token()`、`worker_state<T>()`、`run_blocking(...)`、`db()`、`redis()`、`http_client()`。

---

## 2. 控制器与路由声明（`"ruvia/web/controller.h"`）

控制器是不继承任何框架基类、可默认构造的 `final` 类。通过路由宏在类定义内声明路由，并在 `main` 启动前自动完成静态注册与去重封存。

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
| 缓冲或流式响应路由 | `RUVIA_GET(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 自动提供 `HEAD` 回退并将其列入 `Allow`；仍运行同一处理器与中间件，`c.req().method()` 为 `"HEAD"`。缓冲响应省略正文；流式/SSE 必须先 `co_await writer.commit()`，返回 `completed` 时停止正文生产（HEAD/304 为正常完成，`aborted()` 为 false，不取消 token），返回 `body` 才可写入。请求体消费/排空与响应完成独立；流式/SSE 在请求体完整消费后监测客户端断开并停止 `c.stop_token()`，pipeline 后续请求之后的写半关闭不取消当前响应（见 [streaming-websocket-client.md](streaming-websocket-client.md#客户端断开即取消请求)）。WebSocket 路由不提供 HEAD 回退 |
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

WebSocket 处理器通过 `c.websocket()` 获取连接（`"ruvia/web/websocket.h"`）。允许一个已启动的 `read()` 与一个写操作并行；第二个同方向操作在前一个完成前启动会抛出 `std::logic_error`。单个写者连续发送时，已等待的读者在完整消息边界获得协议轮询机会，并在下一条应用消息写入前处理已收到的 Ping/Close 及自动 Pong/Close 输出；不会中断正在发送的完整消息，也不会隐式创建后台读任务（RFC 6455 §5.5.1–5.5.2）。`send(websocket_message&&)` 借用消息的读取存储，按公开头约定额外占用读 lane，不能与另一个 `read()` 重叠。

### 2.3 路径模式与反向链接生成（`url_for`）

- **静态段**：`/api/hello`
- **命名参数段**：`/api/users/:id`（通过 `c.req().param("id")` 读取）
- **尾部通配符**：`/api/files/*`（匹配余下路径，通过 `c.req().param("*")` 读取）
- **路径等价**：请求路径与注册模式按同一规范形式比较：以字面 `/` 切分路径段，段内按百分号解码后的字节比较。因此 `/users/%61dmin`、`/users/adm%69n` 与 `/users/admin` 命中同一路由（十六进制大小写不影响匹配，如 `caf%c3%a9` 与 `caf%C3%A9`），而 `%2F` 始终留在所在段内，不作为路径分隔符。该形式与路径参数的解码结果、`use_at` 前缀作用域中间件以及前缀错误/404 处理器的匹配规则一致；比 RFC 3986 的等价规则更宽，`%3A` 与 `:` 这类保留字符的编码与字面形式也视为同一路径。同一方法下仅拼写不同的两个模式（如 `/a` 与 `/%61`）属于重复注册，启动时抛出 `std::invalid_argument`。重复判定逐段进行，参数段与通配符段只与同类段相同：以 `:` 开头的字面段或字面 `*` 只能编码书写（`%3Aid`、`%2A`），与同位置的 `:id` 参数或 `*` 通配符是不同路由，可同时注册。
- **反向路由生成**：`c.url_for("/api/users/:id", {"42"})` 返回经过百分号编码的 `std::pmr::string`（若传入未注册的路由模式则直接抛异常，防止生成死链）。

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
| `c.req().negotiate(type, supported)` | 在候选列表中按 `Accept*` 权重协商最佳匹配（单候选时 `has_value()` 即表示客户端接受该值） |
| `co_await c.req().text()` / `co_await c.req().bytes()` | 读取完整缓冲请求体视图（`std::string_view` / `std::span<const std::byte>`） |
| `co_await c.req().discard_body()` | 丢弃剩余请求体字节 |
| `co_await c.req().multipart()` | 解析缓冲式 `multipart/form-data`，返回 `std::pmr::vector<multipart_part>` |
| `c.req().validated<T>()` | 获取由校验中间件（`json_body<T>` 等）已完成解析和校验的模型常量引用 |
| `co_await c.req().json_if<T>()` / `co_await c.req().form_if<T>()` | 按需结构化解析并校验 JSON 或 Form 为 `std::optional<T>`（仅当 `Content-Type` 不匹配时返回 `std::nullopt`） |
| `c.conn().remote()` / `c.conn().client()` | 直连对端地址端口（`.address()` / `.port()`）与经过 `trusted_proxies` 解析后的真实客户端地址 |
| `c.conn().via_trusted_proxy()` / `c.conn().scheme()` / `c.conn().tls()` | 是否经过可信代理、连接有效 scheme（`http` / `https`）及 TLS 传输信息指针 |

HTTP/1 定长正文已完整预读且无需内容解码时，缓冲读取直接借用连接缓冲并同步完成该读取步骤，不创建额外的正文 I/O 协程；返回的视图仍只在当前请求内有效。

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

    // 2. 使用请求 Arena 构造 PMR 字符串并移动返回：
    std::pmr::string body(c.allocator<char>());
    body.append("created\n");
    co_return c.text(std::move(body));
}
```

| `ruvia::context` 响应方法 | 说明 |
| --- | --- |
| `c.draft()` / `c.edit_response()` / `c.policy()` | 返回显式元数据 editor：分别编辑下一次构造的草稿、已选定的缓冲响应、跨替换的请求级响应策略 |
| `editor.status(code)` / `header(name, value, options = {})` / `remove_header(name)` / `priority(fields)` | 设置 `200..599` 状态、响应头或 RFC 9218 优先级；头默认替换，可指定 `http_response_header_mode::append` |
| `editor.set_cookie(options)` / `set_signed_cookie(options)` / `delete_cookie(options)` | 设置普通/HMAC 签名 Cookie 或用过去的 `Expires` 使其过期；`max_age_ = 0s` 同样使用过去的 `Expires`，并覆盖同时设置的 `expires_` |
| `http_response text(...)` / `html(...)` / `body(...)` | 构造 `text/plain; charset=utf-8`、`text/html; charset=utf-8` 或原始响应体（支持字符串字面量、`std::string_view`、`std::pmr::string&&`、`std::span<const std::byte>`、`ruvia::static_text`） |
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
    std::chrono::milliseconds(500),
    [owned = std::move(input_copy)]() {
        return compute_expensive_digest(owned);
    });

if (!result.completed()) {
    co_return c.error({
        .status_ = ruvia::http_status::service_unavailable,
        .code_ = "blocking_rejected",
        .message_ = ruvia::describe_blocking_status(result.status()),
    });
}
auto digest = std::move(result).value();
```

---

## 4. 中间件与内置请求策略

### 4.1 自定义中间件（`"ruvia/web/middleware.h"`）

任何声明为 `final`、不继承基类，且提供 `ruvia::task<void> handle(ruvia::context&, ruvia::next&)` 的类均满足 `ruvia::middleware` 概念；中间件自行产生响应时在返回前调用 `c.respond(...)`：

```cpp
class timing_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        // 跨替换保留的策略在 next 前或后声明含义相同。
        c.policy().header("X-Policy", "retained");
        co_await next_value();
        // 仅对当前缓冲响应后处理；不能修改已经提交的流式响应。
        c.edit_response().header("X-Processed", "true");
    }
};
```

#### 请求级类型化状态传递（`bind_request_state` / `request_state`）

中间件向后续路由传递鉴权身份或上下文数据时，使用 `c.bind_request_state(const T&)`。绑定對象在中间件协程帧上持有并跨越 `co_await next_value()`：

```cpp
struct current_user {
    std::string_view id_;
};

class auth_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
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
| `ruvia::body_limit<MaxBytes>` (`"ruvia/web/controller.h"`) | 仅用于控制器或单条路由（如 `RUVIA_POST("/upload", upload, ruvia::body_limit<65536>)`），收紧该路由允许的最大请求体字节数 |
| `ruvia::rate_limit<MaxReqs, WindowMs>` (`"ruvia/web/rate_limit.h"`) | 控制器或路由级滑动窗口限流中间件（按 `c.conn().client().address()` 在当前 worker 内计数）；全局限流使用 `app().rate_limit({...})`；两者的拒绝都经 `on_error` 应答为 `429` |
| `ruvia::security_headers_middleware` (`"ruvia/web/security_headers.h"`) | 自动注入 `X-Content-Type-Options`、`X-Frame-Options`、`Strict-Transport-Security`、`Content-Security-Policy`、`Referrer-Policy`、`Permissions-Policy` 等安全响应头（对 404 响应同样生效） |
| `ruvia::csrf_protection` (`"ruvia/web/csrf.h"`) | 基于 Double-Submit Cookie（默认 Cookie `"XSRF-TOKEN"` 与请求头 `"X-XSRF-TOKEN"`）的无状态 CSRF 防护中间件；校验失败抛出 `http_error`（`403` / `csrf_token_mismatch`），经 `on_error` 应答 |
| `ruvia::session_middleware` (`"ruvia/web/session.h"`, 需 `RUVIA_ENABLE_REDIS`) | 基于 Redis 的服务端会话中间件；路由中通过 `c.session().data()`、`c.session().set(...)`、`c.session().regenerate()`、`c.session().clear()` 管理会话 |
| `ruvia::make_health_response` / `make_readiness_response` (`"ruvia/web/context.h"`) | 生成标准健康检查（`200 OK`）与就绪探针响应（`readiness_state::ready` 返回 `200`，`unavailable` 返回 `503`） |

两级限流使用相同的地址身份规则：IPv4-mapped IPv6 与对应 IPv4 共用配额；不带 scope 的 IPv6 按 `/64` 网络前缀计数；带 scope 的 IPv6 按完整 128 位地址及数值 scope 计数，压缩、展开和大小写形式不会产生独立配额。不同 scope 保持隔离。地址仍由可信代理配置决定，限流计数仍归当前 worker。

两级限流按 key 计算滑动窗口估计值（与 pingora-limits 的 `Rate`、Cloudflare 的做法相同）：每个 key 的窗口从它的首个请求开始、按 `window_` 首尾相接地滚动，分别记录当前窗口与上一窗口已放行的请求数；请求在 `上一窗口计数 × 当前窗口尚未经过的比例 + 当前窗口计数 < max_requests_` 时放行并计入当前窗口，被拒绝的请求不计数。窗口滚动时当前计数转为上一窗口计数；距当前窗口起点已过两个或更多窗口时两者都清零。上一窗口的计数随时间线性淡出而不是在边界处一次清空，因此不再允许在一个窗口末尾用完配额、紧接着在下一窗口开头再用完一整份配额（此前的固定窗口在边界附近最多放行约两倍配额）；相邻请求间隔（按毫秒计）都大于 `window_ / max_requests_` 的流量始终放行，恰好以该间隔对齐窗口起点到达的请求会因估计值等于上限而被拒绝。拒绝时 `X-RateLimit-Remaining` 为 `0`；`Retry-After` 为估计值再次放行的最早时刻（向上取整到秒，按它等待不会早于真实放行时刻）；`X-RateLimit-Reset` 为估计值降到零、整份配额恢复的时刻（向上取整到秒，不小于 `Retry-After`）。表满时按 `overflow_policy_ = deny` 拒绝的请求两者均为 `1`。估计值的比较使用 128 位精确乘积，延迟按无符号毫秒计算后再向上取整到秒，因此即使 `window_` 接近 `std::chrono::milliseconds::max()`、`max_requests_` 取到 `std::size_t` 上限也不会溢出，最长约两个窗口的 `X-RateLimit-Reset` 也不会被截短。

每个 worker 预分配 `capacity_per_worker_`（默认 `8192`）个滑动窗口计数槽。key 的起始槽位由每个 worker 启动时随机抽取密钥的 SipHash-1-3 计算，攻击者无法离线算出哪些 key 落入同一探测窗口；一次查找或插入只探测该起始槽位起的连续槽位（`min(capacity_per_worker_, 32)` 个），单次请求的开销与表容量无关；这些槽位全被其他 key 的未过期计数占用时，该请求按规则的 `overflow_policy_` 处理（默认 `deny`），即使表的其他部分仍有空位。槽位在其 key 当前窗口起点之后满两个窗口才过期、可被其他 key 复用；在此之前上一窗口计数仍参与估计。容量需要为活跃客户端数留出余量。

会话仅接受固定 48 位小写十六进制 ID，其他长度或字符按不存在的会话处理，不读取或修改其拼接的 Redis key。已有合法 ID 与 `key_prefix_ + ID` 存储格式保持不变；旧的非标准 ID 不再被识别。固定长度防止不同前缀通过变长 ID 穿透命名空间；使用同一 Redis 数据库和相同前缀则共享会话命名空间，隔离责任由配置方承担。

CSRF 的 `header_name_` 必须是前端脚本主动提交 token 的自定义字段，例如 `X-App-CSRF` 或 `CSRF-Token`。构造时拒绝 CORS-safelisted、浏览器保留及已知自动生成/代理字段，如 `Accept`、`Content-Type`、`Origin`、`Sec-*`、`X-Forwarded-*`；旧配置需更换字段并同步前端。这里依赖 [Fetch 的自定义请求头与预检边界](https://fetch.spec.whatwg.org/#cors-safelisted-request-header)，不是给任意等值字段赋予认证意义。部署方须保证代理不注入所选 token 字段，且仅向可信 origin 授予携带凭据的 CORS 权限；该中间件不替代用户认证，也不隔离已经被应用信任的跨源脚本。

---

## 5. 声明式模型 `RUVIA_MODEL`、序列化与参数校验

头文件：`"ruvia/web/model.h"`、`"ruvia/web/validation.h"`。

### 5.1 模型定义与字段类型

使用 `RUVIA_MODEL(ModelName, ...)` 定义编译期反射模型。模型专用的字段类型位于 `namespace ruvia`：

- **标量类型**：`ruvia::bool_value`、`ruvia::int8`、`ruvia::uint8`、`ruvia::int16`、`ruvia::uint16`、`ruvia::int32`、`ruvia::uint32`、`ruvia::int64`、`ruvia::uint64`、`ruvia::float_value`、`ruvia::double_value`
- **字符串与字节**：`ruvia::string`（UTF-8 字符串）、`ruvia::bytes`（JSON 中以 Base64 编码的二进制数据）
- **复合与容器类型**：嵌套的另一个 `RUVIA_MODEL` 类型、`ruvia::array<T>`（连续数组）、`ruvia::boxed_array<T>`（支持前向声明的自引用递归模型数组）

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
#include "ruvia/web/validation.h"

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
  - `std::pmr::string ruvia::to_json(const T& value, model_options options = {})`（无短转义形式的控制字符输出为小写 `\u00xx`）
  - `T ruvia::from_form<T>(std::string_view form, model_parse_options options = {})`（解析失败抛 `std::invalid_argument`，字段校验失败抛携带 issues 的 `ruvia::http_error`）
- **零拷贝动态 JSON 视图（`"ruvia/web/model.h"`）**：
  - `ruvia::json_object::parse(std::string_view body, model_parse_options options = {})` / `ruvia::json_value::parse(std::string_view body, model_parse_options options = {})`（直接返回值，语法非法抛 `std::invalid_argument`）
  - 支持 `obj.get<ruvia::string>("key")`、`obj.get<ruvia::int64>("count")`、`obj.for_each_field([](std::string_view key, const ruvia::json_value& val) { ... })`、`val.for_each_element([](const ruvia::json_value& item) { ... })`。`get<T>()` / `get<T>("key")` 的结果可能借用 token 字节，因此只能在左值 token 上调用，右值 token 上调用编译失败。
  - `get<T>()` 把整个 token 转换为 `T`，返回 `std::optional<T>`。`get<T>("key")` 是成员查找，返回 `ruvia::json_field_lookup<T>`：`status()` 为 `ruvia::unique_lookup_status::absent`（无此成员，非对象 token 一律为此）、`found`（恰好一个成员名按转义解码后等于 `key`）或 `repeated`（同名成员出现两次及以上；RFC 8259 §4 未定义重复名语义，因此不选取任何一次出现，与模型类型化解析拒绝重复字段一致，扫描在第二次出现处立即停止）。`value()` 仅在 `found` 且成员值能转换为 `T`（模型类型还须通过字段校验）时有值；`found` 而 `value()` 为空表示成员存在但不是合法的 `T`。左值查找结果的 `value()` 返回引用，右值查找结果的 `value()` 按值移出 `std::optional<T>`（如 `if (auto port = obj.get<ruvia::uint32>("port").value())`），值同样不能比被查询的 token 活得久。
  - 移动构造保留 token 的借用/拥有方式；移动赋值保留目标 PMR 并拥有化结果。成功移动或右值字段转移后，源 `json_value` 恢复为 `null`，源 `json_object` 恢复为 `{}`，源 PMR 不变，仍可查询、赋值或作为模型成员序列化；重置本身不分配。自移动赋值保持原值；跨资源拥有化失败时两端原值均保留。移动后重新取得借用视图，不继续使用旧视图。
- **命令式校验器 `ruvia::validator`（`"ruvia/web/validation.h"`）**：
  - 提供 `.required(opt, field)`、`.min_length(opt, field, n)`、`.max_length(opt, field, n)`、`.range(opt, field, low, high)`、`.one_of(opt, field, list)`、`.add(field, code, message)`，最后调用 `.throw_if_invalid(validation_failure_options = {})` 抛出携带 issues 的 `ruvia::http_error`（默认 400 / `validation_failed`）。

---

## 6. 静态文件服务与内存级测试

### 6.1 静态文件服务（`"ruvia/web/static_files.h"`）

Ruvia 提供三种静态文件服务方式：

1. **单文件响应**：在路由中调用 `co_return co_await c.file({.path_ = "/path/to/file.pdf", .content_type_ = "application/pdf"})`。
2. **不可变目录快照 `ruvia::static_root`**：启动前构造 `ruvia::static_root assets("public/", {.cache_control_ = "public, max-age=3600"})`，在路由中调用 `co_return co_await c.static_file(assets, {.relative_path_ = c.req().param("*").value_or("")})`。
3. **托管自动刷新的 `app().document_root({...})`**：后台按 `refresh_interval_` 刷新目录索引，并支持 `precompress_gzip_` / `precompress_brotli_` / `precompress_zstd_` 自动预压缩热文件。刷新是事务性的：某一轮重建或采用失败时继续服务上一份完整快照，`http_stats().document_root_refresh_failures_` 计数加一，失败原因经限流的未处理故障通道（`ruvia: document root refresh failed: ...`，写入 stderr）报告。各 worker 只在发布代次变化时才取发布者的锁采用新快照。

三者均内置路径穿越防护、点文件策略（`static_dotfile_policy::deny` 默认拒绝隐藏文件）、`ETag` / `Last-Modified` 条件请求（`304 Not Modified`）与 `Range` 分段响应（`206 Partial Content`）。`c.static_file()` 的 `relative_path_` 是已解码的文件路径，`/` 与 `\` 均为段分隔符；含 `.` 或 `..` 段、空的中间段、绝对路径或 NUL 时一律返回 `403`，不做解析或夹取。命名参数（如 `/files/:name`）可在单个段内解码出 `%2F` / `%5C`，因此 `sub%2F..%2Fsecret` 这类拼写同样被拒绝。

多个范围的请求返回 `multipart/byteranges`，各部分只带 `Content-Type` 与 `Content-Range`；选中预压缩变体（带 `Content-Encoding`）时多范围请求返回完整的 `200` 表示（单个范围仍是带 `Content-Encoding` 的单部分 `206`）。`If-Range` 匹配后的 `206` 只带必需字段（`ETag`、`Cache-Control`、`Vary`、`Content-Range` 或 multipart `Content-Type` 等）及描述所发字节的 `Content-Encoding`，不再重复 `Content-Type`、`Last-Modified`（RFC 9110 §15.3.7）。

非 resident 文件响应会验证索引或构造时记录的文件身份，零字节文件也不例外：空文件在索引后被改写或删除，不能以旧 `Content-Length: 0` 成功提交。`c.file()` 与 `c.static_file()` 返回 `task<http_response>`：文件只打开一次，打开（`c.file()` 的打开与 `fstat` 快照、`static_root` 在根目录之下的按条目打开）在 worker 的阻塞池上执行，任何平台上 worker 都不做文件系统调用；任务完成时状态（`200/206/304/404/403/412/416/500/503`）、`ETag`、`Content-Length`、Range 处理与打开的句柄均已确定，中间件与处理器可直接检查。不发送文件字节的响应（`HEAD`、`304`、`412`、`416`）在池上打开时同时校验索引身份，文件已变化则返回 `500 static_file_changed`。响应正文由同一句柄（读取时为其复制句柄）提供，传输层不再按路径重新解析。两者在返回任务前即完成输入拥有化：路径、`relative_path_` 与 `content_type_` 由调用本身复制到请求存储（`static_file()` 同时完成路径规范化、索引查找与表示选择），保存任务后释放这些来源再 `co_await` 也安全；无法表示文件的路径（`404`）、被拒绝的相对路径（`403`）、未索引的名称（`404`）与不可接受的编码（`406`）在调用时即抛出。只有 `static_file()` 的 `root` 仍是借用，必须活到返回的任务完成；文档根请求的内部分发走同一条池上打开路径，并在等待期间持有快照租约。

文件正文在所有平台、所有传输上走同一条读取链：打开与阻塞读取在阻塞池上执行，每轮最多预读 64 KiB，worker 不做同步文件读取；唯一例外是 Windows 明文 TCP 上大于 16 KiB 的单个范围，经内核 `TransmitFile` 按 64 KiB 分片发送，每片之后重新校验句柄身份，最后一个字节在校验通过后才发出。处理器内的打开与每次文件读取的等待都以 `server_config::write_timeout_` 与请求的 `stop_token()` 为界，超时后放弃等待（打开或读取在池线程上自行结束并释放句柄与缓冲），请求与连接不再被卡住的磁盘占用。

响应头之前读取并校验首个文件范围（multipart 正文也一样，先于首个边界）。提交前失败时连接或流仍得到错误响应：文件被删除、替换或改写、读取出错返回 `500 file_read_failed`；读取未完成（阻塞池队列满、未配置阻塞池、超过 `write_timeout_`）返回 `503 file_read_unavailable`；HTTP/1 随后关闭连接，HTTP/2 只结束该流（响应头已发出后则以 `RST_STREAM(INTERNAL_ERROR)` 结束）。`c.file()` / `c.static_file()` 的打开未完成时同样以 `503 file_read_unavailable` 回答：`co_await` 抛出该 `http_error`，经错误处理器得到响应。真实故障（I/O 错误、句柄耗尽、打开或读取超时、缺少阻塞池）经连接故障通道报告（`on_connection_failure()` 回调；未注册时走限流的未处理故障报告）；文件变化、队列满（计入 `blocking_pool_stats().rejected_`）与停机不报告。提交后失败按当前 HTTP 连接或流终止。resident 和已经复制到响应内的字节仍属于独立快照，不因后续磁盘变化失效。

HTTP/2 文件正文在 `HEADERS` 之前只打开并校验、不预读；之后每块只在对端授予发送窗口后读取，至多读取窗口大小，每个连接同时最多 4 个流占用阻塞池读取或持有读取缓冲，等待窗口的流不持有缓冲。

Linux 上 `static_root` 与文档根的索引和每个请求都在保留的根目录句柄之下打开条目（`openat2` 的 `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`；内核不支持时逐段 `O_NOFOLLOW` 打开），任何一段路径被换成符号链接都会拒绝，不会索引或服务根目录之外的文件。Windows 按规范化后的绝对路径打开，以索引记录的卷序列号与 FileId 身份校验作为防线。

### 6.2 内存内无 Socket 测试（`"ruvia/web/testing.h"`）

`ruvia::test_app` 在单线程内存内直接驱动已注册的控制器路由、模型校验、中间件、Worker 状态和错误处理器，无需绑定网络端口：

```cpp
#include "ruvia/web/testing.h"

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
