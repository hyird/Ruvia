# AGENTS.md

Ruvia 项目协作说明。本文档约束代理在本仓库中的沟通方式、架构边界、性能目标和验证要求。

Ruvia 是小而美的 C++23 HTTP/1.1 Web 框架，核心范围是 HTTP server、内置 HTTPS/TLS、路由/controller、用户自定义 middleware、请求/响应 helper、JSON/form/multipart、请求流式读取、响应流式/SSE、WebSocket、文件/静态文件响应、PMR/zero-copy/per-worker runtime，以及默认内置 DB query + transaction。

## 1. 沟通与优先级

### 回复方式

- 默认使用中文回复；除非用户明确要求，不要切换语言。
- 发现未明确的需求时，优先用一个简短问题确认，不要自行扩展范围。
- 不要回退、覆盖或整理用户已有改动，除非用户明确要求。

### 性能原则

- 性能第一：新增设计必须优先考虑请求热路径。
- 请求热路径目标是 0 抽象成本。
- 启动期可以使用注册表、工厂、虚函数。
- 请求阶段不要新增 mutex、`shared_ptr` 分配、type-erasure 或无必要拷贝。
- 无锁优先：请求热路径禁止新增 mutex、rwlock、spinlock 或共享原子计数争用。
- 优先使用 per-worker 所有权、启动期构建后只读数据、连接私有状态。

## 2. 运行时架构

### Worker 与线程模型

- 每个 worker 拥有且只拥有一个 standalone Asio `io_context`。
- 连接不能跨线程迁移。
- `App::run()` 创建一个 `HttpServer`/acceptor/线程 per worker。
- 不要把 `App::run()` 改成单 acceptor 分发 socket。
- `App::run()` 当前安装 SIGINT/SIGTERM graceful shutdown。
- 停止时只在各 worker 自己的 `io_context` 上关闭 acceptor 和该 worker 拥有的活跃 socket。
- 连接超时和连接数限制必须是 per-worker 所有权，不能跨线程集中管理连接。
- 不要跨线程直接操作连接。

### Socket 选项

- 非 Windows 平台要求 `SO_REUSEPORT`。
- 不可降级为单监听器。
- Windows 使用 `SO_REUSEADDR` 支持多 acceptor。
- DB 后端当前仅支持 MariaDB-compatible Connector/C；应用层使用 `DbHandle` / `DbTransaction` query + transaction API。

### HTTP 解析

- HTTP 请求解析走 Ruvia 自研 zero-copy parser。
- 解析结果默认是指向连接读缓冲的 `std::string_view`。
- 不要重新引入 header、path、body 拷贝。
- `Transfer-Encoding: chunked` 请求体在连接读缓冲内原地解码。
- 当前 header/body 限制在 `include/ruvia/http/HttpParser.h`。
- header 上限是 64KB。
- body 上限是 16MB。

## 3. 内存与分配

### 总体边界

- Ruvia 的正式内存设计必须是项目自身设计。
- 不要把外部项目路径或外部实现名当作需求说明。
- 文档和代码注释要写清楚 Ruvia 自己的对象、生命周期和热路径边界。
- 当前内存池入口是 `include/ruvia/memory/MemoryPool.h`。
- 命名空间保持扁平 `ruvia`，不要新增 `ruvia::memory`。
- `vcpkg.json` 必须包含 `mimalloc`。
- 生产默认上游分配器是 `mimalloc` 包装的 `std::pmr::memory_resource`。
- 不要让系统 `new_delete_resource()` 成为默认生产路径。

### 全局 PMR 约束

- 框架内部代码（`include/ruvia` 和 `src`）凡是拥有动态内存的对象，必须使用 `std::pmr::string` 和 `std::pmr::vector`，不使用默认堆拥有型 `std::string` 或 `std::vector`。
- 公开 API 输入参数优先使用 `std::string_view`、`std::span`、`std::filesystem::path` 或值类型配置；框架内部需要保存时再复制到对应 PMR 生命周期。
- 请求热路径的 pmr 容器必须使用请求 arena resource（通过 `c.resource()` 或 `RequestMemory`）。
- Worker 层 pmr 容器使用 `WorkerMemory` 的 resource。
- 启动期构建的 pmr 容器使用 `std::pmr::get_default_resource()`（即 mimalloc）。
- 异常类（如 `HttpError`）使用 `std::pmr::string` 成员，构造时使用默认 resource。
- `std::string_view` 作为只读视图类型不受此约束，继续广泛使用。
- `Context::text(std::string&)`、`Context::text(std::string&&)`、`Context::text(const std::string&)` 保持 deleted，防止意外使用 `std::string`。
- 测试代码和示例代码不强制使用 pmr，但请求/响应模型宏字段仍必须使用 Ruvia 模型类型。

