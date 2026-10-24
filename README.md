# Ruvia

Ruvia 是基于 C++20 协程的 HTTP/Web 框架，提供协程路由处理、声明式类型化模型、中间件、流式传输、WebSocket、HTTP 隧道、出站客户端，以及可选的 SQL（MariaDB / PostgreSQL）、ORM、Redis 和 JWT 支持。

| 库目录 | CMake Target | 职责与公开依赖 |
| --- | --- | --- |
| `ruvia-core` | `ruvia::core` | 协程任务、事件循环、有界通道与阻塞卸载池；依赖 standalone Asio、`Threads::Threads` 与 mimalloc（不启用 `override` 特性）。 |
| `ruvia-http` | `ruvia::http` | 独立 sans-I/O 的 HTTP/1.0、HTTP/1.1、HTTP/2、内容压缩与 WebSocket 协议层；依赖 zlib、Brotli 与 Zstandard。 |
| `ruvia-web` | `ruvia::web` | 服务端、路由、中间件、Model/ORM、TLS、出站客户端与数据接入；依赖 `ruvia::core`、`ruvia::http` 与 OpenSSL 3.0+。 |

## 构建与接入

需要 CMake 3.28+、支持 C++20 的编译器，以及已引导的 vcpkg 检出目录（设置环境变量 `VCPKG_ROOT`）。Linux 可执行文件要求 glibc；Windows 构建要求 MSVC，并使用 `x64-windows-static` triplet 与静态运行时（`MultiThreaded$<$<CONFIG:Debug>:Debug>`）。

顶层构建在未指定 `CMAKE_TOOLCHAIN_FILE` 时从 `VCPKG_ROOT` 自动选择 vcpkg 工具链，并按启用的构建选项选择 [vcpkg.json](vcpkg.json) 中的依赖特性；作为子项目或外部包接入时，宿主工程自行提供工具链与依赖清单。

| CMake 选项 | 默认值 | 说明与边界 |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | 构建 `ruvia::core`。 |
| `RUVIA_BUILD_HTTP` | `ON` | 构建 `ruvia::http`。 |
| `RUVIA_BUILD_WEB` | `ON` | 构建 `ruvia::web`（要求 `RUVIA_BUILD_CORE=ON` 且 `RUVIA_BUILD_HTTP=ON`）。 |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | 构建 `examples/` 示例程序（要求 `RUVIA_BUILD_WEB=ON`）。 |
| `RUVIA_BUILD_TESTS` | `OFF` | 构建 `tests/` 测试套件。 |
| `RUVIA_ENABLE_MARIADB` | `OFF` | 启用 MariaDB 驱动与公开数据库 API（依赖 `libmariadb`）。 |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | 启用 PostgreSQL 驱动、ORM 与公开数据库 API（依赖 `libpq`）。 |
| `RUVIA_ENABLE_REDIS` | `OFF` | 启用 Redis 客户端、Pub/Sub、Streams、Redis ORM、服务端会话与数据库查询缓存（依赖 `hiredis`）。 |
| `RUVIA_ENABLE_JWT` | `OFF` | 启用 JWT 签发与校验 API。 |

未启用的可选功能不编译对应源码，也不安装对应的公开头文件目录（`db/`、`redis/`、`auth/`）。

安装后的包通过 `find_package(ruvia REQUIRED COMPONENTS core http web)` 与对应的 `ruvia::` target 接入（请求 `web` 组件会自动引入 `core` 与 `http`），消费方工具链须提供相同的第三方依赖。MSVC 消费方需在 `project()` 之前设置匹配的静态 `CMAKE_MSVC_RUNTIME_LIBRARY`。若控制器编译在独立的 `STATIC_LIBRARY` 或 `OBJECT_LIBRARY` 中，使用包配置提供的 `ruvia_link_controllers(<app_target> <controller_target>...)` 确保静态路由注册不被链接器丢弃。

## 快速开始与最小上手

Linux（POSIX shell，已设置 `VCPKG_ROOT`）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release -j$(nproc)
ctest --test-dir build -C Release --output-on-failure
./build/examples/ruvia_example_basic_http
```

Windows（PowerShell，已安装 MSVC 并设置 `$env:VCPKG_ROOT`）：

```powershell
cmake -S . -B build -DRUVIA_BUILD_TESTS=ON -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Release "-j$([Environment]::ProcessorCount)"
ctest --test-dir build -C Release --output-on-failure
.\build\examples\Release\ruvia_example_basic_http.exe
```

示例服务监听 `http://127.0.0.1:8080/api/hello`，按 Ctrl+C 停止。

