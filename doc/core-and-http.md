# `ruvia-core` 运行时与 `ruvia-http` 协议层 API 手册

本文档提供 `ruvia::core`（独立协程与事件循环运行时底座）和 `ruvia::http`（独立 Sans-I/O HTTP/1.0、HTTP/1.1、HTTP/2、HTTP/3、HPACK、QPACK、QUIC、WebSocket 与内容编解码协议层）的公开头文件与核心 API 参考。两库均可脱离 `ruvia::web` 独立使用。所有公开符号位于 `namespace ruvia`（状态码常量位于 `namespace ruvia::http_status`）。

---

## 第一部分：`ruvia::core` 运行时 API

### 1. 协程、任务域与 Asio 桥接

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/task.h"` | `template <typename T> class task final` | 结构化惰性协程（仅支持移动构造与右值 `co_await`）。未启动（cold）或已完成（done）可安全析构；**销毁已启动但仍挂起的 `task` 直接触发 `std::terminate()`** |
| `"ruvia/core/root_task.h"` | `template <typename T> class root_task final` | 由 `event_loop::start(task<T>)` 返回的跨线程等待句柄。提供 `valid()`、`wait()`、`get()`；析构时若未调用 `get()` 则转为后台托管直至完成，**禁止在所属 `event_loop` 自身线程调用 `wait()` / `get()`** |
| `"ruvia/core/task_scope.h"` | `class task_scope final`、`task_scope::join_operation`、`struct task_scope_options` | 单 Worker 亲和的结构化并发任务域。提供 `spawn(task<void>)`、`request_stop()`、`token()`、`stop_requested()`、`size()`、`empty()`、`join()`；构造期建立停止状态，`token() const&` 只读且不分配，第一个子任务失败时作用域自动请求停止；`join()` 返回可丢弃、仅启动前可移动的单次冷 `join_operation`，创建和启动须在绑定 Worker，启动后独占该作用域并关闭 `spawn`，并重新抛出第一个子任务失败；之后与其不等价（同一异常对象，或动态类型与 `what()` 均相同）的子任务失败在该子任务完成时交给 `report_unhandled_failure`，不被静默丢弃；析构前若曾 `spawn` 过任务，必须完成 `co_await scope.join()` |
| `"ruvia/core/scoped_operation.h"` | `class operation_scope final`、`class scoped_capability_registration final`、`make_scoped_operation(...)` | 将借用型能力（capability）与惰性协程操作 `task<T>` 绑定到同一 Worker 作用域，`close_and_join()` 先回收冷协程帧并等待已启动操作完成后再失效 capability；`bind_worker(worker)` 借用该 Worker 句柄后，作用域内每个操作的启动、完成与冷帧回收都必须在该 Worker 上，否则 `std::terminate()` |
| `"ruvia/core/async.h"` | `template <typename T> asio::awaitable<T> as_awaitable(task<T> task_value)`、`template <typename Result = void> class asio_completion final`、`template <typename Result = void, typename Initiate> auto async_asio(Initiate&& initiate)` | 将 `ruvia::task<T>` 适配为 `asio::awaitable<T>`，或将基于完成回调的 Asio 异步启动操作适配为 Ruvia 协程可直接 `co_await` 的 awaiter（返回 `asio_completion<Result>`，提供 `error_code()`、`succeeded()`、`failed()`、`result()` / `take_result()`） |

---

### 2. 停止令牌、超时、截止期与定时器

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/stop_token.h"` | `class stop_source final`、`class stop_token final`、`class stop_registration final`、`class stop_group final` | `stop_token` 只观察一个状态；`stop_group` 显式拥有父源注册和下游取消状态。`token.register_callback(fn)` 返回自有注册；`token.register_callback(registration, fn)` 替换调用方持有的注册。每个注册只嵌入一个侵入式节点，不额外分配节点；析构自动注销，并等待其他线程正在执行的回调结束 |
| `"ruvia/core/operation_options.h"` / `"ruvia/core/operation_timeout.h"` | `struct operation_options final`、`validate_operation_options`、`override_operation_options`、`class operation_timeout final`、`template <typename Kind = std::monostate> class operation_deadline final` | 通用操作选项（`timeout_` 与 `stop_token_`）、跨异步阶段共享的 `steady_clock` 截止时间计算器与零分配三态（inactive / active / expired）截止期状态机 |
| `"ruvia/core/worker_timer.h"` | `class worker_timer_registration final`、`class worker_timer_cancellation final`、`enum class worker_timer_outcome { expired, cancelled }` | 单 Worker 侵入式定时器注册句柄，配合 `worker_handle::schedule_timer` 使用；提供 `cancel()`（静默取消）与 `cancel_and_notify()` |
| `"ruvia/core/timer.h"` | `enum class timer_sleep_result { elapsed, stop_requested }`、`class sleep_operation final`、`sleep_for(const worker_handle&, duration, [stop_token])`、`sleep_until(const worker_handle&, deadline, [stop_token])` | 在绑定 Worker 上挂起当前协程指定时长或直至截止时间点；返回可丢弃、仅启动前可移动的单次冷 `sleep_operation`，须在绑定 Worker 上 `co_await` 启动。若传入的 `stop_token` 触发或 Worker 停止定时器，返回 `stop_requested` 而不抛异常 |

`stop_group group{first.token(), second.token(), ...}` 或 `stop_group group{tokens_span}` 可组合任意数量的父状态，也可输入另一个 group 的 `token()`。group 不可复制或移动，必须保持地址稳定并存活到使用其 token 的所有操作结束 / join；token 的复制不会延长 group 的父源注册寿命。0 / 1 个不同有效源直接透传，不创建新状态；真实多源组合创建一个下游状态，前两个父注册内嵌，更多父注册按实际数量分配。重复源与嵌套源都只触发每个下游回调一次，不存在用户源容量被框架内部源占用的问题。

`request_stop()` 支持跨线程；已停止状态的注册同步执行回调。回调可注销自身，也可销毁正在转发的 group；group 先注销父注册再退役自身取消源。回调在请求取消的线程执行，不可直接修改其他 Worker 所有的状态，应向 owner Worker 投递通知。源先销毁不会使已有 token 悬空；`has_state()` 区分空 token 与源已退役的 token，`stoppable()` 表示状态是否仍可能取消或已经取消。

所有客户端 handle 的 `with_options` 使用相同规则：未指定 token 继承，提供非空 token 则替换原用户取消状态，不隐式累积；超时始终取已有值与新值中较紧的上限。需要累计时由调用方持有 `stop_group` 并传入其 token。客户端关闭通过自身 backend 的关闭、等待唤醒和 join 主链独立生效，不因 token 替换而失效，也不为每次派生 handle 建立共享取消树。Redis 独立客户端未提供 token 时沿用客户端生命周期 token，满足阻塞命令 / 订阅需要明确取消边界的契约；显式 token 替换后仍受客户端关闭约束。数据库借用外部 Redis 的缓存操作由缓存 owner 的独立停止源中止，不关闭外部 Redis 客户端。

---

### 3. 事件循环、Worker 运行时与阻塞卸载池

```cpp
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/timer.h"

ruvia::event_loop_pool pool({.loop_count_ = 2, .queue_capacity_ = 1024});
pool.start();

ruvia::event_loop loop = pool.next_loop();
auto root = loop.start([](ruvia::worker_handle worker) -> ruvia::task<int> {
    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
    co_return 42;
}(loop.handle()));

int answer = root.get();
pool.join();
```

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/event_loop.h"` | `class event_loop final`、`class event_loop_stop_registration final`、`class event_loop_attachment final`、`attach_event_loop(asio::io_context&, options = {})` | 指向单个 Worker 运行时及其 `asio::io_context` 的轻量句柄（提供 `start(task<T>)`、`post(fn)`、`defer_cleanup(fn)`、`on_stop(fn)`、`handle()`、`io_context()`、`executor()`、`valid()`、`accepting()`、`is_current()`、`id()`、`report_failure(e)`；`post` 与状态查询转发到 `handle()` 返回的 `worker_handle`），以及将 Ruvia Worker 挂载到外部自管 `asio::io_context` 的适配器（`loop()`、`run()`、`stop()`、`valid()`） |
| `"ruvia/core/event_loop_pool.h"` | `class event_loop_pool final`、`struct event_loop_pool_options` | 管理固定数量独立事件循环线程的池。提供 `start()`、`stop()`、`join()`、`loop_count()`、`loop(index)`、`next_loop()`、`loop_for(key)` |
| `"ruvia/core/worker_handle.h"` | `class worker_handle final`、`using worker_id_type = std::uint64_t`、`enum class post_status`、`template <typename Sig> class post_outcome final` | 跨线程安全的稳定派发端点与有界队列投递结果。提供 `valid()`、`accepting()`、`is_current()`、`id()`、`post(fn)`、`post_factory(factory)`、`schedule_timer(...)` |
| `"ruvia/core/worker_runtime.h"` | `class worker_runtime final`、`struct worker_runtime_options`、`struct worker_runtime_hooks`、`enum class worker_runtime_state` | 底层单 Worker 线程与两阶段启停状态机（`startup -> ready -> serve -> stop_admission -> finalize -> drain -> shutdown -> join`） |
| `"ruvia/core/worker_group.h"` | `class worker_group final` | 协调多个 `worker_runtime` 同步完成两阶段启动屏障（`start()` / `serve()`）与停机回收（`request_stop()` / `join()`） |
| `"ruvia/core/blocking_pool.h"` | `class blocking_pool final`、`template <typename T> class blocking_result final`、`run_blocking(worker_handle, [operation_options], fn)` | 固定大小阻塞卸载线程池，由调用方拥有：通过 `event_loop_pool_options`、`attach_event_loop` 的选项或 `worker_runtime_options` 的 `blocking_pool_` 交给事件循环借用，须比事件循环存活更久；core 不持有隐式进程级池。`run_blocking` 在绑定 Worker 上 `co_await`，将 `fn` 提交至该 Worker 借用的池并在完成后回到原 Worker 恢复协程；Worker 没有池时返回 `blocking_status::unavailable`，不提交 `fn`（`worker_handle::has_blocking_pool()` / `event_loop::has_blocking_pool()` 可预先查询）。`operation_options` 的 `timeout_` / `stop_token_` 只限定等待，非正超时或已停止的令牌同样不提交 `fn`。调用或交付值的异常保存在结果中并由 `value()`（取出值时使用 `std::move(result).value()`）重新抛出。超时、取消或 Worker 停机先释放等待者；已开始的调用继续完成，但只持有自身数据，迟到结果被丢弃 |

