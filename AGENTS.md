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

三个 target 的公开头和安装命名根严格一一对应：`ruvia-core` 只能拥有并安装
`include/ruvia/core/**`，`ruvia-http` 只能拥有并安装 `include/ruvia/http/**`，
`ruvia-web` 只能拥有并安装 `include/ruvia/web/**`。禁止在本 target 下创建或安装到
另一个 target 的命名根，也禁止在 CMake source/header 列表中直接加入另一个 target
目录里的文件。

跨 target 复用的编译期契约头必须放在所属 target 的 `include/ruvia/<target>/detail/`
（HTTP 使用 `include/ruvia/http/detail/`），通过 target include interface 使用。禁止任何
target、示例或测试把另一个 target 的 `src/` 加入 include path，也禁止通过相对或绝对
物理路径包含另一个 target 的源码或私有头。target 之间只能通过 `target_link_libraries`
传播的公开 include interface 使用依赖方已安装的头。

`src/` 下最多保留一层有业务含义的分类目录，例如 `server/`、`http2/`、`websocket/`、
`client/`。不要再引入 `src/net/...`、`src/*/core/...` 这类重复层级；`ruvia-core/src/`
保持扁平。三个 target 的 `src/` 都只保存实现和 target 自有 `pch.h`，契约头统一放在
各自的 `include/.../detail/`。

根 `CMakeLists.txt` 只负责全局选项、依赖发现、package export、install helper 和 `add_subdirectory(...)`。不要再拆出额外的仓库内 `.cmake` 片段。

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
- multipart/form/url encoding、SSE frame formatting 与纯 parser。
- WebSocket 协议 helper。
- HTTP/2 sans-I/O 连接核心 `Http2Connection`（同一实现供 server 与 client 两种角色驱动）、HTTP/1 zero-copy parser/framer primitives、WebSocket sans-I/O 核心 `WsConnection`。
- multipart/SSE/content-encoding 等 wire-format 和协议语义实现；runtime reader/writer facade 留在 `ruvia-web`。
- 纯协议 primitive（零 core、零 asio、零 socket；server I/O runtime 由 `ruvia-web` 驱动，client role 只由外部 runtime 驱动）。
- 无分配的 `HttpProtocolError` 协议失败信号及其 HTTP status；不得携带 Web JSON error code/details。

禁止包含：

- App、Context、Controller、Router、route macro、middleware、Next。
- Model/validation 宏。
- DB、Redis、JWT、CSRF、Session、CORS、security headers、RateLimit 的 Web 集成。
- `HttpErrorInfo`、`HttpError`、默认 JSON 错误 envelope、自定义 error handler 等应用错误模型。
- 通用 JSON escape/serialization、模型 JSON writer、健康检查或校验错误 JSON。
- 静态文件扩展名/MIME 推断、文件时间转换、文件 ETag 生成或 runtime 文件读缓冲。
- origin/cache/purge/rule 等产品策略。

### ruvia-web

`ruvia-web` 是完整 Web 框架产品。

包含：

- App 配置和启动。
- Context、Controller、Router、middleware、Next、route macro。
- HTTP server runtime、TLS、HTTP2 server、WebSocket route、response streaming。
- Model、JSON/form parsing/serialization、validation middleware。
- `HttpErrorInfo`、`HttpError`、JSON 错误响应和自定义 error/not-found handler。
- Session、CSRF、RateLimit、CORS、安全头、静态文件目录扫描/索引、AutoHTTPS redirect 等基于 HTTP 的 Web 应用能力。
- 静态文件扩展名/MIME 推断、文件时间与 validator 元数据生成、runtime 文件读缓冲。
- 可选 MariaDB、Redis、JWT 集成。

`ruvia-web` 依赖 `ruvia::core` 和 `ruvia::http`，但不得把 Web-only API 下沉到 `ruvia-http`。

### HTTP 协议与上层应用边界

`ruvia-http` 拥有 HTTP 协议本体：wire/message/framing/connection 语义，以及跨 server/client/外部 runtime 都能复用的 sans-I/O 状态机和纯协议 helper。所有 HTTP/1、HTTP/2、WebSocket、SSE、multipart、content-coding 等协议实现都应留在 `ruvia-http`。包括但不限于 HTTP/1 request/response 解析、chunked 与 Content-Length framing、keep-alive 与 `Connection` 语义、`Expect: 100-continue`、WebSocket Upgrade 握手字节、HTTP/1.0 close-delimited 响应流、HTTP/2 frame/HPACK/settings/flow-control、response head 序列化、WebSocket frame/close code/permessage-deflate 协议处理。

HTTP method 必须分成 wire token 与固定语义分类两层。RFC 9110 的 method 是可扩展且大小写敏感的 token；`HttpRequest::method()`、`ContextRequest::method()`、`RawRequestClone::method()` 与 `AccessLogRecord::method()` 必须保留 HTTP/1/HTTP/2 原始 token。`HttpKnownMethod` 和对应 `knownMethod()` 只用于框架已知的路由、body 与安全策略，`classifyHttpMethod()` 不承担语法校验，`isValidHttpMethodToken()` 也不得要求已知分类。`PROPFIND` 或 `get` 等合法未知 token 不是 HTTP/1 parse error，也不是 HTTP/2 stream protocol error；`ruvia-web` 必须通过正常 error handler 生成 501，只有不符合 token grammar 的 method 才按协议失败处理。RFC 8441 Extended CONNECT 在 request、clone 和 access log 中必须仍为 `CONNECT`；只有 WebSocket 选路允许 `Http2RequestBuilder::routeMethod()` 映射到 GET route，禁止改写原始 request method。

单 byte-range 的解析、representation-size 归一化与 server ignore policy 只能走
`resolveHttpByteRange()`，返回 `HttpByteRangeResolution` 的三个互斥 alternative：
`HttpByteRangeIgnored` 与 `HttpByteRangeUnsatisfiable` 不得携带 offset/length，只有
`HttpResolvedByteRange` 可以通过 `offset()`/`length()` 暴露非空、已 clamp 的切片。禁止恢复
`HttpRangeOutcome + default HttpByteRange` tuple、公开构造任意零长度 range，或在 `ruvia-web`
先用独立 comma helper 预扫再调用 resolver。依据
[RFC 9110 §14.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.1)，range-unit 大小写不敏感；
依据 [§14.1.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.1.2)，超大合法十进制必须防止
整数转换溢出并保持 huge start/last/suffix 的比较与 clamp 语义；依据
[§14.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.2)，unknown unit 必须 ignore，当前
single-range server 对 invalid/multipart set 与空 representation 统一选择允许的 ignore policy。
`ruvia-web` 只能把 ignored、unsatisfiable、resolved 分别映射为完整 200、带 unsatisfied
Content-Range 的 416、单切片 206，不得重写协议判断。

multipart boundary 必须先构造成固定容量、拥有自身字节且符合 RFC 2046 1–70 字节 grammar 的
`MultipartBoundary`；不得把裸 `string_view` 分别传给 buffered、streaming 和 Content-Type 调用链。
Content-Type 提取必须返回 `HttpMultipartBoundaryParseResult` 的 boundary/failure 判别结果；part header
提取必须返回 `HttpMultipartPartHeaderParseResult` 的 headers/failure 判别结果，禁止恢复 status + output
parameter。共享 delimiter scanner 必须返回 `HttpMultipartDelimiterResult`，用 no-match、need-input、
`HttpMultipartPartDelimiter`、`HttpMultipartCloseDelimiter` 四个互斥 alternative 表达状态；只有候选
delimiter 可以暴露 offset，只有完整 regular/close delimiter 可以暴露 line bytes，禁止恢复
`status + offset + lineBytes` 字段 tuple。

