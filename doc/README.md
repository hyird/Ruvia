# Ruvia 文档索引

每个 CMake target 对应一份公开 API 手册。无论通过安装包还是 `add_subdirectory` 接入，下游都只应使用目标公开的头文件和非 `detail` 符号。

## 1. Target 手册

| CMake Target | 完整公开 API 与使用指南 | 公开头目录 |
| --- | --- | --- |
| `ruvia::core` | [ruvia-core.md](ruvia-core.md) | [`ruvia-core/include/`](../ruvia-core/include) |
| `ruvia::http` | [ruvia-http.md](ruvia-http.md) | [`ruvia-http/include/`](../ruvia-http/include) |
| `ruvia::web` | [ruvia-web.md](ruvia-web.md) | [`ruvia-web/include/`](../ruvia-web/include) |

手册按公开头组织，包含类型、函数与重载、公开成员、枚举、常量和宏；条件功能在所属 target 中说明，不另设平行手册。核心生命周期、协议状态机与 Web 驱动的契约分别在其定义 target 中维护，跨层只链接引用。

---

## 2. 构建、安装与 CMake 接入

### 2.1 CMake 选项与条件编译宏

根 `CMakeLists.txt` 提供以下构建与功能开关：

| CMake 选项 | 默认值 | 公开编译宏 | 说明 |
| --- | --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | `ASIO_STANDALONE`（`ruvia::core` PUBLIC） | 构建 `ruvia::core` |
| `RUVIA_BUILD_HTTP` | `ON` | — | 构建 `ruvia::http` |
| `RUVIA_BUILD_WEB` | `ON` | — | 构建 `ruvia::web`（需开启 `CORE` 与 `HTTP`） |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | — | 构建 `examples/` 下的示例程序（需开启 `WEB`） |
| `RUVIA_BUILD_TESTS` | `OFF` | — | 构建 `tests/` 测试套件 |
| `RUVIA_ENABLE_HTTP2` | `ON` | `RUVIA_ENABLE_HTTP2` | 启用 nghttp2 会话与 HTTP/2 公开 API；仅在 HTTP/3 也关闭时可关闭 |
| `RUVIA_ENABLE_HTTP3` | `ON` | `RUVIA_ENABLE_HTTP3` | 启用 nghttp3/ngtcp2 协议层与 Web QUIC TLS 接入；开启时自动开启 `RUVIA_ENABLE_HTTP2` |
| `RUVIA_ENABLE_MARIADB` | `OFF` | `RUVIA_ENABLE_MARIADB`、`RUVIA_ENABLE_DATABASE` | 启用 MariaDB 驱动与 `"db_entity.h"` / `"db_entity_codec.h"` / `"db_executor.h"` / `"db_migration.h"` / `"db_query.h"` / `"db_repository.h"` / `"db_schema.h"` / `"db_sql_scan.h"` 公开头 |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | `RUVIA_ENABLE_POSTGRESQL`、`RUVIA_ENABLE_DATABASE` | 启用 PostgreSQL 驱动、ORM 与 `"db_entity.h"` / `"db_entity_codec.h"` / `"db_executor.h"` / `"db_migration.h"` / `"db_query.h"` / `"db_repository.h"` / `"db_schema.h"` / `"db_sql_scan.h"` 公开头 |
| `RUVIA_ENABLE_REDIS` | `OFF` | `RUVIA_ENABLE_REDIS` | 启用 `"redis_executor.h"` / `"redis_orm.h"` / `"session.h"`、服务端 `session_middleware` 与数据库查询缓存 |
| `RUVIA_ENABLE_JWT` | `OFF` | `RUVIA_ENABLE_JWT` | 启用 `"jwt.h"` |

顶层构建在未指定 `CMAKE_TOOLCHAIN_FILE` 时从环境变量 `VCPKG_ROOT` 选择 vcpkg，并加载项目 triplet、按开关选择 manifest 特性。子项目由宿主提供工具链与依赖清单。未启用功能不编译对应源码，也不安装相应公开头文件。

- **协议裁剪单一来源**：HTTP/2、HTTP/3 的源码与安装头共用同一套协议路径选择规则。

依赖使用当前 vcpkg 端口；Core 要求 mimalloc 3.5+，HTTP/3 要求 ngtcp2 1.26.0+ 与 nghttp3 1.17+。非 Windows 静态 mimalloc 必须启用 `override`，不得叠加其他全局分配器替换。

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

从 `1.0.0` 起，同一主版本内保持非 `detail` 公开 API 的源码兼容，包括公开宏、配置类型、错误码及所有权和生命周期契约；新增能力以兼容方式加入，破坏性调整进入下一主版本。CMake 包兼容性在 `0.x` 要求主版本和次版本均相同，从 `1.x` 起要求主版本相同。此承诺适用于相同平台、工具链和功能开关组合；不承诺跨版本 ABI 兼容，升级后应重新编译下游程序。

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