`blocking_result<T>` 是不可复制、不可重新赋值的完成快照，支持 payload 所允许的移动构造。保存位置需要替换时使用新局部变量或 `std::optional::emplace()`。移动失败结果后，源与目标仍保留相同异常；移动成功结果后，源 payload 遵循自身移动语义，状态分类不变。`completed()`、`failed()` 与 `value()` 不会因结果替换或错误信息被移走而出现“成功但无值”的状态；任务与结果传输仍允许抛异常，不要求 payload 的移动为 `noexcept`。

---

### 4. 跨线程通道、单次交付、信号与内存池

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/worker_wait_result.h"` | `enum class worker_wait_status { value, closed, worker_stopping, timed_out, cancelled }`、`template <typename T> class worker_wait_result final` | Worker 等待操作的统一返回包装（提供 `status()`、`has_value()`、`value()`、`take_value()`） |
| `"ruvia/core/channel.h"` | `make_channel<T>(worker_handle, channel_options)`、`class channel_sender<T>`、`class channel_receiver<T>`、`channel_receiver<T>::receive_operation` | 有界 MPSC 通道：`channel_sender` 可拷贝并支持多生产者跨线程 `send(value)`；`channel_receiver` 绑定单个目标 Worker，`receive([stop_token])` / `receive_for(duration, [stop_token])` 返回可丢弃、仅启动前可移动的单次冷 `receive_operation`，须在绑定 Worker 上 `co_await` 启动。容量限制排队值与正在构造的发送预留；预分配一个额外交付槽，若所有槽均暂被接收或销毁操作占用，发送可返回 `full`。并发发送按发布顺序交付；构造期间关闭通道则返回被拒绝的值。`send()` 调用期间须保持调用者对象存活，包括值的移动和析构 |
| `"ruvia/core/one_shot.h"` | `make_one_shot<T>(worker_handle, one_shot_options = {})`、`class one_shot_completion<T>`、`class one_shot_receiver<T>`、`one_shot_receiver<T>::wait_operation` | 跨线程单次值交付原语：`complete(value)` 可在任意线程调用，`wait([stop_token])` / `wait_for(duration, [stop_token])` 返回可丢弃、仅启动前可移动的单次冷 `wait_operation`，须在绑定 Worker 上 `co_await` 启动。另一交付正在构造值时返回 `in_progress` 和被拒绝的值；构造失败后可重试，构造期间接收端关闭或 Worker 停机则拒绝交付。已交付值在接收端移动失败时异常传给接收端且值已消费。调用期间须保持 completion 句柄存活，包括值的移动和析构 |
| `"ruvia/core/worker_signal.h"` | `class worker_signal final`、`worker_signal::wait_operation` | 同 Worker 内部闩锁合并通知；`wait()` 返回可丢弃、仅启动前可移动的单次冷等待器，不是 `task<void>`。创建和启动须在所属 Worker，启动后必须等待完成；支持多个等待者，直接挂入调用方协程帧 |
| `"ruvia/core/worker_notification.h"` | `class worker_notification final` | 跨线程闩锁合并通知原语（任意线程无锁 `notify()`，所属 Worker 上 `co_await notification.wait()`） |
| `"ruvia/core/pool_lease_scheduler.h"` | `class pool_lease_scheduler final`、`class pool_lease_acquire final` | 单 Worker 连接/槽位池租约调度器（空闲时同步返回槽位下标，满载时挂起排队并支持超时与取消） |
| `"ruvia/core/memory/memory_pool.h"` | `class worker_memory final`、`class request_memory final`、`struct memory_pool_config` | Worker 级可回收内存池与单次请求单调 Arena（支持 `allocator<T>()`、`resource()`、`fork()`、`trim(owner)`） |
| `"ruvia/core/memory/pmr_object.h"` | `std::pmr::memory_resource* process_resource() noexcept`、`pmr_resource_or_default(resource)`、`construct_pmr_object`、`destroy_pmr_object`、`make_pmr_object` | 进程级线程安全 mimalloc PMR 适配资源、回退辅助与 PMR 对象构造/销毁工具 |
| `"ruvia/core/move_only_function.h"` | `template <typename Sig> class move_only_function` | 带 `3 * sizeof(void*)` 小对象内联优化（SBO）的仅移动可调用包装器；移动赋值先提取源对象，再销毁旧目标，支持旧目标拥有源包装器 |
| `"ruvia/core/local_ring_queue.h"` | `class local_ring_queue<T>` | 预分配容量的单线程零分配环形队列 |
| `"ruvia/core/fnv1a.h"` | `fnv1a_offset_basis`、`fnv1a_prime`、`fnv1a_accumulate`、`fnv1a_accumulate_integer` | 零分配、可编译期求值的 64 位 FNV-1a 非密码学散列；整数按小端字节混入，跨平台结果一致 |
| `"ruvia/core/bytes.h"` / `"ruvia/core/failure_report.h"` / `"ruvia/core/socket.h"` | `as_bytes`、`as_chars`、`error_code_text`、`system_error_text`、`parse_ip_address`、`close_socket` | 字节/字符零拷贝视图转换（拒绝绑定临时拥有型字符串或连续字节容器，调用方须保持底层存储存活到消费完成）、跨平台 UTF-8 系统错误文本转换、严格 IPv4/IPv6 字面量解析（返回 `std::optional<asio::ip::address>`）与取消未完成 I/O 后关闭 TCP socket |

`move_only_function<R(Args...)>` 的引用结果必须由目标直接返回兼容引用，允许增加 cv 限定及派生到基类转换；返回值或需要产生临时量的结果不能绑定到引用签名。不同工具链采用相同约束，不依赖编译警告阻止悬垂。需要自定义引用转换（包括按值返回 `std::reference_wrapper`）时，用显式 `-> T&` / `-> const T&` 的适配 lambda 返回实际存活对象；包装器不延长该对象的寿命。值返回签名仍支持正常隐式转换。

`worker_wait_result<T>` 的状态分类在构造后固定；当 `T` 支持时可复制或移动构造，但不可重新赋值。替换保存的结果时使用新局部变量或 `std::optional` 的 `emplace()`，不要给结果或其 `optional` 直接赋值。`value()` 仍可修改 payload，移动构造和 `take_value()` 不改变源结果的状态分类，payload 遵循自身移动语义；其构造或提取抛异常不会让已有结果失去可查询的状态。通道与单次交付仍支持可抛异常的 payload，不要求其移动为 `noexcept`。

`pool_lease_scheduler` 的空闲槽位仍同步获取；`release(slot)` 向排队者提交槽位归属后，通过后续 Worker 轮次恢复该协程，不在释放调用栈内执行下一位消费者，避免连续立即释放形成递归链。`close()` 同样先向全部排队者提交 `closed` 结果，再通过后续 Worker 轮次恢复它们，调用方的关闭序列在任何等待者观察到关闭前完成。提交后的取消或 `close()` 不撤销已交接槽位；尚未交接的等待者按原取消、超时和关闭规则完成。不得依赖 `release()` 或 `close()` 返回代表排队协程已经完成，销毁 scheduler 前仍须等待已启动的 acquire 完成。