`MultipartParser` 只暴露 `feed()`、显式 EOF 的 `finishInput()`，并返回 `MultipartPollResult`：结果只能是
`MultipartPollNeedInput`、`MultipartStreamPart`、`MultipartPollDone` 或 `MultipartPollFailure`，只有 part
alternative 能暴露 borrowed metadata/body 及 `MultipartChunkPhase`，只有 failure 能暴露
`MultipartParseError`。malformed/incomplete wire 必须走 typed failure，不得抛 wire-format exception；
`ruvia-web` facade 再统一映射为无分配 `HttpProtocolError`。禁止恢复 `PollStatus + optional part`、会在非
part 状态抛异常的 `part()` accessor，或 `partBegin`/`partEnd` 布尔组合。边界行起始、transport-padding、关闭
分隔符和 chunk/EOF 歧义由 `ruvia-http` 的同一 scanner 判定，符合
[RFC 7578 §4.1](https://www.rfc-editor.org/rfc/rfc7578.html#section-4.1) 与
[RFC 2046 §5.1.1](https://www.rfc-editor.org/rfc/rfc2046.html#section-5.1.1)。`ruvia-web` 只负责喂入
body、通知 EOF，并在 done 后消费协议语义上忽略的 epilogue，确保连接复用前 HTTP body 已完整读取；
不得重扫 delimiter bytes 或从状态枚举重建协议阶段。

`ruvia-web` 拥有 HTTP 之上的应用能力：App/Context/Router/middleware/controller、route validation、session、CSRF、JWT、rate limit、CORS 策略与中间件、安全头中间件、静态文件目录扫描/索引/产品配置、AutoHTTPS redirect、DB/Redis 集成、WebSocket route 绑定等。它们可以读写 HTTP header，但这不等于它们属于 HTTP 协议本体。

边界判断：如果代码决定“字节如何解析/分帧/序列化、连接是否保持、协议升级是否成立、协议失败对应哪个 HTTP status”，应放在 `ruvia-http`；如果代码决定“协议失败如何变成应用 error code/JSON envelope，或某个 Web 产品/路由/中间件/配置要不要设置某些 header 或执行某种策略”，应放在 `ruvia-web`。Router/error handler 不得设置 `Connection: close` 或接收 `closeConnection` 参数；HTTP/1 runtime 必须在知道 request-body/keep-alive 状态后统一最终化连接语义。流式响应在 handler 前由 `ruvia-http` 的 `Http1ResponseStreamPlan` 绑定 request connection plan、body 状态、chunked/close-delimited framing 和类型化的外部强制关闭策略；提交 response head 时必须再由 `PreparedHttp1ResponseStream` 合并 response 的 `Connection` 选项、规范化响应信号并产出最终 `Http1ServerConnectionPlan`。prepared 结果还必须携带唯一 `Http1ResponseHeadPlan`，其 framing 只能是 `Http1BufferedResponseHead`、`Http1ChunkedResponseStreamHead` 或 `Http1CloseDelimitedResponseStreamHead`；`appendResponseHead()` 只能接收该 plan，禁止恢复 `policy + suppressAutoContentLength` 标量入口。chunked alternative 由 writer 唯一生成 canonical `Transfer-Encoding: chunked`，不得保留应用自定义 transfer coding；body-open close-delimited alternative 必须过滤应用 `Transfer-Encoding` 与 `Content-Length`，HEAD/304 等 body-suppressed 响应可以保留 representation length metadata，但对 HTTP/1.0 仍不得发送 Transfer-Encoding。该边界遵守 [RFC 9112 §6.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1) 与 [§6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3)。`ruvia-web` 只能驱动 prepared plan，不得把 pre-commit plan 的下界当成最终 socket 生命周期结论。method/status 的响应 content 语义必须由无分配 `HttpResponseContentSemantics` 唯一分类为 `HttpInformationalResponseContent`、`HttpProtocolSwitchResponseContent`、`HttpConnectTunnelResponseContent`、`HttpResponseWithoutContent` 或 `HttpResponseWithContent` alternative；HTTP/1 client、HTTP/2 client 和 `HttpResponseBodyPlan` 必须消费同一结果，禁止各自重写 HEAD/1xx/204/304/CONNECT 判断。发送侧再由 `HttpResponseBodyPlan` 绑定 status write policy，buffered 响应由 `HttpBufferedResponseWritePlan` 绑定 representation length 与最终 send-body 结论；HTTP/1、HTTP/2 和 streaming 调用链不得在 `ruvia-web` 通过 `skipBody` 等松散布尔值重复判断。HEAD 保留对应 GET representation 的协商 metadata 和长度，但 HTTP/1 不发送 payload、HTTP/2 不发送 DATA。`Http2Connection` 必须拥有完整的本地发送 phase：interim head 不关闭 initial-head phase，request/final response/WebSocket initial head 只能成功提交一次，DATA 只能在 body-open phase 提交。`submitData()` 的 `kQueued` 表示 core 已复制并接管未发送后缀，调用方不得重试同一输入；`kBackpressured` 表示本次零接管，调用方等待已排队数据 drain 后重试。已接管的终止信号与已经物化到输出缓冲的 `END_STREAM` 必须分别记录；任何本地或对端 reset/reject 都必须统一清理未物化 DATA、trailers 和 drain 通知。所有 inbound HEADERS field block 即使最终因 local reset、drain refusal 或 stream error 被丢弃，也必须连续接收同 stream 的 CONTINUATION，并在 detached scratch 中完整 HPACK 解码后才应用完成动作；owner 在 field block 中途关闭 live stream 时必须转移已累积的压缩字节。local RST 必须是本端在该 stream 的最后一帧，不得在 discarded block 完成后再次发送 RST；无法完成强制解压时必须使用 connection-level `COMPRESSION_ERROR`。RFC 9113 已废弃 priority tree，合法形状的 dependency/weight 只能忽略，不得再触发 stream 状态或 reset。`ruvia-web` 只能驱动这些类型化状态，不得自行复制 HTTP/2 head/data/terminal 判断。`ruvia-web` 只能用 core runtime、asio/TLS/socket/timeouts 驱动 `ruvia-http` 的协议 core，不要重写协议判断；`ruvia-http` 可以提供 header token 解析、value 校验、`Vary` 合并等通用工具，但不得依赖 Context/App/Router。

buffered response 的 `HttpResponseBodyPlan` 必须同时拥有 exact numeric status、status write policy
与 method/status content semantics；`HttpBufferedResponseWritePlan` 再绑定 representation length 和
最终 send-body 结论。它只能由 `httpBufferedResponseWritePlan(HttpKnownMethod, const HttpResponse&)`
构造，禁止恢复接受外部 `HttpResponseBodyPlan + HttpResponse` 的 loose overload。HTTP/1 serializer
和 HTTP/2 head planner 都必须在 wire/HPACK mutation 前拒绝 response status 与 plan status 不一致，
H2 使用 `kResponseStatusMismatch`。

HTTP/1 的 final `Http1ResponseHeadPlan` 必须拥有准确的 HTTP/1.0/HTTP/1.1 status-line 版本；
`appendResponseHead()` 只能从该 plan 序列化版本，HTTP/1.0 request 必须得到 `HTTP/1.0` response line，
符合 [RFC 9110 §2.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.5)。buffered 输出必须组合为
不可默认构造的 `Http1BufferedResponsePlan`，把 `HttpBufferedResponseWritePlan` 与 head plan 绑定；
`Http1BufferedResponseHead::contentLength()` 必须与 write plan 的 representation length 相同，
`ruvia-web` writer 不得分别接收后再重建版本或长度，禁止恢复独立的
`http1BufferedResponseHeadPlan()` factory。

HTTP/1 parser 必须先产出不可拆分的 `Http1ServerConnectionPlan`，把准确的
`HttpProtocolVersion::kHttp10`/`kHttp11` 与 `Http1ConnectionDisposition` 直接绑定；只能用
`http1PlanHttp10RequestConnection()` / `http1PlanHttp11RequestConnection()` 构造已解析请求，
`http11Close()` 只允许用于尚无有效 request version 的错误响应。禁止恢复有损的
`Http1ResponseConnectionSignal`、`responseSignal()` 或接受通用 `HttpProtocolVersion` 的
`http1PlanRequestConnection()`；
request、body reader、buffered、streaming 与 WebSocket failure 分支都只能传递或
收紧同一个 plan。不得恢复 `http1RequestNeedsKeepAliveSignal`、`keepAlive`、`closeAfterWrite` 等
并行标量或布尔值。请求 body 是否完整消费必须用 `Http1RequestBodyConsumption` 收紧 plan，外部
request-limit 用 typed close policy 收紧 plan，应用响应的 `Connection: close` 则在最终化时收紧
plan；`requireClose()` 必须保留原版本，任一路径都不得仅传 disposition 后再重算 status-line 或
Connection field。
重复 `Connection` field line 必须作为同一个 list 检查；任一 `close` token 都收紧 plan 并把矛盾
字段规范化为唯一 `Connection` field，通常为 `close`；若响应保留 `Upgrade` field，则必须为
`close, Upgrade`，不能在关闭规范化时破坏 hop-by-hop 配对。HTTP/1.0 复用时，即使已有其他
connection option，也必须
补入 `keep-alive`，不得因“header 已存在”跳过必要信号。
`Connection` option 与 `Upgrade` protocol list 必须统一走 `ruvia-http` 的
`HttpConnectionOptions` / `HttpUpgradeProtocols` 无分配状态，server request、client request writer、
client response parser 和 server response finalizer 不得各自拆 token。接收路径按 RFC 9110
忽略 header 上限内的空 list member，发送路径必须拒绝空 member 和非 token；重复 field line 必须延续
同一状态。发送 `Upgrade` 或 `TE` 时必须分别已经发送 `Connection: Upgrade` 或
`Connection: TE`。不得恢复 `HttpRequestFlags`、`httpUpdateConnectionFlags` 或 WebSocket
`request + flags` 双输入；HTTP/1 WebSocket 握手必须从同一个已解析 `HttpRequest` 扫描完整 repeated
field set，不能用 known-header 的单值 cache 决定 Upgrade 或重复握手字段。
server response finalizer 是响应 `Upgrade` 配对的唯一 owner：缺少 option 时必须自动补
`Connection: Upgrade`；若最终连接计划关闭，则必须生成 `Connection: close, Upgrade`，不得抛弃
426/其他响应中的 Upgrade 广告，也不得留下一条失去 Connection option 的 Upgrade field。
出站状态和协议控制必须先统一产出不可默认构造的 `HttpFinalResponseControlPlanResult`；result 只能是
`HttpFinalResponseControlPlan` 或 `HttpFinalResponseControlPlanFailure` alternative，只有 failure 可通过
`HttpFinalResponseControlPlanError` 暴露错误。成功 plan 必须再且仅能持有
`Http1FinalResponseControl` 或 `Http2FinalResponseControl`；只有 HTTP/1 alternative 可以暴露一次解析完成的
`HttpConnectionOptions` / `HttpUpgradeProtocols`，finalizer 不得重扫 response fields。禁止恢复
`HttpFinalResponseControlStatus`、`status()/accepted()` 加默认 Upgrade payload 的 tuple，也不得让
failure 暴露貌似可提交的 plan。
HTTP/2 buffered、streaming 与成功 CONNECT final head 都必须在 HPACK/output/stream mutation 前取得
`Http2FinalResponseControl`，encoder 必须接收该 proof。依据
[RFC 9113 §8.2.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2.2)，endpoint 生成的 response
若含 `Connection`、`Proxy-Connection`、`Keep-Alive`、`Transfer-Encoding`、`Upgrade` 或 `TE` 必须
事务性拒绝；`TE: trailers` 例外只适用于 request。禁止把 origin/application response 当成 intermediary
translation 静默过滤这些字段，也禁止只在 buffered/streaming/CONNECT 的部分路径验证。

有效 HTTP status 仅为 `100..599`，
`HttpResponse`/`Context`/普通 buffered/streaming handler 只能表示 `200..599` final response；非 101
的 1xx 必须使用无 body、不可变状态的 borrowed `HttpInterimResponseHead`。HTTP/1.1 只能通过无分配、
事务式 `Http1InterimResponseWriter::prepare()` 编码，HTTP/2 只能通过
`Http2Connection::submitInterimResponseHead()` 提交；`ruvia-web` 的自动 100 Continue 必须消费
`HttpServerExpectationAction`，HTTP/1 驱动前者，HTTP/2 在等待 DATA 前驱动后者。不得保存或拼接独立
status-line/HPACK 字节，socket 写失败必须作为 transport `system_error`，不得伪装成
无效请求。header storage 和字符串必须稳定到同步 prepare/submit 返回，禁止 initializer-list/rvalue
container 造成悬垂。两种 writer 必须在 output buffer/HPACK/stream mutation 前共享完整字段校验，拒绝
无效字段、Content-Length、Transfer-Encoding、Trailer 和重复 singleton；HTTP/1 还必须事务性执行
64-field/64-KiB 上限、Connection/Upgrade 配对并把 `Connection: close` 产出为
`requiresFinalConnectionClose()`，且不得隐式注入 Server/Date 产品策略；HTTP/2 另行拒绝全部
connection-specific field。101 会转移连接所有权，只能由
专用协议 driver 生成，不得装进以上任一通用 response type。HTTP/1 的 426 在任何
连接/header mutation 前必须携带至少一个语法有效的 `Upgrade` protocol，并继续由 finalizer 配对
`Connection: Upgrade`；HTTP/2 因 RFC 9113 禁止 Upgrade field，必须把 426 作为 typed invalid message
事务性拒绝，`ruvia-web` driver 不得在拒绝后把 stream 留在无响应的 open 状态。专用 HTTP/1 WebSocket
握手和 HTTP/2 Extended CONNECT 仍分别拥有真正的协议转换，不得回退为通用 `HttpResponse` 状态。
通用 response message 必须保持协议版本中立：`HttpResponse`、`Context` 的 body/text/json/html/
redirect helper 和 `Context::ResponseInit` 只携带数值 status code，不得恢复 `statusText()`、自定义
reason-phrase 参数、`HttpStatusEntry` 或预烘焙 status-line 表。reason phrase 只是 HTTP/1 wire 的可选
展示文本；final/interim writer 在序列化时统一调用 `httpReasonPhrase()`，未知 status（例如 299）返回
空 phrase，但仍按 [RFC 9112 §4](https://www.rfc-editor.org/rfc/rfc9112.html#section-4) 保留 status code
后的必需 SP。不得再按 4xx/5xx 类别伪造 “Bad Request”/“Internal Server Error”。HTTP/2 按
[RFC 9113 §8.3.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.3.2) 只编码 `:status`，不得读取
reason phrase。`ruvia-web` 的 `HttpErrorInfo::statusText` 只允许作为 JSON/error label；它可以用已知
phrase 作展示 fallback，但不得流入任何协议 wire response。
`ruvia-http` 的 HTTP/1、HTTP/2 final/interim response encoder 只保留调用方显式提供的
`Server` field，默认不得生成 `Server` product identity；专用 WebSocket 握手路径也不得
隐式注入产品标识，更不得在纯协议 target 硬编码 `Server: ruvia`。产品 banner 属于
`ruvia-web`/应用策略。[RFC 9110 §10.2.4](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.2.4)
将 `Server` 定义为可选字段，并警告不要暴露过细的实现信息。不要因此移除规范要求的
`Date`：HTTP/1/HTTP/2 final response 在缺失时仍由协议 encoder 生成，以满足
[RFC 9110 §6.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.6.1) 对带时钟 origin server
2xx/3xx/4xx 响应的强制要求；非 switching 1xx writer 必须继续精确编码，不得自动添加。
`PreparedHttp1ResponseStream` 必须在 commit 时继续合并 method/status：HTTP/1.0
body-allowed stream 使用 close-delimited 并关闭连接，但 HEAD/204/205/304 已禁止 payload，在
request 允许持久化且外部策略未强制关闭时是 self-delimited，必须允许复用并规范化
`Connection: keep-alive`。

response trailer 只能作为 `ResponseStreamWriter::end(std::span<const HttpHeaderView>)` 的完整
终止 section 提交，禁止恢复逐字段 `addTrailer()` 或在 runtime 先接收、结束时静默丢弃。
`ruvia-http` 的 `ResponseStreamCommitPlan` 必须把最终数值 status、选定 framing、
method/status 派生的 `HttpResponseBodyPlan`、head 后的 body-open/trailers-only/message-ended
phase 与实际 trailer framing 绑定为一个结论；`prepareResponseStreamHead()` 必须拒绝 status
与 plan 不一致的 response，Web sink 必须保留完整 committed plan，不得再拆成
`trailerFraming_` 等平行字段。HTTP/1 仅
body-allowed chunked response 可发送 trailer，HTTP/1.0 close-delimited 和 HEAD/1xx/204/304
必须报告 unavailable；HTTP/2 必须用 `Http2LocalSendState` 的显式
`Http2LocalResponseTrailersOnly` alternative 为禁止 DATA 的 response 保留 trailer 终止能力，以
trailing HEADERS 携带 `END_STREAM`，不得把它标成 body-open
或回退为空 DATA。`Http2Connection::submitResponseTrailerSection()` 只能在 final response
head 后原子接管非空 section，必须先验证全部字段及 Content-Length 完成状态再修改 HPACK/stream
状态，并以 `Http2ResponseTrailerSubmitStatus` 显式报告 closed、wrong phase、empty、invalid field
或 incomplete length；`finishResponse()` 仍是终止帧顺序的唯一 owner。

stream route 与 runtime completion 必须使用互斥 alternative：handled route 不得携带 dummy
`HttpResponse`，buffered/failure-before-commit 才拥有 response；completed、peer-aborted-after-commit
与 failed-after-commit 只能携带 committed plan 的真实 status；peer-aborted-before-commit 不得伪造
status。HTTP/1 close-delimited stream 必须先记录 access log 再关闭 socket，HTTP/1/HTTP/2 都不得从
默认 response 重建 200。禁止恢复 `RouteStreamDispatchOutcome + HttpResponse` tuple、
`responseStreamDispatched`、`result.streamed()`/`hasBufferedResponse()` 这类松散调用链。
自动 `ResponseStreamWriter::end()` 必须在 route coroutine 内、所绑定 `Context` 仍存活时执行；外层
transport driver 只能消费 committed plan，不得在 route 返回后通过 sink 保存的 `Context*` 再构造 head。

HTTP/2 本地发帧权限必须由一个 `Http2LocalSendState` 独占，并且只能是
`Http2LocalHeadPending`、`Http2LocalRequestContentOpen`、`Http2LocalResponseContentOpen`、
`Http2LocalResponseTrailersOnly`、`Http2LocalConnectPending`、`Http2LocalTunnelOpen`、
`Http2LocalEndStreamQueued`、`Http2LocalEndStreamCommitted` 或 `Http2StreamAborted` 之一。request、
response、tunnel DATA 权限不得合并；trailers-only 不得开放 DATA；queued END_STREAM 表示 core 已
接管但仍排在 flow-control DATA/trailer 后，committed END_STREAM 表示终止 HEADERS/DATA 已物化到
输出缓冲。只有 `Http2StreamAborted` 可以拥有不可变且非 `kNone` 的
`Http2StreamCloseSource`；它统一表示 local/peer RST_STREAM 与被 peer GOAWAY last-stream-id
排除的请求，后者不是 reset，不得恢复 `Http2LocalReset`、`isReset()`、`markReset()` 或
`removeReset()` 这套错误词汇。状态查询统一使用 `isAborted()`；`abort(source)` 是异常终止的唯一
mutation，必须同时进入 remote-aborted alternative 并清除 ready-queue ownership；正常 END_STREAM
只能进入 committed half-close，不能伪装成 abort。
`Http2StreamLifecycle` 与 `Http2StreamState` 只暴露一个 const `localSend()` view，禁止恢复
`Http2LocalSendPhase + Http2LocalMessageKind + bool + closeSource` 笛卡尔积、对应 forwarding
accessor 或分散 mutation。`Http2LocalSendState` 的 transition 只能由 friend
`Http2StreamLifecycle` 驱动，lifecycle mutation 又只能由 friend `Http2StreamState` 驱动；Connection
只能调用 `Http2StreamState` 的 typed mutation，其他层只能观察 alternative，不得绕过 stream-owned
tunnel/content 联合校验。该状态必须遵守
[RFC 9113 §5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1) 的 half-closed(local) 转换、
[§6.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.1) 的 DATA/END_STREAM 限制和
[§6.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.2) 的 HEADERS/END_STREAM 语义。
RST_STREAM 的 whole-stream closed 转换遵守
[§6.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4)，GOAWAY 排除未处理请求的语义遵守
[§6.8](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8)。

HTTP/2 远端收帧权限必须由一个 `Http2RemoteReceiveState` 独占，并且只能是
`Http2RemoteHeadPending`、`Http2RemoteHeadEndStreamPending`、`Http2RemoteContentOpen`、
`Http2RemoteConnectPending`、`Http2RemoteConnectPendingEndStream`、
`Http2RemoteConnectRejectedAwaitingEndStream`、`Http2RemoteTunnelOpen`、
`Http2RemoteEndStream` 或 `Http2RemoteAborted` 之一。initial/final HEADERS 必须原子选择普通 content、
CONNECT 决策、tunnel 或 peer half-close 语义；1xx 继续停留在 head-pending。禁止恢复
`headersDecoded()`、`bodyEnded()`、`peerEndStream()` 及其独立 mutation，也禁止用“CONNECT 的 HTTP
content 必为空”冒充“对端已发送 END_STREAM”。server 拒绝尚未 half-close 的 CONNECT 后，只能接受
空 DATA，并以其中的 `END_STREAM` 正常终止；pending CONNECT 也必须允许空 terminal DATA 在决策前
完成 peer half-close。接受仍开放的 CONNECT 后必须进入 `Http2RemoteTunnelOpen`，延迟消费
tunnel DATA 时在真实 peer END_STREAM 前同时补回 connection 与 stream receive window。
`Http2StreamLifecycle` 与 `Http2StreamState` 只能暴露一个 const `remoteReceive()` view，mutation 继续
遵守 lifecycle 到 stream 的 friend-only ownership chain。该状态遵守
[RFC 9113 §5.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1) 的 peer half-close、
[§8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1) 的 message termination、
[§8.5](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.5) 的 CONNECT tunnel DATA 与空 terminal
DATA，以及
[RFC 8441 §5](https://www.rfc-editor.org/rfc/rfc8441.html#section-5) 的 Extended CONNECT orderly
closure。

HTTP/1 parser 必须产出不可变的 `Http1RequestBodyPlan`，其 framing 只能是
`Http1RequestWithoutBody`、`Http1KnownLengthRequestBody` 或 `Http1ChunkedRequestBody`；缺少 framing
与显式 `Content-Length: 0` 必须保持不同 alternative。只有 known-length alternative 可以暴露
`contentLength()`，只有 chunked alternative 可以暴露 chunked 前的一项 transfer-coding 顺序；Expect
状态是 plan 的共同 metadata，并从 active alternative 是否需要消费得出唯一 action。plan 的构造权必须
private 给 `Http1ServerRequestParser`，安装消费者与 `ruvia-web` 不得手工构造、注入未经 wire parser
验证的 coding count，或恢复 `Http1RequestBodyMode + contentLength + transferCodings` tuple、公开
`none()`/`knownLength()`/`chunked()` factory、五个松散标量的 `http1PlanRequestBody(...)`。
buffered/stream reader 与 WebSocket gate 只能接收该 plan，并按 active alternative 驱动；禁止读取非
known-length 状态的假 0、非 chunked 状态的假空 coding，或恢复 `contentLength`、`chunked`、
`transferCodings`、`sendContinue` 等松散构造参数及 `http1WantsContinue` 并行判断。alternative 划分必须
遵循 [RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) 的 ordered request framing。
请求 `Transfer-Encoding` 必须以 chunked 结尾；允许一个受支持的
gzip/deflate coding 位于 final chunked 前（例如 `gzip, chunked`），先去 chunk framing 再增量
解码 transfer coding；未知 coding 映射 501，HTTP/1.0 Transfer-Encoding 拒绝。

HTTP/1 chunked framing 只能由 `ruvia-http` 的同一协议实现产出两种消费视图。增量入口必须使用
HTTP/1 专名的 `Http1ChunkedBodyDecoder`，其结果只能是 `std::variant` 中互斥的
`Http1ChunkDecodeNeedMore`、`Http1ChunkDecodeBodyChunk` 或 `Http1ChunkDecodeComplete`；三者都拥有
本次调用已消费的输入前缀，但只有 body-chunk alternative 可以暴露 borrowed bytes，禁止恢复
`kind + body + consumedBytes` 松散 event。整消息扫描只能返回 `HttpChunkScanNeedMore`、
`HttpChunkScanComplete` 或 `HttpChunkScanFailure`；只有 complete 拥有最终 message boundary，只有 failure
拥有 typed chunk error，need-more/failure 不得暴露貌似可复用的 consumed length。`ruvia-web` 只能驱动
这些 accessor 并管理读缓冲，不得解析 size line、delimiter 或 trailer。

chunked complete 必须严格遵守
[RFC 9112 §7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1)：只有读到 zero-size last chunk、
可选 trailer section 以及终止空行后才能完成；缺少该边界的输入必须保持 need-more，并按
[RFC 9112 §8](https://www.rfc-editor.org/rfc/rfc9112.html#section-8) 作为 incomplete message 处理，禁止
把 transport EOF 当作隐式完成。旧的通用 body framer 头和 sentinel/status tuple 不得恢复。

入站 Expect 必须由 `ruvia-http` 的固定大小 `HttpRequestExpectations` 统一解析 RFC 9110
`#expectation` list：重复 field line 延续同一状态，recipient 侧空 member 必须忽略，
`100-continue` 大小写不敏感，其他非空 member 只记录为 `kUnsupported`。Expect 是可扩展语义；合法
扩展不得恢复为 `HttpParseError::kExpectationFailed`，也不得成为 HTTP/2 stream protocol error。
HTTP/1 的 `Http1RequestBodyPlan` 与 HTTP/2 的 `Http2StreamState` 必须从该状态产出同一个
`HttpServerExpectationAction`；HTTP/1.0 忽略 100-continue。`ruvia-web` 产品当前选择通过正常 error
handler 对 unsupported 返回 417；若 request content 将到达，则 HTTP/1 body reader 驱动
`Http1InterimResponseWriter`，HTTP/2 session 必须在等待 DATA 前立即驱动
`submitInterimResponseHead()`，禁止各版本重扫原始 header 或保存并行布尔状态。

HTTP protocol version 是每条 message 的 control data，必须统一使用 `ruvia-http` 的
`HttpProtocolVersion::{kHttp10,kHttp11,kHttp2}`。入站 `HttpRequest` 与 owning
`HttpClientResponse` 只通过 `protocolVersion()` 暴露强类型值；不得恢复借用字符串的
`httpVersion()`/`httpVersion_`，也不得创建 `HttpResponseProtocolVersion` 等平行枚举或
`isHttp11` 持久布尔状态。HTTP/1 parser 必须先按 start-line 规则验证大小写敏感的 wire token，
然后只转换一次；HTTP/2 由 connection protocol 直接记录 `kHttp2`，不得伪造 `"HTTP/2"` wire
view。connection persistence、response-stream framing、WebSocket handshake、client response
framing 与 final-response control 必须消费同一枚举。

access log 也必须消费同一个 message control value。`AccessLogRecord` 只借用 callback 期间仍有效的
唯一 `HttpRequest`，`method()`、`knownMethod()`、`path()` 与 `protocolVersion()` 都从该 request 派生；
remote address 仍由 Web transport 单独提供。`recordHttpAccess()` 禁止接收 `bool http2` 或另一个版本
参数，HTTP/1 与 HTTP/2 调用点都不得重建版本。禁止恢复 `http2()`、`http2_` 或复制
`method_ + knownMethod_ + path_` 的平行 tuple；该日志边界不得增加分配、虚调用或 type-erasure。
streaming status 必须来自提交 response head 的同一个 `ResponseStreamCommitPlan`；buffered HTTP/2
status 必须来自 `Http2SubmittedResponseHead<HttpBufferedResponseWritePlan>` 持有的同一 write plan。
buffered HTTP/1 写出必须返回 `Http1BufferedResponseWriteResult`，并且只能是 completed、
failed-before-commit 或 failed-after-commit：completed 与 failed-after-commit 的 status 必须来自实际
序列化 head 的 `Http1BufferedResponsePlan`，failed-before-commit 只能携带 transport error，不能携带
status。只有 composed write 的累计 bytes 覆盖完整 response head 才算 committed；部分 status line/header
不是 HTTP response。禁止恢复 `Task<void> + std::error_code&` writer、写出后读取
`HttpResponse::status()` 或“只要尝试 write 就记 response-completion log”的调用链。
`ruvia-web` 必须用 `Http2BufferedResponseDispatchResult` 的 completed、peer-aborted-before-commit、
peer-aborted-after-commit、failed-before-commit、failed-after-commit 互斥 alternatives 表达完成状态，
只有 post-commit alternatives 可以携带 status。所有有效 buffered 分支（包括提前产生的 417/429）
必须统一经过 prepare/compress/CORS、submit 和 access-log 调用链；在 final head 前被 peer abort 或
事务性拒绝的请求没有 HTTP status，不得用 `HttpResponse::status()` 或默认 200 调用 response-completion
access log。

公开整消息入口只允许 `Http1RequestParser`，其 `Http1RequestParseResult` 必须以
`Http1RequestNeedMore`、`Http1ParsedRequest`、`Http1RequestParseFailure` 三个互斥 alternative
表达结果。need-more 只能用可选 `requiredTotalBytes()` 表达 Content-Length 已知的所需总长度；
success 才能读取 request、唯一 `Http1RequestBodyPlan`、第一条消息的 consumed length 和完整
`wireBody()`。chunked wire body 必须保留 size line、data、delimiter 与 trailer section 供共享 decoder
驱动，不得再把 body 静默置空；failure 不得携带 `HttpParseError::kNone`，已知 required total 不得为零。
内部复用入口必须使用 HTTP/1 专名的 `Http1ServerRequestParser` 和
`Http1ServerRequestParseState`。Web 热路径只能在 `kRequestHeadReady` 分派；完整消息扫描成功只能是
`kRequestMessageReady`，不得再用同一个 `complete` 状态表达两者。`kNeedRequestHead` 与
`kNeedRequestBody` 必须分离，`messageBytes` 只在 message-ready 有效，可选 `requiredTotalBytes`
只表达 fixed-length body 的未来所需容量；不得恢复公共 `HttpParseStatus`，也不得用同一个
`consumedBytes` 同时表示已消费长度和未来所需容量。

outbound HTTP/1 request 必须先把 content 建模为 `HttpClientRequestContent::none()` 或
`bytes(...)`，禁止恢复无法区分 absent 与显式空 content 的裸 `request.body`；`bytes("")` 必须明确
生成 `Content-Length: 0`，`none()` 不得生成 content framing。该值必须是
`HttpClientRequestWithoutContent` 与 `HttpClientRequestBytes` 的判别联合，只有 bytes alternative
可以暴露 `value()`；禁止恢复 `HttpClientRequestContentMode + value` tuple 或让 absent 状态读取假空
bytes。该区分必须遵循
[RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) 的 ordered request framing。
完整 wire 计划只能由公开、零分配的
`Http1ClientRequestWriter` 写入 caller-provided head buffer：它必须在修改 buffer 前原子校验 method、
direct-origin target、header、数量/大小与 TRACE/OPTIONS content 约束，统一生成首个 `Host`、唯一
Content-Length、外部关闭策略要求的 `Connection: close` 和 `Expect: 100-continue`。
`Http1ClientRequestWirePolicy` 是 Expect 的唯一入口；调用方提供 Host、Content-Length、
Transfer-Encoding、Trailer 或 Expect 必须类型化拒绝，禁止 writer 与 header list 形成两个 framing/
content-gate source。调用方提供的 `Connection` / `Upgrade` 必须在写出前按 sender list grammar
完整验证；`Upgrade` 与 `TE` 缺少对应 Connection option 必须类型化拒绝。CONNECT 只能走
`prepareConnect()`，由 typed `HttpOrigin` 生成带显式非零 port 的
authority-form target 与对应 Host，普通 origin-form 入口必须拒绝 CONNECT。

request prepare 结果必须是 buffer-too-small、`PreparedHttp1ClientRequest` 或 typed failure 三态，
失败/容量不足不得留下 partial bytes。Prepared 同时绑定 head 与不可变
`Http1ClientRequestContentPlan`，其 alternative 只能是 `Http1ClientRequestWithoutContent`、
`Http1ClientImmediateRequestContent` 或 `Http1ClientContinueGatedRequestContent`；只有 immediate 与
continue-gated 可以暴露 `bytes()`，后者必须生成 Expect 且仅允许非空 content。禁止恢复
`Http1ClientRequestContentDisposition + bytes` tuple、plan-wide `bytes()` 或在无 content alternative
读取假空 payload。外部 runtime 决定有限等待时长，但不得重扫 header 判断 gate。
`Http1ClientResponseParser` 只能从对应 Prepared 构造，禁止暴露 response context accessor，亦禁止从
裸 request、method 或 header/close/expect 标量重建，以确保 method、Upgrade offer、content gate 与
close policy 和实际 wire 一致；private response context 也不得复制 `expectsContinue` 布尔值，parser
必须从 continue-gated alternative 初始化 Expect exchange 状态。

outbound HTTP/1 response head 必须通过公开 sans-I/O `Http1ClientResponseParser` 解析完整增长缓冲；
parser 是 per-request stateful exchange，必须跨 1xx 复用直到 final/tunnel/upgrade，final 或 protocol
failure 后再次 parse 必须类型化拒绝。它必须自己查找 CRLF CRLF，并返回判别式
`Http1ClientResponseParseResult`：
`Http1ClientResponseNeedMore`、拥有 PMR `HttpClientResponse` 的 parsed head，或带类型化错误的
`Http1ClientResponseParseFailure`。成功结果的 `consumedBytes()` 必须精确指向 head 末尾，余下字节
由外部 runtime 按 body/tunnel/upgrade/follow-up response 驱动。协议失败路径必须零分配、不得抛
wire-format exception，也不得通过 response out-parameter 暴露半写入状态；只有整个 head 与 plan
验证成功后才能物化 owning response，成功物化时的资源耗尽仍可抛出。

parsed head 必须携带不可变、判别式的 `Http1ClientResponsePlan`，其 alternative 只能是
`Http1ClientInformationalResponse`、`Http1ClientResponseWithoutContent`、
`Http1ClientKnownLengthResponse`、`Http1ClientChunkedResponse`、
`Http1ClientCloseDelimitedResponse`、`Http1ClientConnectTunnel` 或
`Http1ClientProtocolUpgrade`。只有 known-length alternative 暴露 `contentLength()`，只有 chunked/
close-delimited 暴露 transfer-coding 解码顺序，只有可自定界的 final response 暴露
`Http1ClientResponsePersistence`；close-delimited 必须隐含 EOF consumption + close，informational/
tunnel/upgrade 不得伪装成 body mode 或可复用 final response。parser 必须直接构造 alternative，禁止
恢复 `Http1ClientResponseBodyMode + contentLength + transferCodings + connectionDisposition` tuple、
`ResponsePlanData` mutable 中间态，或 `responseMayHaveBody`、`hasTransferEncoding`、`isChunked`、
`closeAfterResponse` 等平行字段。alternative 划分必须遵循
[RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) 的 ordered message-length
precedence；连接复用必须遵循
[RFC 9112 §9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3)，只有完整消费可自定界 response
后才允许 reuse。对 continue-gated request，plan 必须同时产出唯一
`Http1ClientRequestContentSignal`：无关 1xx 为 none，100 为 continue，final/101 为 exchange-complete；
不得让 runtime 从 status/header 重建 content gate。Upgrade 与 Expect 并存时，未先观察到
100 Continue 的 101 必须拒绝；任何非空 request content 还必须由 runtime 在完整写出后调用
`completeRequestContent()`，其 `Http1ClientRequestContentCompletionStatus` 必须区分
completed/already-complete/terminal，
未完成 request content 时不得接受 101。redirect rewrite 也必须使用同一精确 method token 语义。
body-allowed response 没有 Content-Length 且 final coding 不是 chunked 时必须 close-delimited；
HTTP/1.0 默认关闭，仅响应显式 `Connection: keep-alive` 时允许复用；2xx CONNECT 忽略
Content-Length/Transfer-Encoding 并产出 `Http1ClientConnectTunnel`。合法
`101 Switching Protocols` 只有在
request 与 response 都发送 `Connection: Upgrade`、response 选择了 request 已提供的 `Upgrade`
协议且 101 不携带 Content-Length/Transfer-Encoding 时才产出 `Http1ClientProtocolUpgrade`；protocol name
大小写不敏感，protocol version token 必须精确匹配。客户端 framing 不得把 205 与 204 合并：
RFC 9110 虽禁止 server 为 205 生成 content，但 RFC 9112 的 header-terminated 优先级只有
HEAD/1xx/204/304；client 对 205 仍必须消费显式 Content-Length/chunked framing，无 framing 时必须
close-delimited，不能提前复用连接。HTTP/1 request 与 client response 必须共用
`HttpContentLengthState` 完整解析 Content-Length：`5, 5` 等合并列表只有全部十进制值相同才接受，
任一无效或冲突值必须拒绝，禁止各调用链自行取首值或末值。两条调用链也必须共用
`HttpTransferEncodingState`，跨重复 field line 保持 list 顺序，chunked 出现时必须为 final；
gzip/deflate/chunked coding name 不得携带参数，chunk extension 只属于 body 内的 chunk-size line。

outbound client 的公开消息模型统一放在 `HttpClient.h`：`HttpScheme` 与不可变、borrowed 的
`HttpOrigin::http()`/`https()` 绑定 scheme/host/port，工厂必须在构造时校验非空 RFC 3986
`uri-host`，IP-literal 输入统一使用 bracketed IPv6/IPvFuture 形态；borrowed host storage 必须覆盖
origin 生命周期且字节保持不变，所有 rvalue `basic_string` 工厂入口必须 deleted。Host、absolute-form、origin authority 与 redirect 必须共用不可构造非法状态的
`HttpAuthorityView`，保留 absent/empty/numeric port 三态；HTTP origin 比较把 absent/empty 映射到
scheme default，并统一做 host 大小写、percent-encoding hex case 与 unreserved octet normalization；
percent-encoded reserved 字符不得与 raw spelling 等同。numeric port 0 是可表达的
独立 URI origin，是否可连接由外部 transport 决定；CONNECT 仍必须拒绝 empty/zero tunnel port。
redirect authority 必须用 `HttpClientOriginAuthorityStatus` 区分 same、different 和 malformed/userinfo；
后者是 invalid Location，不能压成合法的跨 origin。禁止恢复 helper 侧的
`validateHttpOrigin`/`kInvalidOrigin` 补校验、裸 `tls` 布尔、可变 owning host
或可默认构造的空 origin；`HttpClientRequest` 只表达 borrowed method/target/header 与 typed content，
`HttpClientResponse` 与 `HttpClientResponseHeader` 用 PMR 拥有解析结果。没有 client runtime 能兑现的
redirect 次数、Expect 等待、stream decode、TLS 文件、连接池和 timeout 不得伪装成协议模型字段，
也不得恢复拆分的 client model 头或 Fetch 命名。redirect 必须返回 typed method/content plan：
301/302 只允许 POST 历史性地改为 GET，303 使用 GET（HEAD 保持 HEAD），307/308 不得改变 method；
丢弃 content 时必须同时告知外部 driver 移除 representation 与 content-specific fields。Location 是
URI-reference，same-origin resolver 必须相对当前 request target 解析、移除 dot segments，再统一校验
scheme/host/port；response field lookup 不得把 absent、空值和 repeated 混成同一个空 view。

redirect 规则是外部 runtime 要驱动的公开 sans-I/O 协议 API，必须放在公开
`HttpClientRedirect.h` 与 `ruvia` namespace；禁止恢复已安装的
`detail/client/HttpClientRedirect.h`。`lookupUniqueHttpClientResponseHeader()` 只能返回
`HttpClientResponseHeaderLookupResult` 的 `HttpClientResponseHeaderAbsent`、
`HttpClientResponseHeaderFound`、`HttpClientResponseHeaderRepeated` 三种 alternative，且只有
Found 能暴露 borrowed value。`resolveHttpClientSameOriginRedirectTarget()` 必须接收 PMR resource，
返回 move-only `HttpClientRedirectTargetResult`：只有 `HttpClientRedirectTarget` 拥有解析后的 target，
只有 `HttpClientRedirectTargetFailure` 暴露 `HttpClientRedirectTargetError`；禁止恢复
`HttpClientRedirectTargetStatus + std::pmr::string& outTarget`、失败时保留旧输出或任何 status/payload
tuple。Location 是单个 URI-reference，重复 field 不得猜测恢复，符合
[RFC 9110 §10.2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.2.2)；relative reference 的
path/query 继承、merge 与 dot-segment removal 必须符合
[RFC 3986 §5.2](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.2)，并保留
[RFC 3986 §5.3](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.3) 的 undefined/empty component
区别。`HttpClientOriginAuthorityStatus` 的 alternatives 都没有 payload，因此继续使用 enum，不为形式
统一强行包装 variant。

WebSocket close handshake 必须由 `ruvia-http` 的 `WsClosePhase` 与不可变 `WsOutputPlan` 统一拥有：
输入只走 `poll()`；它必须返回 `std::optional<WsEvent>`，其中 `std::nullopt` 只表示需要更多 transport
bytes，每个实际事件必须由零分配 `std::variant` 持有且仅持有 `WsMessageEvent`、`WsPingEvent`、
`WsPongEvent`、`WsCloseEvent`、`WsProtocolErrorEvent` 或 `WsTransportEndEvent` 之一。禁止恢复 `kNone`
event 和可写的 `kind + opcode + payload + closeCode` tuple。message/control payload 与 Close reason
的 view 只保证有效到下一次 `poll()`。依据
[RFC 6455 §5.5.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.5.1)，`WsCloseEvent` 只暴露
解析后的 status code 与 UTF-8 reason；无 status 时本地使用 1005，且依据
[RFC 6455 §7.1.5](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.1.5) 将它定义为本地 close
code；同时依据 [§7.4.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.4.1) 不得把 1005
写上 wire。
Ping/Pong payload 必须各归自己的事件；core 必须按
[RFC 6455 §5.5.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.5.2) 自动用相同 payload 回 Pong。
`poll()` 内部的入站调用链也必须全程类型化：依据
[RFC 6455 §5.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.2)，
`webSocketTryReadFrame()` 只能返回 `WebSocketFrameReadResult` 的 need-input、borrowed frame 或
`WebSocketProtocolFailure` 之一，不得恢复 byte-count/EOF side channel，也不得为 peer wire byte
抛异常；依据 [RFC 6455 §5.4](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.4)，
`WebSocketInboundAssembler::accept()` 只能返回 `WebSocketInboundResult` 的 continue、control frame、
带 content encoding 的 message 或 failure 之一，禁止恢复 action enum + output message 参数。
framing/fragmentation、UTF-8 与 size-limit 失败必须按
[RFC 6455 §7.4.1](https://www.rfc-editor.org/rfc/rfc6455.html#section-7.4.1) 精确携带 1002、1007、
1009；只能由 `WsConnection` 将其统一映射为 Close frame 与 `WsProtocolErrorEvent`。
输出必须同时携带 opaque frame bytes 与类型化 keep-open/end-transport disposition；
`WsTransportEndEvent` 只能停止 input pump，runtime 不得用它替代 `WsOutputPlan` 推导 transport 动作。
本端先发送 Close 只能进入等待 peer Close 的 phase，不得立即关闭 transport，也不得继续向应用交付
data message；收到 peer Close 后才允许 orderly transport end。RFC 8441 adapter 只能把 plan 的
terminal disposition 映射为 HTTP/2 `END_STREAM`，禁止在 `ruvia-web` 用 `closeSent`/`endStream`
布尔值重新猜测。
transport EOF 不得伪造正常 1000 Close。heartbeat、Pong timeout 与 close-handshake timeout 都是
`ruvia-web` 的 `WebSocketLifecycleOptions` runtime policy，不得下沉到 `ruvia-http`；单个 HTTP/2
WebSocket tunnel 超时只能 `RST_STREAM(CANCEL)`，不得通过 scanner 关闭承载其他 stream 的整条连接。

205 Reset Content 必须由 HTTP-owned status plan 禁止内容：HTTP/1 规范化为唯一的
`Content-Length: 0` 且不得发送 transfer coding，HTTP/2 在 response HEADERS 上结束
stream；调用方提供的 body 或矛盾 framing field 必须过滤，`ruvia-web` 不得复制该判断。

HTTP/2 连接启动只能走 role-aware、幂等的 `beginConnection()`：client 必须先发送 connection
magic 再发送 SETTINGS，server 必须等待该 magic 且自身只发送 SETTINGS。不得恢复拆分的
SETTINGS、client magic 或 preface-expectation 启动入口。not-started、等待 client magic、等待
peer SETTINGS、ready 必须由单一类型化 phase 表示，不得拆成可形成非法组合的布尔状态；
RFC 9113 §3.1/§3.3 已移除 HTTP/1.1 Upgrade 与 `HTTP2-Settings` 能力：TLS 只能通过 ALPN `h2`
建立 HTTP/2，明文 HTTP/2 只能用 out-of-band prior knowledge 并直接发送 client connection
preface。禁止恢复 101 Upgrade handshake、synthetic stream-1 seed 或并行 upgraded-session 入口。
`feed()` 的输入所有权只能由直接枚举 `Http2FeedResult` 表示，不得恢复
`Http2FeedStatus + consumed` 字段 tuple。`beginConnection()` 前必须返回
`kConnectionNotStarted`，保留完整输入且不修改事件/协议状态，以便 owner 启动连接后原样重试；
未拉完 `nextEvent()` 时必须返回 `kEventsPending`，保留完整输入、原 event 与零拷贝 body view，
owner 排空后原样重试。`kAccepted` 与 `kNeedInput` 都表示 core 已接管完整输入，后者只表示仍缓存
不完整的 connection preface 或 frame；`kProtocolFailure` 是终态，当前输入必须丢弃且不得重试。
这与 [RFC 9113 §3.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4) 的 connection preface
以及 [RFC 9113 §4.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-4.1) 的固定 9 字节 frame header
加可变 payload 边界一致。完成角色对应的 magic 边界后，client/server 都必须要求对端首帧是非 ACK
SETTINGS；SETTINGS ACK 不得冒充 peer preface，只有进入 ready phase 后
`receivedPeerSettings()` 才能为 true。`ruvia-web` 的 initial bytes 和普通 socket read 必须走同一个
feed-and-drain 入口，只能在 `kEventsPending` 时排空事件并重试同一 span；不得计算消费 offset、重新
查询 `connectionError()` 解释 feed 结果，或分别复制这套所有权判断。
`nextEvent()` 必须返回 `std::optional<Http2Event>`，`std::nullopt` 是 event queue 已排空的唯一
信号；每个实际 `Http2Event` 必须由零分配 `std::variant` 持有且仅持有一种类型化 payload。禁止恢复
`kNone` sentinel，禁止恢复可写的 `kind + streamId + bytes + error` 松散 tuple，也禁止让 payload
不匹配的字段成为可观察状态。`Http2MessageBodyChunkEvent` 与 `Http2TunnelDataEvent` 的零拷贝 view
只保证有效到下一次消费输入的 `feed()`。依据 [RFC 9113 §6.4](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4)，
`Http2StreamClosedEvent` 必须同时携带 close source 与 `RST_STREAM` 的原始 error code；本地失败则
携带本端写入 RST_STREAM 的同一 error。`ruvia-web` 处理该事件时必须直接按事件的 stream ID 清理，
不得先向 core 重新查询可能已经移除的 unpinned stream。
收到 GOAWAY 后的连接/流生命周期必须由 `Http2Connection` 统一收口：`peerGoaway()` 返回最新
last-stream-id 与 error 的类型化值，client role 立即禁止创建新流；所有高于该边界且尚未收到
response head 的本地请求必须由 core 清理 deferred DATA/trailers、flow-control debt、peer
concurrency slot 与 stream-table storage，并按 stream ID 顺序产生
`Http2RequestUnprocessedEvent` 安全重试事件。依据
[RFC 9113 §6.8](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8)，connection-level 的
last-stream-id 与 error 只属于 `Http2GoawayEvent`/`peerGoaway()`；每个 safe-retry event 只携带其
stream ID，不得重复塞入 GOAWAY error。后续 GOAWAY 的 last-stream-id 只能保持或降低；提高边界，
或用新边界排除已经开始响应的请求，必须按 connection-level `PROTOCOL_ERROR` 拒绝。合法 peer
GOAWAY 必须进入同一个幂等
`beginDrain()` 路径发出方向相反的本地 GOAWAY，但不得变成 fatal 状态；同一输入 batch 中后续
frame 与已建立 stream 必须继续处理到完成。本端检测到的 fatal frame-layer error 只能通过
`std::optional<Http2ErrorCode> connectionError()` 表达，`ruvia-web` 只按该值停止 reader；不得恢复
含义混乱的 `closing()` 布尔入口。`ruvia-core` 的 generic sans-I/O pump 必须由协议适配方显式传入
可内联 stop predicate，不得要求所有协议把 graceful drain、close handshake 和 fatal error 压成
同一成员。公开入口只允许 `beginDrain()` 发起本地 graceful shutdown；不得恢复接受裸 error
code 的 GOAWAY 发送 API，也不得让 runtime 自行猜测待清理流。
`Http2LocalSettings` 是本地接收能力的唯一来源，必须同时决定 wire SETTINGS、可接收 frame
上限、stream/connection receive window、stream table 与 ready queue 容量。
`Http2Connection` 构造不得接收 route/body limit 或其他 runtime policy；connection send window 必须从
RFC 默认值开始，之后只由对端 SETTINGS 与
WINDOW_UPDATE 推进。`Http2PeerSettings` 构造时必须绑定本地 `Http2Role`，不得恢复无角色的
peer SETTINGS 解析；client 收到 server 的 `SETTINGS_ENABLE_PUSH=1` 必须使用
connection-level `PROTOCOL_ERROR` 拒绝，server 则可接受 client 发来的合法 0/1。单个 setting
的 `apply()` 必须返回判别联合 `Http2PeerSettingApplyResult`：普通合法 setting 与按
[RFC 9113 §6.5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2) 忽略的 unknown ID
只能得到无 payload 的 `Http2PeerSettingApplied`；只有 `Http2PeerInitialWindowChange` 可以通过
`delta()` 携带有符号初始窗口差值；只有 `Http2PeerSettingFailure` 可以通过 `error()` 携带失败
原因。禁止恢复 `status + initialWindowChanged + initialWindowDelta` tuple。`Http2Connection`
必须是唯一消费方：failure 统一映射 GOAWAY，window change 按
[RFC 9113 §6.9.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.2) 应用到所有 active
stream；`SETTINGS_ENABLE_CONNECT_PROTOCOL` 从 1 回退到 0 的拒绝继续遵守
[RFC 8441 §3](https://www.rfc-editor.org/rfc/rfc8441.html#section-3)。

所有结构合法的 inbound DATA 必须先按完整 payload 长度（包含 Pad Length 与 padding）扣减
connection receive window，再查询 stream 或执行 closed/reset/peer-GOAWAY 等语义丢弃；只有直接
终止连接的 frame-level error 可以跳过后续记账。connection credit 不足必须统一产生
connection-level `FLOW_CONTROL_ERROR`，不得因 stream 已关闭而绕过。成功扣减后若 DATA 被丢弃，
只能精确归还一次 connection credit 并发送 stream 0 WINDOW_UPDATE；只有 live stream 才继续独立
扣减 stream window。deferred delivery 必须同时保留两级 debt，直到 owner release 或 stream close。
禁止恢复把两级扣减揉成单一结果的 helper，也禁止用 `windowConsumed` 之类松散布尔值在早退分支
猜测是否需要归还 credit。

HTTP/2 本地响应的 `Content-Length` 必须由 stream-owned、无分配的
`Http2LocalContentState` 绑定到 core 接管的 DATA。state 必须是 `Http2LocalContentUnset`、
`Http2LocalContentForbidden`、`Http2LocalContentUnbounded`、`Http2LocalContentKnownLength` 四个
互斥 alternative 之一，只有 known-length 可以暴露 `declaredLength()`；禁止恢复
`Http2LocalContentMode + declaredLength` tuple、plan-wide 假 0，或在 `Http2StreamState` 转发独立
mode/has/length/counter accessor。stream 只暴露一个 const `localContent()` state。unset 状态必须以
`kNotStarted` 拒绝 DATA 且不得报告 length complete，符合
[RFC 9113 §8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1) 的 HEADERS/DATA 顺序；已声明
Content-Length 必须遵循 [§8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1) 的 DATA
总长度一致性。`kAccepted`/`kQueued` 按整次输入只推进一次 accepted，DATA 真正物化进 outbound
buffer 时才推进 committed；`kBackpressured`、
超长输入和短 END_STREAM 都必须零接管且不得修改 output/window/phase。exact body 未达到
声明长度时 `finishResponse()` 必须拒绝并保持 body-open，WINDOW_UPDATE drain 不得重复推进
accepted。HEAD/204/205/304 的 Content-Length 是无内容或 representation metadata，不得变成
DATA 长度契约；WebSocket/CONNECT tunnel 保持 unbounded。

HTTP/2 final response 在 HPACK 与 stream mutation 前必须生成唯一、不可默认构造的
`Http2ResponseHeadPlan`；其 Content-Length 所有权只能是
`Http2CanonicalResponseContentLength`、`Http2ExplicitResponseContentLength`、
`Http2AbsentResponseContentLength`、`Http2ForbiddenResponseContentLength` 四个互斥 alternative。
buffered、streaming、成功 CONNECT 必须分别走 `http2BufferedResponseHeadPlan()`、
`http2StreamingResponseHeadPlan()`、`http2ConnectResponseHeadPlan()`；invalid explicit length 只允许
存在于 `Http2ResponseHeadPlanResult` 的 failure alternative。显式长度只能解析一次，该 plan 的同一
数值既驱动 canonical HPACK bytes，又初始化 `Http2LocalContentKnownLength`；encoder 只能接收 plan，
禁止恢复 `autoContentLength + emitAutoContentLength` 标量入口或在 connection driver 二次解析。

HTTP/2 对端发来的 message content 必须由独立、无分配的 `Http2RemoteContentState` 统一记账。
state 只能是 `Http2RemoteContentAllowedWithoutLength`、`Http2RemoteContentAllowedKnownLength`、
`Http2RemoteContentMetadataOnlyWithoutLength` 或 `Http2RemoteContentMetadataOnlyKnownLength` 四个
互斥 alternative 之一；只有 known-length 可以暴露 `declaredLength()`，只有 allowed 可以拥有
received bytes。禁止恢复 `Http2StreamBodyAccounting`、`hasContentLength + contentLength` tuple、
缺失长度的假 0，或在 `Http2StreamState` 转发 presence/length/received/completion accessor；stream
只暴露一个 const `remoteContent()` view。HTTP/2 client 解出 final HEAD/204/304 head 后必须按共享
`HttpResponseContentSemantics` 原子切换到 metadata-only，并保留 representation Content-Length。
此后非空 DATA 必须返回 `kContentForbidden` 并产生 stream-level `PROTOCOL_ERROR`；空 DATA 可以只携带
terminal `END_STREAM`，但不得产出 content event。该行为遵循
[RFC 9110 §6.4.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.4.1) 的无 content 定义以及
[RFC 9113 §8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1) 的 malformed response
处理。第一个 DATA byte accepted 后不得再切换为 known-length 或 metadata-only；late transition
必须失败并保持原 alternative。所有普通 content DATA 必须走单次原子 `account()`：
`kCounterOverflow`、`kDeclaredLengthExceeded`、`kContentForbidden` 都保持 `receivedBytes()` 不变，
只有 `kAccepted` 才提交计数，禁止恢复可被调用方拆开的 `checkAccept() + accept()`。完整 DATA payload
（含 Pad Length/padding）的 connection/stream flow-control 记账仍按
[RFC 9113 §6.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.1) 先执行。initial HEADERS、DATA 与
trailing HEADERS 的 END_STREAM 必须读取 active state 的 `terminalLengthValid()`；metadata-only
不能因 representation length 被误判为缺失 DATA，其余已声明长度严格匹配 DATA payload 总和。

HTTP/2 非空 content/tunnel DATA event 必须保留完整 flow-controlled payload（包括 Pad Length 与
padding）的 connection/stream debt，直到 owner 已复制或消费当前交付的所有该 stream DATA 后调用唯一
`releaseReceivedData(streamId)`；依据
[RFC 9113 §5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.2) 与
[§6.9.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.1)，WINDOW_UPDATE 只能表示接收方真正
释放的容量。stream 关闭必须只归还 connection debt，empty/padding-only 或不产生应用 event 的
metadata-only DATA 可以立即归还。禁止恢复 `deferStreamWindowRelease()`、`releaseStreamWindow()`、
可选 defer flag，或在产生非空 event 时默认立即 WINDOW_UPDATE。

route-selected body 存储与限额只属于 `ruvia-web`。`ruvia-http` 的 `Http2StreamState` 不得包含
`RequestBodyMode`、coroutine waiter、应用 body queue/buffer、queued backlog、response compression
scratch 或 `Http2ConnectionLimits`；`Http2BodyState.h`、`Http2BodyQueue.h`、
`Http2StreamBodyQueue.h`、`Http2StreamBodyPolicy.h` 必须保持删除。协议 core 只通过
`Http2RemoteContentState::account()` 校验 Content-Length/message semantics 并产生有序 event。
`ruvia-web` 必须用 PMR-stable `Http2SansIoStreamRuntimeTable`、`Http2RequestBodyRuntime` 与
`Http2SansIoBodyQueue` 保存每个 active stream 的 route resolution 和 body；同一 `feed()` 内 HEADERS
后紧跟的 DATA 也必须先按 message-head event 选择 Web `RequestBodyMode`，再应用 total/backlog limit。
route selection 必须只走 `Http2SansIoStreamRuntime::selectRoute()`，一次性绑定
`RouteResolution` 与 optional `RequestBodyMode`；`Http2RequestBodyRuntime` 不得公开独立 mode selector，
不得恢复默认 buffered mode + `modeSelected` 布尔组合。
`runHttp2SansIoSession()` 必须按值接收不可默认构造的 `Http2SansIoSessionContext`，在 coroutine 启动前
一次性绑定 `ContextServices`（其中已携带 typed `ConnInfo`）、`HttpServerOptions`、
`ConnectionScanner::Entry` 与 graceful-shutdown atomic。禁止恢复独立的 remote address、client
certificate、`secure` 参数，全空 `Http2SansIoSessionEnv` 指针包、静态默认 options、
未链接的 local scanner 或 nullable shutdown 状态；裸 session 默认值只能存在于 `tests/` fixture，不能让
测试便利反向弱化安装后的生产调用契约。
同一个 `Http2SansIoStreamRuntime` 还必须拥有 optional `Http2SansIoStreamSignal` 作为 dispatch lease；
runtime-level `beginDispatch()` 必须是 table-only friend 操作，table 必须在 `co_spawn` 前同步增加
`dispatchedCount()`，禁止绕过 aggregate lease；body mode 尚未 selected 时必须拒绝 admission。
writer exit 只能看该 dispatch count，idle phase 必须看同一 table 的 `size()`，确保尚未 dispatch 的
buffered body 仍属于 active payload。teardown wake 与
runtime 删除只能遍历这一份生命周期。禁止恢复默认堆 `streamSignals` vector、per-signal `unique_ptr` 或另一个
`inFlight` 标量；body reader、WebSocket transport 与 response-stream sink 必须接收非空 signal reference，
不得重新暴露 nullable signal state。signal timer deadline 只能在构造时设置一次；注册 concurrent waiter
不得修改 expiry 并误取消已有 waiter，一次 wake 必须唤醒全部已注册 wait 后分别重查 readiness。
未完成一次性 mode selection 时，`store()` 必须返回 `kModeNotSelected`，不得静默采用 buffered 默认值。
buffered event batch 完整复制后才能统一调用 `releaseReceivedData()`；stream request/CONNECT tunnel
必须等 Web queue drain 后归还。`Http2RequestBuilder::build()` 只接收 Web owner 提供的 body view；
response compression scratch 必须留在 handler-local Web storage，不得借用 protocol stream storage。
owner-side reset 不会回送 `kStreamClosed` 给同一 owner；尚未 dispatch 的 Web runtime 必须在 reset
调用链立即删除，已 dispatch 的 runtime 则保留到 handler cleanup，禁止泄漏并发槽。

HTTP/2 client role 的普通请求头只能走 `submitRegularRequestHead()`，并以无分配值类型
`Http2RequestContent` 在 `none()`、`knownLength(n)`、`streaming()` 三种契约中显式选择。
三者必须分别产出 `Http2RequestWithoutContent`、`Http2KnownLengthRequestContent`、
`Http2StreamingRequestContent` alternative，且只有 known-length alternative 可以暴露 `length()`；
禁止恢复 `Http2RequestContentMode + length` tuple，或在 absent/streaming 状态读取假 0。该值必须统一
决定 canonical Content-Length、HEADERS END_STREAM 与
`Http2LocalContentState`，普通 header span 中的 `content-length` 必须在 HPACK/output/phase
修改前事务性拒绝。该边界必须遵循
[RFC 9113 §8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1) 的 HEADERS/DATA/END_STREAM
framing 与 [§8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1) 的 Content-Length/DATA
总长度一致性。普通/standard CONNECT/Extended CONNECT 三种 request-head 入口必须各自
通过 `Http2RequestHeadSubmitResult` 原子完成语义校验、odd stream ID 分配、对端
`SETTINGS_MAX_CONCURRENT_STREAMS` 门控与 HEADERS 提交。该 result 必须是
`Http2SubmittedRequestHead` 与 `Http2RequestHeadSubmitFailure` 的判别联合：只有 submitted
alternative 可以通过 `streamId()` 暴露已分配的非零奇数流，只有 failure alternative 可以通过
`error()` 暴露 `Http2RequestHeadSubmitError`；禁止恢复顶层 `status()/accepted()/streamId()`、
`status + streamId` 字段 tuple 或用 stream ID 0 伪装失败流。依据
[RFC 9113 §5.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.1)，0 只属于 connection
control，新 client stream 必须使用递增且不可复用的奇数 ID；依据
[RFC 9113 §5.1.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.2)，新流还必须服从对端
并发限制；[RFC 9113 §6.5.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2) 进一步规定
`SETTINGS_MAX_CONCURRENT_STREAMS` 是限制对端创建流数量的单向设置。失败不得消耗 ID、创建
RFC-idle stream、写 HPACK/output 或占用并发槽。并发槽从 HEADERS 提交开始，只在 RST_STREAM
或本地/对端两个 END_STREAM half 都完成后释放；不得把 peer limit 留给 owner 自行遵守，也不得
恢复公开的两段式空流预分配入口。client request head 在
`beginConnection()` 前必须拒绝。普通入口不得接受 CONNECT、伪首部、connection-specific field、非法
name/value 或与 `:authority` 冲突的 Host。standard CONNECT 必须走
`submitConnectRequestHead()` 并校验带非零端口的 authority-form；Extended CONNECT 必须走
`submitExtendedConnectRequestHead()`，且只能在对端声明
`SETTINGS_ENABLE_CONNECT_PROTOCOL=1` 后提交。二者在最终 2xx 前都不得提交或交付 tunnel
DATA；server 也只能在 `beginConnection()` 已幂等启动本地 capability advertisement 后接受
`:protocol`。server 只能通过
`submitConnectResponseHead()`（WebSocket 使用专用 handshake）接受。
成功后的 DATA/END_STREAM 必须分别产出 `kTunnelData`/`kTunnelEnd`，两侧 half-close 独立；
对端 FIN 后的 DATA、connected stream 上的非 DATA/management frame、CONNECT trailers 和
成功响应的 Content-Length/Transfer-Encoding 都由 `ruvia-http` 统一拒绝或按 RFC 忽略。
`peerExtendedConnectEnabled()` 只允许查询能力，不是提交旁路。

每个 stream 的 CONNECT 进度必须由无分配的 `Http2TunnelState` 独占，state 只能是
`Http2NotConnect`、`Http2ConnectPending`、`Http2TunnelOpen`、`Http2ConnectRejected` 四个互斥
alternative 之一。只有 pending 可以通过 `Http2ConnectForm` 暴露 standard/extended form；open
与 rejected 必须无 payload，Extended CONNECT 在之后由已校验并保留的 `:protocol` 表达。禁止恢复
`Http2ConnectKind + Http2TunnelPhase` 笛卡尔积、kind/phase accessor、独立 CONNECT boolean marker，
或在 `Http2StreamState` 转发 standard/extended/pending/open/rejected accessor；stream 只能暴露一个
const `tunnel()` view。`beginStandardConnect()`/`beginExtendedConnect()` 只能从 not-connect 进入
pending，只有 pending 能 `acceptConnect()` 或 `rejectConnect()`。该状态机必须遵循
[RFC 9113 §8.5](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.5) 的 2xx 后 tunnel DATA 与
connected-stream frame 限制，以及
[RFC 8441 §4](https://www.rfc-editor.org/rfc/rfc8441.html#section-4) 的 `:protocol` Extended CONNECT
契约。

HTTP/2 final response head 的提交结果必须与 request head 一样使用判别联合：
`submitResponseHead()` 返回 `Http2BufferedResponseHeadSubmitResult`，
`submitStreamingResponseHead()` 返回 `Http2StreamingResponseHeadSubmitResult`。只有
`Http2SubmittedResponseHead<Plan>` alternative 可以通过只读 `plan()` 暴露已提交的 buffered write
plan 或 streaming commit plan；只有 `Http2ResponseHeadSubmitFailure` 可以通过 `error()` 暴露
`Http2ResponseHeadSubmitError`。禁止恢复顶层 `status()`、`accepted()`、`plan()` 或
`status + default plan` tuple，也不得让 failure 携带貌似可驱动 DATA/END_STREAM 的 plan。
closed stream、错误 phase 和 malformed response 必须在 HEADERS/HPACK/output/stream phase 修改前
事务性失败；只有 submitted alternative 代表 initial response HEADERS 与后续 content plan 已一起
commit。该边界遵守 [RFC 9113 §8.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1) 的同
stream response framing、[§8.1.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1) 的
malformed response 规则，以及 [§6.2](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.2) 的
HEADERS/END_STREAM 状态转换。

Web runtime 的 buffered send completion 必须继续使用不可默认构造的
`Http2BufferedResponseDispatchResult`；pre-commit failure 只携带 `Http2ResponseHeadSubmitError`，
pre-commit peer abort 不携带 payload，completed/peer-aborted-after-commit/failed-after-commit 只携带
submitted write plan 的 exact status。禁止恢复 `Task<void> submitResponse` 后无条件读取可变
`response.status()` 的调用链，也禁止让早退 error/rate-limit 分支绕过统一的 buffered preparation。

## 性能原则

- 请求热路径目标是 0 抽象成本。
- 启动期可以使用注册表、工厂、虚函数和一次性构建。
- 请求期不要新增 mutex、rwlock、spinlock、共享原子计数争用、type-erasure、`shared_ptr` 分配或不必要拷贝。
- 优先使用 per-worker 所有权、连接私有状态、启动期构建后只读数据。
- 跨线程操作连接状态默认禁止；必须先设计明确的 worker mailbox 或 intrusive MPSC 边界。
- 固定窗口、超时等时间边界测试必须使用编译期可注入时钟，禁止依赖 `sleep` 碰运气跨边界；
  生产请求热路径不得因此引入函数指针、虚调用或 type-erasure。

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
- method/path/version/header 默认是指向连接读缓冲的 `std::string_view`；`method()` 返回精确 wire token，固定语义必须显式读取 `knownMethod()`。
- header 上限 64KB，普通 body 上限 16MB。
- chunked 请求体在连接读缓冲中原地解码。
- 普通 route dispatch 前完整读取 body；大请求体必须显式使用 stream route。
- stream body reader 返回的 view 只保证有效到下一次 `read()`。
- 响应写出采用栈上固定 header buffer + scatter-gather I/O。
- 禁止为了写出把 body 拼成完整 response 字符串。
- 文件响应不全量读入内存；plain TCP 优先使用平台零拷贝路径。
- response streaming 和 WebSocket 必须通过显式 route macro 注册。
- 普通路由返回的 `HttpResponse` 只允许空、borrowed/owned bytes 或 file body；响应流必须走显式 streaming route 和 `ResponseStreamWriter`，不得增加动态或类型擦除的响应体旁路。
- buffered `HttpResponse` 的存储必须由 `HttpResponseBody` 唯一表达，只能是
  `HttpEmptyResponseBody`、`HttpBorrowedResponseBytes`、`HttpStaticResponseBytes`、
  `HttpOwnedResponseBytes`、`HttpOwnedResponseFile` 或 `HttpBorrowedResponseFile`。读取只能走唯一
  `responseBody(response)`，`bytes()`、`file()`、`size()` 必须由 active alternative 派生；
  `ResponseFileBody` 必须不可默认构造且只能由 file alternative 产出，zero-length file 不得折叠为空 body。
  禁止恢复 `BodyKind + string + view + optional file` 常驻笛卡尔积、
  `responseHasFileBody()` + `responseFileBody()` 两阶段读取、
  `responseBodyBytes()`/`responseBodySize()` 并行入口，或为选择 body
  representation 引入分配、虚调用和 type-erasure。
- 每个 `Context` 的请求体来源必须由 `ContextRequestBodySource` 唯一表达，只能是
  `ContextBufferedRequestBodySource`、`ContextLazyRequestBodySource` 或
  `ContextStreamingRequestBodySource`；响应输出必须由 `ContextResponseOutput` 唯一表达，只能是
  `ContextBufferedResponseOutput`、`ContextResponseStreamOutput` 或 `ContextWebSocketOutput`。
  非 buffered alternative 必须持有不可为空的 runtime facade，`ContextServices` 按值把两个判别联合
  传给 `Context`。禁止恢复 `bodyReader + bodyLoader`、`responseStream + webSocket` 四个 nullable
  pointer slot、`withBodyReader()`/`withBodyLoader()` 手工清空对端，或为这两个状态轴引入分配、虚调用、
  type-erasure 和请求期同步。

## 路由和中间件

- 路由注册只允许通过 controller/group/route 宏完成。
- 不暴露直接 `Router::addRoute(...)` 或 `Router::group(...)` 注册 API。
- 注册到 dispatch 的 endpoint 必须由 move-only `RouteEndpoint` 判别联合唯一表达，只能是
  `BufferedRouteEndpoint`、`ResponseStreamRouteEndpoint` 或 `WebSocketRouteEndpoint`。handler 形态、
  request-body mode、`ResponseStreamKind` 与 WebSocket metadata 必须属于对应 alternative；禁止恢复
  同时保存 `RouteHandler`/`RouteStreamHandler` 再用 `ResponseBodyMode` 选择的笛卡尔积，也禁止恢复
  `HttpResponseStreamKindAdapter` 二次映射。
- `RouteResolution` 必须只包含 `ResolvedRoute`、`RouteMethodNotAllowed`、`RouteNotFound` 三种互斥
  alternative。只有 `ResolvedRoute` 能暴露 route 与其拥有的 `RouteMatch`，只有
  `RouteMethodNotAllowed` 能暴露非零 Allow mask；禁止恢复 top-level `found()`、`route()`、`match()`、
  `allowedMethods()` 或 `RouteDisposition` payload tuple。`resolve()` 自己拥有动态 match scratch 并把
  结果移入 `ResolvedRoute`，HTTP/1、HTTP/2 和其他调用方不得再传入或长期保存第二份 `RouteMatch`。
- HTTP/1、HTTP/2、response streaming 与 WebSocket 的请求期 lookup/dispatch 必须直接依赖唯一、
  启动期冻结的 concrete `RouteTable`。禁止恢复只有单一实现的 `RequestDispatcher` 虚接口，禁止为
  route index 形状解耦而在每请求/每 stream 路径引入 vtable；测试替身也不得反向决定生产调用边界。
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
- socket/TLS 连接元数据统一通过 `getConnInfo(c)` 读取；`HttpRequest`、`ContextRequest` 和
  `RawRequestClone` 不得保存或暴露 remote address、TLS 状态、客户端证书身份。
- `ConnInfo` 必须只包含 `PlainConnectionTransport` 或 `TlsConnectionTransport` 一个 active
  alternative；只有 TLS alternative 暴露 `clientCertificateSubject()`，TLS 无客户端证书时该值为空。
  禁止恢复 top-level `secure()`、top-level `clientCertificateSubject()`、`withTransport(..., bool secure)`，
  或在 `ContextServices`/`Context` 中并行保存 remote address、certificate 与安全布尔值。server adapter
  必须在 accepted plain socket 或成功 TLS handshake 处构造一次 typed connection value，HTTP/1、
  cleartext HTTP/2、ALPN HTTP/2、`ContextServices` 与 `Context` 只能传递该值，不得按 stream/request
  重新推导安全状态；address/certificate view 必须借用覆盖整个连接的 storage，所有 owning string
  rvalue refinement 入口必须 deleted；该调用链不得增加分配、虚调用或 type-erasure。
  `plain()`/`tls()` 返回 active alternative 指针，只允许在 `ConnInfo` lvalue 上调用；rvalue accessor
  必须 deleted，禁止从临时 `getConnInfo(c)` 保存悬垂 alternative 指针。
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
- 可通过 `RUVIA_BUILD_CORE`、`RUVIA_BUILD_HTTP`、`RUVIA_BUILD_WEB` 选择构建组件；三者默认均为 `ON`。
- `RUVIA_BUILD_WEB=ON` 要求同时启用 core 与 http；core-only/http-only 配置不得查找或安装未选择组件的依赖。
- MariaDB、Redis、JWT 是严格 feature：
  - `RUVIA_ENABLE_MARIADB=ON`
  - `RUVIA_ENABLE_REDIS=ON`
  - `RUVIA_ENABLE_JWT=ON`
- outbound HTTP client 只保留在 `ruvia-http` 的底层 sans-I/O API（typed `HttpOrigin`、`HttpClientRequest`/`HttpClientResponse` 消息模型、origin/authority 校验、HTTP/1 request writer/response parser、重定向规则、内容解码和 HTTP/2 client role 协议状态），不得包含证书文件、连接池或运行时 timeout 配置。`ruvia-web` 不提供 client socket/TLS runtime、连接池、`fetch`、`proxy`、client 注册或反向代理集成；需要出站 HTTP 的应用自行用外部 I/O runtime 驱动 `ruvia-http` 协议 API。
- 下游推荐：

```cmake
find_package(ruvia CONFIG REQUIRED COMPONENTS web)
target_link_libraries(app PRIVATE ruvia::web)
```

- 需要更小依赖面时使用 `COMPONENTS core` 或 `COMPONENTS http`。
- 安装包必须暴露组件 target：`ruvia::core`、`ruvia::http`、`ruvia::web`；不要再暴露历史 Web 框架别名。
- 三个 target 必须使用独立安装 export；package config 只能导入请求组件的依赖闭包。请求 `core` 不得创建或查找 `http/web`，请求 `http` 不得创建或查找 `core/web`，请求 `web` 才导入 `core + http + web`。
- `ruvia_AVAILABLE_COMPONENTS` 必须根据当前安装前缀中实际存在且依赖闭包完整的 export 计算，不能照搬构建期开关。组件级安装中，`web` 只有在 `core`、`http`、`web` 三个组件都已安装时才可用。
- 缺失的 required component 必须令包查找失败；缺失的 `OPTIONAL_COMPONENTS` 只将对应 `ruvia_<component>_FOUND` 置为 false，不得拖垮其他已安装组件。

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
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
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
