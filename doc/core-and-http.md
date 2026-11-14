# `ruvia-core` 运行时与 `ruvia-http` 协议层 API 手册

本文档提供 `ruvia::core`（独立协程与事件循环运行时底座）和 `ruvia::http`（独立 Sans-I/O HTTP/1.0、HTTP/1.1、HTTP/2、HTTP/3、HPACK、QPACK、QUIC、WebSocket 与内容编解码协议层）的公开头文件与核心 API 参考。两库均可脱离 `ruvia::web` 独立使用。所有公开符号位于 `namespace ruvia`（状态码常量位于 `namespace ruvia::http_status`）。

---

## 第一部分：`ruvia::core` 运行时 API

### 1. 协程、任务域与 Asio 桥接

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"task.h"` | `template <typename T> class task final`、`template <typename T> class task_awaiter final` | 结构化惰性协程（仅支持移动构造与右值 `co_await`）。未启动（cold）或已完成（done）可安全析构；**销毁已启动但仍挂起的 `task` 直接触发 `std::terminate()`**。`co_await std::move(t)` 返回的 `task_awaiter<T>` 仅由 `task` 构造，不可复制、移动或取址 |
| `"event_loop.h"` | `template <typename T> class root_task final` | 由 `event_loop::start(task<T>)` 或 `try_start(task<T>)` 返回的跨线程等待句柄。提供 `valid()`、`ready()`、`wait()`、`wait_for(operation_timeout)`、`std::move(root).get()`（仅右值提取并消费结果）；析构时若未调用 `get()` 则转为后台托管直至完成，**禁止在所属 `event_loop` 自身线程调用 `wait()` / `get()`** |
| `"task_scope.h"` | `class task_scope final`、`task_scope::join_operation`、`struct task_scope_options` | 单 Worker 亲和的结构化并发任务域。提供 `spawn(task<void>)`、`request_stop()`、`token()`、`stop_requested()`、`size()`、`empty()`、`join()`；构造期建立停止状态，`token() const&` 只读且不分配，第一个子任务失败时作用域自动请求停止；`join()` 返回可丢弃、仅启动前可移动的单次冷 `join_operation`，创建和启动须在绑定 Worker，启动后独占该作用域并关闭 `spawn`，并重新抛出第一个子任务失败；之后与其不等价（同一异常对象，或动态类型与 `what()` 均相同）的子任务失败在该子任务完成时交给 `report_unhandled_failure`，不被静默丢弃；析构前若曾 `spawn` 过任务，必须完成 `co_await scope.join()` |
| `"task_scope.h"` | `class operation_scope final`、`class scoped_capability_registration final`、`make_scoped_operation(...)` | 将借用型能力（capability）与惰性协程操作 `task<T>` 绑定到同一 Worker 作用域，`close_and_join()` 先回收冷协程帧并等待已启动操作完成后再失效 capability；`bind_worker(worker)` 借用该 Worker 句柄后，作用域内每个操作的启动、完成与冷帧回收都必须在该 Worker 上，否则 `std::terminate()` |
| `"event_loop.h"` | `async_start(task<T>&&, token)`、`template <typename T> using task_completion_result`、`template <typename T> asio::awaitable<T> as_awaitable(task<T> task_value)`、`template <typename Result = void> class asio_completion final`、`template <typename Result, typename Initiate> class asio_completion_awaiter final`、`template <typename Result = void, typename Initiate> auto async_asio(Initiate&& initiate)` | 将 `ruvia::task<T>` 作为任意 Asio 完成令牌的异步操作启动，或适配为 `asio::awaitable<T>`；将基于完成回调的 Asio 异步启动操作适配为 Ruvia 协程可直接 `co_await` 的 `asio_completion_awaiter`（返回 `asio_completion<Result>`，提供 `error_code()`、`succeeded()`、`failed()`、`result()` / `take_result()`） |

- **Asio 启动**：`async_start()` 在调用线程内联启动任务，经处理器关联执行器交付 `task_completion_result<T>`；`void` 任务为 `std::exception_ptr`，其余为 `std::variant<T, std::exception_ptr>`（值在前、失败在后）。
- **启动分配**：操作状态使用处理器关联分配器，交付前先回收协程帧；`as_awaitable()` 与 `event_loop::try_start()` / `start()` 均基于 `async_start()`。
- **原生等待物**：`asio_completion_awaiter` 不可复制、移动或取址；跨越单个 await 表达式保存时，以启动函数原地构造（如 `std::optional::emplace`）。
- **根任务限时等待**：`wait_for()` 只接受 `operation_timeout`：相对上限自调用时计时，非正值在构造 `operation_timeout` 时抛 `std::invalid_argument`；已过的绝对截止期只报告当前完成状态；`ready()` 在结果被 `get()` 消费后为 `false`。
- **根任务替换**：`root_task` 移动赋值先接管源状态，再退役旧状态；旧结果析构时重写源句柄或释放其宿主，不会丢失已接管的任务。
- **作用域任务移交**：`make_scoped_operation()` 的结果可直接交给 `task_scope::spawn()`；执行完毕后的内部移交仍保留作用域登记，直到帧释放才允许 `close_and_join()` 完成。
- **关闭重入**：冷帧清理可重入已开始关闭的 `operation_scope::close()`；存在运行中任务或尚未释放帧的完成任务时，仍须使用 `close_and_join()`，不能同步关闭。
- **Worker 引用借用**：`task_scope`、`worker_signal` 构造及 `operation_scope::bind_worker()`、`sleep_for()`、`sleep_until()` 只接受左值 `worker_ref`，拒绝含 `const` 的右值；引用宿主须地址稳定并覆盖作用域、信号或等待操作的寿命。

---

### 2. 停止令牌、超时、截止期与定时器

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"stop_token.h"` | `class stop_source final`、`class stop_token final`、`class stop_registration final`、`class stop_group final` | `stop_token` 只观察一个状态；`stop_group` 显式拥有父源注册和下游取消状态。`token.register_callback(fn)` 返回自有注册；`token.register_callback(registration, fn)` 替换调用方持有的注册。每个注册只嵌入一个侵入式节点，不额外分配节点；析构自动注销，并等待其他线程正在执行的回调结束 |
| `"stop_token.h"` | `class timeout_duration final`、`saturating_duration_cast<Target>()`、`saturating_deadline(now, d)`、`ceil_milliseconds(d)`、`deadline_after(d)` | 通用时长工具。`timeout_duration` 是配置输入（超时、心跳间隔、Web 流等待）的受检整数时长，可为零或负，由消费者自定策略；`value()` 显式返回 `std::chrono::milliseconds`，不接受浮点 chrono 输入 |
| `"stop_token.h"` | `class operation_timeout final`、`struct operation_options final { std::optional<operation_timeout> timeout_; stop_token stop_token_; }`、`override_operation_options(base, overrides)` | 单次操作上限规格（正的相对时长或绝对 `steady_clock::time_point` 截止期；相对构造非正抛 `std::invalid_argument`，`try_create()` 为返回 `std::nullopt` 的容错入口；`duration()` / `deadline()` 分别在对应形态有值，`deadline_from(start)` 解析截止期）与唯一的操作入参形状 |
| `"stop_token.h"` | `class operation_deadline final`、`template <typename Kind = std::monostate> class deadline_lifecycle final` | `operation_deadline` 是已启动操作解析后的截止期，在异步阶段间共享：默认无上限，`operation_deadline(std::optional<operation_timeout>)` 按当前时刻解析；`remaining()` 返回 `std::optional<std::chrono::milliseconds>`，`timeout()` 以绝对形态转交给嵌套操作。`deadline_lifecycle` 是分配无关的截止期状态（inactive / active / expired 互斥） |
| `"stop_token.h"` | `class worker_timer_registration final`、`class worker_timer_cancellation final`、`enum class worker_timer_outcome { expired, cancelled }` | 单 Worker 侵入式定时器注册句柄，配合 `worker_ref::schedule_timer` 使用；提供 `cancel()`（静默取消）与 `cancel_and_notify()` |
| `"task_scope.h"` | `enum class timer_sleep_result { elapsed, stop_requested }`、`class sleep_operation final`、`sleep_for(const worker_ref&, timeout_duration, [stop_token])`、`sleep_until(const worker_ref&, steady_clock::time_point, [stop_token])` | 在绑定 Worker 上挂起当前协程指定时长或直至截止时间点；时长是睡眠目标而非操作上限，`sleep_for(d)` 等价于 `sleep_until(now + d)`，非正时长与已过截止期一样不挂起直接 `elapsed`；返回可丢弃、仅启动前可移动的单次冷 `sleep_operation`，须在绑定 Worker 上 `co_await` 启动。若传入的 `stop_token` 触发或 Worker 停止定时器，返回 `stop_requested` 而不抛异常 |

- **注册重入边界**：两种 `register_callback()` 入口均先固定取消状态，再内部移交回调或退役旧注册；后续不再读取 token 宿主，旧载荷析构可清空或销毁 token。
- **取消目标借用**：`worker_cancellation_registration` 不保留目标的共享引用，只接受左值 `shared_ptr`、拒绝含 `const` 的右值；目标须由调用方持续持有，直至 `reset()` 等待回调结束并清除操作身份。
- **时长转换**：`saturating_duration_cast<Target>()` 对不宽于 `uintmax_t` 的整数计数按有理周期精确换算；正小数 tick 向上取整，负数向零截断，越界饱和。浮点换算下溢时仍为原始正值保留一个目标 tick。目标默认 `steady_clock::duration`，`timeout_duration` 使用毫秒目标，不先窄化源计数。
- **输入与禁用**：超时配置直接接受整数 chrono 时长；显式整数构造按毫秒计。未配置仍为 `std::nullopt`，零或负值不能代替未配置。
- **等待入参**：`channel_receiver::receive`、`channel_sender::send_wait`、`one_shot_receiver::wait`、`pool_lease_scheduler::acquire`、`run_blocking` 与 `worker_single_wait_awaiter` 只接受 `operation_options`，没有截止期平行重载；已启动操作把剩余预算交给嵌套操作时传 `{.timeout_ = deadline.timeout()}`（绝对截止期），不重新折算为相对时长。
- **非正与过期**：上限（bound）一律是 `operation_timeout`，非正相对值在构造时即抛 `std::invalid_argument`，无需另行校验；相对超时自 `co_await` 启动计时。已过期的绝对截止期不是错误，等待在无可交付结果时立即以 `timed_out` 结束。睡眠时长是目标（target）而非上限，`sleep_for()` / `sleep_until()` 的非正时长立即 `elapsed`。
- **上限合并**：`override_operation_options()` 中 token 未指定则继承、指定则替换；timeout 只收紧不放宽：同为相对取较短，同为绝对取较早，混合时把相对值按合并时刻解析为绝对截止期后取较早。
- **显式解包**：`timeout_duration` 读取计数用 `.value().count()`（optional 用 `->value().count()`）；与经过时长比较前必须 `.value()`，禁止混合类型比较把经过时长先向上取整而提前触发超时。
- **统一入口**：槽位租约、HTTP 客户端/服务器、WebSocket、数据库、Redis 的超时与探活使用同一类型；缓存 TTL、会话有效期与限流窗口不属于该契约。

`stop_group group{first.token(), second.token(), ...}` 或 `stop_group group{tokens_span}` 可组合任意数量的父状态，也可输入另一个 group 的 `token()`。group 不可复制或移动，必须保持地址稳定并存活到使用其 token 的所有操作结束 / join；token 的复制不会延长 group 的父源注册寿命。0 / 1 个不同有效源直接透传，不创建新状态；真实多源组合创建一个下游状态，前两个父注册内嵌，更多父注册按实际数量分配。重复源与嵌套源都只触发每个下游回调一次，不存在用户源容量被框架内部源占用的问题。

`request_stop()` 支持跨线程；已停止状态的注册同步执行回调。回调可注销自身，也可销毁正在转发的 group；group 先注销父注册再退役自身取消源。回调在请求取消的线程执行，不可直接修改其他 Worker 所有的状态，应向 owner Worker 投递通知。源先销毁不会使已有 token 悬空；`has_state()` 区分空 token 与源已退役的 token，`stoppable()` 表示状态是否仍可能取消或已经取消。

所有客户端 handle 的 `with_options` 使用相同规则：未指定 token 继承，提供非空 token 则替换原用户取消状态，不隐式累积；超时始终取已有值与新值中较紧的上限。需要累计时由调用方持有 `stop_group` 并传入其 token。客户端关闭通过自身 backend 的关闭、等待唤醒和 join 主链独立生效，不因 token 替换而失效，也不为每次派生 handle 建立共享取消树。Redis 独立客户端未提供 token 时沿用客户端生命周期 token，满足阻塞命令 / 订阅需要明确取消边界的契约；显式 token 替换后仍受客户端关闭约束。数据库借用外部 Redis 的缓存操作由缓存 owner 的独立停止源中止，不关闭外部 Redis 客户端。

---

### 3. 事件循环、Worker 运行时与阻塞卸载池