### 进程内存层

- 进程级内存层只在启动期初始化。
- 进程级内存层负责上游资源和只读全局配置。
- 当前 `ruvia::ProcessMemory` 持有 `ruvia::MimallocMemoryResource` 和默认配置。
- 内存池配置通过 `ruvia::app().setMemoryPoolConfig(...)` 在 `run()` 前设置。
- `ruvia::ProcessMemory::configure(...)` 只能在 freeze 前使用；创建 `ruvia::WorkerMemory` 后进程内存配置视为冻结。
- 请求热路径不能直接访问共享进程层做普通分配。

### Worker 内存层

- worker 内存层与 worker 线程一一对应。
- worker 内存层随 worker 的 `io_context` 创建和销毁。
- 当前每个 `HttpServer` 持有自己的 `ruvia::WorkerMemory`，直接挂在进程级 `ruvia::MimallocMemoryResource` 上（mimalloc 自身已分桶，不再额外叠一层 `unsynchronized_pool_resource`）。
- 连接读缓冲等 worker 内对象优先走该资源。
- 跨 worker 不共享可变分配状态。

### 请求内存层

- 请求内存层使用短生命周期 arena。
- 优先使用连接私有初始缓冲上的 `std::pmr::monotonic_buffer_resource`。
- 当前每次请求在 `HttpServer::handleSession()` 中创建 `ruvia::RequestMemory`。
- `ruvia::RequestMemory` 会传入 `ruvia::Context`。
- `c.text(...)` 和 `HttpResponse` header/body 优先走该 arena。
- 一次请求写出完成后整体释放。
- 不逐个释放 header、参数、临时解析对象。

### 启动期构建

- 内存池配置必须在 worker 启动前完成。
- 路由表必须在 worker 启动前完成。
- controller factory 必须在 worker 启动前完成。
- middleware chain 必须在 worker 启动前完成。
- 全局响应模板必须在 worker 启动前完成。
- 运行期禁止重建全局池。
- 禁止让旧 generation 分配出的对象跨 generation 存活。

### 诊断统计

- 当前不保留诊断统计编译开关。
- 分配路径直通上游且没有计数分支。

## 4. 连接、请求与响应热路径

### 连接状态

- 每个 HTTP 连接由所属 worker 独占。
- 连接状态至少包含 socket。
- 连接状态至少包含可复用读缓冲。
- 连接状态至少包含已使用字节数。
- 连接状态至少包含 keep-alive 状态。
- 连接状态至少包含响应头栈缓冲。
- 连接状态至少包含空闲扫描 intrusive entry。
- 这些状态不得跨线程迁移。

### 读缓冲与解析视图

- 连接读缓冲初始容量建议 8KB。
- 连接读缓冲跨 keep-alive 请求复用。
- header 不完整时按上限增长。
- 达到 `src/net/HttpServer.cpp` 的 header 64KB 限制后拒绝请求。
- 不要每个请求重新分配 header 容器。
- 请求解析结果的 method、path、version 默认指向连接读缓冲。
- 请求解析结果的 header name/value 默认指向连接读缓冲。
- 固定上限 header 表使用栈或连接内固定数组。
- 当前上限保持 64 个左右的小对象设计。

### Body 读取

- 请求 body 需要所有权时直接读入最终 body 缓冲。
- `Content-Length` 路径先按长度 resize 一次再读入。
- chunked 路径在连接读缓冲内原地解码，避免临时完整 body。
- `Expect: 100-continue` 要在读取 body 前返回 interim 100 响应，避免等待 body 的客户端死锁。
- 大请求体需要流式处理时必须显式使用 `RUVIA_POST_STREAM`、`RUVIA_PUT_STREAM` 或 `RUVIA_PATCH_STREAM`。
- 普通 route 仍保持 dispatch 前完整读取 body，不要让所有路由默认进入流式路径。
- stream route 在 header 完整后进入 handler，通过 `c.bodyReader()` 逐块读取 Content-Length 或 chunked body。
- `BodyReader::read()` 返回的 chunk view 只保证有效到下一次 `read()`，不要跨 read 或跨请求保存。
- multipart/form-data 解析结果默认是指向当前请求 body 的 part view。
- stream route 可通过 `c.multipartReader()` 逐块读取 multipart/form-data；part body view 同样只保证有效到下一次 `read()`。
- stream multipart 的 part name、filename、contentType、body 都是 view，生命周期只保证到下一次 `read()`。
- 避免“临时 body 再拷贝到 request”的两段式分配。