框架默认内存由 mimalloc 承载：`process_resource()` 是存活至进程结束的线程安全 PMR 适配器，不叠加进程级共享 pool；默认 Worker 内存、停止状态与协程帧使用该资源。Worker 提供小对象 pool、大块缓存、协程帧缓存及 `trim(owner)` 内存修剪入口。接受显式 PMR 的 API 始终优先使用调用方资源，调用方仍须保证资源覆盖所分配对象的生命周期。

`worker_memory` 构造中的分配失败以异常传给调用方，并将本次构造已经取得的存储全部归还上游；不因内部 pool 初始化失败终止进程。调用方可在资源恢复后重新构造，不能使用构造失败的对象。

非 Windows 构建启用 mimalloc 普通 `override`，在最终链接中保留静态库，对标准 `malloc` / `free` 与全局 `new` / `delete` 使用 mimalloc；不得叠加另一套全局 allocator replacement。Windows 保持原静态 CRT 与普通 mimalloc 依赖，仅通过显式 API 承载框架资源和协程帧，不替换宿主标准分配接口。两种接入都不替换 `std::pmr::get_default_resource()` 对象，也不改变显式 PMR 的优先级。独立 `ruvia::http` 不依赖 core 或 mimalloc，未显式指定 PMR 时仍遵循调用方的标准 PMR 默认资源。

---

## 第二部分：`ruvia::http` Sans-I/O 协议层 API

`ruvia::http` 不执行任何 socket I/O、定时器或线程调度，全部协议状态机均以内存字节流（`std::string_view` / `std::span` / `http_io_buffer`）与强类型事件/计划对象交互。

### 1. 基础类型、缓冲与编码工具

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/borrowed_text.h"` | `class borrowed_text final`、`template <typename element_type> class borrowed_span final` | 只读字符串与连续元素序列借用视图，拒绝直接绑定临时拥有型字符串、临时连续容器（`std::vector`、`std::pmr::vector`、`std::array`）及 `std::initializer_list`，但不延长底层 owner 的生命周期；调用方仍须保持命名存储有效直到消费入口完成读取或拥有化 |
| `"ruvia/http/port_number.h"` | `class port_number final` | 原始整数宽度受检的端口值；在格式化或原生 API 边界用 `value()` 显式解包 |
| `"ruvia/http/protocol_byte_limit.h"` | `class protocol_byte_limit final` | 显式有限／无限字节预算：默认和 `unlimited()` 不限量，`limited(n)` 接受原始整数，在窄化前检查严格正数且可表示为 `std::size_t`，否则抛出 `std::invalid_argument`；浮点和枚举不隐式接受，不要预先窄化输入；`SIZE_MAX` 仍是有限预算，不是无限哨兵 |
| `"ruvia/http/http_io_buffer.h"` | `class http_io_buffer final` | PMR 流式读写缓冲区：`prepare(n)` / `prepare_spare()`、`commit(n)`、`append(bytes)`、`data()` / `view()`、`consume(n)`、`clear_retaining_small()`；返回借用的四个入口只接受左值，缓冲区存储须覆盖异步 I/O 与视图消费。`prepare_spare()` 优先返回尾部空间，仅尾部耗尽且已消费前缀覆盖存活后缀时压缩；可返回空 span，调用方需要更多空间时使用 `prepare(n)`，并按实际 span 长度读取 |
| `"ruvia/http/http_protocol_version.h"` / `"ruvia/http/http_known_method.h"` | `http_protocol_version`（`http10`, `http11`, `http2`）、`http_scheme`（`http`, `https`）、`http_known_method`、`classify_http_method`、`is_valid_http_method_token` | 协议版本、Scheme 与 HTTP 方法分类及幂等/安全性判断 |
| `"ruvia/http/http_status.h"` | `class http_status_code final`、`namespace ruvia::http_status`、`http_reason_phrase` | 强类型 HTTP 状态码（`100..599`）与标准状态码常量（`http_status::ok`、`not_found` 等） |
| `"ruvia/http/http_protocol_error.h"` | `class http_protocol_error final : public std::exception`、`enum class http_parse_error` | 零动态分配的协议错误异常（`http_protocol_error(status, code, message)`：携带 `http_status_code`、静态存储期的稳定 snake_case 机器码 `code()`（如 `missing_host`、`conflicting_content_length`）与定长诊断文本）与解析错误枚举 |
| `"ruvia/http/http_request_target.h"` | `http_authority_view`、`http_origin_view`、`parse_http_authority`、`is_valid_http_origin_form_target`、`is_valid_http_connect_authority`、`is_valid_http_serialized_origin` | RFC 9110 / RFC 3986 请求目标、Authority 与 Origin 语法校验和解析 |
| `"ruvia/http/http_target_origin.h"` | `decide_http_target_origin`、`http_target_origin`、`http_target_origin_connection`、`http_target_origin_action` | RFC 9110 §7.4 目标源判定：服务器提供连接事实（是否 TLS、对端是否可信网关），返回接受、需按证书校验的主机（DNS 名去掉末尾点 / IPv4 / IPv6 网络字节序）或 `421` 拒绝；明文连接上的 `https` 目标直接判为 misdirected，零分配 |
| `"ruvia/http/url_encoding.h"` / `"ruvia/http/http_hex.h"` / `"ruvia/http/http_base64.h"` | `decode_url_component`、`validate_url_encoding`、`visit_url_encoded_pairs`、`percent_encode_uri_reference`、`uri_paths_equivalent`、`compare_uri_paths`、`http_encode_lower_hex`、`http_base64_encode`、`http_base64_decode` | URL 百分号/表单编解码、RFC 3986 URI-reference 规范化编码（`decode_url_component` 与 `percent_encode_uri_reference` 无需变换时返回输入视图本身、不分配，否则把结果写入调用方提供的 `std::pmr::string` 并返回其视图；解码只在百分号编码非法时返回 `nullopt`）、路由使用的路径规范形式比较（字面 `/` 分段、段内按百分号解码后的字节比较，`%2F` 不作分隔符）、十六进制转换、标准与 URL-safe Base64 编解码 |

`decode_url_component()` 与 `percent_encode_uri_reference()` 的文本输入使用 `borrowed_text`：字符串字面量、`std::string_view` 和命名的 `std::string` / `std::pmr::string` 可直接传入，临时拥有型字符串（含 `const` 右值）在编译期拒绝。返回值可能借用原始输入，也可能借用输出 `storage`，两者的 owner 均须覆盖结果消费；不要根据某次内容是否需要转换来推断寿命。需要传入生成的字符串时先保存为命名 owner，需要独立结果时在 owner 仍有效期间复制视图；显式构造 `string_view` 不会延长 owner 寿命。支持零拷贝和输入／输出别名。

`http_status_code::from_value()`、`try_from_value()` 与 `is_valid_value()` 接受原始整数类型并在窄化前检查 `100..599`；超宽或负数不能通过截断变成有效状态码。浮点、枚举及其他依赖隐式转换的调用须先由调用方完成明确的整数转换和相应校验；不要预先窄化到 `uint16_t`，否则会丢失原始边界信息。

端口输入统一使用 `port_number`：包括 `http_origin_options::port_`、`http_connect_udp_target_view::port_`、`http_authority_view::effective_port()` / `http_authorities_equal()` 的默认端口，以及 Web 的监听、数据库和 Redis 配置；可选端口仍保留 `std::optional`。合法整数可直接填入，窄化前按原始宽度检查 `0..65535`，负数或超范围值抛出 `std::invalid_argument`；需要无异常探测时用 `port_number::try_from_value()`。浮点与枚举不隐式接受，也不提供到原始整数的隐式转换；不要在调用前窄化。零端口与缺省端口不同，各入口按自身契约解释零端口：Authority 比较的默认端口 `0` 仍表示未知 scheme 默认值，省略／空端口不能因此等于显式 `:0`。已解析的协议端口和系统观察结果仍可返回 `std::uint16_t`；默认值回填后的裸数值不能独自保留这些语义差异，Authority 等价判断使用 `http_authorities_equal()`。

---

