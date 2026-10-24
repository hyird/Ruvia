# Ruvia 用户文档与 API 参考手册

本文档目录提供 Ruvia 框架（`ruvia::core`、`ruvia::http`、`ruvia::web`）的使用指南与公开 API 参考手册。所有说明均基于当前仓库非 `detail` 公开头文件与 `examples/web/` 可运行示例。

## 文档导航

| 文档 | 内容范围 |
| --- | --- |
| [doc/README.md](README.md) | 架构总览、CMake 构建与工程接入、生命周期与所有权准则、错误处理约定、示例程序全集索引 |
| [doc/web-guide.md](web-guide.md) | `ruvia::web` 服务端核心：应用配置（`ruvia::app()`）、控制器与路由宏、中间件、`context` / `context_request`、声明式模型 `RUVIA_MODEL` 与校验、静态文件、运维能力与内存级测试 `test_app` |
| [doc/streaming-websocket-client.md](streaming-websocket-client.md) | 流式请求/响应与 SSE、HTTP/1.1 & HTTP/2 高级服务端能力（`103 Early Hints`、优先级、Server Push、Trailer）、WebSocket 服务端、HTTP `CONNECT` / `CONNECT-UDP` 隧道、出站 `http_client` 与 `websocket_client` |
| [doc/data-and-auth.md](data-and-auth.md) | SQL 数据库（MariaDB / PostgreSQL 连接池、直接查询、事务、迁移、SQL/DDL/过程构建器）、强类型关系型 ORM、Redis 客户端（命令、Pipeline、事务、Streams、Pub/Sub、RediSearch ORM）与 JWT 认证 |
| [doc/core-and-http.md](core-and-http.md) | `ruvia::core` 运行时（`task<T>`、`event_loop_pool`、`blocking_pool`、有界通道、停止令牌、定时器、PMR 内存资源）与 `ruvia::http` 独立 Sans-I/O 协议层（HTTP/1、HTTP/2、HPACK、WebSocket、内容压缩、Multipart、SSE） |

---

## 1. 分层架构与命名空间

Ruvia 由三个独立的静态库组成，所有公开类型、函数与概念均位于顶层命名空间 `namespace ruvia`（HTTP 状态码常量位于嵌套命名空间 `namespace ruvia::http_status`）：

| 库目录 | CMake Target | 头文件前缀 | 职责与边界 |
| --- | --- | --- | --- |
| `ruvia-core` | `ruvia::core` | `"ruvia/core/..."` | 协程任务（`task<T>` / `root_task<T>`）、事件循环（`event_loop` / `event_loop_pool` / `event_loop_attachment`）、有界通道（`channel` / `one_shot`）、阻塞卸载池（`blocking_pool`）与 PMR 内存池；仅公开依赖 standalone Asio 与 `Threads::Threads` |
| `ruvia-http` | `ruvia::http` | `"ruvia/http/..."` | 独立 sans-I/O 协议实现：HTTP/1.0、HTTP/1.1、HTTP/2、HPACK、WebSocket、内容压缩（gzip / deflate / br / zstd）、Multipart、SSE、RFC 9297 Datagram / Capsule 与 RFC 9298 `CONNECT-UDP`；不依赖 `ruvia::core`、Asio、socket 或 OpenSSL |
| `ruvia-web` | `ruvia::web` | `"ruvia/web/..."` | Web 应用框架：服务器与路由、中间件、声明式 Model/校验、TLS/mTLS/SNI、出站 HTTP/WebSocket 客户端、SQL 数据库/ORM、Redis/RediSearch ORM 与 JWT；公开依赖 `ruvia::core` 与 `ruvia::http` |

> **头文件使用原则**：只包含 `include/ruvia/{core,http,web}/` 下的非 `detail` 公开头文件，不直接引用任何 `detail/` 子目录下的内部头文件。

---

## 2. 构建、安装与 CMake 接入

### 2.1 CMake 选项与条件编译宏

根 `CMakeLists.txt` 提供以下构建与功能开关：

