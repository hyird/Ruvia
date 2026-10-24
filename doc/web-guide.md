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
| `template <middleware M, typename... Args> application& use_at(middleware_scope_options options, Args&&... args)` | 注册路径前缀作用域中间件（如 `{.prefix_ = "/api"}`，按完整路径段匹配） |
| `template <typename S, typename... Args> application& use_worker_state(Args&&... args)` | 注册 per-worker 状态类型 `S`，在每个 worker 启动时独立构造一份实例 |
| `application& load_dotenv(dotenv_options options = {})` | 从当前目录 `.env`（或指定 `std::filesystem::path`）加载环境变量 |
| `const ruvia::env& env() const noexcept` | 获取进程环境变量只读视图 |
| `application& compression(compression_config config)` / `compression(std::nullptr_t)` | 启用或禁用响应内容协商压缩（gzip / deflate / br / zstd） |
| `application& cors(cors_config config)` / `cors(std::nullptr_t)` | 启用或禁用全局 CORS 策略与自动预检处理 |
| `application& document_root(document_root_config config)` / `document_root(std::nullptr_t)` | 启用或禁用后台定时刷新的静态文档根目录服务 |
| `application& trusted_proxies(trusted_proxy_config config)` / `trusted_proxies(std::nullptr_t)` | 配置可信反向代理 CIDR 列表与 `X-Forwarded-Proto` 信任策略 |
| `application& rate_limit(rate_limit_config config)` / `rate_limit(std::nullptr_t)` | 启用或禁用全局按客户端 IP 限流 |
| `application& blocking_pool(blocking_pool_options config)` / `blocking_pool(std::nullptr_t)` | 配置常驻阻塞卸载线程池 |
| `application& http_client(http_client_registration_config config)` / `http_client(std::nullptr_t)` | 注册绑定到每个 worker 的出站 HTTP 客户端池（支持命名 `alias_`，默认 `"default"`） |
| `application& database(db_registration_config config)` / `database(std::nullptr_t)` | 注册 per-worker SQL 数据库连接池（需 `RUVIA_ENABLE_DATABASE`） |
| `application& redis(redis_registration_config config)` / `redis(std::nullptr_t)` | 注册 per-worker Redis 连接池（需 `RUVIA_ENABLE_REDIS`） |
| `application& on_error(http_error_handler handler)` | 注册全局错误处理器（另有 `scoped_error_handler_options` 重载支持按路径前缀注册） |
| `application& on_not_found(http_not_found_handler handler)` | 注册全局 404 处理器（另有 `scoped_not_found_handler_options` 重载支持按路径前缀注册） |
| `application& on_access(access_log_callback callback)` | 注册请求访问日志回调（`noexcept`） |
| `application& on_connection_failure(connection_failure_callback callback)` | 注册连接/握手层异常回调（`noexcept`） |
| `application& on_start(app_hook hook)` / `on_stop(app_hook hook)` | 注册启动后（accept 开始前）与停机后的生命周期钩子（在 `run()` 调用线程上执行） |
| `void run()` | 启动所有业务 worker 并阻塞，直到调用 `stop()` 或收到停止信号；排空完成后返回 |
| `void stop()` | 线程安全地请求停机：停止接入新连接 → 在 `drain_timeout_` 内排空已接纳请求并发送 WebSocket `1001` 关闭帧 → 强制关闭超时连接 → 退役退出 |
| `http_server_stats http_stats() const` | 线程安全地汇总所有 worker 的连接与故障统计 |
| `blocking_pool_stats blocking_pool_stats() const` | 线程安全地读取阻塞线程池统计 |
| `std::vector<web_worker_handle> workers() const` | 获取全部业务 worker 的跨线程派发句柄 |
| `web_worker_handle worker_for(std::uint64_t key) const` / `worker_for(std::string_view key) const` | 按哈希键确定性选择单个业务 worker 句柄 |

---

### 1.2 服务器、监听、TLS、压缩与 CORS 配置（`"ruvia/web/server_config.h"`）

