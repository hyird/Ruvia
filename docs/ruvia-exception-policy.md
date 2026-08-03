# Ruvia 异常处理与容错规范

> 文档状态：定稿并开始实施（2026-07-23）
>
> 适用范围：`ruvia-core`、`ruvia-http`、`ruvia-web`
>
> 相关文档：[worker runtime 与并发原语规范](ruvia-concurrency-primitives.md)

本文件是异常与失败处理的唯一权威。新代码按此规范写，评审按此规范判。

## 0. 三条根本原则

1. **失败永不静默。** 任何被捕获的异常都必须有去处：重新抛出、转换为契约内的返回值、
   或送达上报通道。空的 `catch (...) {}` 一律视为缺陷。
2. **失败不外溢。** 每个失败被限制在它所属的隔离单元内，绝不升级为更大范围的故障。
   一个请求的失败不影响连接，一个连接的失败不影响节点。
3. **只有不变量被破坏才终止进程。** `std::terminate` 是断言，不是错误处理手段。
   资源耗尽（`bad_alloc`）不是不变量破坏，必须被最近的隔离边界容纳。

## 1. 用什么表达错误

按"错误是否可预期"选择，而不是按层选择。

| 错误类别 | 表达方式 | 例子 |
| --- | --- | --- |
| 可预期的输入/协议错误 | 返回值（`enum` / `optional` / 结果聚合） | `HpackDecodeError`、`Http1ChunkDecodeError`、`MultipartParseError` |
| 可预期的 I/O 失败 | `std::error_code` / `asio::error_code` | 全部 socket、文件操作 |
| 调用方用错 API、不变量违反 | 抛 `std::logic_error` / `std::invalid_argument` | 跨 worker 调用、重复 `join()`、空 `Task` |
| `noexcept` 上下文内的不变量违反 | `std::terminate` | `Task` promise、`WorkerSignal`、析构路径 |
| 资源耗尽 | 异常（`bad_alloc`），由隔离边界容纳 | 协程帧分配、缓冲区扩容 |
| 应用逻辑失败（Web 请求内） | 异常，由请求级降级链处理 | `HttpError`、`ValidationError`、DB 异常 |

`ruvia-http` 是 sans-I/O 解析层，**解析失败一律用返回值**，不抛异常。它抛异常只在
"调用方用错 API"（非法 header 名、未打开的 multipart 输入）。

## 2. 隔离单元（容错的核心）

失败被限制在它发生的那一级。每一级都定义了"失败后什么继续可用"。

| 级别 | 边界代码 | 失败后果 | 上报去向 |
| --- | --- | --- | --- |
| 请求 | `RouteTable::dispatch` / `handleException` | 返回 5xx，**连接继续** | `Context` 上的 `exception_ptr` → 应用 `onError` |
| 连接 | `HttpServer::handleSession` 的 catch-all | 关闭该连接，**服务继续** | `App::onConnectionFailure` |
| 任务 | detached 任务边界（`asio::post`、`BlockingPool` 任务的 catch 包裹） | 该任务结束，**进程继续** | 应用 sink，兜底 `reportUnhandledFailure` |
| worker | `WorkerDispatcher::runContext` | 停该 worker，**进程继续** | `workerFailure` |
| 进程 | `std::terminate` | 进程结束 | 仅用于不变量破坏 |

**关键规则：接受循环永不因失败退出。** 监听 socket 开着却不再 accept，是最坏的
失败模式——进程活着、端口通着、服务实际已死，健康检查发现不了。accept 循环对所有
瞬时错误（描述符耗尽、`ECONNABORTED`、`EINTR` 等）都退避 50ms 后继续，只有
`operation_aborted` / 监听器关闭才退出。

退避不是可选的礼貌：描述符耗尽时失败的 accept **立刻**就绪，不退避就是 100% CPU
的忙循环，而占着 CPU 的正是本该释放描述符的那些会话。