```cpp
#include "event_loop.h"
#include "task_scope.h"

ruvia::event_loop_pool pool({.loop_count_ = 2, .queue_capacity_ = 1024});
pool.start();

ruvia::event_loop loop = pool.next_loop();
auto root = loop.start([](ruvia::worker_ref worker) -> ruvia::task<int> {
    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
    co_return 42;
}(loop.worker()));

int answer = std::move(root).get();
pool.join();
```

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"event_loop.h"` | `class event_loop final`、`class event_loop_stop_registration final`、`class event_loop_attachment final`、`attach_event_loop(asio::io_context&, options = {})`、`class event_loop_pool final`、`struct event_loop_pool_options` | 指向单个 Worker 运行时及其 `asio::io_context` 的轻量引用，只承担循环生命周期职责（`valid()`、`worker()`、`io_context()`、`executor()`、`report_failure(e)`、`defer_cleanup(fn)`、`try_start(task<T>)` / `start(task<T>)`、`on_stop(fn)`）；准入状态、身份、投递与定时器经 `worker()` 返回的 `worker_ref` 访问，将 Ruvia Worker 挂载到外部自管 `asio::io_context` 的适配器（`loop()`、`run()`、`stop()`、`valid()`），以及管理固定数量独立事件循环线程的池（`start()`、`stop()`、`join()`、`loop_count()`、`loop(index)`、`next_loop()`、`loop_for(key)`） |
| `"worker_ref.h"` | `class worker_ref final`、`using worker_id_type = std::uint64_t`、`enum class post_status`、`template <typename Sig> class post_outcome final`、`class worker_shutdown_registration final`、`enum class worker_registration_status { registered, worker_stopping }`、`concept worker_shutdown_observer` | 跨线程安全的稳定派发端点与有界队列投递结果。提供 `valid()`、`accepting()`、`is_current()`、`id()`、`blocking_pool()`（借用的阻塞池或空）、`executor()`、`post(fn)`、`post_factory(factory)`、`post_payload(payload, wrap)`（准入后才把负载包装成回调，拒绝时退回负载）、`schedule_timer(...)`、`defer_resume(handle)`、`defer_completion(fn)`、`register_shutdown(owner, &Owner::registration)` |
| `"worker_ref.h"` | `class worker_runtime final`、`struct worker_runtime_options`、`struct worker_runtime_hooks`、`enum class worker_runtime_state` | 底层单 Worker 线程与两阶段启停状态机（`startup -> ready -> serve -> stop_admission -> finalize -> drain -> shutdown -> join`）；`take_failure()` 接管首个失败的上报责任，析构不再上报，重复调用返回同一失败；`loop() &` 返回该 Worker 的 `event_loop`（首次调用时创建，`report_failure()` 使运行时失败），此后运行时的 `stop_admission` 等待循环的根任务、停机回调与清理排空后才执行；循环借用运行时，任何副本不得比运行时活得久 |
| `"worker_ref.h"` | `class worker_group final` | 协调多个 `worker_runtime` 同步完成两阶段启动屏障（`start()` / `serve()`）与停机回收（`request_stop()` / `join()`） |
| `"blocking_pool.h"` | `class blocking_pool final`、`template <typename T> class blocking_result final`、`run_blocking(worker_ref, [operation_options], fn)`、`using blocking_submit_result` | 固定大小阻塞卸载线程池，由调用方拥有：通过 `event_loop_pool_options`、`attach_event_loop` 的选项或 `worker_runtime_options` 的 `blocking_pool_` 交给事件循环借用，须比事件循环存活更久；core 不持有隐式进程级池。`run_blocking` 在绑定 Worker 上 `co_await`，将 `fn` 提交至该 Worker 借用的池并在完成后回到原 Worker 恢复协程；Worker 没有池时返回 `blocking_status::unavailable`，不提交 `fn`（`worker_ref::blocking_pool()` 可预先查询）。`operation_options` 的 `timeout_` / `stop_token_` 只限定等待（相对上限自任务启动计时，绝对截止期可与调用方其他阶段共享）；已停止的令牌或已过期截止期同样不提交 `fn`。调用或交付值的异常保存在结果中并由 `value()` 或 `take_value()` 重新抛出。超时、取消或 Worker 停机先释放等待者；已开始的调用继续完成，但只持有自身数据，迟到结果被丢弃 |

- **启动准入**：`try_start()` 先取有界准入槽位再移动任务，结果 `event_loop_start_result<T>`（即 `worker_delivery_result<task<T>, post_status, accepted, root_task<T>>`）的类别构造后固定；接受时以 `value()` / `take_value()` 取得 `root_task`，`queue_full` 或 `worker_stopping` 时以 `rejected()` / `take_rejected()` 退回未消费的冷任务，仅分配失败抛异常。
- **严格启动**：`start()` 基于 `try_start()`，拒绝时销毁冷任务并抛 `event_loop_start_rejected`（`std::runtime_error` 子类），`status()` 区分队列满与停机；已准入但因停机未运行的根任务同样以该异常（`worker_stopping`）完成。
- **完成通道**：`worker_ref::defer_completion(fn)` 是已准入操作的完成、取消与恢复通道，不受队列满拒绝，本线程调用也在后续轮次执行，不另取退役保护；执行上下文分离后返回 `false` 并在调用线程销毁 `fn`。
- **循环清理**：`event_loop::defer_cleanup(fn)` 是循环级冷路径资源清理，不属于任何已准入操作：绕过有界准入但取退役保护，回调及其输入在循环释放执行上下文前退役；退役开始后可返回 `false`。
- **停机观察**：`register_shutdown(owner, &Owner::registration)` 将所有者成员链入 Worker 停机登记，停机线程先对全部所有者调用 `worker_stopping() noexcept`，再调用 `worker_stopping_complete() noexcept`；仅持弱引用，已在析构的所有者被跳过。
- **停机登记寿命**：`worker_shutdown_registration` 须为所有者首个数据成员，析构即摘链；Worker 已停止接收时返回 `worker_stopping` 且不链入，重复登记抛 `std::logic_error`。
- **原生执行器**：`worker_ref::executor()` 返回 `asio::io_context::executor_type`，可直接创建 socket、定时器并接入其他 Asio 库；不是有界业务准入入口。
- **执行器寿命**：空引用或已退役 Worker 获取执行器抛 `std::logic_error`；停机排空期间仍可供在途操作完成。执行器借用物理 context，不延长其寿命；调用方须在运行时退役前结束原生操作。
- **外部循环失败出口**：`event_loop_attachment_options::failure_` 拥有接收 `std::exception_ptr` 的回调；未配置时保留默认诊断。具名 options 通过 `std::move` 交给 `attach_event_loop()`。
- **失败交付线程**：先请求停止，再在报告线程锁外同步调用；可能跨线程并发或重入，接收器负责自身同步。回调异常仅进入诊断，不逃出宿主 `run()`，不打断清理。
- **失败出口寿命**：回调保留到最后一个 loop-state owner 释放；借用捕获须覆盖该寿命，禁止捕获 owning loop 形成环。全部 owner 消失后的迟到根任务异常回落默认诊断。
- **停机回调退役**：`event_loop_stop_registration` 的析构、覆盖与 `reset()` 取消尚未启动的回调；已启动回调的捕获保留至异步清理完成。
- **停机注册移交**：移动赋值先接管新注册，再退役旧回调；旧捕获析构重入写入的注册最终生效。
- **退休池诊断**：已 `join()` 的池经保留的 loop 上报异常时，在池锁外读取异常诊断；非 Worker 线程上的 `what()` 可重入 `join()`。
- **Windows 退出准备**：在另一个 worker 上 `co_await runtime.prepare_thread_join(caller)`，等待停止派发退出并启动必要的冷排空。
- **Windows 退出完成**：返回调用方拥有的 `SYNCHRONIZE` 线程 `HANDLE`（以 `uintptr_t` 交付；`0` 表示已 join）；异步等待句柄信号后调用 `join()`，不占阻塞池。
- **Windows 退出寿命**：运行时须存活至异步操作完成，不得并发执行其他 join。
- **阻塞池投递**：`submit(fn)` 返回 `blocking_submit_result`：先取槽位再移动负载，`queue_full` / `pool_stopped`（含移动期间停机）时经 `rejected()` / `take_rejected()` 完整退回闭包，不在调用方线程销毁；`run_blocking` 被拒后由协程销毁退回闭包，其守卫以 `pool_stopped` 应答无人等待的 one_shot。
- **阻塞池停止**：`stop()` 永久关闭准入，在锁外销毁未启动任务，不中断运行中的调用。
- **阻塞池等待**：`join()` 等待工作线程、在途投递和队列任务析构完成；池线程不得自等，非池排空线程重入 `join()` 时不等待自身析构。

`blocking_result<T>` 是不可复制、不可重新赋值的完成快照，支持 payload 所允许的移动构造。保存位置需要替换时使用新局部变量或 `std::optional::emplace()`。移动失败结果后，源与目标仍保留相同异常；移动成功结果后，源 payload 遵循自身移动语义，状态分类不变。`completed()`、`failed()` 与 `value()` 不会因结果替换或错误信息被移走而出现“成功但无值”的状态；任务与结果传输仍允许抛异常，不要求 payload 的移动为 `noexcept`。

- **阻塞结果构造**：`with_value(value)` / `with_value()`（`void`）、`with_error(exception)` 与 `with_status(status)` 构造与 `run_blocking` 相同的结果；空异常或 `with_status(completed)` 抛 `std::invalid_argument`。
- **阻塞结果借用与提取**：`value()` 仅允许左值；右值统一使用 `std::move(result).take_value()`，包括 `blocking_result<void>`。

---

### 4. 跨线程通道、单次交付、信号与内存资源

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"worker_ref.h"` | `enum class worker_wait_status { value, closed, worker_stopping, timed_out, cancelled }`、`template <typename T> class worker_wait_result final` | Worker 等待操作的统一返回包装（提供 `status()`、`has_value()`、`value()`、`take_value()`）；`with_value(value)` / `with_status(status)` 构造同类结果，`with_status(value)` 抛 `std::invalid_argument`。同头的 `worker_delivery_result<T, Status, Accepted, AcceptedValue = void>` 是 post / submit / send / complete / start 的统一投递结果：拒绝时持有未消费负载，`AcceptedValue` 非 `void` 时接受结果持有其值（`value()` / `take_value()`）；只能由投递原语产生，外部不可构造 |
| `"worker_wait.h"` | `template <typename T, typename State> class worker_wait_slot final`、`template <typename T, typename State> class worker_single_wait_awaiter final`、`concept worker_wait_state`、`make_worker_wait_state<State>(args...)` | 单挂起等待者的 Worker 绑定等待构件，`channel` 与 `one_shot` 即由其组合；外部原语按同一契约组合超时、取消与 `worker_stopping` |
| `"worker_wait.h"` | `make_channel<T>(worker_ref, channel_options)`、`class channel_sender<T>`、`class channel_receiver<T>`、`channel_receiver<T>::receive_operation` | 有界 MPSC 通道：`channel_sender` 可拷贝并支持多生产者跨线程 `send(value)`；`channel_receiver` 绑定单个目标 Worker，`receive(operation_options = {})` 返回可丢弃、仅启动前可移动的单次冷 `receive_operation`，须在绑定 Worker 上 `co_await` 启动。`send_wait(value, operation_options = {})` 返回冷 `channel_send_operation<T>`：有空位立即投递，满载时挂起，多个等待发送方按 FIFO 取得接收方释放的空位（空位预留给队首等待者，经其 Worker 的恢复通道恢复，不内联）；须在某个 Worker 线程上 `co_await`，在该 Worker 恢复，不在 Worker 上抛 `std::logic_error`；关闭、任一方 Worker 停机（`worker_stopping`）、超时（`timed_out`）或取消（`cancelled`）时经结果完整退回值，`send()` 永不返回后两者。容量限制排队值与正在构造的发送预留；预分配一个额外交付槽，若所有槽均暂被接收或销毁操作占用，发送可返回 `full`。并发发送按发布顺序交付；构造期间关闭通道则返回被拒绝的值。`send()` 调用期间须保持调用者对象存活，包括值的移动和析构 |
| `"worker_wait.h"` | `make_one_shot<T>(worker_ref)`、`class one_shot_completion<T>`、`class one_shot_receiver<T>`、`one_shot_receiver<T>::wait_operation` | 跨线程单次值交付原语：`complete(value)` 可在任意线程调用，`wait(operation_options = {})` 返回可丢弃、仅启动前可移动的单次冷 `wait_operation`，须在绑定 Worker 上 `co_await` 启动。另一交付正在构造值时返回 `in_progress` 和被拒绝的值；构造失败后可重试，构造期间接收端关闭或 Worker 停机则拒绝交付。已交付值在接收端移动失败时异常传给接收端且值已消费。调用期间须保持 completion 存活，包括值的移动和析构 |
| `"task_scope.h"` | `class worker_signal final`、`worker_signal::wait_operation` | 同 Worker 内部闩锁合并通知；`wait()` 返回可丢弃、仅启动前可移动的单次冷等待器，不是 `task<void>`。创建和启动须在所属 Worker，`notify()` 同样须在所属 Worker 调用，否则二者都抛 `std::logic_error`；启动后必须等待完成；支持多个等待者，直接挂入调用方协程帧 |
| `"event_loop.h"` | `class worker_notification final` | 跨线程闩锁合并通知原语（任意线程无锁 `notify()`，所属 Worker 上 `co_await notification.wait()`）。从 `event_loop` 构造时仅接受左值、拒绝含 `const` 的右值；借用的 loop 须保持地址稳定并覆盖通知对象的寿命 |
| `"worker_wait.h"` | `class pool_lease_scheduler final`、`class pool_lease_acquire final` | 单 Worker 连接/槽位池租约调度器（空闲时同步返回槽位下标，满载时挂起排队并支持超时与取消）；`acquire(operation_options = {})`。与 `channel` / `one_shot` 一致，绑定 Worker 的停机通知到达或定时器队列停止后，排队中与新到的满载 acquire 以 `worker_stopping` 完成，空闲槽位仍同步租出 |
| `"task.h"` | `void collect_thread_heap() noexcept` | 强制收集调用线程的 mimalloc 线程堆及共享 arena 的待回收范围，不影响存活分配 |
| `"stop_token.h"` | `template <typename Sig> class move_only_function` | 带 `3 * sizeof(void*)` 小对象内联优化（SBO）的仅移动可调用包装器；支持普通与 `noexcept` 签名，后者编译期拒绝可抛目标。`const` 包装器仍可调用可变目标，跨线程调用须由目标同步。移动赋值与赋空共用退役路径：先提取源并提交，再于独立存储销毁旧目标，支持旧目标拥有源包装器；旧目标析构时的重入赋值最终生效 |
| `"worker_ref.h"` | `fnv1a_offset_basis`、`fnv1a_prime`、`fnv1a_accumulate`、`fnv1a_accumulate_integer` | 零分配、可编译期求值的 64 位 FNV-1a 非密码学散列；整数按小端字节混入，跨平台结果一致 |
| `"event_loop.h"` | `as_bytes`、`as_chars`、`error_code_text`、`system_error_text`、`parse_ip_address`、`close_socket` | 字节/字符零拷贝视图转换（拒绝绑定临时拥有型字符串或连续字节容器，调用方须保持底层存储存活到消费完成）、跨平台 UTF-8 系统错误文本转换、严格 IPv4/IPv6 字面量解析（返回 `std::optional<asio::ip::address>`）与取消未完成 I/O 后关闭 TCP socket |

**未知 Windows 系统错误**：`error_code_text()` 无法取得系统消息时返回 `category:value`，其中 `value` 为带符号十进制错误码。

`move_only_function<R(Args...)>` 的引用结果必须由目标直接返回兼容引用，允许增加 cv 限定及派生到基类转换；返回值或需要产生临时量的结果不能绑定到引用签名。不同工具链采用相同约束，不依赖编译警告阻止悬垂。需要自定义引用转换（包括按值返回 `std::reference_wrapper`）时，用显式 `-> T&` / `-> const T&` 的适配 lambda 返回实际存活对象；包装器不延长该对象的寿命。值返回签名仍支持正常隐式转换。

`worker_wait_result<T>` 的状态分类在构造后固定；当 `T` 支持时可复制或移动构造，但不可重新赋值。替换保存的结果时使用新局部变量或 `std::optional` 的 `emplace()`，不要给结果或其 `optional` 直接赋值。`value()` 仍可修改 payload，移动构造和 `take_value()` 不改变源结果的状态分类，payload 遵循自身移动语义；其构造或提取抛异常不会让已有结果失去可查询的状态。通道与单次交付仍支持可抛异常的 payload，不要求其移动为 `noexcept`。

- **拒绝负载提取**：`worker_delivery_result::take_rejected()`（及接受值的 `take_value()`）成功后清空负载；失败时不额外销毁负载，内容遵循 payload 自身移动语义，可检查后重试。两种情况下结果状态均不变。
- **交付状态保活**：`one_shot_completion::complete()` 与 `channel_sender::send()` 在移动 payload 前固定状态所有权；同步重绑调用句柄不改变本次操作目标，原状态保留至提交或拒绝处理完成。