#### `ruvia::server_config`

| 字段名 | 类型与默认值 | 说明 |
| --- | --- | --- |
| `worker_count_` | `std::size_t`（默认 `max(1u, hardware_concurrency())`） | 业务 worker 线程数 |
| `process_signal_handlers_` | `process_signal_handler_policy`（默认 `external_owner`） | 设为 `install` 时自动安装 `SIGINT` / `SIGTERM` 信号处理以触发优雅停机 |
| `drain_timeout_` | `std::chrono::milliseconds`（默认 `10s`） | 停机排空阶段等待在途请求完成的最大时长。HTTP/2 连接按 RFC 9113 §6.8 两阶段排空：先发 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳客户端已在途的流；收到 `PING` ACK（或客户端此间新开超过 128 条流）后再发携带最后流 ID 的 `GOAWAY` 并拒绝之后的流 |
| `worker_queue_capacity_` | `std::size_t`（默认 `1024`） | 每个 worker 跨线程任务投递队列容量 |
| `idle_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `75s`） | 空闲连接超时时间 |
| `connection_scan_interval_` | `std::chrono::milliseconds`（默认 `1s`） | 连接超时扫描周期 |
| `request_header_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 请求头读取超时 |
| `request_body_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 请求体单次读取超时 |
| `header_completion_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `30s`） | 请求头整体完成截止时间 |
| `body_completion_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `120s`） | 请求体整体完成截止时间 |
| `write_timeout_` | `std::optional<std::chrono::milliseconds>`（默认 `60s`） | 响应写入超时 |
| `max_connections_per_worker_` | `std::optional<std::size_t>`（默认 `1024`） | 单个 worker 允许的最大并发连接数 |
| `max_requests_per_connection_` | `std::optional<std::size_t>`（默认 `1000`） | 单条连接最大处理请求数；HTTP/2 达到上限时按上述两阶段 `GOAWAY` 排空，往返期间在途的流仍被处理（产品上限：最多再接纳 128 条） |
| `max_buffered_body_bytes_` | `std::size_t`（默认 `default_max_buffered_body_bytes`） | 缓冲式路由默认允许的最大请求体字节数 |
| `max_inbound_buffer_bytes_per_worker_` | `std::size_t`（默认 `256 MiB`） | 单 worker 入站缓冲总字节上限 |
| `max_inbound_buffer_bytes_per_connection_` | `std::size_t`（默认 `64 MiB`） | 单连接入站缓冲字节上限 |
| `max_stream_body_bytes_` | `std::optional<std::size_t>`（默认无上限） | 流式路由请求体总字节上限 |
| `max_websocket_message_bytes_` | `std::size_t`（默认 `default_max_websocket_message_bytes`） | WebSocket 单条消息最大字节数 |
| `memory_pool_` | `memory_pool_config` | 请求 Arena 初始字节数（`request_initial_buffer_bytes_`）与 worker 保留块上限（`retained_block_bytes_`） |
| `http_client_result_budget_` | `http_client_result_budget_config` | 出站 HTTP 客户端保留字节预算 |

连接关闭遵循 [RFC 9112 §9.6/§9.8](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6)：

- 服务器主动结束、而对端仍可能发来数据时采用逗留关闭：HTTP/1 发送要求关闭连接的最终响应（`Connection: close`、请求被拒绝、排空中）后，以及 HTTP/2 以 `GOAWAY` 结束会话（排空完成、空闲超时、连接错误）后，TLS 先发送 `close_notify`，再半关闭 TCP 发送方向，读取并丢弃对端剩余输入，直到对端结束输入、累计 2 秒或丢弃 4 MiB 先发生为止，然后关闭 TCP。这样未读输入不会使 TCP 以 RST 结束连接而冲掉最后的响应。2 秒与 4 MiB 是固定的产品上限。
- 等待请求头时发生空闲超时、请求头超时或排空，以及对端正常结束输入时，TLS 发送 `close_notify` 后关闭。HTTP/2 连接超时（写入阶段除外）先进入 `GOAWAY` 排空；同一连接再次超时则直接关闭。
- 写超时、读取请求体超时、处理器无读写超过 `idle_timeout_`、传输错误以及 `drain_timeout_` 到期后的强制关闭立即关闭连接，不发送关闭告警。逗留中的连接计入 `active_connections_`，受 `drain_timeout_` 约束。

#### `ruvia::listen_config` 与 `ruvia::tls_config`

```cpp
ruvia::app().listen({
    .address_ = "0.0.0.0",
    .http_ = 8080,
    .https_ = 8443,
    .tls_ = {
        .certificate_chain_file_ = "certs/server.crt",
        .private_key_file_ = "certs/server.key",
        .client_certificates_ = {
            .verify_file_ = "certs/ca.crt",
            .requirement_ = ruvia::tls_client_certificate_requirement::optional,
        },
        .sni_ = {
            {
                .host_ = "api.example.com",
                .certificate_chain_file_ = "certs/api.crt",
                .private_key_file_ = "certs/api.key",
            },
        },
    },
    .auto_https_redirect_ = true,
});
```

- `http_` 与 `https_` 至少指定一个；同时指定且 `auto_https_redirect_ = true` 时，明文 `http_` 端口上的请求会自动重定向至 `https_` 端口。
- `tls_.client_certificates_.requirement_` 支持 `tls_client_certificate_requirement::optional` 或 `required`（双向 TLS / mTLS），通过 `c.conn().tls()->client_certificate_subject()` 读取客户端证书主题。
- `sni_` 按 SNI 主机名（ASCII 不区分大小写）选择证书；客户端未发送 server_name 或名称不匹配任何 `sni_` 项时使用主证书完成握手。这是为单证书与通配符证书部署做出的产品选择，不发送 RFC 9325 §3.7 建议的 `unrecognized_name` 告警。
- 协议策略固定：仅 TLS 1.2 / 1.3；TLS 1.2 只协商 ECDHE + AEAD 密码套件（AES-128-GCM 优先），由服务器偏好决定；拒绝重协商；ALPN 依次选择 `h2`、`http/1.1`、`http/1.0`，三者都未提供时以 `no_application_protocol` 告警失败。
- 会话可在同一监听器的任意 worker 上恢复；TLS 1.2 会话只在与建立时相同的 SNI 名称下恢复（RFC 6066 §3），否则完整握手。会话票据密钥每 2 小时（会话有效期）轮换一次，旧密钥再保留一个周期用于解密，随后销毁（RFC 9325 §3.4）。

#### 压缩、CORS 与可信代理配置

- `compression_config`：`min_bytes_`（默认 `1024`）、`sync_bytes_`（默认 `64 KiB`）、`max_bytes_`（默认 `64 MiB`）。`min_bytes_` 只是“压缩是否划算”的阈值：请求以 `identity;q=0` 等方式拒绝 identity 时，小于该值的缓冲响应同样压缩（且不要求结果小于原文），而不是回 `406`。响应已带 `Content-Encoding`、`Content-Range`、`Cache-Control: no-transform` 或完整性字段 `Content-Digest` / `Repr-Digest` / `Content-MD5` 时保持原表示不压缩（RFC 9530：摘要随内容编码而变）。
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
- `ruvia::env`（通过 `app().env()` 或 `c.env()` 访问）：
  - `std::optional<std::string_view> get(std::string_view name) const& noexcept`
  - `template <typename T> std::optional<T> get(std::string_view name) const&`（支持 `std::string_view`、`bool`、整型、浮点型；变量存在但格式非法时抛出 `std::invalid_argument`）
  - `template <typename T> std::optional<T> try_get(std::string_view name) const& noexcept`（格式非法时返回 `std::nullopt`）

---

### 1.4 错误处理与异常（`"ruvia/web/error.h"`, `"ruvia/web/app.h"`）

- **抛出或构造业务错误**：
  - 在路由或中间件中可抛出 `ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "bad_input", .message_ = "..."})`，或直接 `co_return c.error({...});`。`http_error` 是唯一的拥有型应用错误：status / code / message / status_text 以及 `.validation_issues_` 全部深拷贝持有，`info()` 返回携带 issues 的 `http_error_info`。
- **全局与前缀级错误/404 处理器**：
  - `on_error` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c, ruvia::http_error_info error)`。处理器内再次抛出 `http_error`（含校验 issues）会按该错误生成默认响应，不会变成 500；协议层错误的 `error.code()` 为具体机器码（如 `missing_host`、`conflicting_content_length`），而非按状态派生的 `bad_request`。
  - `on_not_found` 回调签名：`ruvia::task<ruvia::http_response>(ruvia::context& c)`。
  - 支持通过 `{.prefix_ = "/api", .handler_ = &api_error}` 注册前缀作用域处理器，按最长完整路径段匹配（例如 `"/api"` 匹配 `"/api/users"` 但不匹配 `"/apix"`）。
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
| 缓冲或流式响应路由 | `RUVIA_GET(path, handler, ...)` | `task<http_response> handler(context&)` 或 `task<void> handler(context&)` | 注册 `GET` 路由并自动提供 `HEAD` 回退（返回 `task<http_response>` 时为缓冲响应；返回 `task<void>` 时自动推导为 `c.stream()` / `c.stream_text()` / `c.stream_sse()` 流式响应）。`HEAD` 请求运行同一处理器与中间件，`c.req().method()` 仍为 `"HEAD"`，框架只发送响应头并丢弃响应体（流式处理器在首次写入响应体时结束），`Allow` 同时列出 `HEAD`；WebSocket 路由不提供 `HEAD` 回退 |
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