### 2. HTTP 首部、缓存、条件请求、范围与 Cookie

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_header.h"` / `"ruvia/http/http_field_syntax.h"` / `"ruvia/http/http_protocol_managed_fields.h"` | `class http_header final`、`class http_header_view final`、`class borrowed_http_headers final`、`is_valid_http_header_name`、`is_valid_http_header_value`、`is_http_ows_char`、`is_http_protocol_managed_request_field`、`is_valid_http_request_application_field`、`is_valid_http_response_field` | PMR 拥有型首部键值对、非拥有型首部视图与首部视图序列、RFC 9110 字段语法校验、分帧/连接专用请求字段判定及按请求/响应方向区分的发送前单字段校验 |
| `"ruvia/http/http_accept_match.h"` / `"ruvia/http/http_accept_encoding.h"` | `class http_accept_match final`、`http_response_coding_selection_for(request)` | `Accept` 媒体类型/语言权重匹配与 `Accept-Encoding` 响应压缩算法协商；`http_response_coding_selection_for` 是 HTTP/1、HTTP/2 与子请求共用的唯一协商入口（HTTP/1 解析器不预折叠 `Accept-Encoding`）；单个媒体类型或媒体范围最多 64 个非空参数（含 `q`），超出即视为非法（`Content-Type` 校验失败、该 `Accept` 成员被忽略） |
| `"ruvia/http/http_date.h"` / `"ruvia/http/http_cache.h"` | `format_http_date`、`parse_http_date`、`class cache_control final`、`parse_cache_control`、`class cache_control_field_parser final` | IMF-fixdate 日期格式化/解析与 `Cache-Control` 指令解析（`parse_cache_control` 解析单个字段值；重复字段行逐行 `update`，`finish` 取得结果） |
| `"ruvia/http/http_conditional_request.h"` / `"ruvia/http/http_byte_range.h"` / `"ruvia/http/http_representation_response_plan.h"` | `get_http_etag_preconditions`、`resolve_http_byte_range_set`、`plan_http_representation_response` | 条件请求（`If-Match` / `If-None-Match` / `If-Modified-Since` / `If-Unmodified-Since` / `If-Range`）与 `Range` 字节范围响应计划计算 |
| `"ruvia/http/cookies.h"` / `"ruvia/http/http_set_cookie.h"` | `struct cookie_options`、`parse_set_cookie`、`class set_cookie_plan final` | Cookie 属性校验（含 `__Secure-` / `__Host-` 前缀规则）、`Set-Cookie` 解析与序列化计划；`http_set_cookie_view::has` 只报告 `secure` / `partitioned` / `http_only`，Path 是否生效以 `path()` 非空判断，`same_site_none()` 等价于 `same_site() == cookie_same_site::none` |
| `"ruvia/http/http_priority.h"` | `class http_priority_urgency`、`struct http_priority`、`struct http_priority_fields`、`parse_http_priority`、`encode_http_priority` | RFC 9218 可扩展 HTTP 优先级首部与 HTTP/2 `PRIORITY_UPDATE` 帧编解码 |