### Pipeline 数据整理

- pipelined 请求残留数据只能在当前请求的所有 `std::string_view` 失效后再 `memmove`。
- 不要在 handler 或 middleware 仍可能读取 request view 时整理读缓冲。

### 响应写出

- 响应写出采用栈上固定 header buffer + scatter-gather I/O。
- 小响应可把 header/body 合并为一次写。
- 大 body 使用 `{header, body}` 两段 buffer。
- 禁止为了写出把 body 拼进完整 response 字符串。
- `HttpResponse` body 必须显式区分 borrowed view、owned arena string、file token；手动构造临时 body 用 `setBodyCopy(...)`，稳定视图才用 `setBodyView(...)`。
- `Context::text(std::pmr::string&)` 会消费非 const arena string 成为 owned body，handler 写 `c.text(body)`；不提供 `std::pmr::string&&` 重载，避免临时 PMR string 入口；`Context::text(std::string_view)` 是 borrowed view，调用者不得传入即将销毁的临时字符串。
- 应用级响应流式必须显式使用 `RUVIA_GET_STREAM` 或 `RUVIA_GET_SSE`；WebSocket 必须显式使用 `RUVIA_GET_WS` 或 `RUVIA_GET_WS_OPTIONS`；普通 `Task<HttpResponse>` route 不隐式进入 response streaming 或 WebSocket。
- response streaming 不进入 `HttpResponse` body kind；由 `Context` 绑定连接私有 `ResponseStreamWriter`，首个 `write()` 提交 header，之后按 HTTP/1.1 chunked 写出，结束时发送 `0\r\n\r\n`。
- response streaming helper 命名对齐 Hono：`c.stream()`、`c.streamText()`、`c.streamSSE()`；SSE 事件通过 `writeSSE({.data = ..., .event = ..., .id = ..., .retry = ...})` 写出。
- `c.stream().write(view)` / `c.streamText().write(view)` 的 view 只需在该次 `co_await` 返回前有效；`c.streamSSE()` 只做 SSE frame 格式化，不引入共享字符串或跨线程引用计数。
- response streaming route 支持与普通 route 相同的 middleware 参数；middleware 可在 `next(c)` 前设置 status/header/cookie，或在首写前短路返回普通 `HttpResponse`；stream 已提交后 middleware 的 post-response 修改不得影响已写出的 stream。
- response streaming route 不自动补 `Content-Length`，使用 `Transfer-Encoding: chunked`；状态和响应 header 必须在第一次 `write()` 前设置。
- `HEAD` 不 fallback 到 response streaming `GET` route；需要 HEAD 语义时显式注册 `RUVIA_HEAD`。
- WebSocket handler 签名同 response streaming：`ruvia::Task<void> ws(ruvia::Context& c)`，通过 `c.webSocket()` 读写；当前实现支持 RFC 6455 upgrade、text/binary、客户端 fragmented message 重组、route 级 subprotocol 协商、route 级自动 heartbeat ping、ping/pong/close、close code/reason 校验、text UTF-8 校验和 WebSocket 专用 idle timeout phase。
- WebSocket 连接状态必须保持连接私有、worker 私有；普通 HTTP 快路径不得为 WebSocket 引入共享队列、mutex 或引用计数。
- keep-alive 的通用响应前缀、Server、Connection、Date 等可由连接或 worker 缓存。
- Date 最多按秒刷新。
- 不能每个请求构造一组通用 header 字符串。
- 当前响应写出已缓存常见 status line，并自动补 `Server`/`Date`。
- 响应 status code/reason、header name/value、cookie name/value/options 必须在设置时校验，非法值抛 `std::invalid_argument`，不要让 CR/LF/NUL 或非法 token 进入写出层。
- 不要回退到每请求完整格式化。

### 文件响应与空闲连接