| CMake 选项 | 默认值 | 公开编译宏（`ruvia::web` PUBLIC） | 说明 |
| --- | --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | `ASIO_STANDALONE`（`ruvia::core` PUBLIC） | 构建 `ruvia::core` |
| `RUVIA_BUILD_HTTP` | `ON` | — | 构建 `ruvia::http` |
| `RUVIA_BUILD_WEB` | `ON` | — | 构建 `ruvia::web`（需开启 `CORE` 与 `HTTP`） |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | — | 构建 `examples/` 下的示例程序（需开启 `WEB`） |
| `RUVIA_BUILD_TESTS` | `OFF` | — | 构建 `tests/` 测试套件 |
| `RUVIA_ENABLE_MARIADB` | `OFF` | `RUVIA_ENABLE_MARIADB`、`RUVIA_ENABLE_DATABASE` | 启用 MariaDB 驱动与 `"ruvia/web/db/..."` 公开头 |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | `RUVIA_ENABLE_POSTGRESQL`、`RUVIA_ENABLE_DATABASE` | 启用 PostgreSQL 驱动、ORM 与 `"ruvia/web/db/..."` 公开头 |
| `RUVIA_ENABLE_REDIS` | `OFF` | `RUVIA_ENABLE_REDIS` | 启用 `"ruvia/web/redis/..."`、服务端 `session_middleware` 与数据库查询缓存 |
| `RUVIA_ENABLE_JWT` | `OFF` | `RUVIA_ENABLE_JWT` | 启用 `"ruvia/web/auth/jwt.h"` |

### 2.2 在下游 CMake 项目中使用

安装 Ruvia：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DRUVIA_ENABLE_POSTGRESQL=ON -DRUVIA_ENABLE_REDIS=ON -DRUVIA_ENABLE_JWT=ON
cmake --build build --config Release -j$(nproc)
cmake --install build --config Release --prefix /path/to/install
```

在下游项目的 `CMakeLists.txt` 中引入：

```cmake
cmake_minimum_required(VERSION 3.28)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(my_service LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ruvia REQUIRED COMPONENTS core http web)

add_executable(my_service src/main.cpp)
target_link_libraries(my_service PRIVATE ruvia::web)
```

消费方提供与 Ruvia 匹配的依赖清单和工具链：非 Windows 为 mimalloc 启用普通 `override` 特性；Windows 使用普通 mimalloc 依赖与上述静态 CRT，不需要 overlay、对象导出或 EXE 后处理，也不覆盖宿主的标准分配接口。独立 HTTP 组件不依赖 mimalloc。`add_subdirectory` 宿主自行提供依赖清单和工具链。

#### 静态库中的控制器链接（`ruvia_link_controllers`）

控制器通过 `RUVIA_ROUTES_BEGIN(Controller)` 宏在 `main()` 之前的静态初始化阶段自动注册路由。如果将控制器拆分为独立的 `STATIC_LIBRARY` 或 `OBJECT_LIBRARY`，普通 `target_link_libraries` 可能会因可执行文件未直接引用控制器符号而丢弃目标文件。`ruviaConfig.cmake` 在找到 `web` 组件时提供了 `ruvia_link_controllers` 辅助函数：

```cmake
add_library(user_controllers STATIC src/user_controller.cpp)
target_link_libraries(user_controllers PUBLIC ruvia::web)