最小 HTTP 服务片段（完整可运行示例见 [examples/web/basic_http.cpp](examples/web/basic_http.cpp)）：

```cpp
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

class basic_http_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN(basic_http_controller)
    RUVIA_GET("/hello", hello);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> hello(ruvia::context& c) {
        co_return c.text(ruvia::static_text("hello from ruvia\n"));
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .run();
}
```

首次接入通用规则：

- 通过进程级单例 `ruvia::app()` 配置应用，在调用 `run()` 前完成监听、中间件、错误处理与后端注册。
- 路由处理器与异步接口统一返回 `ruvia::task<t_type>`。任务默认惰性执行；一旦启动必须 `co_await` 或 `join` 直至完成，取消后同样如此。
- 请求与消息上的视图仅借用所属 owner，不延长其生命周期。跨请求生命周期保存或投递到阻塞卸载池前必须复制为自有数据；阻塞卸载任务不得访问 `ruvia::context` 或 worker 局部状态。
- 服务端与出站客户端支持基于 TCP 和 TLS 的 HTTP/1.0、HTTP/1.1 与 HTTP/2：TLS 通过 ALPN 协商 HTTP/2，明文 HTTP/2 使用 prior knowledge（不支持 HTTP/1 `Upgrade: h2c`）；WebSocket 使用 HTTP/1.1 `Upgrade` 或 HTTP/2 Extended `CONNECT`。
- 服务端每个业务 worker 独立绑定监听器，连接始终留在接受它的 worker 上。Linux 等 POSIX 平台要求 `SO_REUSEADDR` + `SO_REUSEPORT`，不可用则启动失败；Windows 使用 `SO_REUSEADDR` 绑定并通过共享监听句柄矩阵发起原生 `AcceptEx`，不保证严格均衡或端口独占。

## 示例导航与文档

- 基础与 Web 服务：从 [basic_http.cpp](examples/web/basic_http.cpp) 开始，进阶用法见 [api_surface.cpp](examples/web/api_surface.cpp)、[models_validation.cpp](examples/web/models_validation.cpp)、[middleware_next.cpp](examples/web/middleware_next.cpp)、[files_static.cpp](examples/web/files_static.cpp)、[runtime_config.cpp](examples/web/runtime_config.cpp)、[ops.cpp](examples/web/ops.cpp)、[workers_blocking.cpp](examples/web/workers_blocking.cpp)、[request_policies.cpp](examples/web/request_policies.cpp) 与 [testing.cpp](examples/web/testing.cpp)。
- 流式、HTTP/2、WebSocket、隧道与客户端：见 [streaming.cpp](examples/web/streaming.cpp)、[http_features.cpp](examples/web/http_features.cpp)、[websocket.cpp](examples/web/websocket.cpp)、[websocket_client.cpp](examples/web/websocket_client.cpp)、[http_client.cpp](examples/web/http_client.cpp)、[http_client_advanced.cpp](examples/web/http_client_advanced.cpp) 与 [tunnels.cpp](examples/web/tunnels.cpp)（HTTP/2 客户端示例可传入参数 `2` 运行）。
- 数据库、ORM、Redis 与 JWT（需启用对应 CMake 选项）：见 [database.cpp](examples/web/database.cpp)、[sql_queries.cpp](examples/web/sql_queries.cpp)、[orm.cpp](examples/web/orm.cpp)、[orm_columns.cpp](examples/web/orm_columns.cpp)、[orm_relations.cpp](examples/web/orm_relations.cpp)、[redis.cpp](examples/web/redis.cpp)、[redis_orm.cpp](examples/web/redis_orm.cpp)、[redis_streams.cpp](examples/web/redis_streams.cpp)、[redis_pubsub.cpp](examples/web/redis_pubsub.cpp)、[sessions.cpp](examples/web/sessions.cpp)、[event_loop_data.cpp](examples/web/event_loop_data.cpp) 与 [auth_jwt.cpp](examples/web/auth_jwt.cpp)。
- 浏览 [examples/web](examples/web) 查看全部示例源码，构建 target 列表见 [examples/CMakeLists.txt](examples/CMakeLists.txt)。
- 用户使用指南与公开 API 文档见 [doc/README.md](doc/README.md)。

## 许可证

[MIT License](LICENSE)。