- 文件响应不全量读入内存。
- 当前 plain TCP 文件响应优先使用平台零拷贝路径：Linux `sendfile`、Windows `TransmitFile`。
- 不可用零拷贝的 fallback 路径采用 header + 64KB 分块文件缓冲。
- 当前 `c.file(...)` / `c.staticFile(...)` 自动生成 `ETag`、`Last-Modified`、`Accept-Ranges`，支持 `If-None-Match`、`If-Modified-Since` 和单段 `Range` / `206 Partial Content`。
- 应用层不直接设置 `HttpResponse` 文件体；文件体只能由 `Context` 的 `c.file(...)` / `c.staticFile(...)` 构造出的 `FileToken` 设置，避免绕过路径、验证器和 Range 语义。
- 后续再扩展其他平台零拷贝路径或更细的 file fallback 优化。
- 空闲/header/body/write 超时管理采用每 worker 一个扫描器和连接内 intrusive entry。
- 扫描器只在该 worker 线程访问。
- 不为每个连接创建 timer、协程或堆分配节点。
- 当前 `HttpServerOptions` 支持 `idleTimeout`、`scanInterval`、`headerTimeout`、`bodyTimeout`、`writeTimeout`、`maxConnections`、`maxRequestsPerConnection`。
- `ruvia::app().setIdleTimeout(...)`、`setConnectionScanInterval(...)`、`setHeaderTimeout(...)`、`setBodyTimeout(...)`、`setWriteTimeout(...)`、`setMaxConnectionsPerWorker(...)`、`setMaxRequestsPerConnection(...)` 必须在 `run()` 前配置。
- 超时为 `0ms` 表示关闭该类超时；连接/请求限制为 `0` 表示不限制。
- 各超时各管一段：`headerTimeout` 只盯请求头读取（包括 TLS 握手），`bodyTimeout` 只盯请求体读取，`writeTimeout` 只盯响应写出，`idleTimeout` 盯 keep-alive 间隔 **以及 dispatch（handler 运行）阶段**。dispatch 期间扫描器把连接归到 `kIdle` 阶段，由 `idleTimeout` 作为 hung-handler 的兜底 deadman；想限制业务逻辑耗时请用 `idleTimeout` 或在业务层加 cancellation，不要靠 `headerTimeout` / `bodyTimeout`。
- `maxConnections` 触发时应尽量返回 JSON `429 Too Many Requests` 后关闭新连接；`maxRequestsPerConnection` 只是 keep-alive 生命周期限制，不当作错误响应。

### 跨线程发送边界

- 通用 TCP/WebSocket 跨线程 send 可以单独设计 intrusive MPSC 写队列和 thread-local node pool。
- 不要把当前请求期 `ruvia::WebSocket` 轻量对象跨线程保存；后台任务推送必须单独设计 slot/generation + worker 私有 MPSC mailbox。
- 普通 HTTP request-response 快路径不得引入 `shared_ptr<string>`。
- 普通 HTTP request-response 快路径不得引入多态 `WriteNode`。
- 普通 HTTP request-response 快路径不得引入跨线程引用计数。

## 5. 路由与中间件

### 路由索引

- 路由和中间件链必须在启动期预构建。
- exact route 优先构建只读完美哈希表。
- radix 树作为路径匹配或 fallback 结构。
- Hono-like `:param`/`*` 动态路由使用启动期构建的只读 segment trie。
- 同一 method + 同一路径的重复 route 必须在启动期报错，不能静默覆盖旧 handler。
- 同一 HTTP method 下等价动态 route shape 必须在启动期报错，例如 `/users/:id` 与 `/users/:name`，以及被 `*` 通配遮蔽的动态路径。
- `HEAD` 在没有显式 HEAD route 时 fallback 到同路径普通 `GET` route；response streaming `GET` route 不参与隐式 HEAD fallback；`Allow` header 中 GET 可用时必须包含 HEAD。
- 请求期只写栈上固定参数表。

### 注册入口

- 路由注册只允许通过 controller/group 宏完成。
- 不暴露直接 `Router::addRoute(...)` 注册 API。
- 不暴露直接 `Router::group(...)` 注册 API。
- `include/ruvia/router/Router.h` 不暴露 dispatch/resolve/index 内部结构，也不定义 `RouterAccess`。
- `include/ruvia/router/Router.h` 不定义 `RouteHandler`、`RouteMiddleware`、`RouteScope` 的布局；控制器宏所需注册描述只允许放在 `Controller.h` 的 `ruvia::detail` 中，dispatch/resolve/index 只放未安装的 `src/router/RouterInternal.h`。
- 公开 controller 宏只能生成启动期 descriptor/builder；请求期 route table、middleware frame、handler thunk 布局只允许在 `src/router/RouterInternal.h`。
- Router 内部访问入口只放在未安装的 `src/router/RouterInternal.h`，仅供 `HttpServer` 和测试使用。
- 当前 route handler/middleware 快路径是 `void* + 函数指针 thunk`。
- 禁止重建路由索引。
- 禁止重建 `std::function` 链。
- 禁止请求期创建临时容器。

