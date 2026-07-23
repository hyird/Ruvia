# Ruvia 异常处理与容错规范

> 文档状态：定稿并开始实施（2026-07-23）
>
> 适用范围：`ruvia-core`、`ruvia-http`、`ruvia-web`、`ruvia-edge`
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
| 连接 | `HttpServer::handleSession` 的 catch-all；edge 的 `spawnTracked` 完成回调 | 关闭该连接，**服务继续** | edge：`taskFailure(kSession)`；web：见 §7 待办 |
| 任务 | edge `spawnTracked`；`DiskTier::runQueued` | 该任务结束，**节点继续** | `taskFailure(kAcceptLoop/kBackgroundRefresh/kDiskCache)` |
| worker | `WorkerDispatcher::runContext`；edge 工作线程的 `run()` 循环 | web：停该 worker；edge：上报后继续 | web：`workerFailure`；edge：`taskFailure(kWorker)` |
| 进程 | `std::terminate` | 进程结束 | 仅用于不变量破坏 |

**关键规则：接受循环永不因失败退出。** 监听 socket 开着却不再 accept，是最坏的
失败模式——进程活着、端口通着、服务实际已死，健康检查发现不了。web 与 edge 的
accept 循环都对瞬时错误退避后继续（50ms），对 `operation_aborted` 才退出。

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

- `EdgeServerOptions::accessLog` → 失败上报为 `kAccessLog`
- `EdgeServerOptions::taskFailure` → 失败回退到 §5 的最终兜底
- `EventLoop::onStop` → 失败进入 pool 的首失败记录，由 `join()` 重抛

**C 类 — 可抛，传播给调用者。** 同步 API，调用方就在栈上，直接让异常传播或转成
返回值。用于控制面。

- `EdgeServer::addOrigin` / `removeOrigin` / `purge`：`packaged_task` + `future.get()` 重抛
- `EdgeServer::setTlsCertificate`：转成 `false` 返回值，**同时**上报 `kControl`
  携带的拒绝原因（否则"为什么这个 PEM 无效"随异常消失）

## 4. 取消不是失败

Asio 通过"恢复协程并抛 `operation_aborted`"来展开被 terminal-cancel 的协程。因此
**每次优雅停机都会让每个活跃任务以异常结束**。这是任务的正常终止方式。

上报前必须过滤，判据卡到最窄：**已请求关闭** 且 **异常恰好是 `operation_aborted`**。
`awaitable_operators` 的组合（`reader() && writer()`）被取消时抛的是
`asio::multiple_exceptions` 包装，需要递归识别其 `first_exception()`。

参考实现：`EdgeServer::Impl::isCancellationUnwind`。

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
- fire-and-forget 的 `asio::post` / 线程池任务**必须**在 lambda 内包一层 catch。
  handler 抛出会离开线程池的工作线程，直接 `terminate`。参考 `DiskTier::runQueued`。
- 占位后再启动的模式（先 `try_emplace` 占坑再 spawn），启动失败必须释放占位，
  否则后续请求会永久等待一个不存在的 leader。
- 新增 `enum` 失败类别时，在 `switch` 中显式列出，靠 `-Werror` 兜住遗漏。

## 7. 现状与待办

已符合本规范：

- `ruvia-http`：解析面全返回值，无异常控制流。
- `ruvia-core`：契约违反抛异常；`noexcept` 边界 `terminate`；worker 失败经
  `failureHandler` 或 `join()` 重抛；stop 回调失败进首失败记录（2026-07-23）。
- `ruvia-edge`：五类任务失败全部上报，取消展开已过滤，accept 循环退避续跑，
  磁盘线程任务不再 `terminate`（2026-07-23）。
- `ruvia-web`：请求级降级链完整（handler → onError → 默认响应），已提交响应后
  不伪造错误响应而是拆连接。

待办：

- `ruvia-web` 连接级失败无上报通道。`HttpServer::handleSession` 的 catch-all 关闭
  连接后丢弃异常内容；`workerFailure` 是 worker 级、语义是"停 worker"，不适合
  承载单连接失败。需要新增一个连接级 sink。
