# Ruvia

Ruvia 是基于 C++20 协程的 HTTP/Web 框架，提供协程路由处理、声明式类型化模型、中间件、流式传输、WebSocket、HTTP 隧道，以及可选的 SQL（MariaDB / PostgreSQL）、ORM、Redis 和 JWT 支持。

| 库目录 | CMake Target | 职责与公开依赖 |
| --- | --- | --- |
| `ruvia-core` | `ruvia::core` | 协程任务、事件循环、有界通道与阻塞卸载池；依赖 standalone Asio、`Threads::Threads` 与 mimalloc。 |
| `ruvia-http` | `ruvia::http` | 独立 sans-I/O 的 HTTP/1.0、HTTP/1.1、可选 HTTP/2 与 HTTP/3、内容压缩及 WebSocket 协议层；HTTP/2 会话与 HPACK 使用 nghttp2，HTTP/3 的 QPACK/QUIC 使用 nghttp3 与 ngtcp2。 |
| `ruvia-web` | `ruvia::web` | 服务端、出站客户端、路由、中间件、Model/ORM、TLS 与数据接入；依赖 `ruvia::core`、`ruvia::http` 与 OpenSSL 3.0+，HTTP/3 通过 ngtcp2 的 OpenSSL 适配层接入 QUIC TLS。 |

## 构建与接入

需要 CMake 3.28+、支持 C++20 的编译器，以及已引导的 vcpkg 检出目录（设置环境变量 `VCPKG_ROOT`）。Linux 可执行文件要求 glibc；Windows 构建要求 MSVC，并使用 `x64-windows-static` triplet 与静态运行时（`MultiThreaded$<$<CONFIG:Debug>:Debug>`）。

顶层构建在未指定 `CMAKE_TOOLCHAIN_FILE` 时从 `VCPKG_ROOT` 自动选择 vcpkg 工具链，并按启用的构建选项选择 [vcpkg.json](vcpkg.json) 中的依赖特性；作为子项目或外部包接入时，宿主工程自行提供工具链与依赖清单。

依赖版本由 `vcpkg.json` 的 `builtin-baseline` 固定，CI 使用同一提交；HTTP/3 要求 ngtcp2 1.26.0 或更新版本。更新依赖时先更新并重新引导 vcpkg，再更新清单基线、重新运行 CMake 配置与构建；不要只替换静态库而保留旧头文件。

| CMake 选项 | 默认值 | 说明与边界 |
| --- | --- | --- |
| `RUVIA_BUILD_CORE` | `ON` | 构建 `ruvia::core`。 |
| `RUVIA_BUILD_HTTP` | `ON` | 构建 `ruvia::http`。 |
| `RUVIA_BUILD_WEB` | `ON` | 构建 `ruvia::web`（要求 `RUVIA_BUILD_CORE=ON` 且 `RUVIA_BUILD_HTTP=ON`）。 |
| `RUVIA_BUILD_EXAMPLES` | `OFF` | 构建 `examples/` 示例程序（要求 `RUVIA_BUILD_WEB=ON`）。 |
| `RUVIA_BUILD_TESTS` | `OFF` | 构建 GoogleTest 测试套件并通过 CTest 运行（依赖 `gtest`）。 |
| `RUVIA_ENABLE_HTTP2` | `ON` | 启用 HTTP/2 公开 API、实现与测试（依赖 `nghttp2`）。 |
| `RUVIA_ENABLE_HTTP3` | `ON` | 启用 HTTP/3/QUIC 公开 API、服务端 `listen_config::http3_`、实现与测试（依赖 `nghttp3`、`ngtcp2`；Web 同时使用 `ngtcp2[openssl]`）；关闭时配置 `http3_` 启动失败。 |
| `RUVIA_ENABLE_MARIADB` | `OFF` | 启用 MariaDB 驱动与公开数据库 API（依赖 `libmariadb`）。 |
| `RUVIA_ENABLE_POSTGRESQL` | `OFF` | 启用 PostgreSQL 驱动、ORM 与公开数据库 API（依赖 `libpq`）。 |
| `RUVIA_ENABLE_REDIS` | `OFF` | 启用 Redis 客户端、Pub/Sub、Streams、Redis ORM、服务端会话与数据库查询缓存（依赖 `hiredis`）。 |
| `RUVIA_ENABLE_JWT` | `OFF` | 启用 JWT 签发与校验 API。 |