### Middleware API

- middleware API 采用 Drogon-like CRTP。
- middleware 类形如 `class AppMiddleware final : public ruvia::Middleware<AppMiddleware>`。
- middleware 实现异步 `ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next)`。
- middleware 实例和链在启动期构建。
- middleware 只提供用户自定义机制；应用自己的 middleware 通过 controller/group/route 宏显式挂载。
- middleware 不在请求期重建链或创建临时容器。

## 6. Controller API

### 公共命名空间

- 项目公开 API 采用扁平命名空间。
- 优先暴露 `ruvia::App`、`ruvia::Router`、`ruvia::Controller<T>`、`ruvia::Middleware<T>`、`ruvia::HttpRequest`、`ruvia::HttpResponse`。
- 不要新增 `ruvia::http`、`ruvia::router`、`ruvia::app` 这类二级业务命名空间。
- 协程返回类型统一使用 `ruvia::Task<T>`。
- 公开接口和示例不要直接写 `asio::awaitable<T>`。
- 公开 handler/middleware 内部直接 `co_await ruvia::Task<T>`，例如 `co_await next(c)` 和 `co_await c.bodyReader().read()`。
- `ruvia::Task<T>` 是 single-shot；临时 task 可直接 `co_await`，存入局部变量后必须 `co_await std::move(task)`。
- 公开 `ruvia::Task<T>` 不提供 `.asAwaitable()`，也不暴露 Asio awaitable 转换 API。
- `ruvia::Task<T>` 必须是 Ruvia 自己的协程 handle/promise 设计，不继承或依赖 `asio::detail::awaitable_frame`。
- 不提供 `Task<T>` 到 `asio::awaitable<T>` 的隐式转换，也不提供 lvalue await；跨到 Asio 只能在未安装内部边界使用 `src/AsioAwait.h` 的 `ruvia::detail::taskAsAwaitable(...)`。
- 底层 Asio I/O 等待通过内部 callback awaiter 适配到 `ruvia::Task<T>`；不要为了单次 I/O await 新建 `co_spawn` 桥。
- 公开 controller API 是 `ruvia::Controller<T>` 和 `include/ruvia/http/Controller.h`。

### Handler 约定

- handler API 采用 Hono-like 单参数上下文。
- handler 必须是全异步协程。
- 普通 handler 签名形如 `ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c)`；response streaming/WebSocket handler 签名形如 `ruvia::Task<void> stream(ruvia::Context& c)`。
- 不要提供同步 `ruvia::HttpResponse handler(...)` 重载。
- 通过 `c.req()` 读取请求。
- 通过 `c.header(...)`、`c.query(...)`、`c.cookie(...)`、`c.param(...)` 读取常用输入。
- 通过 `c.status(...)`、`c.setHeader(...)`、`c.setCookie(...)` 构造响应元数据。
- 通过 `c.bodyReader()` 在显式 stream route 中读取请求体。
- 通过 `c.stream()` / `c.streamText()` / `c.streamSSE()` 在显式 response streaming route 中写出响应块或 SSE frame。
- 通过 `c.webSocket()` 在显式 WebSocket route 中读写 text/binary/pong/close frame。
- 通过 `c.text(...)`、`c.json(...)`、`c.file(...)`、`c.staticFile(...)`、`c.redirect(...)`、`c.error(status, code, message)` 构造响应体或错误响应。
- `c.header(...)` 是请求 header 读取。
- 响应 header 必须用 `c.setHeader(...)`。
- 需要临时字符串时优先使用 `c.allocator<char>()` 构造 `std::pmr::string`。
- 不要回退到普通堆分配。

### 动态路由参数

- 动态路由采用 Hono-like `RUVIA_GET("/users/:id", handler)`。
- 通配路由采用 Hono-like `RUVIA_GET("/files/*", handler)`。
- 通过 `c.param("id")` 读取命名参数。
- 通过 `c.param("*")` 读取通配剩余路径。
- 参数返回值是指向当前请求 path 的 `std::string_view`。
- 不要跨请求保存参数 view。

### 请求读取 API