### 2.3 路径模式与反向链接生成（`url_for`）

- **静态段**：`/api/hello`
- **命名参数段**：`/api/users/:id`（通过 `c.req().param("id")` 读取）
- **尾部通配符**：`/api/files/*`（匹配余下路径，通过 `c.req().param("*")` 读取）
- **路径等价**：请求路径与注册模式按同一规范形式比较：以字面 `/` 切分路径段，段内按百分号解码后的字节比较。因此 `/users/%61dmin`、`/users/adm%69n` 与 `/users/admin` 命中同一路由（十六进制大小写不影响匹配，如 `caf%c3%a9` 与 `caf%C3%A9`），而 `%2F` 始终留在所在段内，不作为路径分隔符。该形式与路径参数的解码结果、`use_at` 前缀作用域中间件以及前缀错误/404 处理器的匹配规则一致；比 RFC 3986 的等价规则更宽，`%3A` 与 `:` 这类保留字符的编码与字面形式也视为同一路径。同一方法下仅拼写不同的两个模式（如 `/a` 与 `/%61`）属于重复注册，启动时抛出 `std::invalid_argument`。
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
| `c.req().param(name)` / `c.req().param_fields()` | 获取路径参数值（`std::optional<std::string_view>`）或参数列表 |
| `c.req().query(name)` / `c.req().queries(name)` / `c.req().query_fields()` | 获取单个/多个同名 URL 查询参数（已自动 URL 解码） |
| `c.req().header(name)` / `c.req().header_fields()` | 大小写不敏感读取请求头 |
| `c.req().cookie(name)` / `c.req().signed_cookie({.name_ = ..., .secret_ = ...})` | 读取普通 Cookie 或验证 HMAC 签名并提取 Cookie 原值 |
| `c.req().accepts(media_type)` / `c.req().negotiate(type, supported)` | 检查 `Accept` 是否接受指定媒体类型，或在候选列表中按权重协商最佳匹配 |
| `co_await c.req().text()` / `co_await c.req().bytes()` | 读取完整缓冲请求体视图（`std::string_view` / `std::span<const std::byte>`） |
| `co_await c.req().discard_body()` | 丢弃剩余请求体字节 |
| `co_await c.req().multipart()` | 解析缓冲式 `multipart/form-data`，返回 `std::pmr::vector<multipart_part>` |
| `c.req().validated<T>()` | 获取由校验中间件（`json_body<T>` 等）已完成解析和校验的模型常量引用 |
| `co_await c.req().json_if<T>()` / `co_await c.req().form_if<T>()` | 按需结构化解析并校验 JSON 或 Form 为 `std::optional<T>`（仅当 `Content-Type` 不匹配时返回 `std::nullopt`） |
| `c.conn().remote()` / `c.conn().client()` | 直连对端地址端口（`.address()` / `.port()`）与经过 `trusted_proxies` 解析后的真实客户端地址 |
| `c.conn().via_trusted_proxy()` / `c.conn().scheme()` / `c.conn().tls()` | 是否经过可信代理、连接有效 scheme（`http` / `https`）及 TLS 传输信息指针 |