## 3. 示例程序全集索引

仓库 `examples/` 下包含 30 个可独立运行的示例程序（开启 `-DRUVIA_BUILD_EXAMPLES=ON` 构建）：

后端示例通过 `backend_config.h` 共享启动配置：先加载可执行文件旁的 `.env`，进程环境优先；`RUVIA_DB_*` 选择 SQL 连接，`RUVIA_REDIS_*` 选择 Redis 连接（包括 `DATABASE`、认证与 TLS）。ORM 查询缓存使用独立前缀 `RUVIA_ORM_CACHE_REDIS_*`。

独立服务使用不同端口：基础 HTTP `8080`、请求策略 `8097`、运维 `8098`、HTTP 客户端网关 `8099`。客户端示例的上游地址保持各自注释所述配置。

| 示例文件 | CMake Target | 所需开关 | 核心演示内容 |
| --- | --- | --- | --- |
| [basic_http.cpp](../examples/basic_http.cpp) | `ruvia_example_basic_http` | 默认 | 控制器与嵌套路由组、中间件、路径参数 `:id` 与通配符 `*`、Query/Header/Cookie 读取、文本/JSON/重定向/错误响应、`url_for`、全局与前缀级 `on_error` / `on_not_found` |
| [api_surface.cpp](../examples/api_surface.cpp) | `ruvia_example_api_surface` | 默认 | `context` / `context_request` / `http_response` 完整接口：`accepts` 协商、多方法路由 `RUVIA_ALL` / `RUVIA_ON`、缓冲式 `multipart()`、签名 Cookie、流式 `PUT` / `PATCH` |
| [models_validation.cpp](../examples/models_validation.cpp) | `ruvia_example_models_validation` | 默认 | `RUVIA_MODEL` 声明、字段校验规则宏、递归模型 `boxed_array`、`ruvia::bytes` Base64、从 JSON/Form/Query/Path/Header/Cookie 自动绑定校验与 `json_if` / `form_if` |
| [model_values.cpp](../examples/model_values.cpp) | `ruvia_example_model_values` | 默认 | 独立使用 `parse_json` / `to_json` / `parse_form`、动态 JSON 视图与领域校验结果 |
| [middleware_next.cpp](../examples/middleware_next.cpp) | `ruvia_example_middleware_next` | 默认 | 中间件职责链、前置/后置响应头处理、`co_await next_value()` 与 `c.respond(...)` 短路拦截 |
| [request_policies.cpp](../examples/request_policies.cpp) | `ruvia_example_request_policies` | 默认 | 路由级请求体大小限制 `body_limit<N>`、请求级状态 `bind_request_state` / `request_state`、子请求 `c.dispatch(...)` 与 `csrf_middleware` |
| [files_static.cpp](../examples/files_static.cpp) | `ruvia_example_files_static` | 默认 | 单文件响应 `c.file()`、不可变静态快照 `static_root` 与 `c.static_file()`、定时刷新 `document_root`、Range、ETag 与预压缩 |
| [streaming.cpp](../examples/streaming.cpp) | `ruvia_example_streaming` | 默认 | 流式请求体 `request_body()`、流式 Multipart `request_multipart()`、分块文本流 `stream_text()` 与 SSE `stream_sse()` |
| [http_features.cpp](../examples/http_features.cpp) | `ruvia_example_http_features` | 默认 | `103 Early Hints`（`c.inform`）、RFC 9218 优先级（`c.priority`）、HTTP/2 Server Push（`c.push`）、`ORIGIN` / `ALTSVC` 通告与请求/响应 Trailer |
| [websocket.cpp](../examples/websocket.cpp) | `ruvia_example_websocket` | 默认 | WebSocket 服务端路由（`RUVIA_GET_WS`）、子协议协商、心跳与关闭握手、零拷贝帧回显 |
| [http_client.cpp](../examples/http_client.cpp) | `ruvia_example_http_client` | 默认 | 独立事件循环 HTTP 客户端 |
| [http_client_advanced.cpp](../examples/http_client_advanced.cpp) | `ruvia_example_http_client_advanced` | 默认 | 请求策略、流式响应与隧道 |
| [websocket_client.cpp](../examples/websocket_client.cpp) | `ruvia_example_websocket_client` | 默认 | 出站 WebSocket 会话 |
| [tunnels.cpp](../examples/tunnels.cpp) | `ruvia_example_tunnels` | 默认 | HTTP/1.1 & HTTP/2 `CONNECT` 字节流隧道与 RFC 9298 `CONNECT-UDP` 数据报隧道 |
| [runtime_config.cpp](../examples/runtime_config.cpp) | `ruvia_example_runtime_config` | 默认 | `.env` 加载、全局与前缀中间件、连接与入站缓冲预算限制、协商压缩、CORS、TLS / mTLS / SNI 与 HTTP 自动跳转 HTTPS |
| [ops.cpp](../examples/ops.cpp) | `ruvia_example_ops` | 默认 | `security_headers_middleware`、全局与路由级 `rate_limit`、健康与就绪探针、`trusted_proxies`、`stats()`、生命周期与日志钩子 |
| [workers_blocking.cpp](../examples/workers_blocking.cpp) | `ruvia_example_workers_blocking` | 默认 | Worker 局部状态 `use_worker_state<T>()`、跨 Worker 任务投递 `worker.post(...)` 与 `c.run_blocking(...)` 阻塞池卸载 |
| [testing.cpp](../examples/testing.cpp) | `ruvia_example_testing` | 默认 | 使用 `test_app`、`test_request`、`dispatch_response` 在内存内无 Socket 测试完整路由与中间件管线 |
| [auth_jwt.cpp](../examples/auth_jwt.cpp) | `ruvia_example_auth_jwt` | `RUVIA_ENABLE_JWT` | HMAC-SHA256/384/512 JWT 签发 `jwt_sign`、Bearer 提取 `jwt_bearer_token`、值语义校验 `jwt_verify` 与鉴权中间件 |
| [database.cpp](../examples/database.cpp) | `ruvia_example_database` | `MARIADB` 或 `POSTGRESQL` | 数据库连接池配置、启动迁移 `db_migrator::run`、参数化查询、编译期 SQL 校验模板、流式游标 `query_stream` 与事务 |
| [sql_queries.cpp](../examples/sql_queries.cpp) | `ruvia_example_sql_queries` | `MARIADB` 或 `POSTGRESQL` | 离线 SQL 构建器 `db_query`、Schema DDL 构建器 `db_schema`、PL/pgSQL 过程块 `db_procedure` 与 TimescaleDB 策略生成 |
| [orm.cpp](../examples/orm.cpp) | `ruvia_example_orm` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL 强类型 ORM：实体与投影宏、Upsert / FindAndCount / Update / Increment、`query_builder`、窗口函数、`RETURNING`、事务抢占与 Redis 查询缓存 |
| [orm_columns.cpp](../examples/orm_columns.cpp) | `ruvia_example_orm_columns` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL `STORED` 生成列与事务隔离级别（`repeatable_read`）/ 只读事务模式（`read_only`） |
| [orm_relations.cpp](../examples/orm_relations.cpp) | `ruvia_example_orm_relations` | `RUVIA_ENABLE_POSTGRESQL` | PostgreSQL ORM 实体关系：一对多、多对一、一对一、多对多（含连接表）、`.relations_` 预加载与 `left_join_and_select` |
| [event_loop_data.cpp](../examples/event_loop_data.cpp) | `ruvia_example_event_loop_data` | `POSTGRESQL` + `REDIS` | 在自管 `event_loop_pool` 上独立运行 `db_client` 与 `redis_client` 并执行 ORM 操作 |
| [sessions.cpp](../examples/sessions.cpp) | `ruvia_example_sessions` | `RUVIA_ENABLE_REDIS` | 基于 Redis 的服务端会话中间件 `session_middleware`、自动轮换会话 ID 与 `csrf_middleware` |
| [redis.cpp](../examples/redis.cpp) | `ruvia_example_redis` | `RUVIA_ENABLE_REDIS` | Redis 默认与命名别名池、String/Hash/List/Set/ZSet、SCAN 游标、`pipeline()`、`transaction()`、Lua 脚本与 `blpop` / `brpop` |
| [redis_orm.cpp](../examples/redis_orm.cpp) | `ruvia_example_redis_orm` | `RUVIA_ENABLE_REDIS` | `RUVIA_REDIS_ENTITY` 声明、RediSearch 索引创建、带 TTL 写入与条件/排序分页检索 |
| [redis_streams.cpp](../examples/redis_streams.cpp) | `ruvia_example_redis_streams` | `RUVIA_ENABLE_REDIS` | Redis Streams 消费者组、`xread_group` 阻塞拉取与通用 `command(...)` 接口 |
| [redis_pubsub.cpp](../examples/redis_pubsub.cpp) | `ruvia_example_redis_pubsub` | `RUVIA_ENABLE_REDIS` | Redis `publish` 与独占连接 `subscription()` 订阅频道/模式并实时推送到 SSE 流 |
