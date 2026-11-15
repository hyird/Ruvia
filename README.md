# Ruvia

Ruvia 是基于 C++20 协程的 HTTP/Web 框架，提供协程路由处理、声明式类型化模型、中间件、流式传输、WebSocket、HTTP 隧道，以及可选的 SQL（MariaDB / PostgreSQL）、ORM、Redis 和 JWT 支持。

| 库目录 | CMake Target | 职责与公开依赖 |
| --- | --- | --- |
| `ruvia-core` | `ruvia::core` | 协程任务、事件循环、有界通道与阻塞卸载池；依赖 standalone Asio、`Threads::Threads` 与 mimalloc。 |
| `ruvia-http` | `ruvia::http` | 独立 sans-I/O 的 HTTP/1.0、HTTP/1.1、可选 HTTP/2 与 HTTP/3、内容压缩及 WebSocket 协议层；HTTP/2 会话与 HPACK 使用 nghttp2，HTTP/3 的 QPACK/QUIC 使用 nghttp3 与 ngtcp2。 |
| `ruvia-web` | `ruvia::web` | 服务端、出站客户端、路由、中间件、Model/ORM、TLS 与数据接入；依赖 `ruvia::core`、`ruvia::http` 与 OpenSSL 3.0+，HTTP/3 通过 ngtcp2 的 OpenSSL 适配层接入 QUIC TLS。 |

## 构建与接入

需要 CMake 3.28+、支持 C++20 的编译器，以及已引导的 vcpkg 检出目录（设置环境变量 `VCPKG_ROOT`）。Linux 可使用 glibc 或项目的 musl 工具链；Windows 要求 MSVC 与静态运行时。

构建选项、依赖要求与安装包/子项目接入统一见[构建、安装与 CMake 接入](doc/README.md#2-构建安装与-cmake-接入)。

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

最小 HTTP 服务片段（完整可运行示例见 [examples/basic_http.cpp](examples/basic_http.cpp)）：

```cpp
#include "app.h"
#include "controller.h"

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


## 示例导航与文档

- 基础与 Web 服务：从 [basic_http.cpp](examples/basic_http.cpp) 开始，进阶用法见 [api_surface.cpp](examples/api_surface.cpp)、[models_validation.cpp](examples/models_validation.cpp)、[middleware_next.cpp](examples/middleware_next.cpp)、[files_static.cpp](examples/files_static.cpp)、[runtime_config.cpp](examples/runtime_config.cpp)、[ops.cpp](examples/ops.cpp)、[workers_blocking.cpp](examples/workers_blocking.cpp)、[request_policies.cpp](examples/request_policies.cpp) 与 [testing.cpp](examples/testing.cpp)。
- 流式、HTTP/2、WebSocket 与隧道：见 [streaming.cpp](examples/streaming.cpp)、[http_features.cpp](examples/http_features.cpp)、[websocket.cpp](examples/websocket.cpp) 与 [tunnels.cpp](examples/tunnels.cpp)。
- 独立入站与外部事件循环接管：使用 [`http_server` / `http_server_group`](doc/web-guide.md#19-独立入站宿主http_serverh)；原始出站 TCP/TLS 使用 [`tcp_client`](doc/web-guide.md#110-原始-tcptls-出站tcp_clienth)，均无需 `application`、路由或控制器。
- 出站 HTTP、WebSocket 与隧道统一使用 `c.client(alias)` 的链式 API；独立事件循环使用 `http_client(loop, config)` 或多 origin 配置集合。见 [客户端手册](doc/streaming-websocket.md#5-出站-http-客户端)、[http_client.cpp](examples/http_client.cpp)、[http_client_advanced.cpp](examples/http_client_advanced.cpp) 与 [websocket_client.cpp](examples/websocket_client.cpp)。
- 数据库、ORM、Redis 与 JWT（需启用对应 CMake 选项）：见 [database.cpp](examples/database.cpp)、[sql_queries.cpp](examples/sql_queries.cpp)、[orm.cpp](examples/orm.cpp)、[orm_columns.cpp](examples/orm_columns.cpp)、[orm_relations.cpp](examples/orm_relations.cpp)、[redis.cpp](examples/redis.cpp)、[redis_orm.cpp](examples/redis_orm.cpp)、[redis_streams.cpp](examples/redis_streams.cpp)、[redis_pubsub.cpp](examples/redis_pubsub.cpp)、[sessions.cpp](examples/sessions.cpp)、[event_loop_data.cpp](examples/event_loop_data.cpp) 与 [auth_jwt.cpp](examples/auth_jwt.cpp)。
- 浏览 [examples](examples) 查看全部示例源码，构建 target 列表见 [examples/CMakeLists.txt](examples/CMakeLists.txt)。
- 用户使用指南与公开 API 文档见 [doc/README.md](doc/README.md)。

## 许可证

[MIT License](LICENSE)。
