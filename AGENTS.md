# AGENTS.md

Ruvia 项目协作说明。默认用中文回复。本文件只记录稳定的仓库约束；实现细节、迁移历史和具体类型清单由代码、测试与边界守卫维护，不在这里逐轮追加。

README 面向使用者，说明构建、安装和公开能力；AGENTS 面向贡献者，说明目录、分层、性能和验证规则。不要在两个文件中重复记录同一内部实现。

## 项目定位

Ruvia 是 C++23 HTTP/Web 框架仓库，采用 monorepo + 多 CMake target：

```text
ruvia-core  -> ruvia::core
ruvia-http  -> ruvia::http
ruvia-web   -> ruvia::web
```

依赖方向固定：

```text
ruvia-web  -> ruvia-core + ruvia-http
```

新代码、新示例和新文档使用 `ruvia::web`，不保留历史 Web 框架别名。

## 沟通规则

- 默认中文回复。
- 不要回退、覆盖或整理用户已有改动，除非用户明确要求。
- 需求不清时只问一个必要问题；能从仓库上下文判断时直接执行。
- 讨论协议行为时，以 HTTP、TLS、WebSocket、SSE、HTTP/2 相关 RFC 和标准优先。
- 如果项目约束与协议标准冲突，优先修实现和文档以符合标准。
- README 不写内部重构历史；AGENTS 不累积逐类型防回归目录。

## 目录规则

顶层源码目录只允许：

```text
ruvia-core/
ruvia-http/
ruvia-web/
examples/
tests/
```

仓库根目录不保留源码级 `include/`、`src/`、`fuzz/`、`core/`、`http/`、`web/` 或 `edge/`。

每个库目录必须自带：

```text
<target>/
  CMakeLists.txt
  include/
  src/
```

三个 target 的公开头和安装命名根严格对应：

- `ruvia-core` 只能拥有并安装 `include/ruvia/core/**`。
- `ruvia-http` 只能拥有并安装 `include/ruvia/http/**`。
- `ruvia-web` 只能拥有并安装 `include/ruvia/web/**`。

禁止在本 target 下创建或安装到另一个 target 的命名根，也禁止在 CMake source/header 列表中直接加入另一个 target 目录里的文件。

跨 target 复用的编译期契约头放在所属 target 的 `include/ruvia/<target>/detail/`。禁止把另一个 target 的 `src/` 加入 include path，也禁止通过物理相对或绝对路径包含另一个 target 的源码或私有头。target 之间只能通过 `target_link_libraries()` 传播的公开 include interface 使用依赖方已安装的头。

`src/` 下最多保留一层业务分类目录，例如 `server/`、`http2/`、`websocket/`、`client/`；不要引入 `src/net/...`、`src/*/core/...` 等重复层级。`ruvia-core/src/` 保持扁平。`src/` 只保存实现和 target 自有 `pch.h`，契约头统一放在公开 `detail/` 根。

根 `CMakeLists.txt` 只负责全局选项、依赖发现、package export、install helper 和 `add_subdirectory(...)`。不要再拆出额外的仓库内 `.cmake` 片段。

本地工具目录 `.codex/`、`.claude/`、`.agents/`、`.codegraph/` 必须保持 ignored，不作为源码提交。

## Target 边界

### ruvia-core

`ruvia-core` 是可独立使用的 runtime 底座。

可以包含：

- `ruvia::Task<T>`、coroutine promise/awaiter、Asio awaiter/driver glue。
- PMR、memory resource、mimalloc 包装、对象生命周期 helper。
- worker/request memory、connection scanner、socket/runtime helper。
- ASCII、base64/base64url、constant-time、number/path 等小型通用 helper。

禁止包含：

- HTTP/Web 语义。
- App、Context、Controller、Router、middleware、model、DB、Redis、JWT。
- 对 HTTP/Web 协议语义、OpenSSL、zlib、brotli、zstd、MariaDB、hiredis 的公开依赖。

### ruvia-http

`ruvia-http` 是可独立使用的纯协议 target，不依赖 `ruvia-core`、Asio、socket 或 Ruvia runtime。

可以包含：

- HTTP method/status/header/request/response 类型。
- HTTP/1 parser、chunk parser、request target parser。
- cookie、cache、range、conditional request、content negotiation、header token/value helper。
- multipart、form、URL encoding、SSE formatting 与纯 parser。
- HTTP/1 与 HTTP/2 sans-I/O 协议状态、HPACK、WebSocket sans-I/O 核心。
- content-coding、framing、connection、client role 等可由任意 runtime 驱动的纯协议 primitive。
- 无分配的 `HttpProtocolError` 及其 HTTP status；不得携带 Web JSON error code/details。

禁止包含：