- 请求读取 API 保持零拷贝优先。
- `ruvia::HttpRequest` 对应用层是只读请求视图。
- `HttpRequest` 只允许 parser/server 在内部填充，不要恢复公开可写字段或测试 accessor 后门。
- 测试需要请求对象时优先用 `HttpParser` 解析 HTTP 原文构造。
- `HttpRequest::header(...)` 返回 header value view。
- `query(...)` 返回 `std::optional<std::string_view>`。
- `cookie(...)` 返回 `std::optional<std::string_view>`。
- 请求 body I/O 只放在 `Context` 上；普通路由通过 `co_await c.body()`、`co_await c.json<T>()`、`co_await c.form<T>()`、`co_await c.multipart()` 懒读取。
- `HttpRequest` 不公开同步 `body/json/form/multipart` 入口；不要恢复这些旧 API。
- `co_await c.multipart()` 返回请求 arena 中的 part 表，part body 仍指向当前请求 body。
- header/query/cookie 按请求原文返回 view。
- header/query/cookie 返回值仍指向当前请求读缓冲；body/multipart 返回值指向当前请求 arena 或 body 缓冲。
- 不要跨请求保存返回 view。

### 错误响应

- 统一错误响应入口是 `include/ruvia/http/Error.h`。
- 框架内部 400、404、500 都输出 JSON 错误体。
- `c.error(status, code, message)` 输出 JSON 错误体。
- `code` 是应用自定义错误码。
- 普通 handler 想中断也可抛 `ruvia::HttpError`。
- 请求层统一转换 `ruvia::HttpError`。

## 7. 请求与响应模型

### 宏入口

- 请求/响应模型宏入口是 `include/ruvia/http/Model.h`。
- `Model.h` 由 `Controller.h` 间接暴露。
- 模型内部实现头只放在 `include/ruvia/http/detail/model`；用户代码不要直接 include 内部头。
- `RUVIA_MODEL` 是请求 body 和响应 JSON 的统一 schema 入口。
- 示例：`RUVIA_MODEL(Login, RUVIA_FIELD(username, ruvia::String), RUVIA_FIELD(password, ruvia::String))`。
- 同一模型可用于 `co_await c.json<T>()`、`co_await c.form<T>()` 和 `c.json(model)`。
- `RUVIA_FIELD` / `RUVIA_FIELD_NAME` 只允许字段类型和模型选项；当前模型选项只支持 `RUVIA_DEFAULT(value)`、`RUVIA_OMIT_EMPTY`、`RUVIA_EMIT_NULL`。
- 字段校验规则禁止写在 `RUVIA_FIELD` / `RUVIA_FIELD_NAME` 上；必须通过 route middleware 的 `RUVIA_VALIDATE_JSON(Model, RUVIA_RULE(...))` 或 `RUVIA_VALIDATE_FORM(Model, RUVIA_RULE(...))` 声明。
- 当前校验规则支持 `RUVIA_REQUIRED(message)`、`RUVIA_MIN(value, message)`、`RUVIA_MAX(value, message)`、`RUVIA_ONE_OF(message, ...)`、`RUVIA_EMAIL(message)`、`RUVIA_PATTERN(message, "regex")`、`RUVIA_MATCH(message, predicate)`、`RUVIA_CUSTOM(message, predicate)`。
- `RUVIA_PATTERN` 的同一 pattern 只能编译一次并复用，不要在每次请求校验时重新构造 `std::regex`。
- 轻量字符串匹配优先用 `RUVIA_MATCH`，predicate 接收 `std::string_view`；不要为了简单前缀/字符检查使用 regex。
- 需要 JSON/form wire name 与 C++ 字段名不一致时使用 `RUVIA_FIELD_NAME("wire_name", field, type, options...)`。
- 对 wire name 不同的字段声明校验时使用 `RUVIA_RULE_NAME("wire_name", field, rules...)`。
- 路由校验 middleware 使用 `RUVIA_VALIDATE_JSON(Model, rules...)` 和 `RUVIA_VALIDATE_FORM(Model, rules...)`。
- 模型字段类型只允许 Ruvia 自定义模型类型：`ruvia::String`、`ruvia::Array<T>`、`ruvia::List<T>`、嵌套 `RUVIA_MODEL`、`ruvia::Bool`、`ruvia::Int32`、`ruvia::UInt32`、`ruvia::Int64`、`ruvia::UInt64`、`ruvia::Float`、`ruvia::Double`；不允许 raw `bool`、整数、浮点、`std::string`、`std::string_view`、`std::vector` 或 `std::pmr::string` 作为模型字段类型。
- 不要把 param/header 绑定混进 body 模型。

### JSON 与 Form 规则