### 3.2 构造响应与设置响应元数据

```cpp
ruvia::task<ruvia::http_response> handler(ruvia::context& c) {
    c.status(ruvia::http_status::created);
    c.header("X-Custom", "value");
    c.set_cookie({
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
| `void status(http_status_code code)` | 设置最终响应状态码（`200..599`） |
| `void header(name, value, options = {})` / `void remove_header(name)` | 设置（默认 `replace`，可指定 `http_response_header_mode::append`）或移除响应头 |
| `void set_cookie(set_cookie_options)` / `set_signed_cookie(set_signed_cookie_options)` / `delete_cookie(delete_cookie_options)` | 设置普通 Cookie、HMAC 签名 Cookie 或使指定 Cookie 立即过期（发送过去的 `Expires` 日期，不使用 `Max-Age=0`） |
| `http_response text(...)` / `html(...)` / `body(...)` | 构造 `text/plain; charset=utf-8`、`text/html; charset=utf-8` 或原始响应体（支持字符串字面量、`std::string_view`、`std::pmr::string&&`、`std::span<const std::byte>`、`ruvia::static_text`） |
| `template <typename T> http_response json(const T& model) const` | 将 `RUVIA_MODEL` 实例序列化为 `application/json` 响应 |
| `http_response redirect(redirect_response_options options) const` | 构造重定向响应（默认 `302 Found`，可通过 `.status_` 指定 `301`/`307`/`308` 等） |
| `http_response file(file_response_options options) const` | 发送磁盘文件响应（支持自动 Range 与条件请求处理） |
| `http_response static_file(const static_root& root, static_file_response_options options) const` | 从预构建的 `static_root` 快照发送静态文件响应 |
| `http_response error(http_error_info_options options) const` | 构造标准 JSON 错误响应 |
| `task<http_response> not_found()` | 调用当前匹配的 404 处理器生成响应 |
| `void respond(http_response&& response)` / `const http_response* response() const noexcept` | 在返回 `task<void>` 的中间件中提前提交响应或检查下游响应 |

### 3.3 进程内子请求（`"ruvia/web/context.h"`）

在控制器或中间件内可通过 `co_await c.dispatch({...})` 在当前 worker 内直接重入路由表发起虚拟子请求（不经过网络 socket）：

```cpp
auto sub = co_await c.dispatch({
    .method_ = "GET",
    .target_ = "/api/internal/profile",
});
// sub.status(), sub.body(), sub.header("Content-Type")
// 子请求内可通过 c.is_subrequest() 判断当前是否为子请求
```

### 3.4 阻塞任务卸载（`c.run_blocking`）

需先通过 `app().blocking_pool({...})` 配置（或使用默认启用的有界阻塞池）。在请求中卸载耗时同步计算：

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

任何声明为 `final`、不继承基类，且提供 `handle(ruvia::context&, ruvia::next&)`（返回 `ruvia::task<void>` 或 `ruvia::task<ruvia::http_response>`）的类均满足 `ruvia::middleware` 概念：

```cpp
class timing_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        // 前置处理
        co_await next_value(); // single-shot，推进下游中间件与路由处理器
        // 后置处理（此时可通过 c.response() 查看响应或通过 c.header() 追加响应头）
        c.header("X-Processed", "true");
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
| `ruvia::rate_limit<MaxReqs, WindowMs>` (`"ruvia/web/rate_limit.h"`) | 控制器或路由级固定窗口限流中间件（按 `c.conn().client().address()` 在当前 worker 内计数）；全局限流使用 `app().rate_limit({...})` |
| `ruvia::security_headers_middleware` (`"ruvia/web/security_headers.h"`) | 自动注入 `X-Content-Type-Options`、`X-Frame-Options`、`Strict-Transport-Security`、`Content-Security-Policy`、`Referrer-Policy`、`Permissions-Policy` 等安全响应头（对 404 响应同样生效） |
| `ruvia::csrf_protection` (`"ruvia/web/csrf.h"`) | 基于 Double-Submit Cookie（默认 Cookie `"XSRF-TOKEN"` 与请求头 `"X-XSRF-TOKEN"`）的无状态 CSRF 防护中间件 |
| `ruvia::session_middleware` (`"ruvia/web/session.h"`, 需 `RUVIA_ENABLE_REDIS`) | 基于 Redis 的服务端会话中间件；路由中通过 `c.session().data()`、`c.session().set(...)`、`c.session().regenerate()`、`c.session().clear()` 管理会话 |
| `ruvia::make_health_response` / `make_readiness_response` (`"ruvia/web/context.h"`) | 生成标准健康检查（`200 OK`）与就绪探针响应（`readiness_state::ready` 返回 `200`，`unavailable` 返回 `503`） |

