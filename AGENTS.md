# AGENTS.md

Ruvia 项目协作说明。默认用中文回复。本文档约束本仓库的目录边界、架构边界、性能要求和验证方式。

## 项目定位

Ruvia 是 C++23 HTTP/Web 框架仓库，当前采用 monorepo + 多 CMake target：

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
- 讨论协议行为时，以 HTTP/TLS/WebSocket/SSE/HTTP2 相关 RFC 和标准优先。
- 如果项目约束与协议标准冲突，优先修实现和文档以符合标准。

## 目录规则

顶层源码目录只允许：

```text
ruvia-core/
ruvia-http/
ruvia-web/
examples/
tests/
```

仓库根目录不再保留源码级 `include/`、`src/`、`fuzz/`、`core/`、`http/`、`web/` 或 `edge/`。

每个库目录必须自带：

```text
<target>/
  CMakeLists.txt
  include/
  src/
```

根 `CMakeLists.txt` 只负责全局选项、依赖发现、package export、install helper 和 `add_subdirectory(...)`。不要再拆出额外的仓库内 `.cmake` 片段。

本地构建目录只允许使用仓库根目录下的 `build/`。不要创建 `build-*`、`out/`、`cmake-build-*` 或其他临时构建目录；构建缓存或生成结果有问题时，直接删除 `build/` 后重新配置。

本地工具目录 `.codex/`、`.claude/`、`.agents/`、`.codegraph/` 必须保持 ignored，不作为源码提交。

## Target 边界

### ruvia-core

`ruvia-core` 是 runtime 底座库，可给外部用户单独使用，也承载 Ruvia 的 Asio/Task/内存运行时基础设施。

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

`ruvia-http` 是通用 HTTP/协议库，可给外部用户单独使用。它是纯协议 target，不依赖 `ruvia-core`、Asio、socket 或 Ruvia runtime。

可以包含：

- HTTP method/status/header/request/response 类型。
- HTTP/1 parser、chunk parser、request target parser。
- cookie/cache/range/conditional request/content negotiation/header token 与 header value 通用 helper。
- multipart/form/url encoding、SSE frame formatting、opaque body handle 与纯 parser。
- WebSocket 协议 helper。
- HTTP/2 sans-I/O 连接核心 `Http2Connection`（同一实现供 server 与 client 两种角色驱动）、HTTP/1 sans-I/O 连接核心 `Http1Connection`、WebSocket sans-I/O 核心。
- multipart/SSE/content-encoding 等 wire-format 和协议语义实现；runtime reader/writer facade 留在 `ruvia-web`。
- 纯协议 primitive（零 core、零 asio、零 socket；client/server 的 I/O runtime 由 `ruvia-web` 或外部 runtime 驱动）。

禁止包含：

- App、Context、Controller、Router、route macro、middleware、Next。
- Model/validation 宏。
- DB、Redis、JWT、CSRF、Session、CORS、security headers、RateLimit 的 Web 集成。
- origin/cache/purge/rule 等产品策略。

### ruvia-web

`ruvia-web` 是完整 Web 框架产品。

包含：

- App 配置和启动。
- Context、Controller、Router、middleware、Next、route macro。
- HTTP server runtime、TLS、HTTP2 server、WebSocket route、response streaming。
- Model、JSON/form parsing、validation middleware。
- Session、CSRF、RateLimit、CORS、安全头、静态文件目录扫描/索引、AutoHTTPS redirect 等基于 HTTP 的 Web 应用能力。
- 可选 MariaDB、Redis、JWT 集成。

`ruvia-web` 依赖 `ruvia::core` 和 `ruvia::http`，但不得把 Web-only API 下沉到 `ruvia-http`。

### HTTP 协议与上层应用边界