未启用的可选功能不编译对应源码，也不安装对应的公开头文件或目录；HTTP/2 与 HTTP/3 可独立关闭，数据库、Redis、JWT 分别对应 `db/`、`redis/`、`auth/`。

安装后的包通过 `find_package(ruvia REQUIRED COMPONENTS core http web)` 与对应的 `ruvia::` target 接入（请求 `web` 组件会自动引入 `core` 与 `http`），消费方工具链须提供相同的第三方依赖：非 Windows 使用启用 `override` 的静态 mimalloc，不得同时使用另一套全局 allocator replacement；Windows 使用不带该特性的普通 mimalloc 依赖，不做全局分配接口覆盖。MSVC 消费方需在 `project()` 之前设置匹配的静态 `CMAKE_MSVC_RUNTIME_LIBRARY`。独立 `ruvia::http` 不依赖 mimalloc。

若控制器编译在独立的 `STATIC_LIBRARY` 或 `OBJECT_LIBRARY` 中，使用包配置提供的 `ruvia_link_controllers(<app_target> <controller_target>...)` 确保静态路由注册不被链接器丢弃。

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

Visual Studio 多配置构建（包括安装包消费方）无需设置 `CMAKE_BUILD_TYPE`；HTTP/2 与 QUIC 的查找模块按配置选择同一依赖安装中的 `lib` 或 `debug/lib`，避免 Release 链入 Debug 静态运行时。

本地构建默认不启用 LTO；CI 的 Release 构建显式设置
`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON`。
测试可执行文件支持 GoogleTest 的 `--gtest_list_tests` 与
`--gtest_filter=Ruvia.<测试名>`；CTest 保持按可执行文件隔离进程级夹具。

真实网络测试通过 `tests/web/support/tcp_port_reservation.h` 选择 loopback
临时端口并持有全局可见的保留，直到服务器停止并 join；预留 socket 仅 bind、
不 listen，避免抢走测试连接。POSIX 选端口后以 `SO_REUSEPORT` 共享并保持绑定；
Windows 的 Acceptor 以 `SO_EXCLUSIVEADDRUSE` 独占监听，因此选端口后释放 socket，
仅持有全局具名端口租约，其他夹具遇到该租约会重新选端口，租约持续至服务器退出。
禁止不持有租约的“bind 0 → close → 启动服务器”端口预选：并行进程可能选中同一端口。
此测试夹具约束不改变生产监听器的绑定语义。

Windows 测试需配对运行 Debug 与 Release：Debug 保留默认 STL 迭代器诊断，
Release 执行完整分配失败扫描。MSVC 调试 STL 会在空容器构造及移动的
`noexcept` 路径分配迭代器代理；测试不能按分配大小或序号猜测并跳过代理，
也不能向这些路径注入 `bad_alloc`。此类扫描在 Debug 仍执行成功路径、值语义、
所有权及回收检查，但不注入失败；GoogleTest XML 报告通过
`stl_allocation_failure_scan` 属性标记遗漏，其他失败仍照常使测试失败。
真实可捕获的裸分配及已构造容器增长失败仍执行。Release 从首个失败位置逐点
扫描至成功，并检查回滚、重试与回收；只有 Debug 与 Release 配对结果才能声称
覆盖这两类契约。可向测试可执行文件传入 `--gtest_output=xml:report.xml` 获取报告。

```powershell
cmake --build build --config Debug "-j$([Environment]::ProcessorCount)"
ctest --test-dir build -C Debug -V
cmake --build build --config Release "-j$([Environment]::ProcessorCount)"
ctest --test-dir build -C Release --output-on-failure
```