- **等待构件状态**：共享状态提供 `worker()`、`lock_state()` 与 `wait_slot()`，所有槽位调用都持该锁；`worker_wait_state` 还要求名为 `shutdown_registration_` 的 `worker_shutdown_registration` 数据成员并满足 `worker_shutdown_observer`，经它把 `worker_stopping` 交给槽位。
- **等待构件创建**：`make_worker_wait_state<State>(args...)` 分配状态并把 `shutdown_registration_` 链入其 Worker 的停机登记；Worker 已停止接收时抛 `std::runtime_error`。
- **等待构件协议**：每次等待按值持有一个 `worker_single_wait_awaiter`；`await_ready` 先 `start()`，再持锁 `complete_ready()` 已有结果或 `complete_or_publish()` 挂起，随后 `suspend()` / `take_result()`。
- **等待构件交付**：领域事件持锁经 `wait_slot().complete(...)` 交付平凡可移动值或终止状态；启动前可移动，启动后地址固定，同一状态只允许一个挂起等待者。

`pool_lease_scheduler` 的空闲槽位仍同步获取；`release(slot)` 向排队者提交槽位归属后，通过后续 Worker 轮次恢复该协程，不在释放调用栈内执行下一位消费者，避免连续立即释放形成递归链。`close()` 同样先向全部排队者提交 `closed` 结果，再通过后续 Worker 轮次恢复它们，调用方的关闭序列在任何等待者观察到关闭前完成。提交后的取消或 `close()` 不撤销已交接槽位；尚未交接的等待者按原取消、超时和关闭规则完成。不得依赖 `release()` 或 `close()` 返回代表排队协程已经完成，销毁 scheduler 前仍须等待已启动的 acquire 完成。

框架内存由 mimalloc 原生承载，全仓使用标准容器，不设 `std::pmr` 包装层或额外池层；协程帧直接使用 mimalloc。

- **原生线程堆**：不另建 `mi_heap_t` 或 Worker 级池；分配直接落在调用线程的 mimalloc 线程堆，可在任意线程释放；不叠加尺寸类缓存、对象头或协程帧缓存。
- **Asio 缓存**：`ruvia::core` 公开传递 `ASIO_DISABLE_SMALL_BLOCK_RECYCLING` 与 `ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`，所有翻译单元保持一致；不在 mimalloc 之上叠加 handler 或 awaitable 帧缓存。
- **HTTP/1 工作集**：仅在活跃请求期间持有，流水线请求连续复用；连接空闲、关闭或异常退出时直接释放给 mimalloc，不维护空闲工作集缓存。
- **原生回收与手动收集**：不覆盖 mimalloc 的原生回收默认值；Web 在完整空闲扫描周期后于该 Worker 上触发一次 `collect_thread_heap()`，强制收集调用线程的线程堆及共享 arena 的待回收范围，不自建回收线程。
- **透明大页与 musl 构建**：Linux 的 `x64-linux` 与 `x64-linux-musl` triplet 为 mimalloc 设置 `-DMI_NO_THP=ON`；`x64-linux-musl` 另设 `-DMI_LIBC_MUSL=ON`。
- **失败与检测**：分配失败抛 `std::bad_alloc`；sanitizer 构建使用系统分配器，不保留自建缓存。`error_code_text()` 与 `system_error_text()` 返回 `std::string`。

非 Windows 构建启用 mimalloc 普通 `override`，在最终链接中保留静态库，对标准 `malloc` / `free` 与全局 `new` / `delete` 使用 mimalloc；不得叠加另一套全局 allocator replacement。Windows 保持原静态 CRT 与普通 mimalloc 依赖。独立 `ruvia::http` 不依赖 core 或 mimalloc。

---

## 第二部分：`ruvia::http` Sans-I/O 协议层 API

`ruvia::http` 不执行任何 socket I/O、定时器或线程调度，全部协议状态机均以内存字节流（`std::string_view` / `std::span` / `http_io_buffer`）与强类型事件/计划对象交互。

### 1. 基础类型、缓冲与编码工具

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"borrowed_text.h"` | `class borrowed_text final`、`template <typename element_type> class borrowed_span final` | 只读字符串与连续元素序列借用视图，拒绝直接绑定临时拥有型字符串、临时连续容器（`std::vector`、`std::array`）及 `std::initializer_list`，但不延长底层 owner 的生命周期；调用方仍须保持命名存储有效直到消费入口完成读取或拥有化 |
| `"borrowed_text.h"` | `class port_number final` | 原始整数宽度受检的端口值；在格式化或原生 API 边界用 `value()` 显式解包 |
| `"borrowed_text.h"` / `"http_request.h"` | `ip_address`、`ip_address_family`、`ip_endpoint`、`ip_cidr`、`http_client_ip_extractor`、`http_client_ip_config`、`extract_http_client_ip` | 无 Asio 的网络字节序 IP 地址、CIDR 前缀匹配（IPv4 块自动匹配 `::ffff:a.b.c.d`）、`normalized_ipv4()` 规范化，以及支持可信代理集合、按优先级解析 `Forwarded`（RFC 7239）、逗号列表型（`X-Forwarded-For`）及单 IP 型转发头并可选提取 `X-Forwarded-Proto` / `Forwarded` `proto` 的无状态客户端 IP 提取器 |
| `"http_request.h"` | `proxy_protocol_parser`、`proxy_protocol_header`、`proxy_protocol_parse_result` | [HAProxy PROXY protocol v1/v2](https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt) 单遍增量解析；`parse(span)` 只消费头字节，`complete_` 后通过 `header()` 读取源/目标与 TLV 视图；非法或超限抛 `proxy_protocol_error` 并进入 failed，仅 `reset()` 可复用。v1 上限 107 字节，v2 默认整头 4096 字节，可配置且至少 52；`LOCAL` 忽略其不透明负载，源/目标为空 |
| `"http_header.h"` | `class protocol_byte_limit final` | 显式有限／无限字节预算：默认和 `unlimited()` 不限量，`limited(n)` 接受原始整数，在窄化前检查严格正数且可表示为 `std::size_t`，否则抛出 `std::invalid_argument`；浮点和枚举不隐式接受，不要预先窄化输入；`SIZE_MAX` 仍是有限预算，不是无限哨兵 |
| `"borrowed_text.h"` | `class http_io_buffer final` | 流式读写缓冲区：`prepare(n)` / `prepare_spare()`、`commit(n)`、`append(bytes)`、`data()` / `view()`、`consume(n)`、`clear_retaining_small()`；返回借用的四个入口只接受左值，缓冲区存储须覆盖异步 I/O 与视图消费。`prepare_spare()` 优先返回尾部空间，仅尾部耗尽且已消费前缀覆盖存活后缀时压缩；可返回空 span，调用方需要更多空间时使用 `prepare(n)`，并按实际 span 长度读取 |
| `"http_header.h"` | `http_protocol_version`（`http10`, `http11`, `http2`）、`http_scheme`（`http`, `https`）、`http_known_method`、`classify_http_method`、`is_valid_http_method_token` | 协议版本、Scheme 与 HTTP 方法分类及幂等/安全性判断 |
| `"http_header.h"` | `class http_status_code final`、`namespace ruvia::http_status`、`http_reason_phrase` | 强类型 HTTP 状态码（`100..599`）与标准状态码常量（`http_status::ok`、`not_found` 等） |
| `"http_header.h"` | `class http_protocol_error final : public std::exception`、`enum class http_parse_error` | 零动态分配的协议错误异常（`http_protocol_error(status, code, message)`：携带 `http_status_code`、静态存储期的稳定 snake_case 机器码 `code()`（如 `missing_host`、`conflicting_content_length`）与定长诊断文本）与解析错误枚举 |
| `"http_request.h"` | `http_authority_view`、`http_origin_view`、`parse_http_authority`、`normalize_http_proxy_request_target`、`parse_http_max_forwards`、`http_method_sends_zero_content_length`、`estimate_http1_response_head_bytes`、`estimate_http1_field_section_bytes` | RFC 9110 / RFC 3986 请求目标、代理 `absolute-form` 规范化（含空路径 `OPTIONS` 转 `*`）、`Max-Forwards` 单值校验与饱和递减、无请求体方法 `Content-Length: 0` 语义查询，以及 HTTP/1.1 线面头部/Trailer 等价字节数估算 |
| `"http_request.h"` | `decide_http_target_origin`、`http_target_origin`、`http_target_origin_connection`、`http_target_origin_action` | RFC 9110 §7.4 目标源判定：服务器提供连接事实（是否 TLS、对端是否可信网关），返回接受、需按证书校验的主机（DNS 名去掉末尾点 / IPv4 / IPv6 网络字节序）或 `421` 拒绝；明文连接上的 `https` 目标直接判为 misdirected，零分配 |
| `"borrowed_text.h"` | `decode_url_component`、`validate_url_encoding`、`visit_url_encoded_pairs`、`percent_encode_uri_reference`、`uri_paths_equivalent`、`compare_uri_paths`、`http_encode_lower_hex`、`http_base64_encode`、`http_base64_decode`、`struct utf8_sequence final`、`scan_utf8_sequence`、`is_valid_utf8` | URL 百分号/表单编解码、RFC 3986 URI-reference 规范化编码（`decode_url_component` 与 `percent_encode_uri_reference` 无需变换时返回输入视图本身、不分配，否则把结果写入调用方提供的 `std::string` 并返回其视图；解码只在百分号编码非法时返回 `nullopt`）、路由使用的路径规范形式比较（字面 `/` 分段、段内按百分号解码后的字节比较，`%2F` 不作分隔符）、十六进制转换、标准与 URL-safe Base64 编解码，以及共享 UTF-8 公开 API（`constexpr`、`noexcept`、零分配） |
| `"borrowed_text.h"` | `http_format_signed`、`http_format_unsigned`、`http_append_signed`、`http_append_unsigned`、`http_append_formatted_number`、`http_append_formatted_finite_number` | 整数切片格式化与字符串追加共用同一入口；浮点按原类型生成最短十进制文本，有限值入口在修改输出前拒绝 NaN 与无穷大 |

- **缓冲退役顺序**：`http_io_buffer` 清空或重分配时先提交状态，再释放旧存储；`append()` 的新增字节一并提交，退役回调的重入写入保留。
- **缓冲分配失败**：保留原窗口与已借用字节，恢复资源后可使用原输入重试。

`decode_url_component()` 与 `percent_encode_uri_reference()` 的文本输入使用 `borrowed_text`：字符串字面量、`std::string_view` 和命名的 `std::string` 可直接传入，临时拥有型字符串（含 `const` 右值）在编译期拒绝。返回值可能借用原始输入，也可能借用输出 `storage`，两者的 owner 均须覆盖结果消费；不要根据某次内容是否需要转换来推断寿命。需要传入生成的字符串时先保存为命名 owner，需要独立结果时在 owner 仍有效期间复制视图；显式构造 `string_view` 不会延长 owner 寿命。支持零拷贝和输入／输出别名。

- **URL 编码失败回滚**：`http_append_percent_byte()` 与 `percent_encode_uri_reference()` 发生分配或长度错误时保留原输出内容，允许恢复资源后重试。

`http_status_code::from_value()`、`try_from_value()` 与 `is_valid_value()` 接受原始整数类型并在窄化前检查 `100..599`；`http_received_status::try_from_value()` 同样在原始整数域检查 `0..999`。超宽或负数不能通过截断变成有效状态码。这些入口均使用整数模板，浮点、枚举及其他依赖隐式转换的调用须先由调用方完成明确的整数转换和相应校验；不要预先窄化到 `uint16_t`，否则会丢失原始边界信息。

端口输入统一使用 `port_number`：包括 `http_origin_options::port_`、`http_connect_udp_target_view::port_`、`http_authority_view::effective_port()` / `http_authorities_equal()` 的默认端口，以及 Web 的监听、数据库和 Redis 配置；可选端口仍保留 `std::optional`。合法整数可直接填入，窄化前按原始宽度检查 `0..65535`，负数或超范围值抛出 `std::invalid_argument`；需要无异常探测时用 `port_number::try_from_value()`。浮点与枚举不隐式接受，也不提供到原始整数的隐式转换；不要在调用前窄化。零端口与缺省端口不同，各入口按自身契约解释零端口：Authority 比较的默认端口 `0` 仍表示未知 scheme 默认值，省略／空端口不能因此等于显式 `:0`。已解析的协议端口和系统观察结果仍可返回 `std::uint16_t`；默认值回填后的裸数值不能独自保留这些语义差异，Authority 等价判断使用 `http_authorities_equal()`。

---

### 2. HTTP 首部、缓存、条件请求、范围与 Cookie

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"http_header.h"` / `"http_request.h"` / `"http_cache.h"` | `class http_header final`、`class http_header_view final`、`class borrowed_http_headers final`、`is_valid_http_header_name`、`is_valid_http_header_value`、`http_connection_nominated_fields`、`parse_http_connection_nominated_fields`、`is_http_hop_by_hop_field_name`、`is_http_hop_by_hop_or_nominated_field`、`rebuild_http_connection_field` | PMR 拥有型首部键值对、非拥有型首部视图、RFC 9110 §7.6.1 逐跳字段判定、多行 `Connection` 大小写不敏感提名集合解析，以及保留 `Upgrade` 或非标准连接选项的 `Connection` 字段重建 |
| `"http_cache.h"` | `http_cors_policy`、`http_cors_policy_config`、`http_cors_preflight_input`、`http_cors_preflight_result`、`evaluate_http_cors_preflight`、`rewrite_http_cors_response_headers`、`merge_http_vary_tokens` | 无状态 CORS 策略原语（同时作为 `ruvia-web` 服务端唯一 CORS 实现）：支持 `*`、精确 Origin 列表与标签边界域名尾缀匹配，提供预检求值（`evaluate_preflight`）、代理响应头改写（`rewrite_response_headers`）与服务端响应头应用（`apply_response_headers`，含 `Allow` 提升） |
| `"http_content_coding.h"` | `class http_accept_match final`、`http_response_coding_selection_for(request)`、`select_http_accept_encoding`、`negotiate_http_content_coding` | `Accept` 媒体类型/语言权重匹配与 `Accept-Encoding` 响应压缩算法协商；支持按调用方给定的有序候选编码列表决胜同权重项，并比较显式/隐式 `identity` 权重 |
| `"http_cache.h"` | `format_http_date`、`parse_http_date`、`class cache_control final`、`parse_cache_control`、`parse_targeted_cache_control`、`resolve_targeted_cache_control`、`strip_http_cache_private_headers`、`strip_http_cache_no_cache_headers`、`http_cache_control_has_no_transform`、`rewrite_http_cache_control_max_age`、`rewrite_http_response_cache_max_age`、`http_vary_spec`、`parse_http_vary`、`format_http_vary_cache_key`、`http_vary_requests_match`、`compute_http_cache_age`、`is_http_304_forbidden_header`、`sanitize_http_304_response_headers`、`merge_http_304_response_headers` | IMF-fixdate 日期格式化/解析、RFC 9213 `CDN-Cache-Control` 等结构化字典定向缓存头解析与优先级选择、RFC 9111 §5.2.2.3/§5.2.2.7 `private="field"` / `no-cache="field"` 字段级限定判定与原地剥离、客户端 `max-age` 重写（清除 `Expires`）、多行 `Vary` 归一化与请求匹配、RFC 9111 §4.2.3 `Age` 计算，以及 RFC 9110 §15.4.5 `304` 表示元数据清洗与合并 |
| `"http_cache.h"` / `"http_response.h"` | `get_http_etag_preconditions`、`is_valid_http_entity_tag`、`http_strong_etag_equals`、`http_weak_etag_equals`、`weaken_http_etag`、`resolve_http_byte_range_set`、`http_byte_range_options`、`parse_http_content_range`、`http_byte_range_slicer`、`compute_http_byte_range_content_length`、`plan_http_representation_response` | 条件请求与单值 ETag 校验/强弱比对/弱化、`Range` 字节范围解析（支持可配区间上限及遇无序/重叠直接 `ignored` 的严格模式）、`Content-Range` 解析，以及支持按序或带偏移喂入数据块的单区间 / `multipart/byteranges` 流式切片器 |
| `"http_cache.h"` | `struct cookie_options`、`parse_set_cookie`、`class set_cookie_plan final`、`class http_cookie_jar final` | Cookie 属性校验（含 `__Secure-` / `__Host-` 前缀规则）、`Set-Cookie` 解析与序列化计划，以及无 I/O 的客户端 RFC 6265 Cookie 容器 `http_cookie_jar`（支持 Domain/Path/Secure 匹配、过期淘汰、容量上限裁剪与出站 `Cookie` 请求头生成） |
| `"http_header.h"` / `"http_request.h"` / `"http2_connection.h"` / `"http3_connection.h"` | `class http_priority_urgency`、`struct http_priority`、`tls_client_hello_info`、`parse_tls_client_hello`、`compute_tls_client_hello_fingerprints`、`http2_akamai_fingerprint_collector`、`http3_fingerprint_collector` | RFC 9218 优先级首部，以及无 OpenSSL 依赖的 TLS ClientHello 原始握手解析（自动过滤 GREASE，计算标准 JA3、区分 TCP `t`/QUIC `q` 的 JA4 与 PeetPrint）、HTTP/2 Akamai 指纹收集器与 HTTP/3 指纹收集器 |