---

## 5. 声明式模型 `RUVIA_MODEL`、序列化与参数校验

头文件：`"ruvia/web/model.h"`、`"ruvia/web/validation.h"`。

### 5.1 模型定义与字段类型

使用 `RUVIA_MODEL(ModelName, ...)` 定义编译期反射模型。模型专用的字段类型位于 `namespace ruvia`：

- **标量类型**：`ruvia::bool_value`、`ruvia::int8`、`ruvia::uint8`、`ruvia::int16`、`ruvia::uint16`、`ruvia::int32`、`ruvia::uint32`、`ruvia::int64`、`ruvia::uint64`、`ruvia::float_value`、`ruvia::double_value`
- **字符串与字节**：`ruvia::string`（UTF-8 字符串）、`ruvia::bytes`（JSON 中以 Base64 编码的二进制数据）
- **复合与容器类型**：嵌套的另一个 `RUVIA_MODEL` 类型、`ruvia::array<T>`（连续数组）、`ruvia::boxed_array<T>`（支持前向声明的自引用递归模型数组）

#### 字段声明与属性修饰宏

| 宏 | 说明 |
| --- | --- |
| `RUVIA_REQUIRED_FIELD(field, type, ...)` | 必填字段（缺失则校验失败） |
| `RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...)` | 指定外部序列化字段名 `wire_name` 的必填字段 |
| `RUVIA_OPTIONAL_FIELD(field, type, ...)` | 可选字段（仅允许字段缺失；若需允许 JSON `null` 须额外加 `RUVIA_NULLABLE`） |
| `RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...)` | 指定外部序列化字段名 `wire_name` 的可选字段 |
| `RUVIA_NULLABLE` | 允许字段值为 `null`（通过 `m.is_null<"field">()` 判断） |
| `RUVIA_DEFAULT(value)` | 反序列化时若字段缺失（非 `null`），自动填充默认值 `value`（仅用于可选字段） |
| `RUVIA_INITIAL(value)` | 在 C++ 中默认构造模型实例时将该字段初始化为 `value` |
| `RUVIA_OMIT_EMPTY` / `RUVIA_EMIT_NULL` | 控制序列化时缺失或空值字段的输出行为 |