add_executable(my_service src/main.cpp)
target_link_libraries(my_service PRIVATE ruvia::web)
ruvia_link_controllers(my_service user_controllers)
```

---

## 3. 核心架构与生命周期准则

### 3.1 Shared-Nothing Worker 线程模型

- **单 Worker 独占连接生命周期**：生产应用 `ruvia::app()` 按 `server_config::worker_count_` 启动业务 worker 线程。每个 worker 独占一个 standalone Asio `io_context`、自己的监听器、控制器实例、中间件实例、PMR 内存池以及绑定的 `db_client` / `redis_client` / `http_client` 连接池。
- **无跨线程连接交接**：待接受的 socket 从发起 accept 的 origin worker 创建起，其后的 TCP/TLS I/O、HTTP/1 或 HTTP/2 解析、路由执行、协议定时器和关闭排空全程留在该 worker 线程，无需加锁访问 worker 局部状态（`c.worker_state<T>()`）。
- **跨线程调度与卸载**：
  - 跨 worker 通信通过 `web_worker_handle::post`、`event_loop::post` 或有界通道 `make_channel` / `make_one_shot` 进行。
  - 阻塞或 CPU 密集型计算必须通过 `c.run_blocking(...)` 或 `ruvia::run_blocking(...)` 显式卸载到 `blocking_pool`。**卸载闭包必须按值/移动持有自有数据，严禁在阻塞池线程中访问 `ruvia::context`、请求 Arena 或任何 worker 局部状态**。

### 3.2 协程与作用域操作契约

- **`ruvia::task<T>`**：全库统一的惰性（lazy）协程。调用返回 `task<T>` 的函数仅分配协程帧而不开始执行，直到被右值 `co_await` 或传入 `event_loop::start()`。`context`、`http_client`、`db_handle`、`redis_handle` 等对象上的异步 I/O 方法返回的 `task<T>` 同时绑定到所属请求或客户端的 `operation_scope`。
  - **未启动（cold）**的 `task<T>` 可直接析构丢弃，或随所属 `operation_scope` 关闭自动回收。
  - **已启动（started）但尚未完成**的 `task<T>` 严禁直接析构（会直接触发 `std::terminate()`）。请求取消（`stop_source::request_stop()`）后，仍必须 `co_await` 或 `join` 等待其完成退出。

### 3.3 内存资源与借用生命周期

- **`c.arena()` 与 `c.pool()`**：
  - `c.arena()` 返回单次请求专属的单调缓冲区（`std::pmr::memory_resource*`），适合分配与请求同寿命的模型、字符串和容器（如 `c.allocator<char>()`），在请求结束时一次性回收。
  - `c.pool()` 返回当前 worker 的可回收内存池，适合在单次请求内部反复申请和释放的较大临时块（必须在请求结束前析构归还）。
- **借用视图与防悬垂保护**：
  - `c.req().header(...)`、`c.req().query(...)`、`c.req().param(...)`、`c.req().text()` 等返回的 `std::string_view` / `std::span` 均借用自当前请求缓冲，仅在当前请求（或流式读取的下一次 `read()` 之前）有效。
  - 公开 API 参数广泛使用 `ruvia::borrowed_text`，并在返回借用视图的成员函数上标注 `RUVIA_LIFETIMEBOUND` 且 `= delete` 右值重载，在编译期阻止绑定临时 `std::string` 右值导致的悬垂引用。
  - 构造响应体时（如 `c.text(...)`、`c.body(...)`），直接传入 `std::string` 左值或右值被显式 `= delete`；请传入 `std::string_view`（复制到响应中）、`std::pmr::string&&`（转移所有权）、字符串字面量或 `ruvia::static_text("...")`（静态存储期零拷贝）。

### 3.4 配置结构体与错误处理约定

- **配置结构体字段命名**：所有配置/选项结构体（`*_config`、`*_options`、`*_rule` 等）的公开成员变量均以**下划线 `_` 结尾**（例如 `listen_config{.address_ = "0.0.0.0", .http_ = 8080}`），支持 C++20 指定初始化器（designated initializers）。
- **错误处理形状**：
  - **正常协议/队列状态推进**：通过状态枚举或结果类型表示（如 `post_status::queue_full`、`channel_send_status::full`、`blocking_status::timed_out`、`worker_wait_status::closed`）。
  - **操作失败或业务/协议错误**：抛出类型化异常（如路由中抛出 `ruvia::http_error`（可携带校验 issues）由错误处理器转换为对应 HTTP 响应；协议原语抛出 `ruvia::http_protocol_error`，携带状态码与稳定机器码 `code()`；客户端抛出 `http_client_error`、`websocket_client_error`、`db_error`、`redis_error`）。
  - **纯解析/值校验边界**：返回 `std::optional<T>`、`std::variant<T, Error>`，或直接返回值并在格式/规则非法时抛出 `std::invalid_argument` / 携带 issues 的 `ruvia::http_error`（如 `ruvia::parse_integer<T>`、`ruvia::from_json<T>`）。

---

## 4. 示例程序全集索引

仓库 `examples/web/` 下包含 30 个可独立运行的示例程序（开启 `-DRUVIA_BUILD_EXAMPLES=ON` 构建）：

| 示例文件 | CMake Target | 所需开关 | 核心演示内容 |
| --- | --- | --- | --- |
| [basic_http.cpp](../examples/web/basic_http.cpp) | `ruvia_example_basic_http` | 默认 | 控制器与嵌套路由组、中间件、路径参数 `:id` 与通配符 `*`、Query/Header/Cookie 读取、文本/JSON/重定向/错误响应、`url_for`、全局与前缀级 `on_error` / `on_not_found` |
| [api_surface.cpp](../examples/web/api_surface.cpp) | `ruvia_example_api_surface` | 默认 | `context` / `context_request` / `http_response` 完整接口：`accepts` 协商、多方法路由 `RUVIA_ALL` / `RUVIA_ON`、缓冲式 `multipart()`、签名 Cookie、流式 `PUT` / `PATCH` |
| [models_validation.cpp](../examples/web/models_validation.cpp) | `ruvia_example_models_validation` | 默认 | `RUVIA_MODEL` 声明、字段校验规则宏、递归模型 `boxed_array`、`ruvia::bytes` Base64、从 JSON/Form/Query/Path/Header/Cookie 自动绑定校验与 `json_if` / `form_if` |
| [model_values.cpp](../examples/web/model_values.cpp) | `ruvia_example_model_values` | 默认 | 独立使用 `from_json` / `to_json` / `from_form`、零拷贝动态 JSON 视图 `json_object` / `json_value` 与命令式 `ruvia::validator` |
| [middleware_next.cpp](../examples/web/middleware_next.cpp) | `ruvia_example_middleware_next` | 默认 | 中间件职责链、前置/后置响应头处理、`co_await next_value()` 与 `c.respond(...)` 短路拦截 |
| [request_policies.cpp](../examples/web/request_policies.cpp) | `ruvia_example_request_policies` | 默认 | 路由级请求体大小限制 `body_limit<N>`、请求级状态 `bind_request_state` / `request_state`、子请求 `c.dispatch(...)` 与 `csrf_protection` |
| [files_static.cpp](../examples/web/files_static.cpp) | `ruvia_example_files_static` | 默认 | 单文件响应 `c.file()`、不可变静态快照 `static_root` 与 `c.static_file()`、定时刷新 `document_root`、Range、ETag 与预压缩 |
| [streaming.cpp](../examples/web/streaming.cpp) | `ruvia_example_streaming` | 默认 | 流式请求体 `body_reader()`、流式 Multipart `multipart_reader()`、分块文本流 `stream_text()` 与 SSE `stream_sse()` |
| [http_features.cpp](../examples/web/http_features.cpp) | `ruvia_example_http_features` | 默认 | `103 Early Hints`（`c.inform`）、RFC 9218 优先级（`c.priority`）、HTTP/2 Server Push（`c.push`）、`ORIGIN` / `ALTSVC` 通告与请求/响应 Trailer |
| [websocket.cpp](../examples/web/websocket.cpp) | `ruvia_example_websocket` | 默认 | WebSocket 服务端路由（`RUVIA_GET_WS`）、子协议协商、心跳与关闭握手、零拷贝帧回显 |
| [tunnels.cpp](../examples/web/tunnels.cpp) | `ruvia_example_tunnels` | 默认 | HTTP/1.1 & HTTP/2 `CONNECT` 字节流隧道与 RFC 9298 `CONNECT-UDP` 数据报隧道（含服务端与 `--client` 客户端模式） |
| [http_client.cpp](../examples/web/http_client.cpp) | `ruvia_example_http_client` | 默认 | Worker 绑定的出站 `http_client`：超时控制、POST 转发、`read_all()` 与 `pipe_to(c.stream())` 流式反向代理 |
| [http_client_advanced.cpp](../examples/web/http_client_advanced.cpp) | `ruvia_example_http_client_advanced` | 默认 | 独立 `event_loop_pool` 上的 HTTP/1.1 & HTTP/2 高级客户端：流式上传、`100-continue`、Trailer、1xx 响应、重设优先级、接收 Push 与通告、同源重定向 |
| [websocket_client.cpp](../examples/web/websocket_client.cpp) | `ruvia_example_websocket_client` | 默认 | 独立 `event_loop_pool` 上的 WebSocket 客户端（支持 HTTP/1.1 Upgrade 与 HTTP/2 Extended CONNECT） |
| [runtime_config.cpp](../examples/web/runtime_config.cpp) | `ruvia_example_runtime_config` | 默认 | `.env` 加载、全局与前缀中间件、内存池与连接限制、协商压缩、CORS、TLS / mTLS / SNI 与 HTTP 自动跳转 HTTPS |
| [ops.cpp](../examples/web/ops.cpp) | `ruvia_example_ops` | 默认 | `security_headers_middleware`、全局与路由级 `rate_limit`、健康与就绪探针、`trusted_proxies`、`http_stats()`、生命周期与日志钩子 |
| [workers_blocking.cpp](../examples/web/workers_blocking.cpp) | `ruvia_example_workers_blocking` | 默认 | Worker 局部状态 `use_worker_state<T>()`、跨 Worker 任务投递 `worker.post(...)` 与 `c.run_blocking(...)` 阻塞池卸载 |
| [testing.cpp](../examples/web/testing.cpp) | `ruvia_example_testing` | 默认 | 使用 `test_app`、`test_request`、`test_response` 在内存内无 Socket 测试完整路由与中间件管线 |
| [auth_jwt.cpp](../examples/web/auth_jwt.cpp) | `ruvia_example_auth_jwt` | `RUVIA_ENABLE_JWT` | HMAC-SHA256/384/512 JWT 签发 `jwt_sign`、Bearer 提取 `jwt_bearer_token`、校验 `jwt_verify` 与鉴权中间件 |
| [database.cpp](../examples/web/database.cpp) | `ruvia_example_database` | `MARIADB` 或 `POSTGRESQL` | 数据库连接池配置、启动迁移 `db_migrator::migrate`、参数化查询、编译期 SQL 校验模板、流式游标 `query_stream` 与事务 |
| [sql_queries.cpp](../examples/web/sql_queries.cpp) | `ruvia_example_sql_queries` | `MARIADB` 或 `POSTGRESQL` | 离线 SQL 构建器 `db_query`、Schema DDL 构建器 `db_schema`、PL/pgSQL 过程块 `db_procedure` 与 TimescaleDB 策略生成 |
| [orm.cpp](../examples/web/orm.cpp) | `ruvia_example_orm` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL 强类型 ORM：实体与投影宏、Upsert / FindAndCount / Update / Increment、`query_builder`、窗口函数、`RETURNING`、事务抢占与 Redis 查询缓存 |
| [orm_columns.cpp](../examples/web/orm_columns.cpp) | `ruvia_example_orm_columns` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL `STORED` 生成列与事务隔离级别（`repeatable_read`）/ 只读事务模式（`read_only`） |
| [orm_relations.cpp](../examples/web/orm_relations.cpp) | `ruvia_example_orm_relations` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL ORM 实体关系：一对多、多对一、一对一、多对多（含连接表）、`.relations_` 预加载与 `left_join_and_select` |
| [event_loop_data.cpp](../examples/web/event_loop_data.cpp) | `ruvia_example_event_loop_data` | `POSTGRESQL` + `REDIS` | 在自管 `event_loop_pool` 上独立运行 `db_client` 与 `redis_client` 并执行 ORM 操作 |
| [sessions.cpp](../examples/web/sessions.cpp) | `ruvia_example_sessions` | `RUVIA_ENABLE_REDIS` | 基于 Redis 的服务端会话中间件 `session_middleware`、自动轮换会话 ID 与 `csrf_protection` |
| [redis.cpp](../examples/web/redis.cpp) | `ruvia_example_redis` | `RUVIA_ENABLE_REDIS` | Redis 默认与命名别名池、String/Hash/List/Set/ZSet、SCAN 游标、`pipeline()`、`transaction()`、Lua 脚本与 `blpop` / `brpop` |
| [redis_orm.cpp](../examples/web/redis_orm.cpp) | `ruvia_example_redis_orm` | `RUVIA_ENABLE_REDIS` | `RUVIA_REDIS_ENTITY` 声明、RediSearch 索引创建、带 TTL 写入与条件/排序分页检索 |
| [redis_streams.cpp](../examples/web/redis_streams.cpp) | `ruvia_example_redis_streams` | `RUVIA_ENABLE_REDIS` | Redis Streams 消费者组、`xread_group` 阻塞拉取与通用 `command(...)` 接口 |
| [redis_pubsub.cpp](../examples/web/redis_pubsub.cpp) | `ruvia_example_redis_pubsub` | `RUVIA_ENABLE_REDIS` | Redis `publish` 与独占连接 `subscription()` 订阅频道/模式并实时推送到 SSE 流 |