`ruvia-http` 拥有 HTTP 协议本体：wire/message/framing/connection 语义，以及跨 server/client/外部 runtime 都能复用的 sans-I/O 状态机和纯协议 helper。所有 HTTP/1、HTTP/2、WebSocket、SSE、multipart、content-coding 等协议实现都应留在 `ruvia-http`。包括但不限于 HTTP/1 request/response 解析、chunked 与 Content-Length framing、keep-alive 与 `Connection` 语义、`Expect: 100-continue`、Upgrade/h2c/WebSocket 握手字节、HTTP/1.0 close-delimited 响应流、HTTP/2 frame/HPACK/settings/flow-control、response head 序列化、WebSocket frame/close code/permessage-deflate 协议处理。

`ruvia-web` 拥有 HTTP 之上的应用能力：App/Context/Router/middleware/controller、route validation、session、CSRF、JWT、rate limit、CORS 策略与中间件、安全头中间件、静态文件目录扫描/索引/产品配置、AutoHTTPS redirect、DB/Redis 集成、WebSocket route 绑定等。它们可以读写 HTTP header，但这不等于它们属于 HTTP 协议本体。

边界判断：如果代码决定“字节如何解析/分帧/序列化、连接是否保持、协议升级是否成立、协议错误如何映射”，应放在 `ruvia-http`；如果代码决定“某个 Web 产品/路由/中间件/配置要不要设置某些 header 或执行某种策略”，应放在 `ruvia-web`。`ruvia-web` 只能用 core runtime、asio/TLS/socket/timeouts 驱动 `ruvia-http` 的协议 core，不要重写协议判断；`ruvia-http` 可以提供 header token 解析、value 校验、`Vary` 合并等通用工具，但不得依赖 Context/App/Router。

## 性能原则

- 请求热路径目标是 0 抽象成本。
- 启动期可以使用注册表、工厂、虚函数和一次性构建。
- 请求期不要新增 mutex、rwlock、spinlock、共享原子计数争用、type-erasure、`shared_ptr` 分配或不必要拷贝。
- 优先使用 per-worker 所有权、连接私有状态、启动期构建后只读数据。
- 跨线程操作连接状态默认禁止；必须先设计明确的 worker mailbox 或 intrusive MPSC 边界。

## 线程和运行时

- 每个 worker 拥有一个 standalone Asio `io_context`。
- 连接不能跨线程迁移。
- `App::run()` 创建 acceptor/server/thread per worker。
- 非 Windows 平台要求 `SO_REUSEPORT`；Windows 使用 `SO_REUSEADDR`。
- graceful shutdown 只能在各 worker 自己的 `io_context` 上关闭该 worker 的 acceptor 和活跃 socket。
- idle/header/body/write timeout、连接数限制和请求数限制保持 per-worker 所有权。

## 内存规则

- 框架内部拥有动态内存的对象默认使用 PMR 容器：`std::pmr::string`、`std::pmr::vector`。
- 公开 API 输入优先使用 `std::string_view`、`std::span`、`std::filesystem::path` 或值类型配置。
- 请求热路径的 PMR 容器使用请求 arena。
- Worker 层容器使用 `WorkerMemory`。
- 启动期构建容器使用默认 resource，即 mimalloc-backed resource。
- 不要让 `std::pmr::new_delete_resource()` 成为生产默认路径。
- `Context::text(std::string&)`、`std::string&&`、`const std::string&` 入口保持 deleted，防止默认堆字符串误入响应热路径。

## HTTP 解析和响应

- HTTP 请求解析走 Ruvia 自研 zero-copy parser。
- method/path/version/header 默认是指向连接读缓冲的 `std::string_view`。
- header 上限 64KB，普通 body 上限 16MB。
- chunked 请求体在连接读缓冲中原地解码。
- 普通 route dispatch 前完整读取 body；大请求体必须显式使用 stream route。
- stream body reader 返回的 view 只保证有效到下一次 `read()`。
- 响应写出采用栈上固定 header buffer + scatter-gather I/O。
- 禁止为了写出把 body 拼成完整 response 字符串。
- 文件响应不全量读入内存；plain TCP 优先使用平台零拷贝路径。
- response streaming 和 WebSocket 必须通过显式 route macro 注册。

## 路由和中间件