#### 声明式校验规则宏（附加在字段宏末尾）

- `RUVIA_MIN(limit, message)`：数值下限或字符串 UTF-8 字节长度下限
- `RUVIA_MAX(limit, message)`：数值上限或字符串 UTF-8 字节长度上限
- `RUVIA_EMAIL(message)`：电子邮件格式校验
- `RUVIA_ONE_OF(message, val1, val2, ...)`：枚举白名单校验
- `RUVIA_PATTERN(message, pattern)`：整串锚定正则模式校验
- `RUVIA_CUSTOM(message, predicate)`：自定义谓词函数校验

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

- `m.get<"username">()`：获取字段引用（对 `ruvia::string` 可调用 `.view()`，对标量包装可调用 `.value()` 或隐式转换）
- `m.set<"username">("alice")`：设置字段值（支持链式调用）
- `m.is_present<"role">()` / `m.is_null<"nickname">()`：检查字段是否已设置或是否为 `null`
- `m.ensure<"nested">()`：确保嵌套对象/数组已初始化并返回可变引用
- `m.reset<"nickname">()`：重置可选或可空字段

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
  - `std::pmr::string ruvia::to_json(const T& value, model_options options = {})`
  - `T ruvia::from_form<T>(std::string_view form, model_parse_options options = {})`（解析失败抛 `std::invalid_argument`，字段校验失败抛携带 issues 的 `ruvia::http_error`）