- App、Context、Controller、Router、route macro、middleware、Next。
- Model/validation 宏。
- DB、Redis、JWT、CSRF、Session、CORS、安全头、RateLimit 的 Web 集成。
- `HttpErrorInfo`、`HttpError`、默认 JSON 错误 envelope、自定义 error handler。
- 通用 JSON/model serialization、健康检查或校验错误 JSON。
- 静态文件 MIME 推断、文件时间/ETag、runtime 文件读缓冲。
- origin/cache/purge/rule 等产品策略。

### ruvia-web

`ruvia-web` 是完整 Web 框架产品，依赖 `ruvia::core` 和 `ruvia::http`。

包含：

- App 配置和启动。
- Context、Controller、Router、middleware、Next、route macro。
- HTTP server runtime、TLS、HTTP/2 server、WebSocket route、response streaming。
- Model、JSON/form parsing/serialization、validation middleware。
- `HttpErrorInfo`、`HttpError`、JSON 错误响应和自定义 error/not-found handler。
- Session、CSRF、RateLimit、CORS、安全头、静态文件、AutoHTTPS redirect。
- 可选 MariaDB、Redis、JWT 集成。

不得把 Web-only API 下沉到 `ruvia-http`。

### HTTP 协议与应用边界

`ruvia-http` 拥有 wire/message/framing/connection 语义，以及跨 server/client/runtime 复用的 sans-I/O 状态机和纯协议 helper。HTTP/1、HTTP/2、WebSocket、SSE、multipart、content-coding 等协议实现留在 `ruvia-http`。

`ruvia-web` 拥有 HTTP 之上的 App、Context、Router、middleware、controller、validation、session、CSRF、JWT、rate limit、CORS、安全头、静态文件产品策略、AutoHTTPS、DB/Redis 和 WebSocket route 绑定。读取或设置 HTTP header 不等于拥有协议语义。

边界判断：

- 决定字节如何解析、分帧、序列化，连接是否保持，升级是否成立，协议失败对应哪个 HTTP status：放在 `ruvia-http`。
- 决定协议失败如何变成应用错误/JSON，或 Web 产品、路由、中间件、配置执行何种策略：放在 `ruvia-web`。

Router/error handler 不得设置 `Connection: close` 或接收 `closeConnection` 参数；HTTP/1 runtime 在知道 request-body 与 persistence 状态后统一最终化连接语义。

流式响应的 HTTP 版本、framing、复用与响应信号由 `ruvia-http` 的 `Http1ResponseStreamPlan` 统一产出；`ruvia-web` 只传入请求上限等外部关闭策略并驱动计划。响应 body 许可由 `HttpResponseBodyPlan` 决定，buffered 响应再由 `HttpBufferedResponseWritePlan` 绑定 representation length；HTTP/1、HTTP/2 和 streaming 不得在 Web 层用 `skipBody` 等布尔值重判。HEAD 保留 GET representation metadata 和长度，但 HTTP/1 不发 payload、HTTP/2 不发 DATA。

`Http2Connection` 必须记录本地 `END_STREAM`，之后的 `submitData()` 必须拒绝。Web 只能用 core runtime、Asio/TLS/socket/timeouts 驱动 HTTP 协议 core，不得复制协议判断。

## 性能原则

- 请求热路径目标是 0 抽象成本。
- 启动期可以使用注册表、工厂、虚函数和一次性构建。
- 请求期不要新增 mutex、rwlock、spinlock、共享原子争用、type-erasure、`shared_ptr` 分配或不必要拷贝。
- 优先使用 per-worker 所有权、连接私有状态、启动期构建后只读数据。
- 跨线程操作连接状态默认禁止；必须先设计明确的 worker mailbox 或 intrusive MPSC 边界。

## 线程和运行时

- 每个 worker 拥有一个 standalone Asio `io_context`。
- 连接不能跨线程迁移。
- `Task` 是 lazy structured coroutine owner：未启动任务可以丢弃，已启动任务必须在所属执行上下文运行到完成；取消只能显式请求后 await/join，禁止通过析构销毁或静默 detach 挂起中的协程帧。
- `App::run()` 创建 acceptor/server/thread per worker。
- 非 Windows 平台要求 `SO_REUSEPORT`；Windows 使用 `SO_REUSEADDR`。
- graceful shutdown 只能在各 worker 自己的 `io_context` 上关闭 acceptor 和活跃 socket。
- idle/header/body/write timeout、连接数限制和请求数限制保持 per-worker 所有权。

## 内存规则

- 框架内部拥有动态内存的对象默认使用 PMR 容器。
- 公开 API 输入优先使用 `std::string_view`、`std::span`、`std::filesystem::path` 或值类型配置。
- 请求热路径 PMR 容器使用请求 arena；Worker 层容器使用 `WorkerMemory`。
- 启动期容器使用 mimalloc-backed 默认 resource。
- 不要让 `std::pmr::new_delete_resource()` 成为生产默认路径。
- `Context::text(std::string&)`、`std::string&&`、`const std::string&` 入口保持 deleted。