## 2.1 容错机制

隔离只保证失败不扩散，下面这些决定失败发生时服务还能做什么。

| 机制 | 位置 | 作用 |
| --- | --- | --- |
| 连接预算 | `HttpServerOptions::maxConnections` | 超出即接受并立即关闭，主动卸载而不是让 backlog 无限增长直到撞上描述符上限 |
| 瞬时错误退避 | accept 循环 | 见上 |
| 请求级降级链 | handler → onError → 默认响应 | 见 §3 |
| 上报限流 | `reportUnhandledFailure` | 故障风暴时保证第一条（信息量最大的）失败不被后续后果淹没，也避免阻塞的 stderr 拖慢恢复 |
| 失败计数 | `App::httpStats()` | 无回调也能被健康检查观测到 |

## 3. 用户回调的三类契约

每个接受应用回调的 API 必须明确属于哪一类，并在声明处注明。

**A 类 — 编译期禁止抛。** 用 `requires std::is_nothrow_invocable_*` 钉死，回调自己
负责内部处理。用于热路径与析构路径。

```cpp
// ruvia-web ServerConfig.h：AccessLogCallback::bind
requires std::is_nothrow_invocable_r_v<void, Listener&, const AccessLogRecord&>
```

**B 类 — 可抛，被容纳并上报。** 回调失败不改变服务行为，异常送上报通道。用于观测、
诊断、清理类回调。

- `App::onConnectionFailure` → 失败回退到 §5 的最终兜底
- `EventLoop::onStop` → 失败进入 pool 的首失败记录，由 `join()` 重抛

**C 类 — 可抛，传播给调用者。** 同步 API，调用方就在栈上，直接让异常传播或转成
返回值。用于控制面。

- `App::run()`：启动期配置与 listener 失败直接传播给调用者
- `EventLoopPool::join()`：worker 与 stop 回调的首个失败重抛给调用者

转成返回值的控制面调用必须**同时**上报被降级掉的原因，否则"为什么被拒绝"随异常
消失（见 §8）。

## 4. 取消不是失败

Asio 通过"恢复协程并抛 `operation_aborted`"来展开被 terminal-cancel 的协程。因此
**每次优雅停机都会让每个活跃任务以异常结束**。这是任务的正常终止方式。

上报前必须过滤，判据卡到最窄：**已请求关闭** 且 **异常恰好是 `operation_aborted`**。
`awaitable_operators` 的组合（`reader() && writer()`）被取消时抛的是
`asio::multiple_exceptions` 包装，需要递归识别其 `first_exception()`。

不做这个过滤的后果是每次 `stop()` 刷出一行/连接的假告警，把真失败淹掉——等价于
违反原则 1。

## 5. 上报通道与最终兜底

每个"无人接收"的失败都有唯一出口：

```
应用提供的 sink（taskFailure / workerFailure / failureHandler）
        ↓ 未提供，或 sink 自己抛了
ruvia::detail::reportUnhandledFailure()   ← 唯一的最终兜底
        ↓
stderr: "ruvia: <context> failed: <what>"
```

`reportUnhandledFailure`（`ruvia/core/detail/util/FailureReport.h`）是全项目唯一
向 stderr 写诊断的地方。这样做的理由：

- **不能静默丢弃** —— 违反原则 1，故障不可诊断。
- **不能 terminate** —— 违反原则 3，把局部故障升级为全局故障。
- 应用提供 sink 即可完全接管，库不与应用的日志系统竞争。

有同步汇合点的失败（`start()`、`join()`、控制面调用）**不需要 sink**：直接重抛给
调用者即可。sink 只服务于 detached 任务与回调。

## 6. 编写规则

- 捕获后必须做三件事之一：`throw;`（回滚后重抛）、转成契约内返回值、送上报通道。
- 析构函数、`noexcept` 函数内调用用户回调，一律 `try`/`catch` + 上报，绝不让它
  变成 `terminate`。