- **零拷贝动态 JSON 视图（`"ruvia/web/model.h"`）**：
  - `ruvia::json_object::parse(std::string_view body, model_parse_options options = {})` / `ruvia::json_value::parse(std::string_view body, model_parse_options options = {})`（直接返回值，语法非法抛 `std::invalid_argument`）
  - 支持 `obj.get<ruvia::string>("key")`、`obj.get<ruvia::int64>("count")`、`obj.for_each_field([](std::string_view key, const ruvia::json_value& val) { ... })`、`val.for_each_element([](const ruvia::json_value& item) { ... })`。
- **命令式校验器 `ruvia::validator`（`"ruvia/web/validation.h"`）**：
  - 提供 `.required(opt, field)`、`.min_length(opt, field, n)`、`.max_length(opt, field, n)`、`.range(opt, field, low, high)`、`.one_of(opt, field, list)`、`.add(field, code, message)`，最后调用 `.throw_if_invalid(validation_failure_options = {})` 抛出携带 issues 的 `ruvia::http_error`（默认 400 / `validation_failed`）。

---

## 6. 静态文件服务与内存级测试

### 6.1 静态文件服务（`"ruvia/web/static_files.h"`）

Ruvia 提供三种静态文件服务方式：

1. **单文件响应**：在路由中调用 `c.file({.path_ = "/path/to/file.pdf", .content_type_ = "application/pdf"})`。
2. **不可变目录快照 `ruvia::static_root`**：启动前构造 `ruvia::static_root assets("public/", {.cache_control_ = "public, max-age=3600"})`，在路由中调用 `c.static_file(assets, {.relative_path_ = c.req().param("*").value_or("")})`。
3. **托管自动刷新的 `app().document_root({...})`**：后台按 `refresh_interval_` 刷新目录索引，并支持 `precompress_gzip_` / `precompress_brotli_` / `precompress_zstd_` 自动预压缩热文件。

三者均内置路径穿越防护、点文件策略（`static_dotfile_policy::deny` 默认拒绝隐藏文件）、`ETag` / `Last-Modified` 条件请求（`304 Not Modified`）与 `Range` 分段响应（`206 Partial Content`）。

多个范围的请求返回 `multipart/byteranges`，各部分只带 `Content-Type` 与 `Content-Range`；选中预压缩变体（带 `Content-Encoding`）时多范围请求返回完整的 `200` 表示（单个范围仍是带 `Content-Encoding` 的单部分 `206`）。`If-Range` 匹配后的 `206` 只带必需字段（`ETag`、`Cache-Control`、`Vary`、`Content-Range` 或 multipart `Content-Type` 等）及描述所发字节的 `Content-Encoding`，不再重复 `Content-Type`、`Last-Modified`（RFC 9110 §15.3.7）。

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