- 路由注册只允许通过 controller/group/route 宏完成。
- 不暴露直接 `Router::addRoute(...)` 或 `Router::group(...)` 注册 API。
- 路由表、中间件链、controller factory 必须在 worker 启动前构建完成。
- 请求期不得重建 route index、middleware chain 或 `std::function` 链。
- 同 method + 同 path 的重复 route 必须启动期报错。
- 等价动态 route shape 必须启动期报错。
- `HEAD` 在无显式 HEAD route 时 fallback 到普通 GET route；streaming GET 不参与隐式 HEAD fallback。
- middleware API 保持 CRTP + async `handle(Context&, Next&)`。
- `next()` 是 single-shot。

## Controller API

- 公开 handler 形态是 Hono-like 单参数上下文。
- 普通 handler：`ruvia::Task<ruvia::HttpResponse> handler(ruvia::Context& c)`。
- streaming/WebSocket handler：`ruvia::Task<void> handler(ruvia::Context& c)`。
- 公开协程返回类型统一是 `ruvia::Task<T>`，不要暴露 `asio::awaitable<T>`。
- 读取请求统一走 `c.req()`。
- 设置响应 metadata 走 `c.status(...)`、`c.header(...)`、`c.setCookie(...)` 等。
- 构造响应走 `c.body(...)`、`c.text(...)`、`c.html(...)`、`c.json(...)`、`c.file(...)`、`c.staticFile(...)`、`c.redirect(...)`、`c.error(...)`。
- 公开 API 一个操作只保留一个名字，不新增别名。

## Model 和校验

- `RUVIA_MODEL` 是请求 body 和响应 JSON 的统一 schema 入口。
- 模型字段类型必须使用 Ruvia 模型类型，例如 `ruvia::String`、`ruvia::Array<T>`、`ruvia::List<T>`、`ruvia::Bool`、`ruvia::Int64`。
- 不允许 raw `std::string`、`std::vector`、`std::string_view` 或基础整数类型作为模型字段。
- 字段校验规则必须通过 route validation middleware 声明，不写进 `RUVIA_FIELD`。
- JSON 支持嵌套模型和数组；form 只支持扁平 key-value 基础字段。
- validation 不应为判断 invalid type 或 duplicate 再扫描 body。
- `RUVIA_PATTERN` 的同一 pattern 只能编译一次并复用。

## CMake 和安装

- 默认构建 `ruvia-core`、`ruvia-http`、`ruvia-web`。
- MariaDB、Redis、JWT 是严格 feature：
  - `RUVIA_ENABLE_MARIADB=ON`
  - `RUVIA_ENABLE_REDIS=ON`
  - `RUVIA_ENABLE_JWT=ON`
- outbound HTTP client 的协议模型和协议/策略半部是 `ruvia-http` 能力（响应解析、重定向规则、内容解码、配置校验，全部 sans-I/O）；与 server 同构地把 asio/TLS 运行时 driver 和 Web 代理 facade 放在上层：`ruvia-web/src/client/` 与 `HttpClientRuntime.h`（HttpClientPool / Http2ClientSession / HttpClientRegistry / `ProxyOptions`，经 `Context::fetch/fetchStream/proxy` 使用，无构建开关）。
- 下游推荐：

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(app PRIVATE ruvia::web)
```

- 需要更小依赖面时使用 `COMPONENTS core` 或 `COMPONENTS http`。
- 安装包必须暴露组件 target：`ruvia::core`、`ruvia::http`、`ruvia::web`；不要再暴露历史 Web 框架别名。

## 验证要求

改动完成前至少运行和任务相关的最小验证。

目录/文档/CMake 清理：

```powershell
git diff --check
rg -n '<stale split terms>' README.md AGENTS.md CMakeLists.txt ruvia-core ruvia-http ruvia-web tests examples
```

构建验证：

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DRUVIA_BUILD_TESTS=ON `
  -DRUVIA_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

安装验证：

```powershell
cmake --install build --config Debug --prefix build/install
```

不要把 `build/`、`vcpkg_installed`、本地工具目录或 CodeGraph 索引提交。