- 回滚型 catch 用 RAII guard 优先；手写 `catch (...) { rollback(); throw; }` 时
  rollback 本身必须 `noexcept`。
- fire-and-forget 的 `asio::post` / 线程池任务**必须**在执行点包一层 catch。
  任务抛出会离开工作线程，直接 `terminate`。参考 `BlockingPool::Impl::run`。
- 占位后再启动的模式（先 `try_emplace` 占坑再 spawn），启动失败必须释放占位，
  否则后续请求会永久等待一个不存在的 leader。
- 新增 `enum` 失败类别时，在 `switch` 中显式列出，靠 `-Werror` 兜住遗漏。

## 7. 现状与待办

已符合本规范：

- `ruvia-http`：解析面全返回值，无异常控制流。
- `ruvia-core`：契约违反抛异常；`noexcept` 边界 `terminate`；worker 失败经
  `failureHandler` 或 `join()` 重抛；stop 回调失败进首失败记录（2026-07-23）。
- `ruvia-web`：请求级降级链完整（handler → onError → 默认响应），已提交响应后
  不伪造错误响应而是拆连接；连接级失败经 `App::onConnectionFailure` 上报，
  包括响应已提交后 handler 抛出的那一类（h1 与 h2 各有端到端测试）（2026-07-23）。

## 8. 反模式：降级即丢弃

最难发现的吞点不是空 `catch`，而是**把异常降级成一个更小的值**之后没人再持有它。
本项目实际出现过多次，形态各不相同但本质相同：

| 位置 | 降级成 | 后果 |
| --- | --- | --- |
| `dispatchResponseStreamWith` | `makeFailedAfterCommit(status)` | 客户端收到截断的 200，服务端连日志都没有——访问日志记的正是那个 200 |
| `finishWebSocketSession` | close code `1011` | 对端知道"出错了"，运维不知道错在哪 |

两个都藏在类型安全、设计良好的抽象背后：一个 variant 结果类型、一个 RFC 定义的
关闭码。类型越干净，越难看出异常已经没了。

**检查方法：每当一个失败被转换成状态码、布尔值、枚举或关闭码，问一句"原始异常
现在谁持有？"** 答案是"没有人"时，就是一个静默吞点。修法统一为：让降级后的值
携带 `exception_ptr` 一路传到最后一个能上报的所有者（通常是传输层或任务边界）。

## 9. 清点结果

全树 `catch (...)` 逐个核实后的分布（2026-07-23）：

- **上报**：web 连接级（h1/h2/WebSocket）、`BlockingPool` 任务、core stop 回调、
  App stop hook、两个析构里的 `join()`。
- **回滚后 `throw;`**：所有资源清理点（`Db`、`PgDb`、`RedisPool`、`PmrObject`、
  `AsioAwait` 等）。
- **转成 `exception_ptr` 继续传播**：请求级降级链、`TaskScope`、`DbMigration`。
- **转成契约内返回值**：`DbSlotSocket` 的探测。
- **分类判断**（异常仍被调用方持有）：`isUnsupportedRequestContentCoding`、
  `ResponseStreamHeadOnlyComplete` 识别。
- **最终兜底自身**：`reportUnhandledFailure` 内部的 `rethrow` 分支。

**丢弃异常的 `catch` 数量：0。**

语法上仍为空的 `catch (...) {}` 只剩两个，全部是分类判断——
`isUnsupportedRequestContentCoding` 与 `ResponseStreamHeadOnlyComplete` 的识别。它们
`rethrow` 一个调用方仍然持有的 `exception_ptr` 只为读出类型，落空分支返回 false 后
调用方照常处理该异常。两处都加了注释注明这一点，扫描者不必再逐个推断。

新增空 `catch` 时按此判断：**空块合法当且仅当异常的所有权在别处**，并且必须写明
所有者是谁。