`http_priority::urgency_` 与 `http_priority_fields::urgency_` 共用 `http_priority_urgency`，后者仍以 `std::optional` 区分缺省参数。按 [RFC 9218 §4.1](https://www.rfc-editor.org/rfc/rfc9218.html#section-4.1)，值域为 `0..7`、默认值为 `3`；本地整数输入在窄化前检查，非法值抛出 `std::invalid_argument`，无异常探测用 `try_from_value()`。不隐式接受浮点或枚举，也不隐式转换回整数；需要整数时用 `urgency_.value()`（可选字段用 `urgency_->value()`），不要预先窄化。接收端仍按 RFC 9218 §4 忽略越界或类型不符的参数，不把本地构造失败规则套用到对端字段。

当两个 `http_header` 均绑定 PMR 时，赋值保留目标的资源对象。移动赋值在资源等价时直接转移存储、不分配；`is_equal()` 只允许互相回收存储，不表示两个资源对象具有相同寿命，因此不会改为借用源资源。拷贝和移动构造仍沿用源对象的资源。

`is_valid_http_request_application_field(name, value)` 在应用配置或请求构造边界校验单个出站请求首部，返回 `bool`、`noexcept` 且不分配内存：检查字段名 token、规范化字段值（包括拒绝首尾 SP/HTAB 及非法控制字节），按大小写不敏感匹配拒绝 `Host`、`Content-Length`、`Transfer-Encoding`、`Trailer`、`Expect`、`Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Upgrade`，并复用请求 writer 的已知字段发送方语义校验（如 `Content-Type`、`Content-Encoding`、`Origin` 与 CORS 请求字段）。合法未知扩展和其空值仍允许；不引入应用配置政策。HTTP/1 与 HTTP/2 的 `TE` 差异已被托管字段边界排除，因此无版本参数。此接口不保留状态，字段间重复、完整消息约束仍由调用方及最终 writer 负责；不得用于响应首部或接收方校验，接收方仍按各协议入口的 recipient 规则处理。

`is_valid_http_response_field(name, value)` 声明在 `http_field_syntax.h`，同样返回 `bool`、`noexcept` 且不分配内存；检查通用名称/规范化字段值语法并复用响应发送方的 `Content-Type`、`Content-Encoding`、`Trailer` 语义校验。它不应用请求专属的 `Origin`、CORS、`Expect` 规则或请求托管字段黑名单，不用于接收方校验，也不替代响应写出入口的版本相关连接/分帧检查（如 `Connection`、`Upgrade`、`TE`）、重复字段检查及完整消息约束。应用禁改首部政策由调用方单独保留；此谓词返回 `true` 不表示字段在任意响应上下文中都可以发送。

---

### 3. 消息模型、内容压缩编解码、Multipart 与 SSE

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_request.h"` / `"ruvia/http/http_request_trailers.h"` | `class http_request final`、`make_parsed_http_request`、`class http_request_trailers final` | 请求头与请求体零拷贝视图描述符、大小写不敏感首部/Cookie 查找与请求 Trailer 校验收集（Trailer 字段名无论来自 `append()`、HTTP/1 `append_http1()` 还是 HTTP/2，均以小写保存） |
| `"ruvia/http/http_response.h"` / `"ruvia/http/http_interim_response.h"` | `class http_response final`、`class http_response_headers final`、`class http_interim_response_head final`、`plan_buffered_http_response_write` | 最终响应（`200..599`）与临时响应（`1xx`）模型，以及针对 `HEAD` / `204` / `205` / `304` 的响应体写计划。`head_metadata_body()` 将正文替换为描述 HEAD 所选表示的空正文；`has_head_metadata_body()` 查询该状态。显式 `Content-Length` 保留为 GET 表示的长度，缺失时不推断为零；重新设置正文会清除标记 |
| `"ruvia/http/http_content_coding.h"` / `"ruvia/http/http_content_codec.h"` / `"ruvia/http/http_content_encoder.h"` | `enum class http_content_coding { identity, gzip, deflate, brotli, zstd }`、`parse_http_content_coding_headers`、`request_content_coding`、`encode_http_content`、`decode_http_content`、`class http_content_encode_context final`、`class http_content_codec_error final`、`class http_content_encoder final` | 接收方 `Content-Encoding` 解析，单次缓冲、可复用缓冲上下文与流式增量的 `gzip` / `deflate` / `br` / `zstd` 内容压缩，以及带解压上限保护的解码器 |
| `"ruvia/http/multipart_parser.h"` | `parse_multipart_boundary`、`parse_multipart_body`、`class multipart_parser final` | 一次性与流式增量（`feed` / `poll`）的 `multipart/form-data` 解析器 |
| `"ruvia/http/sse.h"` | `struct sse_message final`、`format_sse_message` | Server-Sent Events 消息格式化与校验 |

- **Multipart 流式缓冲**：`multipart_parser` 复用 `http_io_buffer`，增长时只复制未消费字节。
- **Multipart 借用重试**：`feed()` 可接收最新分片的正文视图；分配失败后，该视图仍可用于原样重试。

`http_response::body_segment_count()` 枚举表示的存储片段，不等同于待发送的字节数。长度为零的文件范围仍返回一个 file segment，保留路径、句柄与 identity，调用方必须完成适用的文件验证，不能因 `send_body() == false` 丢弃它；普通空字节正文没有 segment。`send_body()` 仍只表示方法/状态允许且正文长度非零时需要传送 payload。

`encode_http_content` 的两个 overload（单个 coding，或按声明顺序依次编码的 coding 列表）与 `http_content_encode_context::encode` 统一返回 `std::optional<std::pmr::string>`：有值表示完整编码成功，`std::nullopt` 只表示结果超过 `max_encoded_bytes_`。预算为零只允许零字节表示；多 coding 按声明顺序编码，每个中间表示都受同一预算约束。输出使用 `options.resource_`；可复用上下文的状态使用构造时的资源，该资源必须覆盖上下文寿命，且上下文不可并发使用。成功、超预算或异常后上下文均可继续编码独立表示，不携带上一次的压缩历史；异常后内部状态直接丢弃，下次编码重新创建。`decode_http_content` 接受单个 coding 或 `Content-Encoding` 给出的 coding 列表，列表按逆序解码，每个中间表示都受同一解码上限约束；超过上限的 identity 表示在分配前即被拒绝；`http_content_decode_error` 只描述表示本身的数据错误（`unsupported_coding`、`invalid_content`、`decoded_size_exceeded`）。解码器工作内存同样受解码上限约束：`br` 声明的环形缓冲超出该上限可容纳的大小时（约为上限的三倍加 3 MiB 固定表空间）直接返回 `decoded_size_exceeded`，不按对端声明的窗口分配；`zstd` 解码器按 RFC 9659 接受最大 8 MiB 窗口，这是协议要求的下限。编码、解码与增量编码器在编解码器自身无法运行（状态无法创建或重置、库内部错误）时统一抛出 `http_content_codec_error`，分配异常保留原始类型。接收方用 `parse_http_content_coding`（单个字段值）、`parse_http_content_coding_headers`（字段行序列或 `http_response_headers`）或 `request_content_coding`（已解析的请求）读取 `Content-Encoding`。

Web 缓冲响应用“小于原文”的预算尝试压缩，超预算是正常的“不划算”结果，仍回退 identity；请求拒绝 identity 时不设该预算，任何可接受的编码都优于 `406`。真实故障沿现有失败边界记录并翻译，不作为预算结果忽略。静态根预压缩同样只略过不划算的表示，真实编码异常使构建或刷新失败，不发布不完整的新快照。

---

### 4. HTTP/1.x、HTTP/2 与 HTTP/3 Sans-I/O 状态机

#### 4.1 HTTP/1.0 & HTTP/1.1 状态机

- **服务端解析与序列化**：
  - `"ruvia/http/http1_server_request_parser.h"`：`http1_server_request_parse_state::parse_head(buffer, offset, resource)` 解析请求行、首部、连接复用计划（`http1_request_connection_plan`）与请求体分帧计划（`http1_request_body_plan`）。首行不包含 HTTP-version 词的明文输入（例如 TLS 记录、SSH banner）按非 HTTP 流量静默丢弃，不返回状态行；只有声明了版本号的非法请求行才回 400（RFC 9112 §2.2 的 400 为 SHOULD）。
  - `"ruvia/http/http1_chunked_body_decoder.h"`：`http1_chunked_body_decoder::decode(available)` 增量解码 `chunked` 请求体与 Trailer。每个 chunk-size 行与 Trailer 区段各自受 `max_http_header_bytes`（64 KiB）约束；此外，同一消息所有 chunk-size 行中超出最短十六进制长度的字节（前导零与 chunk-ext）累计不得超过 `max_http_header_bytes`（RFC 9112 §7.1.1），超限与单行超限同样报告 `framing_limit_exceeded`（请求映射为 `413 chunked_framing_too_large`）。Trailer 中的 obs-fold 由 `http1_chunked_body_decoder_config::obs_fold_` 决定：默认 `reject`（请求，服务端可回 `400`），`accept` 时按展开后的字段校验并原样发布字节，由使用方展开（RFC 9112 §5.2）。
  - `"ruvia/http/http1_server_semantics.h"` & `"ruvia/http/http1_response_head_plan.h"`：提交最终响应头与流式响应头分帧计划。临时、最终、`CONNECT` 与 `CONNECT-UDP` 响应头违反消息规则或超出头部大小上限时，统一抛出 `"ruvia/http/http1_response_head_error.h"` 的 `http1_response_head_error`（派生自 `std::invalid_argument`），表示应用或运行时缺陷而非对端错误。
  - 响应分帧只由协议 writer 决定（RFC 9112 §6.1、RFC 9113 §8.2.2）：应用在最终响应上设置的 `Transfer-Encoding` 只在值（忽略 ASCII 大小写）恰为单个 `chunked` 时作为分帧提示接受，HTTP/1 与 HTTP/2 采用同一规则。公开响应字段 setter 仍要求规范化字段值，首尾 SP/HTAB 在设置时即以 `std::invalid_argument` 拒绝；writer 的 OWS 去除不放宽此入口契约。HTTP/1 用分帧计划自己的字段替换它（仅分块流写出规范的 `Transfer-Encoding: chunked`；缓冲、HTTP/1.0 关闭定界、HEAD 与 `204` / `205` / `304` 不写出），HTTP/2 直接省略该分帧提示。其他任何值（如 `gzip`、`gzip, chunked`、`identity`、空值或畸形列表）声明了 writer 不会施加的编码，两种协议都拒绝写出该响应：HTTP/1 的 `http1_commit_final_response`（修改响应前）、`append_http1_response_head` 与 HTTP/1 分帧的 `prepare_http_response_stream_head` 抛 `http1_response_head_error`，HTTP/2 的 `submit_response_head` / `submit_streaming_response_head` 返回 `invalid_message`，`http2_frames` 分帧的 `prepare_http_response_stream_head` 抛 `std::invalid_argument`。Web 层在响应头尚未提交时可将此异常转换为独立的 `500` 错误响应，不会把非法编码或应用正文写上线。`Content-Length` 按协议规范化；HTTP/2 仍拒绝 `Connection`、`Upgrade`、`Keep-Alive` 与 `Proxy-Connection`。
  - `"ruvia/http/http_response_head_buffer.h"`：响应头缓冲保留 inline scratch 的两阶段写入能力。`stack_cursor(bound)` 只检查当前剩余空间，不占用独占 reservation；对象须存活且地址不变。`commit_stack(end)` 将 inline 已提交长度设到本对象数组内的端点（允许 one-past 和缩短长度），允许抛出异常：已经 spill 到堆时抛 `std::logic_error`，空指针或其他对象的端点抛 `std::invalid_argument`，拒绝时保留已有内容。失败的 reserve 不会使仍处于 inline 状态的 cursor 失效；堆状态须先 `reset()` 再取得新的 inline cursor。
- **客户端写出与响应解析**：
  - `"ruvia/http/http1_client_request_writer.h"` & `"ruvia/http/http1_request_content_writer.h"`：生成客户端请求头与流式分块请求体，产出 `http1_client_exchange_state`。
  - `"ruvia/http/http1_client_response_parser.h"` & `"ruvia/http/http1_client_response_body_decoder.h"`：增量解析服务端 `1xx` 临时响应、最终响应头及定长/分块/EOF 定界响应体。客户端按接收方规则处理响应：首部与 Trailer 中的 obs-fold 均替换为空格（响应体解码器发布的 Trailer 块已展开，仅含折叠时从解码器的 PMR 分配一次）；`000..099` 与 `600..999` 状态码按 `500` 处理，`received_status()` 保留原值；HTTP/1.0 响应携带 `Transfer-Encoding` 时按分帧故障处理：忽略 transfer codings，回退到 `Content-Length` 或关闭定界，并在该响应后关闭连接（RFC 9112 §6.1，HEAD/204/304 同样关闭）；`1xx` 永不终止交换，其 `Connection` 选项不改变连接生命周期，最终响应才决定复用（RFC 9110 §15.2）；`Connection` 仅拒绝点名影响响应处理的字段（`Content-Length`、`Transfer-Encoding`、`Set-Cookie`），请求端接收方另拒绝 `Host`、`Authorization`、`Cookie`、`Origin`。发送方（客户端请求含 `forwarded` 来源、CONNECT-UDP 握手与服务端响应）不得发送点名 `Content-Length`、`Transfer-Encoding`、`Host` 等由协议层管理或面向全部接收方的字段的 `Connection` 选项（RFC 9110 §7.6.1），也不得点名请求来源元数据 `Forwarded`（RFC 7239）、`X-Forwarded-For`、`X-Forwarded-Proto`、`X-Forwarded-Host`：下一跳按逐跳字段移除它们后，路由与客户端地址判定将失去依据（同 Pingora 的受保护集合；伪首部形式的名称不是 token，列表语法已拒绝）。HTTP/1 客户端 writer 对此返回 `invalid_connection`，响应首部写入同样拒绝；`TE`、`Upgrade`、`keep-alive`、`close`、`Proxy-Authorization` 与自定义逐跳字段仍可列出。接收方收到点名这些请求来源字段的 `Connection` 不因此报错。
  - `http1_client_response_parser::parse` 在状态行的换行到达时即校验状态行（以裸 LF 结尾报告 `invalid_status_line`，其余错误与完整头部相同），之后任一以裸 LF 结尾的字段行立即报告 `invalid_header`（RFC 9112 §2.2），不必等到空行、64 KiB 上限或读超时，与服务端请求头的提前拒绝一致；非 HTTP 上游（如 SSH banner）因此在首行即失败。

`http1_request_content_writer::chunk` 与 `http_datagram_write_plan` 是对应写入器／会话产出的只读分段快照，不允许默认构造，也不暴露可写的前缀缓冲、长度及正文成员。保留具名计划，通过 `prefix()`、`payload()`（请求块另有 `suffix()`）组成 scatter-gather 写入。`prefix()` 借用计划自身的内联存储，只允许左值调用；计划必须存活且不变，直到传输已消费前缀。`payload()` 仍借用原始输入，其寿命独立于计划，调用方同样须保留输入直到消费完成。复制／移动计划保留独立的内联前缀与原有正文借用，不分配、不复制正文。

出站长度与范围尺寸共用 `"ruvia/http/http_request_content_semantics.h"` 的 `http_content_length`：包括 `http1_client_request_head_view::content_length_`、`http2_request_content::known_length()` / `streaming()`（可选位置仍保留 `std::optional`），以及 `resolve_http_byte_range_set()`、`make_http_multipart_byte_range_plan()`、`http_selected_representation_metadata::length_`、`http_response::file_body()` / `multipart_file_body()` / `content_range()` / `content_range_unsatisfied()`。非负整数可直接填入，构造前检查 `0..UINT64_MAX`，负数抛出 `std::invalid_argument`，无异常探测用 `http_content_length::try_from_value()`。不隐式接受浮点或枚举，需要整数时用 `value()` 解包；不要预先转成无符号数。已解析或已计算的协议长度仍返回 `std::uint64_t` / `std::size_t`。

HTTP/1 请求头与响应头解析的字段描述符共用内部存储（`parser/http_header_descriptor_storage.h`）：前 32 项内联，超出后整体迁入传入的 PMR（缺省为默认资源）并按实际字段数增长。服务端 `parsed_request_header_block` 与客户端 `http1_client_parsed_response_head` 均不按首部字节上限预留线程栈数组。字段区段同时受 64 KiB 字节上限（`max_http_header_bytes`）与字段行数上限 `max_http_header_fields`（256，与 Pingora 解析器默认值相同；仅凭字节上限，最短 4 字节的字段行可使一个头部携带约 16 K 个字段，其描述符远大于头部本身）约束：服务端请求头第 257 行即返回 `431 header_too_large`，客户端响应头报告 `header_too_large`；`http_field_section_budget` 对接收与生成的头部及 Trailer（`http_request_trailers`、`make_parsed_http_request`、客户端请求写出、响应头与 Trailer 输出、WebSocket 握手）执行同一字节与行数规则。服务端请求视图零拷贝借用输入缓冲区，客户端最终响应头拥有字段数据，原始输入可在解析完成后释放。PMR 分配失败以 `std::bad_alloc` 传出，此时不发布 head_ready 或成功状态；释放资源后可用同一缓冲区与偏移重试，但部分元数据可能残留，不保证回滚。PMR 须存活至其拥有的描述符存储释放：服务端请求为 `http_request::reset()`（恢复为默认构造状态，同时不再引用该 PMR）或请求对象析构，客户端响应头为 `take_headers()` 返回的向量或响应头析构。

请求转发使用 `"ruvia/http/http_client.h"` 的 `enum class http_request_field_source : std::uint8_t { generated, forwarded }`，不要把收到的字段当成新配置字段逐项送入 `is_valid_http_request_application_field`：

- HTTP/1 在 `http1_client_request_wire_policy::field_source_` 设置来源；`prepare`、`prepare_streaming`、两个 `prepare_connect` 重载（`http_origin_view` 隧道源，或 `example.test:443` 形式的 authority，经 `is_valid_http_connect_authority` 校验）和 `prepare_connect_udp` 均消费同一策略。HTTP/2 的 `http2_regular_request_head_view`、`http2_connect_request_head_view`、`http2_extended_connect_request_head_view` 均提供 `field_source_`。缺省 `generated` 保持新生成字段的严格语义；配置生成或重写字段须先按生成规则校验，不能借 `forwarded` 绕过配置边界。
- `forwarded` 只切换值语义：`Content-Type`、`Origin`、`Access-Control-Request-Method`、`Access-Control-Request-Headers` 原样保留，由最终应用解释；`Content-Encoding` 和 HTTP/1 `Upgrade` 容忍合法列表空成员（包括空列表），不要求重建列表，非空成员仍须符合 coding token / upgrade protocol 语法。未知但合法的 content coding 不因本地不支持解码而被丢弃。依据 [RFC 9110 §2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.2)，generate 不包括 forward；[§5.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.1) 区分发送方新生成列表和接收方空成员容忍义务。OPTIONS 携带正文须有 Content-Type 是 §9.3.7 的生成要求，转发已接收请求时不补造该字段；TRACE 请求禁止正文。
- 来源不改变字段名/值字节与 CRLF 注入防护、字段节大小、方法/目标、Host/authority 一致性、正文分帧、Content-Length/Expect 所有权或 singleton 次数。即使接收端容忍重复 `User-Agent` 或条件日期，转发 writer 也不切换整套 recipient 规则，不额外放行这些重复字段。非法来源枚举拒绝，HTTP/1 返回 `invalid_header`，HTTP/2 返回 `invalid_message`，不写出请求或占用新流。
- 代理仍负责按 [RFC 9110 §7.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1) 移除/重建逐跳字段。`Connection`、`TE`、`Trailer` 及协议专属限制不放宽；HTTP/1 `Upgrade` 仍须有正确 `Connection: Upgrade`，响应切换仍须匹配实际 offer。HTTP/2 继续禁止 `Connection`、`Upgrade` 等逐跳字段（RFC 9113 §8.2.2）；CONNECT-UDP 的 Capsule-Protocol 与禁止表示/分帧字段规则仍生效。新生成的专用 WebSocket 握手与 PUSH_PROMISE 仍按 `generated` 检查。


#### 4.2 HTTP/2 与 HPACK 状态机（`"ruvia/http/http2_connection.h"`, `"ruvia/http/hpack.h"`）

`ruvia::http2_connection` 实现了符合 RFC 9113 / RFC 7541 / RFC 8441 的完整客户端与服务端 Sans-I/O HTTP/2 连接状态机。每个连接内部持有一个 `nghttp2` 会话：分帧、HPACK、流状态、流量控制以及 `SETTINGS` / `PING` / `GOAWAY` 处理均由 `nghttp2` 完成，`ruvia-http` 负责 HTTP 消息语义（字段校验、内容长度、`CONNECT` / 隧道阶段）、请求租约、流控凭证与防滥用预算。`feed()` 一次接受整段输入，不完整的尾帧由 `nghttp2` 自行缓存，没有“需要更多输入”的结果；`nghttp2` 的全部内部分配均经连接的 PMR 资源完成。

```cpp
#include "ruvia/http/http2_connection.h"

// 构造客户端或服务端状态机（enable_push_ 与 receive_origin_advertisements_ 仅限客户端，
// 传给 server() 时抛出 std::invalid_argument）：
auto conn = ruvia::http2_connection::server({.resource_ = resource});

// 1. 喂入从网络收到的原始字节：
auto feed_res = conn.feed(inbound_bytes);

// 2. 轮询消费协议事件：
while (auto ev = conn.next_event()) {
    if (auto* req_head = ev->request_head()) {
        std::uint32_t stream_id = req_head->stream_id();
        ruvia::http_response resp(resource);
        resp.status(ruvia::http_status::no_content);
        // 无正文响应由 HEADERS 一次结束；有正文时保留请求租约并驱动下述 DATA 契约。
        (void)conn.submit_streaming_response_head(stream_id, std::move(resp));
        (void)conn.release(std::move(*req_head));
    }
}

// 3. 取出待发往网络的编码字节（按整帧边界分批，游标始终停在帧边界）：
std::pmr::string out(resource);
while (conn.wants_write()) {
    out.clear();
    (void)conn.take_output_batch(16384, out);
    // 写出 out
}
```


`hpack_decoder::set_max_dynamic_table_size()` 的容量输入使用 `hpack_table_size`：窄化前检查 `0..UINT32_MAX`，负数或超范围抛出 `std::invalid_argument`（无异常探测用 `hpack_table_size::try_from_value()`）。不隐式接受浮点或枚举，需要整数时用 `value()` 解包；不要预先窄化输入。`hpack_encoder` 只提供 `encode_header()` 与 `encode_status()` 两个字段级入口，编码器使用容量为零的动态表，调用之间不保留状态：完全命中静态表的字段用索引表示，其余字段用不索引的字面量（`authorization`、`proxy-authorization`、`cookie`、`set-cookie` 用 never-indexed，RFC 7541 §7.1.3），较短时使用 Huffman 编码。
独立 `hpack_decoder` 基于 `nghttp2` 的 HPACK inflater，全部分配（含动态表）使用构造时的 PMR 资源。每次 `decode()` 处理一个完整字段块；回调只借用本次字段的名称和值，必须保持 decoder 的实现存活直到调用返回。同一实现处于字段块解码期间，再次 `decode()` 或调用 `set_max_dynamic_table_size()` 均抛出 `std::logic_error`，且不提交该嵌套操作；容量修改移到块之间，不同 decoder 仍可嵌套使用。这是库的状态独占契约，不是 RFC 的额外输入限制。回调返回 `false` 或抛出异常时停止后续回调，但仍解码完当前块以保持动态表同步，随后分别报告 `callback_rejected` 或重抛该异常。`hpack_decode_error` 只区分 `compression_error` 与 `callback_rejected`：任何格式错误或截断都是同一种解压失败（RFC 7541 §3.2 将其规定为连接错误），不再细分原因。解码器不回滚：压缩错误或分配失败后动态表已与对端失去同步，decoder 进入不可恢复状态，此后每次 `decode()` 都报告 `compression_error`，只能丢弃。块首的容量更新按线上顺序生效，出现在字段之后或超过所设上限的容量更新均为压缩错误。把上限降到当前动态表容量以下会立即驱逐条目，且下一个块必须以不大于此后所设最小上限的容量更新开头（RFC 7541 §4.2），否则为压缩错误；调高上限不要求容量更新。

HTTP/2 出站字段直接交给 `nghttp2` 的 HPACK deflater；动态表容量固定为零，普通字段不建立索引，凭据字段使用 never-indexed 表示，避免跨连接共享压缩状态。入站字段块由 `nghttp2` 解码：回调判定字段非法时以流错误重置该流，`nghttp2` 继续解码余下字段块以保持连接级动态表同步；HPACK 解码失败是 `COMPRESSION_ERROR` 连接错误。

- **出站首部暂存**：字段条目与 `nghttp2_nv` 描述符向量分别复用不超过 4 KiB 的容量，超限在提交后归还 PMR。
- **流控凭证 `http2_received_data_credit`**：`message_body_chunk()` 与 `tunnel_data()` 事件携带移动专属的流控凭证；当上层消费完缓冲数据后，通过析构凭证或调用 `conn.acknowledge(std::move(credit))` 才把这部分字节计入 `nghttp2` 的接收窗口并按需发送连接与流级 `WINDOW_UPDATE`（关闭了 `nghttp2` 的自动窗口更新），实现精确背压。本端通告的 1 MiB 初始流窗口在对端确认本端 `SETTINGS` 后才生效，此前对端只能使用 RFC 默认的 65535 字节（RFC 9113 §6.9.2）；对端按协议先回 `SETTINGS` ACK 再发数据时不受影响。
- **服务端请求租约 `http2_request_head_event`**：持有服务端流请求头的底层内存租约；请求处理完毕后调用 `conn.release(std::move(event))` 归还存储。
- **DATA 部分消费 `submit_data(stream_id, bytes, end_stream, max_payload_bytes)`**：返回 `http2_data_submit_result{consumed_bytes_, status_}`，本次只编码 `min(bytes.size(), max_payload_bytes, 连接窗口, 流窗口)` 字节；帧头开销另计。`complete` 只表示这次输入全部消费，不表示 socket 已写完；`backpressured` 可以消费零字节或一个前缀。协议层既不借用也不复制未消费后缀，调用方必须保存后缀并按实际消费量推进，取得输出预算或收到 `WINDOW_UPDATE` / `SETTINGS` 后显式重试。窗口更新不会自动产生 DATA；调用方须显式重试提交。`expectation_pending` 和 `closed` 均消费零字节；前者等待 `100 Continue` 或显式 `release_request_content()`，后者停止发送。`pending_data_output_bytes()` 与输出批次 observer 仍用于已编码 DATA 和 socket 写完成记账；它们不能代替应用层输出预算。
- **长度与终结**：每次先验证完整输入及其终结意图是否符合 `Content-Length`，非法长度不提交任何前缀。长度计数只推进 `consumed_bytes_`。`END_STREAM` 仅在终结输入全部消费后提交；结束后不能重复终结。空终结输入仍是一个 DATA 帧，只有连接与流的发送窗口都为正时才发送（`nghttp2` 按 RFC 9113 §6.9 推迟零窗口 DATA）：任一窗口不为正时 `submit_data()` 返回 `{0, backpressured}` 且不改变任何状态，调用方应等待 `send_window_state()` 报告可用窗口（`available_ != 0`）后重试。所有正文都消费完后才调用 `finish_request()` / `finish_response()` 提交 trailers，保证 terminal HEADERS 排在正文之后；finish 不排队等待未消费正文。不带 trailers 时 finish 以空 DATA 帧结束流，遵循同一窗口规则，窗口不足时返回 `http2_finish_status::backpressured` 且不改变状态，调用方等待窗口后重试。`ruvia-web` 的服务端流式响应按此等待现有的发送窗口信号，等待时长受写超时或请求超时约束。
- **分配失败与异常**：`nghttp2` 内部或缓存其输出时的分配失败，以及 `nghttp2` 回调期间抛出的异常，都会使连接进入致命错误（尚能排队时发送 `GOAWAY(INTERNAL_ERROR)`），随后原异常（分配失败为 `std::bad_alloc`）向调用方传播；`nghttp2` 不提供事务回滚，因此不支持在资源恢复后重试同一操作。`take_output_batch()` 复制失败时输出游标与 observer 不变；释放失败的租约或凭证仍由后续操作重试。
- **对端帧元数据 `receive_frame_metadata_`**：开启后（客户端与服务端均可），连接把已应用的非 ACK `SETTINGS`、已应用的 `WINDOW_UPDATE` 及 `HEADERS` 优先级字段，分别以 `received_settings()`、`received_window_update()`、`received_priority()` 事件按线上顺序与其他事件一同发布；`HEADERS` 的优先级事件先于该帧产生的请求头事件，权重为 1..256。`received_settings` 以定长数组内联保存 `nghttp2` 解码后的条目（最多 33 项），不分配、也不借用 `feed()` 输入：重复的已知 ID 合并为一项（取最后的值、位于首次出现处），未知 ID 按线上顺序保留，同一帧先降后升 `HEADER_TABLE_SIZE` 时末尾另附其中的最小值。独立的 `PRIORITY` 帧（RFC 9113 §5.3.2 已弃用）由 `nghttp2` 读取后丢弃，不产生事件。关闭（默认）时不产生这些事件，也不做额外工作。请求伪首部的线上顺序始终由请求头事件的 `request_view().pseudo_header_order_` 提供。
- **优雅排空 `begin_drain()` / `finish_drain()` / `draining()` / `drain_final()`**：服务端按 RFC 9113 §6.8 两阶段排空——先以 `nghttp2` 的 shutdown notice 发出 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳在途的流；收到该 `PING` 的 ACK 或所有者调用 `finish_drain()` 后发出携带最后对端流 ID 的最终 `GOAWAY`。此后编号更大的新流被直接丢弃、不作应答（对端从 `GOAWAY` 得知其未被处理）。往返期间对端新开超过 128 条流时，超出的那条流收到 `RST_STREAM(REFUSED_STREAM)`，随即发出最终 `GOAWAY`（产品上限）。RFC 只要求至少一个往返时间，不要求等到 ACK：所有者以 `finish_drain()` 为往返等待设定时间上限；未在排空时调用则直接以最终边界进入排空（适用于对端已结束输入、无法再开流）。收到对端合法 `GOAWAY` 时同样立即进入最终阶段（其发送方不得再开流），包括已在往返中的排空。`drain_final()` 为真后，所有者在剩余流结束时即可关闭连接。客户端角色一步发出最终 `GOAWAY`；客户端发出 `GOAWAY` 后到达的 `PUSH_PROMISE` 被丢弃。
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


#### 4.3 HTTP/3、QPACK 与 QUIC（`"ruvia/http/http3_connection.h"`, `"ruvia/http/http3_quic_connection.h"`）

`http3_connection` 用 `nghttp3` 封装请求流、控制流与 QPACK，提供 `read_stream()`、`peek_output()` / `commit_output()` / `acknowledge_output()`、请求/响应/正文/Trailer 提交、RFC 9412 `ORIGIN` 帧（`http3_connection_options::origins_` 与 `submit_origin_advertisement()`）以及 RFC 9218 优先级（`submit_priority_update()`、`stream_priority()`、`set_server_stream_priority()`）接口。`peek_output()` 返回借用向量，不复制待发送正文；QUIC 接受前缀后必须以实际字节数调用 `commit_output()`，收到确认后再调用 `acknowledge_output()`，底层正文块才可回收。

- **留存输入借用**：`http3_data_event` 和 `read_stream()` 在编译期拒绝直接传入临时拥有型字符串或连续容器，后者也拒绝 `std::initializer_list`；命名输入及显式借用视图仍可使用。借用模式下，底层存储须覆盖对应事件的消费；显式构造 view/span 不延长 owner 寿命。

`http3_quic_connection` 用 `ngtcp2` 承担 QUIC v1 包处理、丢包恢复、流量控制、流 ID 与定时到期，并把流数据桥接到 `http3_connection`。ngtcp2 的解密回调缓冲在回调返回后失效，因此该包装器强制启用 `own_data_events_`，在返回 `receive_datagram()` 前把 DATA 复制到事件的 PMR 存储；直接使用 `http3_connection` 时可保留默认借用模式。调用方提供 ngtcp2 加密回调、TLS native handle 和 UDP I/O：`receive_datagram()` 输入一个 UDP 数据报，`next_datagram()` 产出一个拥有字节与目标路径的数据报，`expiry()` / `handle_expiry()` 驱动最近到期事件。TLS 使用 `ngtcp2_crypto_ossl` 时适配层属于 `ruvia::web` 的 OpenSSL 依赖；`ruvia::http` 本身只链接 ngtcp2 core 与 nghttp3。

`http3_headers_event::trailers_` 区分请求/响应头与其后的 Trailer 段；`http3_error_code` 给出服务端使用的 RFC 9114 §8.1 应用错误码（`no_error`、`internal_error`、`request_rejected`、`request_cancelled`、`message_error`）。nghttp3 在帧层识别的格式错误（伪首部缺失或非法、连接专属字段等）由 `read_stream()` 报告为连接错误 `H3_MESSAGE_ERROR`，这是 RFC 9114 §8 允许的“把流错误升级为连接错误”。

`ruvia::web` 的 HTTP/3 服务（`listen_config::http3_`）在这两层之上实现：
- **端点与连接 ID 分流**：POSIX 上每个 worker 以 `SO_REUSEADDR` + `SO_REUSEPORT` 绑定同一 UDP 端口，接收循环每次唤醒后最多连读 32 个数据报，按 `ngtcp2_pkt_decode_version_cid` 解析目标连接 ID：命中本 worker 连接表即交给该连接；服务器签发的 20 字节连接 ID 以两字节编码所属 worker 并用监听器密钥计算标签，指向其他 worker 的数据报经有界目录（每 worker 至多 256 个在途，满则丢弃）转交；合法的客户端 Initial（`ngtcp2_accept`）在启用 `http3_retry_` 时先执行无状态 Retry 地址验证（伪造或过期 Token 回复 `INVALID_TOKEN`），验证通过或未启用 Retry 时在本 worker 新建连接并受 `max_connections_per_worker_` 约束（满额时先淘汰最久空闲连接），不支持的版本回复 Version Negotiation（仅对不小于 1200 字节的数据报）；命中本 worker 已退役服务端连接 ID 的短包头数据报按速率限制回复严格小于触发包长的无状态重置（`write_http3_quic_stateless_reset`），其余未知数据报直接丢弃。Windows 由应用级 Acceptor 线程以 `SO_EXCLUSIVEADDRUSE` 独占 UDP socket，按连接 ID（新连接 Initial 按客户端 ID 的密钥哈希）经同一目录转交业务 worker，业务 worker 的出站数据报经每 worker 至多 256 个在途的有界队列交回 Acceptor 发送。
- **连接会话**：每个连接在所属 worker 上持有 OpenSSL `SSL`（监听器的 QUIC TLS 上下文，TLS 1.3、ALPN `h3`）、`ngtcp2_crypto_ossl_ctx` 与随 `expiry()` 重新设定的 worker 定时器；写协程按 `next_datagram()` 返回的路径发送，发送缓冲满时等待可写，受写停滞期限约束。HTTP/3 控制流在应用密钥就绪后启动，恢复的 0-RTT 可提前启动。每个请求流复用 HTTP/1、HTTP/2 的准入、分派与响应尾处理链；缓冲、文件与流式响应受每流未确认输出预算约束，流式请求体消费后才通过 `consume_stream_data()` 归还额度。
- **生命周期**：连接计入 worker 的连接计数，由连接扫描器按 HTTP/2 相同的阶段计时；达到 `max_requests_per_connection_` 或连接超时时依次发送 shutdown-notice GOAWAY、`min(1 s, 3×PTO)` 后的最终 GOAWAY，所有已接纳流关闭后以 `H3_NO_ERROR` 发送 CONNECTION_CLOSE，并在 3×PTO 的关闭期内吸收迟到数据报；`stop()`（直接断连，不设排空阶段）、淘汰或超时宽限期满时立即以 `H3_NO_ERROR` 关闭。
- **扩展与恢复**：支持 CONNECT、RFC 9220 WebSocket、RFC 9297/9298 HTTP Datagram 与 CONNECT-UDP、RFC 9412 `ORIGIN` 帧、RFC 9218 `PRIORITY_UPDATE` 帧、QUIC Retry 与无状态重置；Web 层的 0-RTT 准入和路径迁移策略见[监听器文档](web-guide.md#ruvialisten_config-与-ruviatls_config)。不提供 HTTP/3 服务端推送（nghttp3 不支持）与仅定义于 HTTP/2 的 `ALTSVC` 帧。

独立 QUIC 驱动调用 `start_early_http3()` 时必须提供经认证保存的传输参数及对端 SETTINGS，并同时启用 TLS 早期数据。TLS 拒绝早期数据时调用 `reject_early_data()` 丢弃早期流，是否重发由调用方裁决。`http3_headers_event::early_data_` 保留实际传输来源，不能由请求字段代替。TLS 恢复并接受早期数据，也不保证某个请求属于 0-RTT：发送节拍或重传可能使请求在握手完成后才到达，应用应读取该请求的来源标记，而非连接的 TLS 协商结果。

`migrate(path, timestamp)` 仅用于客户端，先验证新路径；调用方必须按输出数据报的 `path_` 发送，并在验证期间保持新旧路径接收。`submit_datagram(stream_id, payload)` 的载荷位于 Quarter Stream ID 之后；`max_datagram_payload_size()` 给出当前上限，队列满返回 `would_block`，不支持或超长返回 `invalid_state`。

HTTP/2 与 HTTP/3 分别由 `RUVIA_ENABLE_HTTP2`、`RUVIA_ENABLE_HTTP3` 控制。关闭某项时不解析其源码、不安装其连接头文件，也不引入对应的 nghttp2 或 nghttp3/ngtcp2 依赖；两个开关互不要求对方启用。
---

### 5. WebSocket 与扩展隧道协议层

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/websocket_handshake.h"` / `"ruvia/http/websocket_client_negotiation.h"` / `"ruvia/http/http2_connection.h"` | `validate_websocket_handshake`、`make_websocket_server_handshake`、`struct websocket_server_handshake_options`、`class websocket_client_negotiation final`、`prepare_http1_websocket_client_request`、`validate_http1_websocket_client_response`、`submit_http2_websocket_client_request`、`validate_http2_websocket_client_response`、`enum class websocket_client_handshake_error` | WebSocket 服务端与客户端在 HTTP/1.1 Upgrade 及 HTTP/2 Extended CONNECT 下的握手校验、子协议选择与 `permessage-deflate` 扩展参数协商；两种 HTTP 版本共用同一服务端握手选项与同一客户端握手错误枚举。`websocket_client_negotiation::validate_fields` 只校验给定字段中服务端的子协议与扩展选择（不检查状态码与连接专用字段） |
| `"ruvia/http/websocket_connection.h"` / `"ruvia/http/websocket_protocol.h"` | `class websocket_connection final`、`enum class websocket_feed_status`、`struct websocket_event`、`enum class websocket_event_kind`、`struct websocket_deflater_config`、`validate_websocket_deflater_config` | Sans-I/O WebSocket 帧状态机与协议类型：支持增量输入 `feed`（默认构造的连接拥有自己的输入缓冲区，连接实现与全部协议缓冲分配自 `websocket_connection_options::resource_`；`max_buffered_input_bytes_` 限制等待 `next_event` 的字节，超限时返回 `websocket_feed_status::backpressured` 且不消费输入，连接不再活动时返回 `inactive`），也可从调用方的 `http_io_buffer` 读取输入（追加收到的字节后调用 `next_event`，全部协议缓冲分配自该缓冲区的 PMR，连接实现也分配自该 PMR，除非设置了 `resource_`）；事件轮询 `next_event`（`kind()` 返回 `websocket_event_kind`）覆盖消息重组、Ping/Pong、Close 与协议错误，帧与关闭提交 `submit_frame` / `submit_close`，以及 RFC 7692 `permessage-deflate` 压缩编解码（压缩级别与 memLevel 由 `websocket_deflater_config` 统一表示并校验）；关闭握手完成后服务端角色规划结束传输，客户端角色进入 `websocket_liveness_mode::awaiting_peer_eof`，丢弃后续输入并保持传输打开，直到 `notify_transport_eof()`（RFC 6455 §7.1.1 由服务端先关闭 TCP） |
| `"ruvia/http/http_connect_udp.h"` / `"ruvia/http/http_datagram.h"` | `encode_http_connect_udp_path`、`parse_http_connect_udp_path`、`validate_http_connect_udp_request`、`class http_capsule_decoder final`、`class http_datagram_session final` | RFC 9298 `CONNECT-UDP` 请求/目标校验与 RFC 9297 HTTP Capsule / Datagram 帧增量编解码 |

- **WebSocket 出站缓冲**：复用 `http_io_buffer`；部分写出只推进游标，追加优先使用尾部空间。
- **WebSocket 帧提交**：为帧头、掩码与需复制的负载统一预留空间后才发布输出；分配失败保留已有队列。

`websocket_connection::submit_close()` 接受 `websocket_close_code`；`from_value()` / `try_from_value()` 在整数窄化前校验，发送与接收的状态码判定共用该值类型。`websocket_close_status::{normal_closure, going_away, internal_error}` 提供现有生命周期使用的常量，其余支持值由工厂构造；范围、保留码和 Web 关闭配置的用法见[WebSocket 关闭 API](streaming-websocket.md#32-ruviawebsocket-与-ruviawebsocket_message-api)。服务端主动发送客户端专属 `1010` 仍返回 `invalid_code`，UTF-8 原因与控制帧大小均按协议校验。