## HTTP 解析和响应

- 请求解析走 Ruvia 自研 zero-copy parser；method/path/version/header 默认借用连接读缓冲。
- header 上限 64KB，普通 body 上限 16MB。
- chunked 请求体在连接读缓冲中原地解码。
- 普通 route dispatch 前完整读取 body；大 body 必须显式使用 stream route。
- stream body reader 返回的 view 只保证有效到下一次 `read()`。
- 响应写出使用固定 header buffer + scatter-gather I/O，不拼接完整 response 字符串。
- 文件响应不全量读入内存；plain TCP 优先平台零拷贝。
- response streaming 和 WebSocket 必须通过显式 route macro 注册。
- 普通路由的 `HttpResponse` 只允许空、bytes 或 file body；响应流必须走 `ResponseStreamWriter`，不得增加动态/类型擦除旁路。

## 路由和中间件

- 路由注册只允许通过 controller/group/route 宏完成。
- 不暴露直接 `Router::addRoute(...)` 或 `Router::group(...)` API。
- 路由表、中间件链、controller factory 在 worker 启动前构建完成。
- 请求期不得重建 route index、middleware chain 或 `std::function` 链。
- 重复 method + path 或等价动态 route shape 必须启动期报错。
- 无显式 HEAD route 时 fallback 到普通 GET；streaming GET 不参与隐式 HEAD fallback。
- middleware API 保持 CRTP + async `handle(Context&, Next&)`；`next()` 是 single-shot。

## Controller API

- 普通 handler：`ruvia::Task<ruvia::HttpResponse> handler(ruvia::Context& c)`。
- streaming/WebSocket handler：`ruvia::Task<void> handler(ruvia::Context& c)`。
- 公开协程返回类型统一是 `ruvia::Task<T>`，不暴露 `asio::awaitable<T>`。
- 请求统一走 `c.req()`；连接元数据通过 `getConnInfo(c)` 读取。
- `HttpRequest`、`ContextRequest`、`RawRequestClone` 不保存 remote address、TLS 状态或证书身份。
- 响应 metadata 走 `c.status(...)`、`c.header(...)`、`c.setCookie(...)`。
- 响应构造走 `c.body(...)`、`c.text(...)`、`c.html(...)`、`c.json(...)`、`c.file(...)`、`c.staticFile(...)`、`c.redirect(...)`、`c.error(...)`。
- 一个公开操作只保留一个名字，不新增别名。

## Model 和校验

- `RUVIA_REQUEST_MODEL` 是请求解析和校验 schema；`RUVIA_RESPONSE_MODEL` 只约束响应字段类型并生成 JSON，不参与请求解析或校验。
- 字段必须使用 Ruvia 模型类型，不使用 raw `std::string`、`std::vector`、`std::string_view` 或基础整数。
- 校验规则通过 route validation middleware 声明，不写进 `RUVIA_FIELD`。
- 请求 JSON 只嵌套请求模型，响应 JSON 只嵌套响应模型；两者都支持数组。form 只支持扁平 key-value 基础字段。
- validation 不应为 invalid type 或 duplicate 再扫描 body。
- 同一 `RUVIA_PATTERN` 只能编译一次并复用。

## CMake 和安装

- `RUVIA_BUILD_CORE`、`RUVIA_BUILD_HTTP`、`RUVIA_BUILD_WEB` 默认均为 `ON`。
- `RUVIA_BUILD_WEB=ON` 要求 core 与 HTTP 同时启用；core-only/http-only 不得查找或安装未选组件依赖。
- MariaDB、PostgreSQL、Redis、JWT 是严格 feature：`RUVIA_ENABLE_MARIADB`、`RUVIA_ENABLE_POSTGRESQL`、`RUVIA_ENABLE_REDIS`、`RUVIA_ENABLE_JWT`。
- outbound HTTP client 只保留在 `ruvia-http` 的 sans-I/O API；`ruvia-web` 不提供 client socket/TLS runtime、连接池、`fetch`、`proxy` 或反向代理集成。
- 安装包暴露 `ruvia::core`、`ruvia::http`、`ruvia::web`，不暴露历史别名。
- 下游按需请求 `web`、`core` 或 `http` component；消费示例只放在 README。

## 验证要求

改动完成前至少运行任务相关的最小验证。

目录、文档、CMake 清理：

```powershell
git diff --check
rg -n '<stale split terms>' README.md AGENTS.md CMakeLists.txt ruvia-core ruvia-http ruvia-web tests examples
```

构建、测试和安装：

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build/install
```

不要提交 `build/`、`vcpkg_installed`、本地工具目录或 CodeGraph 索引。