- **HTTP/2 与 HTTP/3 字段名规范化**：共用单遍 ASCII 转换；已小写名称在暂存阶段直接借用，需转换时写入拥有缓冲。
- **字段名称权威**：请求、响应与连接控制规则共用按长度索引的字段名称表；响应位标记与二进制协议的小写名称由同一表导出。
- **Accept 参数值比较**：`charset` 及 `text/plain` 的 `format`、`delsp` 值忽略 ASCII 大小写（[RFC 3676 §4](https://www.rfc-editor.org/rfc/rfc3676.html#section-4)）；其他参数值保持大小写敏感，带引号值按解码后的字节比较。
- **Cookie Max-Age 接收**：十进制校验与转换单遍完成，仅对完整数值应用饱和；非法的后续 `Max-Age` 不覆盖此前有效值。
- **Cookie Max-Age 写出**：负值在转为无符号计数前拒绝，仅正值写出该属性；删除行为见[响应 Cookie](web-guide.md#32-构造响应与设置响应元数据)。
- **Cookie 写出原子性**：`write_cookie_request_pair()` 与 `append_cookie_request_pair()` 失败保留原内容；追加器扩容可能使旧借用视图失效，重试时重新获取视图。
- **Cookie 快照预算**：仅暂存与输出重叠的输入；未重叠输入不占临时内存预算，双重叠共享一份快照。

`http_priority::urgency_` 与 `http_priority_fields::urgency_` 共用 `http_priority_urgency`，后者仍以 `std::optional` 区分缺省参数。按 [RFC 9218 §4.1](https://www.rfc-editor.org/rfc/rfc9218.html#section-4.1)，值域为 `0..7`、默认值为 `3`；本地整数输入在窄化前检查，非法值抛出 `std::invalid_argument`，无异常探测用 `try_from_value()`。不隐式接受浮点或枚举，也不隐式转换回整数；需要整数时用 `urgency_.value()`（可选字段用 `urgency_->value()`），不要预先窄化。接收端仍按 RFC 9218 §4 忽略越界或类型不符的参数，不把本地构造失败规则套用到对端字段。

当两个 `http_header` 均绑定 PMR 时，赋值保留目标的资源对象。移动赋值在资源等价时直接转移存储、不分配；`is_equal()` 只允许互相回收存储，不表示两个资源对象具有相同寿命，因此不会改为借用源资源。拷贝和移动构造仍沿用源对象的资源。

- **首部退役提交**：`http_header` 赋空与资源等价的移动赋值先提交新字段状态，再释放旧存储；释放回调重入赋值的最终状态保留。

`is_valid_http_request_application_field(name, value)` 在应用配置或请求构造边界校验单个出站请求首部，返回 `bool`、`noexcept` 且不分配内存：检查字段名 token、规范化字段值（包括拒绝首尾 SP/HTAB 及非法控制字节），按大小写不敏感匹配拒绝 `Host`、`Content-Length`、`Transfer-Encoding`、`Trailer`、`Expect`、`Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Upgrade`，并复用请求 writer 的已知字段发送方语义校验（如 `Content-Type`、`Content-Encoding`、`Origin` 与 CORS 请求字段）。合法未知扩展和其空值仍允许；不引入应用配置政策。HTTP/1 与 HTTP/2 的 `TE` 差异已被托管字段边界排除，因此无版本参数。此接口不保留状态，字段间重复、完整消息约束仍由调用方及最终 writer 负责；不得用于响应首部或接收方校验，接收方仍按各协议入口的 recipient 规则处理。

`is_valid_http_response_field(name, value)` 声明在 `http_header.h`，同样返回 `bool`、`noexcept` 且不分配内存；检查通用名称/规范化字段值语法并复用响应发送方的 `Content-Type`、`Content-Encoding`、`Trailer` 语义校验。它不应用请求专属的 `Origin`、CORS、`Expect` 规则或请求托管字段黑名单，不用于接收方校验，也不替代响应写出入口的版本相关连接/分帧检查（如 `Connection`、`Upgrade`、`TE`）、重复字段检查及完整消息约束。应用禁改首部政策由调用方单独保留；此谓词返回 `true` 不表示字段在任意响应上下文中都可以发送。

---

### 3. 消息模型、内容压缩编解码、Multipart 与 SSE

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"http_request.h"` | `class http_request final`、`make_parsed_http_request`、`class http_request_trailers final` | 请求头与请求体零拷贝视图描述符、大小写不敏感首部/Cookie 查找与请求 Trailer 校验收集：收到的 Trailer 段统一由 `append_received(std::span<const http_header>)` 吸收（HTTP/1 分块解码器与 HTTP/2、HTTP/3 结束事件产出同一视图），计入全部字段预算并丢弃不得出现在请求 Trailer 中的字段；字段名无论来源均以小写保存 |
| `"http_response.h"` / `"http1_server_response.h"` | `class http_response final`、`class http_response_headers final`、`class http_interim_response_head final`、`plan_buffered_http_response_write` | 最终响应（`200..599`）与临时响应（`1xx`）模型，以及针对 `HEAD` / `204` / `205` / `304` 的响应体写计划。`head_metadata_body()` 将正文替换为描述 HEAD 所选表示的空正文；`has_head_metadata_body()` 查询该状态。显式 `Content-Length` 保留为 GET 表示的长度，缺失时不推断为零；重新设置正文会清除标记 |
| `"http_content_coding.h"` | `enum class http_content_coding { identity, gzip, deflate, brotli, zstd }`、`is_http_media_type_compressible`、`parse_http_content_coding_headers`、`request_content_coding`、`encode_http_content`、`decode_http_content`、`class http_content_encode_context final`、`class http_content_codec_error`、`class http_content_transcode_error final`、`class http_memory_budget_resource final`、`class http_content_encoder final`、`class http_content_transcoder final`、`class http_staged_content_transcoder final`、`class http_worker_codecs final`、`class http_stream_encoder_lease final` | 媒体类型可压缩性白名单判定、接收方 `Content-Encoding` 解析，单次缓冲、可复用缓冲上下文、流式增量编码器、每 Worker 可复用编码器租约池 `http_worker_codecs`、多级流式转码器 `http_content_transcoder`，以及面向未知长度（chunked）响应按 `[min_bytes_, max_bytes_]` 暂存并在越界时自动回退原流的 `http_staged_content_transcoder` |
| `"http_content_coding.h"` | `parse_multipart_boundary`、`parse_multipart_body`、`class multipart_parser final` | 一次性与流式增量（`feed` / `poll`）的 `multipart/form-data` 解析器 |
| `"http_content_coding.h"` | `struct sse_message final`、`format_sse_message`、`class sse_parser final`、`struct sse_parser_options final`、`enum class sse_parser_state`、`class sse_event final`、`struct sse_parse_result final` | Server-Sent Events 消息格式化与校验，以及客户端 `text/event-stream` 增量解析 |

- **首部转移模式**：`http_response_headers::transfer_from()` 与 `http_response::transfer_headers_from()` 仅接受 `merge` / `assign` / `apply`；非法值在变更前抛出 `std::invalid_argument`，空源也不例外。
- **响应编码接受性**：`http_response_coding_selection::accepts(token, request)` 支持自定义 coding；`evaluate_http_response_content_coding()` 区分无字段、接受、拒绝与非法栈。编码栈单遍解析，所需扩展 coding 一次性遍历请求偏好；常见栈不分配扩展存储，协商快照不借用请求。
- **响应首部预留**：`http_response_headers::reserve()` 扩容先切换描述符表，再释放旧表，保留退役回调中的首部修改；不复制字段字节，分配失败保留原表，容量足够时不分配。
- **首部移除键退役**：`remove_header()` 保留匹配键存储至描述符表截短与已知字段索引重建完成；该存储释放回调中重新添加的首部保留。
- **Vary 合并**：跨行查重与计长后直接写入最终字段存储，不拼接临时字符串；已有成员或通配符时不分配，新增通配符复用静态描述符。合并完成前保留旧字段，支持借用 token 与分配失败重试。
- **SSE 解析**：`sse_parser` 按 WHATWG HTML“Parsing an event stream”解析任意分块的响应体：`parse(input) &` 消费到下一个完整事件为止，返回 `sse_parse_result{consumed_bytes_, event_}`，调用方以未消费后缀继续调用；`sse_event` 的 `type()`（未命名时为 `message`）、`data()`（多行以 LF 连接、无末尾 LF）与 `last_event_id()` 借用解析器，有效至下一次 `parse()` / `finish()` / `reset()`。支持 `event` / `data` / `id` / `retry`，忽略注释与未知字段；跳过一个前导 UTF-8 BOM，行尾为 CRLF、LF 或 CR（含跨块 CRLF）；值中的非法 UTF-8 按最大子序列替换为 U+FFFD，含 NUL 的 `id` 被忽略，`retry` 仅接受非空 ASCII 数字（超出范围饱和），由 `reconnection_time()` 读取；`last_event_id()` 是重连时 `Last-Event-ID` 的取值。每个字节只扫描一次，未完成行只保留路由后的字段值而不缓存原始行。`sse_parser_options::event_limit_`（缺省 `default_max_sse_event_bytes`，16 MiB）限制一个事件缓冲的数据行、事件类型与待提交字段值字节，注释与忽略字段不占配额；超限进入 `failed`。`finish()` 丢弃未完成事件并进入 `finished`；非 `active` 状态调用 `parse()` 抛 `std::logic_error`，仅 `reset()` 回到新流并保留不超过 4 KiB 的缓冲容量。
- **Multipart 流式缓冲**：`multipart_parser` 复用 `http_io_buffer`，增长时只复制未消费字节。
- **Multipart 借用重试**：`feed()` 可接收最新分片的正文视图；分配失败后，该视图仍可用于原样重试。
- **Multipart 值赋值**：`multipart_part` 先准备全部元数据再提交；复制失败时保留两端原值，移动不分配。解码文本由对象独立持有，未改写字段与正文仍借用原始输入。

`http_response::body_segment_count()` 枚举表示的存储片段，不等同于待发送的字节数。长度为零的文件范围仍返回一个 file segment，保留路径、描述符与 identity，调用方必须完成适用的文件验证，不能因 `send_body() == false` 丢弃它；普通空字节正文没有 segment。`send_body()` 仍只表示方法/状态允许且正文长度非零时需要传送 payload。

- **响应资源退役**：`http_response` 与 `http_response_file_descriptor` 移动赋值先清空源并提交接管，再释放旧资源；保留释放回调复用源对象或重入赋值的新写入，各文件描述符只释放一次。
- **正文替换退役**：先发布完整的新正文表示，再释放旧正文资源；释放回调重入替换的结果保留。

`encode_http_content` 的两个 overload（单个 coding，或按声明顺序依次编码的 coding 列表）与 `http_content_encode_context::encode` 统一返回 `std::optional<std::string>`：有值表示完整编码成功，`std::nullopt` 只表示结果超过 `max_encoded_bytes_`。预算为零只允许零字节表示；多 coding 按声明顺序编码，每个中间表示都受同一预算约束。可复用上下文不可并发使用；成功、超预算或异常后上下文均可继续编码独立表示，不携带上一次的压缩历史；异常后内部状态直接丢弃，下次编码重新创建。`decode_http_content` 接受单个 coding 或 `Content-Encoding` 给出的 coding 列表，列表按逆序解码，每个中间表示都受同一解码上限约束；超过上限的 identity 表示在分配前即被拒绝；`http_content_decode_error` 只描述表示本身的数据错误（`unsupported_coding`、`invalid_content`、`decoded_size_exceeded`）。解码器工作内存同样受解码上限约束：`br` 声明的环形缓冲超出该上限可容纳的大小时（约为上限的三倍加 3 MiB 固定表空间）直接返回 `decoded_size_exceeded`，不按对端声明的窗口分配；`zstd` 解码器按 RFC 9659 接受最大 8 MiB 窗口，这是协议要求的下限。编码、解码与增量编码器在编解码器自身无法运行（状态无法创建或重置、库内部错误）时统一抛出 `http_content_codec_error`，分配异常保留原始类型。接收方用 `parse_http_content_coding`（单个字段值）、`parse_http_content_coding_headers`（字段行序列或 `http_response_headers`）或 `request_content_coding`（已解析的请求）读取 `Content-Encoding`。

- **编码器重置内存**：`http_content_encoder::reset()` 对 zlib/Zstd 复用工作内存；Brotli 无原生重置接口，重建状态时可能重新分配。
- **编码器重置失败**：保持失败态并拒绝后续写入；再次显式 `reset()` 成功后才可继续使用。

Web 缓冲响应用“小于原文”的预算尝试压缩，超预算是正常的“不划算”结果，仍回退 identity；请求拒绝 identity 时不设该预算，任何可接受的编码都优于 `406`。真实故障沿现有失败边界记录并翻译，不作为预算结果忽略。静态根预压缩同样只略过不划算的表示，真实编码异常使构建或刷新失败，不发布不完整的新快照。

---

### 4. HTTP/1.x、HTTP/2 与 HTTP/3 Sans-I/O 状态机

- **跨协议描述符存储**：协议解析与首部写出共用 `http_connection_fields.h`；预留或追加超过 32 项时才迁入堆存储。
- **响应长度语义**：HTTP/1、HTTP/2 与 HTTP/3 共用 `Content-Length` 选择；HTTP/1 仅叠加 chunked 与 close-delimited 分帧，保留 HEAD 元数据及 204、205、CONNECT 规则。
- **流式提交事实**：`http_response_stream_commit_plan` 从 `body_plan()` 派生头后阶段与 Trailer 状态资格，不保留第二份状态或状态码规则。
- **通告赋值提交**：`http_origin_advertisement` 与 `http_alternative_service_advertisement` 复制分配失败时保留两端原值；移动不分配，完成提交后才释放旧存储。
- **请求内容计划**：HTTP/2 与 HTTP/3 普通请求头视图共用 `"http_request.h"` 的 `http_request_content_plan`（`none()`、`known_length()`、`streaming()`），它同时决定生成的 `Content-Length` 与请求头是否结束流；两侧 `authority_` 均为 `std::optional<borrowed_text>`。
- **出站失败形态**：HTTP/2 与 HTTP/3 对同一类失败使用同一形态。调用方可能转发对端数据的情形（DATA 超出或未达声明长度、非法或超限的 Trailer、超出字段节上限的头）以 `invalid_message` 值返回且不改变状态；对已结束或未打开内容的流提交 DATA 或再次结束等调用顺序误用抛 `std::logic_error`；流已消失或被重置返回 `closed`。

#### 4.1 HTTP/1.0 & HTTP/1.1 状态机

- **服务端解析与序列化**：
  - `"http1_server_request.h"`：`http1_server_request_parse_state::parse_head(buffer, offset)` 解析请求行、首部、连接复用计划（`http1_request_connection_plan`）与请求体分帧计划（`http1_request_body_plan`），经左值访问器 `request()` / `body_plan()` / `connection_plan()` 只读借用（右值重载已删除），或以 `std::move(state).take_request()` / `take_body_plan()` / `take_connection_plan()` 提取（留下默认值，此后只可再次 `parse_head()` 或析构；取走请求即释放其描述符存储）。首行不包含 HTTP-version 词的明文输入（例如 TLS 记录、SSH banner）按非 HTTP 流量静默丢弃，不返回状态行；只有声明了版本号的非法请求行才回 400（RFC 9112 §2.2 的 400 为 SHOULD）。
  - `"http1_chunked_body.h"`：`http1_chunked_body_decoder::decode(available)` 增量解码 `chunked` 请求体与 Trailer。每个 chunk-size 行与 Trailer 区段各自受 `max_http_header_bytes`（64 KiB）约束；此外，同一消息所有 chunk-size 行中超出最短十六进制长度的字节（前导零与 chunk-ext）累计不得超过 `max_http_header_bytes`（RFC 9112 §7.1.1），超限与单行超限同样报告 `framing_limit_exceeded`（请求映射为 `413 chunked_framing_too_large`）。Trailer 中的 obs-fold 由 `http1_chunked_body_decoder_config::obs_fold_` 决定：默认 `reject`（请求，服务端可回 `400`），`accept` 时按展开后的字段校验并发布展开值（RFC 9112 §5.2）。完成结果的 `trailers()` 是 `std::span<const http_header>`：字段名小写、值去掉首尾 OWS，按 `trailer_message_`（默认 `request`）丢弃发送方不得生成的 Trailer 字段（RFC 9110 §6.5.1、RFC 9112 §7.1.2）；字段归解码器所有，有效至下一次 `decode()` 或解码器被移动、赋值、析构。
  - `"http1_server_response.h"`：提交最终响应头与流式响应头分帧计划。临时、最终、`CONNECT` 与 `CONNECT-UDP` 响应头违反消息规则或超出头部大小上限时，统一抛出 `http1_response_head_error`（派生自 `std::invalid_argument`），表示应用或运行时缺陷而非对端错误。
  - 响应分帧只由协议 writer 决定（RFC 9112 §6.1、RFC 9113 §8.2.2）：应用在最终响应上设置的 `Transfer-Encoding` 只在值（忽略 ASCII 大小写）恰为单个 `chunked` 时作为分帧提示接受，HTTP/1 与 HTTP/2 采用同一规则。公开响应字段 setter 仍要求规范化字段值，首尾 SP/HTAB 在设置时即以 `std::invalid_argument` 拒绝；writer 的 OWS 去除不放宽此入口契约。HTTP/1 用分帧计划自己的字段替换它（仅分块流写出规范的 `Transfer-Encoding: chunked`；缓冲、HTTP/1.0 关闭定界、HEAD 与 `204` / `205` / `304` 不写出），HTTP/2 直接省略该分帧提示。其他任何值（如 `gzip`、`gzip, chunked`、`identity`、空值或畸形列表）声明了 writer 不会施加的编码，两种协议都拒绝写出该响应：HTTP/1 的 `http1_commit_final_response`（修改响应前）、`append_http1_response_head` 与 HTTP/1 分帧的 `prepare_http_response_stream_head` 抛 `http1_response_head_error`，HTTP/2 的 `submit_response_head` / `submit_streaming_response_head` 返回 `invalid_message`，`http2_frames` 分帧的 `prepare_http_response_stream_head` 抛 `std::invalid_argument`。Web 层在响应头尚未提交时可将此异常转换为独立的 `500` 错误响应，不会把非法编码或应用正文写上线。`Content-Length` 按协议规范化；HTTP/2 仍拒绝 `Connection`、`Upgrade`、`Keep-Alive` 与 `Proxy-Connection`。
  - `"http1_server_response.h"`：响应头缓冲保留 inline scratch 的两阶段写入能力。`stack_cursor(bound)` 只检查当前剩余空间，不占用独占 reservation；对象须存活且地址不变。`commit_stack(end)` 将 inline 已提交长度设到本对象数组内的端点（允许 one-past 和缩短长度），允许抛出异常：已经 spill 到堆时抛 `std::logic_error`，空指针或其他对象的端点抛 `std::invalid_argument`，拒绝时保留已有内容。失败的 reserve 不会使仍处于 inline 状态的 cursor 失效；堆状态须先 `reset()` 再取得新的 inline cursor。
  - **响应头缓冲提交与退役**：复制分配失败时保留两端已提交内容；移动不分配，扩容时先提交两端状态再释放旧存储；`reset()` 在释放超限堆存储前提交空 inline 状态。
- **客户端写出与响应解析**：
  - `"http1_client_request.h"`：生成客户端请求头与流式分块请求体，产出 `http1_client_exchange_state`。`http1_client_request_wire_policy::target_form_`（`http1_request_target_form`，缺省 `origin`）选择非 CONNECT 请求行的目标形式：`origin` 写出 origin-form / asterisk-form；`absolute` 用于经正向代理的请求（RFC 9112 §3.2.2），以源站的 `http` / `https` scheme 与 authority（默认端口省略）前缀 origin-form 目标，生成的 `Host` 与之同源（§3.2），`OPTIONS *` 写为空路径的 `scheme://authority`（§3.2.4）。CONNECT 始终经 `prepare_connect()` 写 authority-form，对其设置 `absolute` 或使用未知枚举值返回 `invalid_target_form`。
  - `"http1_client_response.h"`：增量解析服务端 `1xx` 临时响应、最终响应头及定长/分块/EOF 定界响应体。客户端按接收方规则处理响应：首部与 Trailer 中的 obs-fold 均替换为空格（响应体解码器的 `trailers()->fields()` 是已校验、已展开、字段名小写且已丢弃响应 Trailer 禁用字段的 `std::span<const http_header>`，归解码器所有，有效至下一次 `decode()` / `finish_input()`）；`000..099` 与 `600..999` 状态码按 `500` 处理，`received_status()` 保留原值；HTTP/1.0 响应携带 `Transfer-Encoding` 时按分帧故障处理：忽略 transfer codings，回退到 `Content-Length` 或关闭定界，并在该响应后关闭连接（RFC 9112 §6.1，HEAD/204/304 同样关闭）；`1xx` 永不终止交换，其 `Connection` 选项不改变连接生命周期，最终响应才决定复用（RFC 9110 §15.2）；接收方 `Connection` 仅拒绝分帧/路由歧义字段（`Content-Length`、`Transfer-Encoding`，请求接收方另拒绝 `Host`），不将发送方禁止提名端到端字段的义务误作接收方拒绝规则（RFC 9110 §7.6.1）。发送方（客户端请求含 `forwarded` 来源、CONNECT-UDP 握手与服务端响应）仍严格禁止在 `Connection` 中点名协议管理、端到端或请求来源元数据字段（`Forwarded`、`X-Forwarded-*` 等）。
  - `"http_client_redirect.h"`：对响应头执行唯一字段查找，规划重定向方法与正文处置，分类同源 authority，并将 `http` / `https` URI-reference 解析为拥有型目标。
  - `http1_client_response_parser::parse` 在状态行的换行到达时即校验状态行（以裸 LF 结尾报告 `invalid_status_line`，其余错误与完整头部相同），之后任一以裸 LF 结尾的字段行立即报告 `invalid_header`（RFC 9112 §2.2），不必等到空行、64 KiB 上限或读超时，与服务端请求头的提前拒绝一致；非 HTTP 上游（如 SSH banner）因此在首行即失败。

`http1_request_content_writer::chunk` 与 `http_datagram_write_plan` 是对应写入器／会话产出的只读分段快照，不允许默认构造，也不暴露可写的前缀缓冲、长度及正文成员。保留具名计划，通过 `prefix()`、`payload()`（请求块另有 `suffix()`）组成 scatter-gather 写入。`prefix()` 借用计划自身的内联存储，只允许左值调用；计划必须存活且不变，直到传输已消费前缀。`payload()` 仍借用原始输入，其寿命独立于计划，调用方同样须保留输入直到消费完成。复制／移动计划保留独立的内联前缀与原有正文借用，不分配、不复制正文。

出站长度与范围尺寸共用 `"http_request.h"` 的 `http_content_length`：包括 `http1_client_request_head_view::content_length_`、`http_request_content_plan::known_length()` / `streaming()`（可选位置仍保留 `std::optional`），以及 `resolve_http_byte_range_set()`、`make_http_multipart_byte_range_plan()`、`http_selected_representation_metadata::length_`、`http_response::file_body()` / `multipart_file_body()` / `content_range()` / `content_range_unsatisfied()`。非负整数可直接填入，构造前检查 `0..UINT64_MAX`，负数抛出 `std::invalid_argument`，无异常探测用 `http_content_length::try_from_value()`。不隐式接受浮点或枚举，需要整数时用 `value()` 解包；不要预先转成无符号数。已解析或已计算的协议长度仍返回 `std::uint64_t` / `std::size_t`。

- **解析描述符增长**：服务端 `parsed_request_header_block` 与客户端 `http1_client_parsed_response_head` 按实际字段数增长，不按首部字节上限预留线程栈数组。
- **字段区段上限**：64 KiB 字节上限（`max_http_header_bytes`）与 256 行上限（`max_http_header_fields`，与 Pingora 解析器默认值相同）同时生效。
- **描述符放大防线**：仅凭字节上限，最短 4 字节的字段行可使一个头部携带约 16 K 个字段，其描述符远大于头部本身。
- **行数超额处理**：服务端请求头第 257 行返回 `431 header_too_large`；客户端响应头报告 `header_too_large`。
- **区段预算入口**：`http_field_section_budget` 对接收与生成的头部及 Trailer 执行同一字节与行数规则，覆盖 `http_request_trailers`、`make_parsed_http_request`、客户端请求写出、响应头与 Trailer 输出、WebSocket 握手。
- **输入拥有期**：服务端请求视图零拷贝借用输入缓冲区；客户端最终响应头拥有字段数据，原始输入可在解析完成后释放。
- **分配失败**：以 `std::bad_alloc` 传出，不发布 `head_ready` 或成功状态。
- **失败重试边界**：释放资源后可用同一缓冲区与偏移重试；部分元数据可能残留，不保证回滚。
- **请求与响应退役提交**：`http_request::reset()` 先恢复默认构造状态再释放首部描述符；移动赋值先整体接管新对象再释放旧存储，不分配内存。

请求转发使用 `"http1_client_request.h"` 的 `enum class http_request_field_source : std::uint8_t { generated, forwarded }`，不要把收到的字段当成新配置字段逐项送入 `is_valid_http_request_application_field`：

- HTTP/1 在 `http1_client_request_wire_policy::field_source_` 设置来源；`prepare`、`prepare_streaming`、两个 `prepare_connect` 重载（`http_origin_view` 隧道源，或 `example.test:443` 形式的 authority，经 `is_valid_http_connect_authority` 校验）和 `prepare_connect_udp` 均消费同一策略。HTTP/2 与 HTTP/3 的普通、CONNECT、extended CONNECT 请求头视图均提供 `field_source_`。缺省 `generated` 保持新生成字段的严格语义；配置生成或重写字段须先按生成规则校验，不能借 `forwarded` 绕过配置边界。
- `forwarded` 只切换值语义：`Content-Type`、`Origin`、`Access-Control-Request-Method`、`Access-Control-Request-Headers` 原样保留，由最终应用解释；`Content-Encoding` 和 HTTP/1 `Upgrade` 容忍合法列表空成员（包括空列表），不要求重建列表，非空成员仍须符合 coding token / upgrade protocol 语法。未知但合法的 content coding 不因本地不支持解码而被丢弃。依据 [RFC 9110 §2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.2)，generate 不包括 forward；[§5.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.1) 区分发送方新生成列表和接收方空成员容忍义务。OPTIONS 携带正文须有 Content-Type 是 §9.3.7 的生成要求，转发已接收请求时不补造该字段；TRACE 请求禁止正文。
- 来源不改变字段名/值字节与 CRLF 注入防护、字段节大小、方法/目标、Host/authority 一致性、正文分帧、Content-Length/Expect 所有权或 singleton 次数。即使接收端容忍重复 `User-Agent` 或条件日期，转发 writer 也不切换整套 recipient 规则，不额外放行这些重复字段。非法来源枚举拒绝，HTTP/1 返回 `invalid_header`，HTTP/2 与 HTTP/3 返回 `invalid_message`，不写出请求或占用新流。
- 代理仍负责按 [RFC 9110 §7.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1) 移除/重建逐跳字段。`Connection`、`TE`、`Trailer` 及协议专属限制不放宽；HTTP/1 `Upgrade` 仍须有正确 `Connection: Upgrade`，响应切换仍须匹配实际 offer。HTTP/2 继续禁止 `Connection`、`Upgrade` 等逐跳字段（RFC 9113 §8.2.2）；CONNECT-UDP 的 Capsule-Protocol 与禁止表示/分帧字段规则仍生效。新生成的专用 WebSocket 握手与 PUSH_PROMISE 仍按 `generated` 检查。


#### 4.2 HTTP/2 与 HPACK 状态机（`"http2_connection.h"`）

`ruvia::http2_connection` 实现了符合 RFC 9113 / RFC 7541 / RFC 8441 的完整客户端与服务端 Sans-I/O HTTP/2 连接状态机。每个连接内部持有一个 `nghttp2` 会话：分帧、HPACK、流状态、流量控制以及 `SETTINGS` / `PING` / `GOAWAY` 处理均由 `nghttp2` 完成，`ruvia-http` 负责 HTTP 消息语义（字段校验、内容长度、`CONNECT` / 隧道阶段）、请求租约、流控凭证与防滥用预算。`feed()` 一次接受整段输入，不完整的尾帧由 `nghttp2` 自行缓存，没有“需要更多输入”的结果。

```cpp
#include "http2_connection.h"

// 构造客户端或服务端状态机（enable_push_ 与 receive_origin_advertisements_ 仅限客户端，
// 传给 server() 时抛出 std::invalid_argument）：
auto conn = ruvia::http2_connection::server({});

// 1. 喂入从网络收到的原始字节：
auto feed_res = conn.feed(inbound_bytes);

// 2. 轮询消费协议事件：
while (auto ev = conn.next_event()) {
    if (auto* req_head = ev->request_head()) {
        std::uint32_t stream_id = req_head->stream_id();
        ruvia::http_response resp;
        resp.status(ruvia::http_status::no_content);
        // 无正文响应由 HEADERS 一次结束；有正文时保留请求租约并驱动下述 DATA 契约。
        (void)conn.submit_streaming_response_head(stream_id, std::move(resp));
        (void)conn.release(std::move(*req_head));
    }
}

// 3. 取出待发往网络的编码字节（按整帧边界分批，游标始终停在帧边界）：
std::string out;
while (conn.wants_write()) {
    out.clear();
    (void)conn.take_output_batch(16384, out);
    // 写出 out
}
```


`hpack_decoder::set_max_dynamic_table_size()` 的容量输入使用 `hpack_table_size`：窄化前检查 `0..UINT32_MAX`，负数或超范围抛出 `std::invalid_argument`（无异常探测用 `hpack_table_size::try_from_value()`）。不隐式接受浮点或枚举，需要整数时用 `value()` 解包；不要预先窄化输入。`hpack_encoder` 只提供 `encode_header()` 与 `encode_status()` 两个字段级入口，编码器使用容量为零的动态表，调用之间不保留状态：完全命中静态表的字段用索引表示，其余字段用不索引的字面量（`authorization`、`proxy-authorization`、`cookie`、`set-cookie` 用 never-indexed，RFC 7541 §7.1.3），较短时使用 Huffman 编码。
独立 `hpack_decoder` 基于 `nghttp2` 的 HPACK inflater。每次 `decode()` 处理一个完整字段块；回调只借用本次字段的名称和值，必须保持 decoder 的实现存活直到调用返回。同一实现处于字段块解码期间，再次 `decode()` 或调用 `set_max_dynamic_table_size()` 均抛出 `std::logic_error`，且不提交该嵌套操作；容量修改移到块之间，不同 decoder 仍可嵌套使用。这是库的状态独占契约，不是 RFC 的额外输入限制。回调返回 `false` 或抛出异常时停止后续回调，但仍解码完当前块以保持动态表同步，随后分别报告 `callback_rejected` 或重抛该异常。`hpack_decode_error` 只区分 `compression_error` 与 `callback_rejected`：任何格式错误或截断都是同一种解压失败（RFC 7541 §3.2 将其规定为连接错误），不再细分原因。解码器不回滚：压缩错误或分配失败后动态表已与对端失去同步，decoder 进入不可恢复状态，此后每次 `decode()` 都报告 `compression_error`，只能丢弃。块首的容量更新按线上顺序生效，出现在字段之后或超过所设上限的容量更新均为压缩错误。把上限降到当前动态表容量以下会立即驱逐条目，且下一个块必须以不大于此后所设最小上限的容量更新开头（RFC 7541 §4.2），否则为压缩错误；调高上限不要求容量更新。

HTTP/2 出站字段直接交给 `nghttp2` 的 HPACK deflater；动态表容量固定为零，普通字段不建立索引，凭据字段使用 never-indexed 表示，避免跨连接共享压缩状态。入站字段块由 `nghttp2` 解码：回调判定字段非法时以流错误重置该流，`nghttp2` 继续解码余下字段块以保持连接级动态表同步；HPACK 解码失败是 `COMPRESSION_ERROR` 连接错误。

- **出站首部暂存**：字段条目与 `nghttp2_nv` 描述符向量分别复用不超过 4 KiB 的容量，超限在提交后归还 PMR。
- **输出窗口 `take_output_batch()`**：部分批次只推进消费游标；在下一次追加或完整尾批存储交接时按摊销条件压缩前缀。
- **流控凭证 `http2_received_data_credit`**：`message_body_chunk()` 与 `tunnel_data()` 事件携带移动专属的流控凭证；当上层消费完缓冲数据后析构凭证（唯一归还途径），才把这部分字节计入 `nghttp2` 的接收窗口并按需发送连接与流级 `WINDOW_UPDATE`（关闭了 `nghttp2` 的自动窗口更新），实现精确背压。本端通告的 1 MiB 初始流窗口在对端确认本端 `SETTINGS` 后才生效，此前对端只能使用 RFC 默认的 65535 字节（RFC 9113 §6.9.2）；对端按协议先回 `SETTINGS` ACK 再发数据时不受影响。
- **凭证归属**：凭证携带签发连接，不能经其他连接归还；凭证保活该连接状态，可比 `http2_connection` 门面长寿，所属流已结束时归还为空操作；归还失败时延迟到后续 `feed()` / `next_event()` / 输出排空等调用重试。
- **Trailer 视图 `http2_message_end_event::trailers()`**：`std::span<const http_header>`，字段名小写；客户端已丢弃响应 Trailer 禁用字段，服务端交给 `http_request_trailers::append_received()`；视图借用事件自身存储，事件移动、销毁或 `take_trailers()` 后失效。
- **服务端请求租约 `http2_request_head_event`**：持有服务端流请求头的底层内存租约；请求处理完毕后调用 `conn.release(std::move(event))` 归还存储。
- **初始发送半边**：`http2_request_head_event::snapshot().initial_end_stream_` 保存初始 HEADERS 的 END_STREAM 位，包含 CONNECT；不随后续 DATA、Trailer 或复位改变。
- **请求视图借用**：`build_request(..., borrowed_text body, ...) const&` 同时借用 head 租约与正文；两者均须覆盖返回请求的使用期。临时拥有型正文与右值 head 在编译期拒绝。
- **Web 流资源**：运行时表统一确定元数据与入站正文 PMR；资源通过每流容器的 allocator 保留，不额外缓存指针。
- **Web 正文队列**：待消费缓冲大小是积压额度的唯一计数来源，已交付的活跃缓冲不重复计入。
- **DATA 部分消费 `submit_data(stream_id, bytes, end_stream, max_payload_bytes)`**：返回 `http2_data_submit_result{consumed_bytes_, status_}`，本次只编码 `min(bytes.size(), max_payload_bytes, 连接窗口, 流窗口)` 字节；帧头开销另计。`complete` 只表示这次输入全部消费，不表示 socket 已写完；`backpressured` 可以消费零字节或一个前缀。协议层既不借用也不复制未消费后缀，调用方必须保存后缀并按实际消费量推进，取得输出预算或收到 `WINDOW_UPDATE` / `SETTINGS` 后显式重试。窗口更新不会自动产生 DATA；调用方须显式重试提交。`expectation_pending` 和 `closed` 均消费零字节；前者等待 `100 Continue` 或显式 `release_request_content()`，后者停止发送。`pending_data_output_bytes()` 与输出批次 observer 仍用于已编码 DATA 和 socket 写完成记账；它们不能代替应用层输出预算。
- **Web DATA 驱动**：缓冲与文件响应共用写入状态；参数绑定直接返回底层冷任务，不另建状态转换协程。
- **Web 信用记账**：每份信用集中保存提交、序列化、写出和当前批次进度，复位时整体清零。
- **Web 信用回收**：流重置后仍保留在途 socket 批次的信用，批次完成后才回收。
- **长度与终结**：每次先验证完整输入及其终结意图是否符合 `Content-Length`，非法长度返回 `{0, invalid_message}`，不提交任何前缀。长度计数只推进 `consumed_bytes_`。`END_STREAM` 仅在终结输入全部消费后提交；结束后不能重复终结。
- **空 DATA 终结与规范边界**：[RFC 9113 §6.9.1](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.1) 允许（`MAY`）、但不要求在零窗口发送空 `END_STREAM DATA`。当前 `nghttp2` 后端等待连接与流的发送窗口都为正；这是实现限制，不是 RFC 要求。任一窗口不为正时，`submit_data()` 返回 `{0, backpressured}` 且不改变状态；调用方等待 `send_window_state()` 报告 `available_ != 0` 后重试。
- **Trailers 与 finish**：所有正文都消费完后才调用 `finish_request()` / `finish_response()` 提交 trailers，保证 terminal HEADERS 排在正文之后；finish 不排队等待未消费正文。不带 trailers 时 finish 以空 DATA 帧结束流，遵循同一窗口规则，窗口不足时返回 `http2_finish_status::backpressured` 且不改变状态，调用方等待窗口后重试。正文未达声明长度或 Trailer 非法时返回 `invalid_message`，同样不改变状态。`ruvia-web` 的服务端流式响应按此等待现有的发送窗口信号，等待时长受写超时或请求超时约束。
- **分配失败与异常**：`nghttp2` 内部或缓存其输出时的分配失败，以及 `nghttp2` 回调期间抛出的异常，都会使连接进入致命错误（尚能排队时发送 `GOAWAY(INTERNAL_ERROR)`），随后原异常（分配失败为 `std::bad_alloc`）向调用方传播；`nghttp2` 不提供事务回滚，因此不支持在资源恢复后重试同一操作。`take_output_batch()` 复制失败时输出游标与 observer 不变；释放失败的租约或凭证仍由后续操作重试。
- **对端帧元数据 `receive_frame_metadata_`**：开启后（客户端与服务端均可），连接把已应用的非 ACK `SETTINGS`、已应用的 `WINDOW_UPDATE` 及 `HEADERS` 优先级字段，分别以 `received_settings()`、`received_window_update()`、`received_priority()` 事件按线上顺序与其他事件一同发布；`HEADERS` 的优先级事件先于该帧产生的请求头事件，权重为 1..256。`received_settings` 以定长数组内联保存 `nghttp2` 解码后的条目（最多 33 项），不分配、也不借用 `feed()` 输入：重复的已知 ID 合并为一项（取最后的值、位于首次出现处），未知 ID 按线上顺序保留，同一帧先降后升 `HEADER_TABLE_SIZE` 时末尾另附其中的最小值。独立的 `PRIORITY` 帧（RFC 9113 §5.3.2 已弃用）由 `nghttp2` 读取后丢弃，不产生事件。关闭（默认）时不产生这些事件，也不做额外工作。请求伪首部的线上顺序始终由请求头事件的 `request_view().pseudo_header_order_` 提供。
- **优雅排空 `begin_drain()` / `finish_drain()` / `draining()` / `drain_final()`**：服务端按 RFC 9113 §6.8 两阶段排空——先以 `nghttp2` 的 shutdown notice 发出 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳在途的流；收到该 `PING` 的 ACK 或所有者调用 `finish_drain()` 后发出携带最后对端流 ID 的最终 `GOAWAY`。此后编号更大的新流被直接丢弃、不作应答（对端从 `GOAWAY` 得知其未被处理）。往返期间对端新开超过 128 条流时，超出的那条流收到 `RST_STREAM(REFUSED_STREAM)`，随即发出最终 `GOAWAY`（产品上限）。RFC 只要求至少一个往返时间，不要求等到 ACK：所有者以 `finish_drain()` 为往返等待设定时间上限；未在排空时调用则直接以最终边界进入排空（适用于对端已结束输入、无法再开流）。收到对端合法 `GOAWAY` 时同样立即进入最终阶段（其发送方不得再开流），包括已在往返中的排空。`drain_final()` 为真后，所有者在剩余流结束时即可关闭连接。客户端角色一步发出最终 `GOAWAY`；客户端发出 `GOAWAY` 后到达的 `PUSH_PROMISE` 被丢弃。
- **立即终止 `terminate(http2_error_code)`**：所有者主动关闭连接（RFC 9113 §6.8）：排队携带该错误码与已处理的最后对端流 ID 的 `GOAWAY`，丢弃此后的全部入站帧，`connection_error()` 报告该错误码（`no_error` 亦然），后续提交返回 `closed`；与检测到的连接错误走同一终止路径，也会结束进行中的优雅排空。连接已出错时为空操作，保留首个错误码。所有者写出输出后关闭传输。
- **存活探测 `submit_ping()`**：排队一个非 ACK `PING`，载荷与优雅排空的 `PING` 不同，两者的 ACK 互不影响。同一时刻最多一个探测未确认：在其 ACK 到达前再次调用返回 `accepted` 但不再排队新帧；ACK 不产生事件，任何收到的帧（含该 ACK）都可作为对端存活的证据。连接出错后返回 `closed`。
- **对端诱发输出的预算**：核心没有时钟（`nghttp2` 基于时钟的 reset 与 glitch 限速器因此被关闭），以两个计数器限制对端不读取输出时能诱发的输出，超限即发送 `GOAWAY(ENHANCE_YOUR_CALM)` 并进入连接错误。其一是输出未排空期间的控制帧数：`PING` ACK、`SETTINGS` ACK 与因对端流错误发出的 `RST_STREAM` 合计不超过 1000，输出完全排空（`take_output_batch()` 取空）后清零（同 Go `x/net/http2` 的 `maxQueuedControlFrames`）。其二是连接生命周期内因对端流错误发出的 `RST_STREAM` 总数，上限 1024，不会回补（同 h2 `max_local_error_reset_streams`）：否则对端可反复开流、诱发流错误（如不带 `END_STREAM` 的 trailer 区段），绕过只统计对端 `RST_STREAM` 的 rapid-reset 预算。所有者主动的 `submit_reset()` 不计入这两项。此外，超过 32 项的 `SETTINGS` 帧与单个字段块超过 1024 个 `CONTINUATION` 帧同样以 `GOAWAY(ENHANCE_YOUR_CALM)` 结束连接。
- **由 `nghttp2` 决定的错误类别**：以下情形按连接错误 `PROTOCOL_ERROR` 处理（RFC 9113 §5.4.1 允许把流错误升级为连接错误）：任意流上的零增量 `WINDOW_UPDATE`、长度不足以容纳填充或优先级字段的 `HEADERS`、依赖自身的请求 `HEADERS`、对端在本端 `SETTINGS` 已被确认后超出 `SETTINGS_MAX_CONCURRENT_STREAMS` 的新流（确认前超出的流收到 `RST_STREAM(REFUSED_STREAM)`）、服务端 `PRIORITY_UPDATE` 指向的空闲流超出并发上限，以及非法的客户端连接前言。`nghttp2` 不保留已关闭流的历史：已被其遗忘的流上再到达的帧被忽略；关联流已不存在时到达的 `PUSH_PROMISE` 被忽略，其承诺流以 `RST_STREAM(CANCEL)` 取消。
- **服务端 `CONNECT` 乐观数据**：接受/拒绝前收到的 `DATA` 以 `tunnel_data()` 事件发布，其流控凭证由所有者持有到接受隧道后消费；`submit_connect_response_head()` 接受后再发布 `tunnel_end()`（若请求半边已结束）。非 2xx 拒绝后到达的 `DATA` 丢弃并归还流控额度。RFC 8441 `:protocol = websocket` 的提前 `DATA` 同样保留（RFC 6455 §4.1 的等待规则只约束客户端）。非 2xx 拒绝响应后服务端以 `RST_STREAM(NO_ERROR)` 结束流。已建立隧道上的未知扩展帧被丢弃（RFC 9113 §5.5），`ALTSVC`/`ORIGIN` 帧也被忽略（RFC 7838 §4、RFC 8336 §2.1）。已建立隧道上的 `PUSH_PROMISE` 先完成 HPACK 解码，再依次以 `RST_STREAM(PROTOCOL_ERROR)` 重置关联流、以 `RST_STREAM(CANCEL)` 取消承诺流。
- **客户端 `REFUSED_STREAM`**：客户端在收到任何响应头（含 `1xx` 临时响应与最终响应头）前收到 `RST_STREAM(REFUSED_STREAM)` 时发布 `request_unprocessed()` 而不是 `stream_closed()`（RFC 9113 §8.7：请求未被处理，可安全重试），连接保持可用。
- **Server Push**：`submit_push_request()` 在已打开的服务端推送流（预留状态不计入，RFC 9113 §5.1.2）达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 时返回 `http2_push_submit_error::stream_limit`，不发送 `PUSH_PROMISE`（RFC 9113 §8.4）。客户端在承诺的推送响应超出本端已确认的并发上限时按连接错误处理（同上）。客户端在 `PUSH_PROMISE` 字段块尚未结束时重置其关联流，会一并取消该承诺：承诺流以 `RST_STREAM(CANCEL)` 重置，不发布推送事件。
- **`submit_reset()`**：`nghttp2` 已遗忘的流返回 `closed`。两个方向的 `END_STREAM` 都已关闭流后，只接受针对畸形消息的 `PROTOCOL_ERROR`，且只在本地退役该流、不发送 `RST_STREAM`（RFC 9113 §5.1 禁止在已关闭流上发送帧）。
- **`submit_priority_update()`**：仅客户端可用；服务端未通告 `SETTINGS_NO_RFC7540_PRIORITIES=1`（RFC 9218 §2.1）时返回 `peer_capability_unavailable`，不发送帧。
- **`ORIGIN` / `ALTSVC` 通告**：作为 `nghttp2` 扩展帧发送，载荷上限为对端 `SETTINGS_MAX_FRAME_SIZE` 与 16384 字节（`nghttp2` 扩展帧缓冲）中的较小者，超出返回 `invalid_message`。
- **`SETTINGS_ENABLE_CONNECT_PROTOCOL`**：服务端收到 0 或 1 不产生任何影响（RFC 8441 §3）；任一角色收到 0、1 以外的取值都是 `PROTOCOL_ERROR` 连接错误。客户端仍校验服务端发送的 1→0 回退。

##### 4.2.1 帧与字段观察

`"http2_connection.h"` 提供同步、只读的协议解析事实，与 `receive_frame_metadata_` 的已应用事件独立。

| `http2_connection_options` 字段 | 契约 |
| --- | --- |
| `frame_observer_` | `void(void*, const http2_frame_metadata&) noexcept`；帧完整到达后、最后一段载荷送入协议处理前交付 |
| `header_observer_` | `void(void*, const http2_header_metadata&) noexcept`；按 HPACK 解码器输出次序交付字段节 |
| `observer_context_` | 回调上下文，借用至连接销毁；两个观察器共用 |

| 记录 | 可观察事实 |
| --- | --- |
| `http2_frame_metadata` | `observation_sequence_`、载荷长度、流 ID、帧类型与 `nghttp2` 规范化后的 flags；原始 SETTINGS 次序（含未知、重复 ID），WINDOW_UPDATE 增量，独立 PRIORITY 或 HEADERS 的依赖/独占/权重 |
| `settings()` / `settings_complete_` | 借用内联条目；超过既有 32 项帧预算或载荷不按六字节对齐时标记不完整，不改变协议拒绝规则 |
| `http2_header_metadata` | `begin / field / end` 阶段、流 ID、字段下标、首帧与当前帧观察序号、字段名和值；保留伪首部与普通字段的实际输出次序，CONTINUATION 不另起字段节 |

- **序号范围**：从前言后的 0 开始，只计算 `nghttp2` 暴露的帧头，包含 ACK、未知扩展与 CONTINUATION；预先丢弃的帧（如已重置流上的 DATA）不计数，不是绝对线上帧号。
- **验证边界**：观察不表示协议接受；字段交付先于 HTTP 语义校验，无效字段节可能没有 `end` 或后续字段。flags 不保留解析器剔除的保留位。
- **借用与重入**：回调仅在 `feed()` 内同步执行，记录及字段视图仅在回调期间有效；返回前不得修改、重入、移动或销毁该连接。
- **关闭成本**：空观察器不保留输入、不增加分配或分帧工作；开启时仍使用同一 `nghttp2` 分帧与 HPACK 主链，不另建解析器。


#### 4.3 HTTP/3、QPACK 与 QUIC（`"http3_connection.h"` / `"http3_quic_connection.h"`）

`http3_connection` 用 `nghttp3` 封装请求流、控制流与 QPACK。`read_stream()` 消费输入，`peek_output()` / `commit_output()` / `acknowledge_output()` 管理借用输出与确认；正文块和动态 ORIGIN 帧在确认前保持地址稳定。RFC 9412 通告通过 `origins_` / `submit_origin_advertisement()` 提交，RFC 9218 优先级通过客户端 `submit_priority_update()`、服务端响应 `Priority` 字段与 `stream_priority()` 访问。

- **公开边界**：`http3_connection.h` 仅公开 Sans-I/O HTTP/3 与 QPACK 编解码；`http3_quic_connection.h` 公开依赖 `ngtcp2` 的 QUIC 包、路径、计时与 UDP 数据报契约。只使用编解码层的调用方不再引入 `ngtcp2` 类型。

独立传输驱动可直接使用 `http3_connection`，不必采用 `http3_quic_connection`：

| 传输反馈 | 契约 |
| --- | --- |
| `block_stream(id)` / `unblock_stream(id)` | QUIC 流信用耗尽／恢复时暂停／恢复发送调度；不提交字节，也不恢复应用正文源 |
| `output_flushed(id)` | 当前已提交输出是否全部交给传输；不代表网络写完成或对端确认，也不禁止继续提交正文，已移除的流返回 `true` |
| `shutdown_stream_read(id, error)` | 对端 RESET_STREAM 终止本地接收半边；不关闭发送半边；`error` 为 `std::optional<http3_error_code>` |
| `shutdown_stream_write(id, error)` | 对端 STOP_SENDING 终止本地发送半边；不关闭接收半边 |
| `close_stream(id, receive_error, send_error)` | QUIC 流最终退役，附带终止两个半边的 `http3_error_code`（如有） |
| `set_max_client_streams_bidi(limit)` | 服务端接收 QUIC 单调递增的累计双向流上限，不是增量或并发流数 |
| `http3_transport_action_event::origin()` | `local` 是须执行的传输命令（`http3_quic_connection` 发布前已执行）；`peer` 是已应用的对端通知，禁止回显 |

- **终止收尾**：`shutdown_stream_*` 的 `std::nullopt` 表示不发布通知的本地终止；两者均不发送 QUIC 帧。最终退役仍由驱动调用 `close_stream()`。

| 结果类型 | 入口 | 取值 |
| --- | --- | --- |
| `http3_transport_status` / `http3_transport_result` | `read_stream()`、`read_datagram()`、`peek_output()`、`commit_output()`、`acknowledge_output()`、`block_stream()`、`unblock_stream()`、`shutdown_stream_*()`、`close_stream()`、`bind_control_streams()` | `accepted`、`would_block`、`events_pending`、`closed`、`invalid_state`、`protocol_error`；结果另带 `error_code_`（`protocol_error` 及失败后的 `closed`）与 `credit_bytes_`（`read_stream()` 中核心自行消费、立即归还接收信用的分帧与 QPACK 字节） |
| `http3_submit_status` | 请求头、临时响应、CONNECT 响应、ORIGIN、PRIORITY_UPDATE、`begin_drain()` / `finish_drain()`、`submit_http3_websocket_client_request()` | `accepted`、`closed`、`invalid_state`、`invalid_message`、`peer_settings_pending`、`peer_capability_unavailable`、`backpressured`（仅 ORIGIN：控制流输出预算已满） |
| `http3_data_submit_result` | `submit_data()` | `consumed_bytes_` 为已复制前缀；`complete` 全部复制，`backpressured` 只复制了前缀（可为 0），`closed`，`invalid_message`（超出声明长度，未复制） |
| `http3_finish_status` | `finish_stream()`、`finish_request()`、`finish_response()` | `accepted`、`closed`、`invalid_message`（正文未达声明长度或 Trailer 非法，流保持打开） |

- **错误码**：`http3_error_code` 覆盖 RFC 9114 §8.1 全部 H3 码、RFC 9204 §6 QPACK 码与 RFC 9297 的 `datagram_error`，底层为 `std::uint64_t`；收到的未知或保留码（`0x1f * N + 0x21`）原值保留，按 RFC 9114 §9 视同 `H3_NO_ERROR`。所有 reset、close、事件与 `connection_error()` 都收发该强类型，公开契约不暴露 nghttp3 宏或库错误码。

- **请求构造**：客户端 `submit_request_head()` 分别接受普通、CONNECT 与 extended CONNECT 视图；核心校验目标、字段、期望与对端能力，统一生成伪首部及 Content-Length，凭据始终 never-index。字段节预算按每个名称、值加 32 字节计算，受本地与已知对端限制共同约束。公开 API 不提供原始字段节提交。
- **请求头结果**：角色、流号或重复提交返回 `invalid_state`，消息无法以 HTTP/3 表示返回 `invalid_message`；extended CONNECT 在对端 SETTINGS（或 0-RTT 记忆值）到达前返回 `peer_settings_pending`，对端未启用时返回 `peer_capability_unavailable`。
- **WebSocket 客户端**：`submit_http3_websocket_client_request()` 按 RFC 9220 提交 extended CONNECT，与 HTTP/2 的 `submit_http2_websocket_client_request()` 共用同一字段合成，应用首部均以 `borrowed_http_headers` 传入；两者的最终头都由 `validate_extended_connect_websocket_client_response()` 校验。
- **响应事件**：客户端收到 `http3_informational_head_event` 与 `http3_response_head_event`，与 HTTP/2 同形：`head()` / `take_head()` 交出 `http_client_response_head`，`request_content_signal()` 对带 `Expect: 100-continue` 的请求在首个 100 给出 `continue_value`、在仍等待 100 时到达的最终头给出 `exchange_complete`，`stream_ended()` 表示最终 HEADERS 已结束流。600–999 按 5xx 处理，101 拒绝；保持官方 `nghttp3`，其在应用回调前拒绝 000–099，这是当前实现例外。
- **正文与 trailer**：核心校验 Content-Length 的实际字节数和终结；对端发送半边正常结束时发布 `http3_message_end_event`（隧道接收方向结束同样如此），`trailers()` 以 `std::span<const http_header>` 借用事件自身存储，无 Trailer 时为空，事件移动、销毁或 `take_trailers()` 后失效。客户端收到的 Trailer 已验证并丢弃仅违反发送方 trailer 限制的字段；服务端收到的请求 Trailer 交 `http_request_trailers::append_received()` 吸收。客户端以 `finish_request(stream_id, trailers)` 结束请求内容。驱动不再解析响应语法。
- **未处理请求**：客户端对 GOAWAY 标识及以上仍开启的请求流（在 `http3_goaway_event` 之后，按流号升序），以及最终响应头前被 `H3_REQUEST_REJECTED` 重置或停止发送的流，发布 `http3_request_unprocessed_event`（RFC 9114 §4.1.1、§5.2），可在其他连接重试；核心为仍开启的方向排队本地 `H3_REQUEST_CANCELLED` 传输动作，此后不再有事件（含 `http3_stream_close_event`）提及该流。`http3_goaway_event::id()` 对不限定任何流的 shutdown notice 返回 `std::nullopt`。
- **响应提交与预算**：服务端只通过受检响应头、临时响应和握手入口提交；`available_output_bytes(stream_id)` 是 DATA 剩余容量的唯一权威，终结或关闭流返回零。最终、临时、WebSocket 与 CONNECT 响应共用字段生成规则；临时响应超出字段节上限返回 `invalid_message`，`submit_connect_response_head()` 与 HTTP/2 一样返回提交状态。
- **流式响应方法**：`submit_streaming_response_head()` 使用该流收到的 `:method`，与 HTTP/2 同形，不由调用方重复传入。

- **流控凭证 `http3_received_data_credit`**：`http3_data_event::take_credit()` 交出与 `http2_received_data_credit` 同形的只移动凭证；同流凭证可 `merge()`，析构时把精确字节数交给 `received_data_credit_sink_`，消费方持有到读完正文为止。凭证比连接长寿时归还为空操作。
- **凭证去向**：`http3_quic_connection` 安装自己的回收器并拒绝调用方设置，归还的字节扩展 QUIC 流与连接接收窗口（RFC 9000 §4.1）；独立驱动通过选项提供回收器，未提供时归还被丢弃。
- **延迟消费**：`http3_deferred_consume_event` 是 nghttp3 自行消费的分帧与 QPACK 字节以及核心丢弃的 DATA，无人持有，按 `read_stream()` 的 `credit_bytes_` 同样立即归还，不转为凭证；`http3_quic_connection` 内部处理该事件。
- **留存输入借用**：`read_stream()` 在编译期拒绝直接传入临时拥有型连续容器与 `std::initializer_list`；命名输入及显式借用视图仍可使用。借用模式下，底层存储须覆盖对应 DATA 事件的消费；显式构造 view/span 不延长 owner 寿命。
- **DATA 事件**：只由核心构造；移动赋值先归还目标未取走的凭证，保留目标资源，复制拥有正文分配失败时双方不变。
- **事件形态**：`next_event()` 返回只可移动的 `http3_event`，与 `http2_event` 同形，由 `request_head()`、`informational_head()`、`response_head()`、`data()`、`message_end()`、`transport_action()`、`deferred_consume()`、`stream_close()`、`request_unprocessed()`、`goaway()`、`datagram()`、`origin_advertisement()` 中恰好一个返回非空指针；各事件只由核心构造，以访问器读取，拥有型内容（请求头、Trailer、数据报载荷）经右值 `take_*()` 交出。
- **出站地址稳定**：名称拥有缓冲首次使用时按整节字节上界预留，后续增长不重分配；字段视图只借用至 `nghttp3` 完成提交。
- **出站字段计数**：按实际发送数量预留描述符，扣除被过滤的分帧字段并计入生成字段。
- **请求头校验**：服务端在连接内完成请求头校验（nghttp3 的帧层规则加上目标语法、`:authority`/`Host` 一致、接收方字段规则、Expect 与 Cookie 合并），通过者以 `http3_request_head_event` 发布、`take_head()` 交出 `http3_request_head`；畸形请求由核心按 RFC 9114 §4.1.2 以本地 STOP_SENDING 与 RESET_STREAM（`H3_MESSAGE_ERROR`）回应，不发布请求头、DATA 或消息结束事件，驱动无需自行重置。
- **请求投影 `http3_request_head`**：普通字段索引首次使用时按剩余字段上界一次预留；仅含伪首部与 Cookie 的请求不分配该索引。
- **投影所有权**：`http3_request_head` 仅可移动构造、不可复制或赋值；`std::optional` 持有点使用 `emplace` 重建。
- **Cookie 惰性合并**：首个非空碎片直接借用请求投影已拥有的字段；第二个非空碎片出现时才分配合并缓冲，空碎片不参与。

##### 4.3.1 帧与字段观察

与 HTTP/2 的 [4.2.1](#421-帧与字段观察) 对齐，`http3_connection_options` 提供同步、只读的接收事实：

| 字段 | 契约 |
| --- | --- |
| `frame_observer_` | `void(void*, const http3_frame_metadata&) noexcept`；请求流与对端控制流上每个帧头读完、载荷处理前交付：`observation_sequence_`（连接内从 0 递增）、流 ID、`http3_frame_type`（未知与保留类型原值保留）、载荷长度 |
| `field_observer_` | `void(void*, const http3_field_metadata&) noexcept`；QPACK 解码输出的 `begin / field / end`、头部或 Trailer 段、字段下标与名称值，先于 HTTP 校验；被阻塞的字段节在动态表引用解出后交付 |
| `observer_context_` | 回调上下文，借用至连接销毁；两个观察器共用 |

- **验证边界**：观察不表示协议接受；QPACK 编解码流与其他单向流类型不分帧，不观察。
- **借用与重入**：回调仅在 `read_stream()` 内同步执行，记录与字段视图仅在回调期间有效；返回前不得修改、重入、移动或销毁该连接。
- **关闭成本**：空观察器不保留输入、不分配；开启帧观察时每条接收流只保存一个定长帧头扫描状态。

`http3_quic_connection` 用 `ngtcp2` 承担 QUIC v1 包处理、丢包恢复、流量控制、流 ID 与定时到期，并把流数据桥接到 `http3_connection`。ngtcp2 的解密回调缓冲在回调返回后失效，因此该包装器强制启用 `own_data_events_`，在返回 `receive_datagram()` 前把 DATA 复制到事件自有存储；直接使用 `http3_connection` 时可保留默认借用模式。调用方经 `crypto_callbacks_`（`http3_quic_crypto_callbacks`，仅含 QUIC-TLS 适配器回调）与 `owner_callbacks_`（`http3_quic_owner_callbacks`：连接 ID 签发与移除、握手完成与确认、无状态重置、早期数据被拒，均收到 `callback_context_`）提供回调，流数据、确认与关闭回调由包装器独占、无法传入；另提供 TLS native handle 和 UDP I/O：`receive_datagram()` 输入一个 UDP 数据报，`next_datagram()` 产出一个拥有字节与目标路径的数据报，`expiry()` / `handle_expiry()` 驱动最近到期事件。TLS 使用 `ngtcp2_crypto_ossl` 时适配层属于 `ruvia::web` 的 OpenSSL 依赖；`ruvia::http` 本身只链接 ngtcp2 core 与 nghttp3。`native_handle()` 只供适配器的 `ngtcp2_crypto_conn_ref::get_conn` 使用。`http3_quic_result` 以 `transport_error_code_`（RFC 9000 §20.1）或 `application_error_code_`（`http3_error_code`）给出本端发送或从对端收到的 CONNECTION_CLOSE 的错误码（空闲超时与无状态重置两者皆空），`transport_error()` / `application_error()` 查询同一事实。

nghttp3 在帧层识别的格式错误（伪首部缺失或非法、连接专属字段等）由 `read_stream()` 报告为连接错误 `H3_MESSAGE_ERROR`，这是 RFC 9114 §8 允许的“把流错误升级为连接错误”。

`ruvia::web` 的 HTTP/3 服务（`listen_config::http3_`）在这两层之上实现：
- **端点与连接 ID 分流**：POSIX 上按 worker 下标顺序 `0..N-1` 以 `SO_REUSEADDR` + `SO_REUSEPORT` 绑定同一 UDP 端口；服务器签发的 20 字节连接 ID 将所属 worker 下标经后 16 字节随机数与监听器密钥派生的掩码异或隐藏（防外部关联同一连接的不同 CID，RFC 9000 §9.5）并附加 16 位认证标签；Linux 多 worker 下挂载无特权 `SO_ATTACH_REUSEPORT_CBPF` 在内核态直接校验标签并把携带服务端 CID 的长短包头数据报（含迁移后路径与 Retry/Handshake/0-RTT）直投所属 worker（客户端自选 Initial 与未认证包按密钥哈希分流），非 Linux 平台或落到其他 worker 的数据报经有界目录（每 worker 至多 256 个在途，满则丢弃）转交；接收循环每次唤醒后最多连读 32 个数据报，按 `ngtcp2_pkt_decode_version_cid` 解析目标连接 ID：命中本 worker 连接表即交给该连接；合法的客户端 Initial（`ngtcp2_accept`）在启用 `http3_retry_` 时先执行无状态 Retry 地址验证（伪造或过期 Token 回复 `INVALID_TOKEN`），验证通过或未启用 Retry 时在本 worker 新建连接并受 `max_connections_per_worker_` 约束（满额时先淘汰最久空闲连接），不支持的版本回复 Version Negotiation（仅对不小于 1200 字节的数据报）；命中本 worker 已退役服务端连接 ID 的短包头数据报按速率限制回复严格小于触发包长的无状态重置（`write_http3_quic_stateless_reset`），其余未知数据报直接丢弃。Windows 由应用级 Acceptor 线程以 `SO_EXCLUSIVEADDRUSE` 独占 UDP socket，按连接 ID（新连接 Initial 按客户端 ID 的密钥哈希）经同一目录转交业务 worker，业务 worker 的出站数据报经每 worker 至多 256 个在途的有界队列交回 Acceptor 发送。
- **连接会话**：每个连接在所属 worker 上持有 OpenSSL `SSL`（监听器的 QUIC TLS 上下文，TLS 1.3、ALPN `h3`）、`ngtcp2_crypto_ossl_ctx` 与随 `expiry()` 重新设定的 worker 定时器；写协程按 `next_datagram()` 返回的路径发送，发送缓冲满时等待可写，受写停滞期限约束。HTTP/3 控制流在应用密钥就绪后启动，恢复的 0-RTT 可提前启动。每个请求流复用 HTTP/1、HTTP/2 的准入、分派与响应尾处理链；缓冲、文件与流式响应受每流未确认输出预算约束；流式请求体与隧道字节持有 DATA 凭证，被读取后才析构归还额度。
- **生命周期**：连接计入 worker 的连接计数，由连接扫描器按 HTTP/2 相同的阶段计时；达到 `max_requests_per_connection_` 或连接超时时依次发送 shutdown-notice GOAWAY、`min(1 s, 3×PTO)` 后的最终 GOAWAY，所有已接纳流关闭后以 `H3_NO_ERROR` 发送 CONNECTION_CLOSE，并在 3×PTO 的关闭期内吸收迟到数据报；`stop()`（直接断连，不设排空阶段）、淘汰或超时宽限期满时立即以 `H3_NO_ERROR` 关闭。
- **扩展与恢复**：支持 CONNECT、RFC 9220 WebSocket、RFC 9297/9298 HTTP Datagram 与 CONNECT-UDP、RFC 9412 `ORIGIN` 帧、RFC 9218 `PRIORITY_UPDATE` 帧、QUIC Retry 与无状态重置；Web 层的 0-RTT 准入和路径迁移策略见[监听器文档](web-guide.md#ruvialisten_config-与-ruviatls_config)。不提供 HTTP/3 服务端推送（nghttp3 不支持）与仅定义于 HTTP/2 的 `ALTSVC` 帧。

独立 QUIC 驱动调用 `start_early_http3()` 时必须提供经认证保存的传输参数及对端 SETTINGS，并同时启用 TLS 早期数据。TLS 拒绝早期数据时调用 `reject_early_data()` 丢弃早期流，是否重发由调用方裁决；`early_data_rejected()` 报告适配器或调用方已登记的拒绝。`encode_early_transport_parameters(output)` 编码 0-RTT 依赖的传输参数（RFC 9000 §7.4.1）：客户端取收到的服务端参数，与 TLS 会话票据一同保存、供 `start_early_http3()` 恢复；服务端取本端参数，供票据恢复检查绑定；客户端尚未收到或缓冲不足时返回 `std::nullopt`。`http3_request_head::early_data()` 保留实际传输来源，不能由请求字段代替。TLS 恢复并接受早期数据，也不保证某个请求属于 0-RTT：发送节拍或重传可能使请求在握手完成后才到达，应用应读取该请求的来源标记，而非连接的 TLS 协商结果。

`migrate(path, timestamp)` 仅用于客户端，先验证新路径；调用方必须按输出数据报的 `path_` 发送，并在验证期间保持新旧路径接收。`current_path()` 以平台 sockaddr 字节给出当前发送路径（`http3_quic_path::local_` / `remote_`），借用连接存储至下一次输入、输出或迁移调用。`submit_datagram(stream_id, payload)` 的载荷位于 Quarter Stream ID 之后；`max_datagram_payload_size()` 给出当前上限，队列满返回 `would_block`，不支持或超长返回 `invalid_state`。

HTTP/2 与 HTTP/3 分别由 `RUVIA_ENABLE_HTTP2`、`RUVIA_ENABLE_HTTP3` 控制。关闭某项时不解析其源码、不安装其连接头文件，也不引入对应的 nghttp2 或 nghttp3/ngtcp2 依赖；两个开关互不要求对方启用。
---

### 5. WebSocket 与扩展隧道协议层

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"websocket_handshake.h"` / `"http2_connection.h"` / `"http3_connection.h"` | `validate_websocket_handshake`、`make_websocket_server_handshake`、`struct websocket_server_handshake_options`、`class websocket_client_negotiation final`、`prepare_http1_websocket_client_request`、`validate_http1_websocket_client_response`、`submit_http2_websocket_client_request`、`submit_http3_websocket_client_request`、`validate_extended_connect_websocket_client_response`、`enum class websocket_client_handshake_error` | WebSocket 服务端与客户端在 HTTP/1.1 Upgrade 及 HTTP/2、HTTP/3 Extended CONNECT 下的握手校验、子协议选择与 `permessage-deflate` 扩展参数协商；各 HTTP 版本共用同一服务端握手选项与同一客户端握手错误枚举。`websocket_client_negotiation::validate_fields` 只校验给定字段中服务端的子协议与扩展选择（不检查状态码与连接专用字段） |
| `"websocket_connection.h"` | `class websocket_connection final`、`struct websocket_frame_options final`、`class websocket_borrowed_payload final`、`enum class websocket_feed_status`、`struct websocket_event`、`enum class websocket_event_kind`、`struct websocket_deflater_config`、`validate_websocket_deflater_config`、`enum class websocket_termination` | Sans-I/O WebSocket 帧状态机与协议类型：支持增量输入 `feed`（默认构造的连接拥有自己的输入缓冲区，`max_buffered_input_bytes_` 限制等待 `next_event` 的字节，超限时返回 `websocket_feed_status::backpressured` 且不消费输入，连接不再活动时返回 `inactive`），也可从调用方的 `http_io_buffer` 读取输入（追加收到的字节后调用 `next_event`）；事件轮询 `next_event`（`kind()` 返回 `websocket_event_kind`）覆盖消息重组、Ping/Pong、Close 与协议错误，帧与关闭提交 `submit_frame` / `submit_close`（`submit_frame` 传入非空的 `websocket_deflater_config` 即按此对单帧启用 `permessage-deflate` 压缩，传入 `std::nullopt` 则该帧不压缩且不更新压缩字典），`permessage-deflate` 压缩等级与 `memLevel` 由 `websocket_deflater_config` 统一表示并校验；关闭握手完成（本端已发并收到对端关闭帧，客户端还要刷空写缓冲；`websocket_liveness_mode::awaiting_peer_eof` 在写缓冲刷空后进入等待对端先关闭 TCP）后，`termination()` 返回首个结束原因（`websocket_termination` 枚举，主动 `abort(cause)` 记录超时、I/O 失败等运行期原因），`bytes_received()` / `bytes_sent()` 统计完整帧线面字节，取值与语义见 [WebSocket API](streaming-websocket.md#32-ruviawebsocket-与-ruviawebsocket_message-api) |
| `"http_connect_udp.h"` | `encode_http_connect_udp_path`、`parse_http_connect_udp_path`、`validate_http_connect_udp_request`、`class http_capsule_decoder final`、`class http_datagram_session final` | RFC 9298 `CONNECT-UDP` 请求/目标校验与 RFC 9297 HTTP Capsule / Datagram 帧增量编解码 |

- **WebSocket 出站缓冲**：复用 `http_io_buffer`；部分写出只推进游标，追加优先使用尾部空间。
- **WebSocket 帧提交**：为帧头、掩码与需复制的负载统一预留空间后才发布输出；分配失败保留已有队列。
- **WebSocket 接收限额**：`websocket_connection_options::receive_message_limit_` 只收紧接收方向，与 `message_limit_` 取较小上限；分片累计及解压后消息同样受限，不降低发送上限。

`websocket_connection::submit_close()` 接受 `websocket_close_code`；`from_value()` / `try_from_value()` 在整数窄化前校验，发送与接收的状态码判定共用该值类型。`websocket_close_status::{normal_closure, going_away, internal_error}` 提供现有生命周期使用的常量，其余支持值由工厂构造；范围、保留码和 Web 关闭配置的用法见[WebSocket 关闭 API](streaming-websocket.md#32-ruviawebsocket-与-ruviawebsocket_message-api)。服务端主动发送客户端专属 `1010` 仍返回 `invalid_code`，UTF-8 原因与控制帧大小均按协议校验。

- **CONNECT-UDP 目标构造**：使用 `http_connect_udp_target{std::string(host), port}`，端口参数经 `port_number` 检查；该类型不再支持指定成员初始化，`host_`、`port_` 保持公开。