- `json<T>()` 要求 `Content-Type: application/json`。
- `form<T>()` 要求 `Content-Type: application/x-www-form-urlencoded`。
- `form<T>()` 的 Content-Type 可带 `; charset=...` 参数。
- 无效 JSON body 直接抛异常，并由请求层转成 JSON 400。
- 无效 form body 直接抛异常，并由请求层转成 JSON 400。
- 无效 Content-Type 直接抛异常，并由请求层转成 JSON 400。
- JSON 支持嵌套 `RUVIA_MODEL`。
- JSON 支持数组字段。
- JSON 支持自引用递归数组字段，使用 `ruvia::List<T>`。
- JSON 模型递归解析必须有固定深度限制，当前限制为 64 层左右；超限视为无效 body。
- form 只支持扁平 key-value 基础字段。
- form 不支持嵌套对象或数组。

### 模型字段访问

- `RUVIA_MODEL` 生成私有零拷贝 body 视图和响应 JSON 写出入口。
- 通过 `request.username()` 按宏内字段类型读取字段。
- 不要恢复模型 `operator->`；`request->username()` 不是公开 API。
- 字段 getter 返回 `std::optional<字段类型>`。
- 字段第一次读取后在模型内 lazy cache。
- 字段默认可缺省。
- 缺字段或字段类型不匹配时 getter 返回 `std::nullopt`。
- 模型内部必须显式记录字段 parse 状态：missing、parsed、invalid_type、duplicate；校验阶段不得为了判断 invalid_type 或 duplicate 再扫描 body。
- 校验阶段字段类型不匹配时应输出精确 `invalid_type` 消息，例如 `must be a string`、`must be an array`、`must be an object`、`must be a boolean`、`must be a number`。
- duplicate known field 应由 validator middleware 输出字段级 `duplicate` issue；不要回退成 parse 阶段 `invalid_body`。
- 是否 required 由字段规则或 handler 显式判断。
- 基础字段优先用 Ruvia 类型。
- `ruvia::String` 自动选择零拷贝 view 或解码/复制到当前请求 arena。
- `ruvia::Array<T>` 使用请求 arena。
- `ruvia::List<T>` 使用请求 arena，支持递归/自引用模型数组。
- 用户模型不提供 `std::string/std::vector` 字段兼容路径；需要文本和数组时必须显式使用 `ruvia::String`、`ruvia::Array<T>` 或 `ruvia::List<T>`。
- 需要运行期字段名时可用 `request.get("username")`。
- `request.get("username")` 默认返回指向当前请求 body 的 `std::optional<std::string_view>`。
- 响应模型可用 `Response response(c)` 构造，字符串字段 setter 可直接接收 `std::string_view` 或字面量，并使用模型持有的请求 arena。
- 需要构造嵌套/数组/递归响应字段时使用生成的 `fieldEnsure()` 入口确保字段存在，例如 `category.childrenEnsure().emplace().name("leaf")`；`fieldReset()` 可清空该字段。
- `RUVIA_FIELD_NAME` 的 wire name 用于解析、`request.get("wire_name")`、校验错误路径和响应 JSON key；C++ getter/setter 仍使用字段名。
- 响应 JSON 默认跳过缺省字段；`RUVIA_EMIT_NULL` 会把缺省字段输出为 `null`，`RUVIA_OMIT_EMPTY` 会跳过空字符串/空数组/空列表。

### 序列化与扩展边界

- `c.json(...)` 序列化响应模型到请求 arena。
- multipart/file 不塞进普通模型宏。
- multipart 通过 `co_await c.multipart()` 读取，文件响应通过 `c.file(...)` / `c.staticFile(...)` 读取。
- 不要引入运行期反射。
- 不要引入 map。
- 不要引入 type-erasure。

## 8. 路由宏

### 宏命名

- 路由宏采用 Hono-like 命名。
- 支持 `RUVIA_CONTROLLER_GROUP("/api", Middleware)`。
- 支持 `RUVIA_ROUTES_BEGIN`。
- 支持 `RUVIA_GET("/path", handler, Middleware)`。
- 支持 `RUVIA_POST_STREAM("/upload", handler, Middleware)`、`RUVIA_PUT_STREAM(...)`、`RUVIA_PATCH_STREAM(...)`。
- 支持 `RUVIA_GET_STREAM("/chunks", handler, Middleware)`、`RUVIA_GET_SSE("/events", handler, Middleware)`、`RUVIA_GET_WS("/ws", handler, Middleware)` 和 `RUVIA_GET_WS_OPTIONS("/ws", handler, options, Middleware)`。
- 支持 `RUVIA_GROUP_BEGIN("/api", Middleware)`。
- 支持 `RUVIA_GROUP_END`。
- 支持 `RUVIA_ROUTES_END`。