上述命令适用于默认多配置生成器。单配置生成器需使用各自独立的 Debug/Release
构建目录。无需额外建立关闭迭代器诊断的 Debug 测试目标；若另行选择
`_ITERATOR_DEBUG_LEVEL=0`，必须让应用、Ruvia 库、测试及所有跨 STL ABI 边界的
依赖保持一致，不能只给测试翻译单元添加定义。不得为测试全局关闭 Debug STL。

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
- 服务端支持基于 TCP 和 TLS 的 HTTP/1.0、HTTP/1.1 与 HTTP/2：TLS 通过 ALPN 协商 HTTP/2，明文 HTTP/2 使用 prior knowledge（RFC 9113 §3.1 已废弃并移除 HTTP/1 `Upgrade: h2c`）；WebSocket 使用 HTTP/1.1 `Upgrade` 或 HTTP/2、HTTP/3 Extended `CONNECT`。
- HTTPS 监听器可用 `listen_config::http3_` 在 UDP 端口（可与 `https_` 同号）提供 HTTP/3，并自动通告 Alt-Svc。支持显式开启的 0-RTT（`http3_early_data_`）与 QUIC Retry 地址验证（`http3_retry_`）、无状态重置（Stateless Reset）、连接迁移、CONNECT、WebSocket、CONNECT-UDP、RFC 9412 `ORIGIN` 帧与 RFC 9218 优先级动态更新；策略及配置见 [监听器文档](doc/web-guide.md#ruvialisten_config-与-ruviatls_config)。
- 服务端连接始终留在接收它的业务 worker 上，业务 worker 彼此同构。Linux 等 POSIX 平台由每个业务 worker 以 `SO_REUSEADDR` + `SO_REUSEPORT` 绑定监听器并直接 accept，不可用则启动失败；Windows 由每个应用一个独立 Acceptor 线程以 `SO_EXCLUSIVEADDRUSE` 独占全部监听器（含 HTTP/3 UDP socket），以 `AcceptEx` 接收尚未关联 IOCP 的裸 `SOCKET` 并派发给连接数最少的业务 worker，由其首次绑定自身 IOCP。`stop()` 直接断开全部连接，不设排空阶段。

## 示例导航与文档

- 基础与 Web 服务：从 [basic_http.cpp](examples/web/basic_http.cpp) 开始，进阶用法见 [api_surface.cpp](examples/web/api_surface.cpp)、[models_validation.cpp](examples/web/models_validation.cpp)、[middleware_next.cpp](examples/web/middleware_next.cpp)、[files_static.cpp](examples/web/files_static.cpp)、[runtime_config.cpp](examples/web/runtime_config.cpp)、[ops.cpp](examples/web/ops.cpp)、[workers_blocking.cpp](examples/web/workers_blocking.cpp)、[request_policies.cpp](examples/web/request_policies.cpp) 与 [testing.cpp](examples/web/testing.cpp)。
- 流式、HTTP/2、WebSocket 与隧道：见 [streaming.cpp](examples/web/streaming.cpp)、[http_features.cpp](examples/web/http_features.cpp)、[websocket.cpp](examples/web/websocket.cpp) 与 [tunnels.cpp](examples/web/tunnels.cpp)。
- 出站 HTTP、WebSocket 与隧道统一使用 `c.client(alias)` 的链式 API；独立事件循环使用 `http_client(loop, config)`。见 [客户端手册](doc/streaming-websocket.md#5-出站-http-客户端)、[http_client.cpp](examples/web/http_client.cpp)、[http_client_advanced.cpp](examples/web/http_client_advanced.cpp) 与 [websocket_client.cpp](examples/web/websocket_client.cpp)。
- 数据库、ORM、Redis 与 JWT（需启用对应 CMake 选项）：见 [database.cpp](examples/web/database.cpp)、[sql_queries.cpp](examples/web/sql_queries.cpp)、[orm.cpp](examples/web/orm.cpp)、[orm_columns.cpp](examples/web/orm_columns.cpp)、[orm_relations.cpp](examples/web/orm_relations.cpp)、[redis.cpp](examples/web/redis.cpp)、[redis_orm.cpp](examples/web/redis_orm.cpp)、[redis_streams.cpp](examples/web/redis_streams.cpp)、[redis_pubsub.cpp](examples/web/redis_pubsub.cpp)、[sessions.cpp](examples/web/sessions.cpp)、[event_loop_data.cpp](examples/web/event_loop_data.cpp) 与 [auth_jwt.cpp](examples/web/auth_jwt.cpp)。
- 浏览 [examples/web](examples/web) 查看全部示例源码，构建 target 列表见 [examples/CMakeLists.txt](examples/CMakeLists.txt)。
- 用户使用指南与公开 API 文档见 [doc/README.md](doc/README.md)。

## 许可证

[MIT License](LICENSE)。