### 分组规则

- 多文件 controller 优先用 `RUVIA_CONTROLLER_GROUP` 声明共享前缀和 controller 级 middleware。
- 文件内嵌套前缀再用 group 宏。
- group middleware 顺序保持 controller group -> outer group -> inner group -> route。
- 不要暴露或使用直接 `Router::addRoute(...)` 注册入口。
- 不要暴露或使用直接 `Router::group(...)` 注册入口。

### 兼容边界

- 不要恢复 Drogon 命名的 `METHOD_LIST_BEGIN` 兼容别名，除非用户明确要求。
- 不要恢复 Drogon 命名的 `ADD_METHOD_TO` 兼容别名，除非用户明确要求。
- controller 通过宏静态注册。
- 示例启动只需要 `ruvia::app().setThreadNum(2).run()`。
- 不要再手动 `.addController(std::make_shared<...>())`。

## 9. 构建与验证

### 本地构建

- Windows 本地已用 vcpkg 构建。
- 构建命令：`cmake --build build --config Debug`。
- 重新配置命令：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake`。

### 验证要求

- 最小验证先运行 `cmake --build build --config Debug`。
- 当前仓库不保留 `tests/` 目标。
- `examples/` 作为显式开启的编译型覆盖目标，通过 `-DRUVIA_BUILD_EXAMPLES=ON` 构建；默认保持关闭。
- 不要恢复 CTest 或 package smoke，除非用户明确要求。

### CI

- CI build 在 `.github/workflows/build.yml`。
- build 显式包含 Ubuntu job。
- build 显式包含 Windows job。
- build 不使用 matrix。
- Ubuntu 和 Windows 两个 job 都必须构建默认库目标。
- 当前仓库不保留 benchmark workflow。

## 10. 代码与仓库细节

### 语言与依赖

- C++ 标准是 C++23。
- Ruvia 明确专注 HTTP/1.1 核心能力和轻量运行时。
- 依赖只在 `vcpkg.json` 声明。
- 当前核心依赖包含 `asio`、`mimalloc`、`openssl`、`zlib`。
- TLS 是默认能力，不是可裁剪构建项；后端固定为 OpenSSL。
- MariaDB-compatible DB 是严格 feature：`RUVIA_ENABLE_MARIADB=ON` / vcpkg `mariadb` feature 启用后才查找 `libmariadb`、编译 `src/db/Db.cpp`、安装 `include/ruvia/db`，并暴露 `App::useDb(...)` / `Context::db(...)`。
- Redis 是严格 feature：`RUVIA_ENABLE_REDIS=ON` / vcpkg `redis` feature 启用后才查找 `hiredis`、编译 `src/redis/Redis.cpp`、安装 `include/ruvia/redis`，并暴露 `App::useRedis(...)` / `Context::redis(...)`。
- JWT helper 是严格 feature：`RUVIA_ENABLE_JWT=ON` / vcpkg `jwt` feature 启用后才编译 `src/auth/Jwt.cpp` 并安装 `include/ruvia/auth`。
- 不要新增网络、App 或 TLS 裁剪开关；NET/App/TLS 是默认能力。
- 当前 DB 配置使用 `DbConfig{.host = ..., .username = ..., .database = ...}`；后端固定为 MariaDB-compatible Connector/C。
- DB transaction 入口是 `DbHandle::beginTransaction()`；`DbTransaction` move-only，必须 `commit()` 或 `rollback()`，析构活跃事务会关闭该 slot 连接避免污染后续请求。
- 安装后只暴露 `ruvia::ruvia`；旧安装组件目标不得恢复。
- Windows 默认 vcpkg triplet 是 `x64-windows-static`。
- Windows 默认 triplet 由 `CMakeLists.txt` 设置。

### 目标

- `ruvia` 是静态库目标。

### 文本与本地文件

- `.gitattributes` 强制文本 LF。
- 编辑文件时保持 LF。
- 不要引入 CRLF churn。
- `.slim/` 是本地分析缓存目录。
- `.slim/` 整个目录由 `.gitignore` 忽略。
- 不要把 `.slim/` 中的路径写成 Ruvia 的设计依据。

### 安装与版本

- 发布/集成能力通过 CMake install/export 提供。
- 安装后下游使用 `find_package(ruvia CONFIG REQUIRED)`。
- 安装后下游使用 `target_link_libraries(... PRIVATE ruvia::ruvia)`。
- 版本宏在 `include/ruvia/version.h`。
