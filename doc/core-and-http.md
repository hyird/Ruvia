# `ruvia-core` 运行时与 `ruvia-http` 协议层 API 手册

本文档提供 `ruvia::core`（独立协程与事件循环运行时底座）和 `ruvia::http`（独立 Sans-I/O HTTP/1.0、HTTP/1.1、HTTP/2、HPACK、WebSocket 与内容编解码协议层）的公开头文件与核心 API 参考。两库均可脱离 `ruvia::web` 独立使用。所有公开符号位于 `namespace ruvia`（状态码常量位于 `namespace ruvia::http_status`）。

---

## 第一部分：`ruvia::core` 运行时 API

### 1. 协程、任务域与 Asio 桥接

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/task.h"` | `template <typename T> class task` | 结构化惰性协程（仅支持移动构造与右值 `co_await`）。未启动（cold）或已完成（done）可安全析构；**销毁已启动但仍挂起的 `task` 直接触发 `std::terminate()`** |
| `"ruvia/core/root_task.h"` | `template <typename T> class root_task final` | 由 `event_loop::start(task<T>)` 返回的跨线程等待句柄。提供 `valid()`、`wait()`、`get()`；析构时若未调用 `get()` 则转为后台托管直至完成，**禁止在所属 `event_loop` 自身线程调用 `wait()` / `get()`** |
| `"ruvia/core/task_scope.h"` | `class task_scope final`、`task_scope::join_operation`、`struct task_scope_options` | 单 Worker 亲和的结构化并发任务域。提供 `spawn(task<void>)`、`request_stop()`、`get_stop_token()`、`stop_requested()`、`size()`、`join()`；停止状态在首次请求令牌时才创建，第一个子任务失败时作用域自动请求停止；`join()` 返回可丢弃、仅启动前可移动的单次冷 `join_operation`，创建和启动须在绑定 Worker，启动后独占该作用域并关闭 `spawn`，并重新抛出第一个子任务失败；析构前若曾 `spawn` 过任务，必须完成 `co_await scope.join()` |
| `"ruvia/core/scoped_operation.h"` | `class operation_scope final`、`class scoped_capability_registration final`、`make_scoped_operation(...)` | 将借用型能力（capability）与惰性协程操作 `task<T>` 绑定到同一 Worker 作用域，`close_and_join()` 先回收冷协程帧并等待已启动操作完成后再失效 capability；`bind_worker(worker)` 借用该 Worker 句柄后，作用域内每个操作的启动、完成与冷帧回收都必须在该 Worker 上，否则 `std::terminate()` |
| `"ruvia/core/async.h"` | `template <typename T> asio::awaitable<T> as_awaitable(task<T> task_value)`、`template <typename Result = void, typename Initiate> auto async_asio(Initiate&& initiate)` | 将 `ruvia::task<T>` 适配为 `asio::awaitable<T>`，或将基于完成回调的 Asio 异步启动操作适配为 Ruvia 协程可直接 `co_await` 的 awaiter |

---

### 2. 停止令牌、超时、截止期与定时器

| 头文件 | 核心公开类型 / 函数 | 说明与契约 |
| --- | --- | --- |
| `"ruvia/core/stop_token.h"` | `class stop_source final`、`class stop_token final`、`class stop_registration final`、`class stop_group final` | `stop_token` 只观察一个状态；`stop_group` 显式拥有父源注册和下游取消状态。`token.register_callback(fn)` 返回自有注册；`token.register_callback(registration, fn)` 替换调用方持有的注册。每个注册只嵌入一个侵入式节点，不额外分配节点；析构自动注销，并等待其他线程正在执行的回调结束 |
| `"ruvia/core/operation_options.h"` / `"ruvia/core/operation_timeout.h"` | `struct operation_options final`、`validate_operation_options`、`override_operation_options`、`class operation_timeout final`、`template <typename Kind = std::monostate> class operation_deadline final` | 通用操作选项（`timeout_` 与 `stop_token_`）、跨异步阶段共享的 `steady_clock` 截止时间计算器与零分配三态（inactive / active / expired）截止期状态机 |
| `"ruvia/core/worker_timer.h"` | `class worker_timer_registration final`、`class worker_timer_cancellation final`、`enum class worker_timer_outcome { expired, cancelled }` | 单 Worker 侵入式定时器注册句柄，配合 `worker_handle::schedule_timer` 使用；提供 `cancel()`（静默取消）与 `cancel_and_notify()` |
| `"ruvia/core/timer.h"` | `enum class timer_sleep_result { elapsed, stop_requested }`、`class sleep_operation final`、`sleep_for(const worker_handle&, duration, [stop_token])` | 在绑定 Worker 上挂起当前协程指定时长；返回可丢弃、仅启动前可移动的单次冷 `sleep_operation`，须在绑定 Worker 上 `co_await` 启动。若传入的 `stop_token` 触发或 Worker 停止定时器，返回 `stop_requested` 而不抛异常 |

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
| `"ruvia/core/worker_handle.h"` | `class worker_handle`、`using worker_id_type = std::uint64_t`、`enum class post_status`、`template <typename Sig> class post_outcome final` | 跨线程安全的稳定派发端点与有界队列投递结果。提供 `valid()`、`accepting()`、`is_current()`、`id()`、`post(fn)`、`post_factory(factory)`、`schedule_timer(...)` |
| `"ruvia/core/worker_runtime.h"` | `class worker_runtime final`、`struct worker_runtime_options`、`struct worker_runtime_hooks`、`enum class runtime_lifecycle_phase` | 底层单 Worker 线程与两阶段启停状态机（`startup -> ready -> serve -> stop_admission -> finalize -> drain -> shutdown -> join`） |
| `"ruvia/core/worker_group.h"` | `class worker_group final` | 协调多个 `worker_runtime` 同步完成两阶段启动屏障（`start()` / `serve()`）与停机回收（`request_stop()` / `join()`） |
| `"ruvia/core/blocking_pool.h"` | `class blocking_pool final`、`template <typename T> class blocking_result final`、`run_blocking(worker_handle, operation_options, fn)` | 固定大小阻塞卸载线程池，由调用方拥有：通过 `event_loop_pool_options`、`attach_event_loop` 的选项或 `worker_runtime_options` 的 `blocking_pool_` 交给事件循环借用，须比事件循环存活更久；core 不持有隐式进程级池。`run_blocking` 在绑定 Worker 上 `co_await`，将 `fn` 提交至该 Worker 借用的池并在完成后回到原 Worker 恢复协程；Worker 没有池时返回 `blocking_status::unavailable`，不提交 `fn`（`worker_handle::has_blocking_pool()` 可预先查询）。`operation_options` 的 `timeout_` / `stop_token_` 只限定等待，非正超时或已停止的令牌同样不提交 `fn`。调用或交付值的异常保存在结果中并由 `value()` 重新抛出。超时、取消或 Worker 停机先释放等待者；已开始的调用继续完成，但只持有自身数据，迟到结果被丢弃 |

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
| `"ruvia/core/move_only_function.h"` | `template <typename Sig> class move_only_function` | 带 `3 * sizeof(void*)` 小对象内联优化（SBO）的仅移动可调用包装器 |
| `"ruvia/core/local_ring_queue.h"` | `class local_ring_queue<T>` | 预分配容量的单线程零分配环形队列 |
| `"ruvia/core/bytes.h"` / `"ruvia/core/failure_report.h"` / `"ruvia/core/socket.h"` | `as_bytes`、`as_chars`、`error_code_text`、`system_error_text`、`parse_ip_address`、`close_socket` | 字节/字符视图转换、跨平台 UTF-8 系统错误文本转换、严格 IPv4/IPv6 字面量解析（返回 `std::optional<asio::ip::address>`）与取消未完成 I/O 后关闭 TCP socket |

`move_only_function<R(Args...)>` 的引用结果必须由目标直接返回兼容引用，允许增加 cv 限定及派生到基类转换；返回值或需要产生临时量的结果不能绑定到引用签名。不同工具链采用相同约束，不依赖编译警告阻止悬垂。需要自定义引用转换（包括按值返回 `std::reference_wrapper`）时，用显式 `-> T&` / `-> const T&` 的适配 lambda 返回实际存活对象；包装器不延长该对象的寿命。值返回签名仍支持正常隐式转换。

`worker_wait_result<T>` 的状态分类在构造后固定；当 `T` 支持时可复制或移动构造，但不可重新赋值。替换保存的结果时使用新局部变量或 `std::optional` 的 `emplace()`，不要给结果或其 `optional` 直接赋值。`value()` 仍可修改 payload，移动构造和 `take_value()` 不改变源结果的状态分类，payload 遵循自身移动语义；其构造或提取抛异常不会让已有结果失去可查询的状态。通道与单次交付仍支持可抛异常的 payload，不要求其移动为 `noexcept`。

`pool_lease_scheduler` 的空闲槽位仍同步获取；`release(slot)` 向排队者提交槽位归属后，通过后续 Worker 轮次恢复该协程，不在释放调用栈内执行下一位消费者，避免连续立即释放形成递归链。提交后的取消或 `close()` 不撤销已交接槽位；尚未交接的等待者按原取消、超时和关闭规则完成。不得依赖 `release()` 返回代表排队协程已经完成，销毁 scheduler 前仍须等待已启动的 acquire 完成。

框架默认内存由 mimalloc 承载：`process_resource()` 是存活至进程结束的线程安全 PMR 适配器，不再叠加进程级共享 pool；默认 Worker 内存、停止状态与协程帧使用该资源。Worker 的小对象 pool、大块缓存、协程帧缓存及 `trim(owner)` 契约保持不变。接受显式 PMR 的 API 始终优先使用调用方资源，调用方仍须保证资源覆盖所分配对象的生命周期。

`worker_memory` 构造中的分配失败以异常传给调用方，并将本次构造已经取得的存储全部归还上游；不因内部 pool 初始化失败终止进程。调用方可在资源恢复后重新构造，不能使用构造失败的对象。

非 Windows 构建启用 mimalloc 普通 `override`，在最终链接中保留静态库，对标准 `malloc` / `free` 与全局 `new` / `delete` 使用 mimalloc；不得叠加另一套全局 allocator replacement。Windows 保持原静态 CRT 与普通 mimalloc 依赖，仅通过显式 API 承载框架资源和协程帧，不替换宿主标准分配接口。两种接入都不替换 `std::pmr::get_default_resource()` 对象，也不改变显式 PMR 的优先级。独立 `ruvia::http` 不依赖 core 或 mimalloc，未显式指定 PMR 时仍遵循调用方的标准 PMR 默认资源。

---

## 第二部分：`ruvia::http` Sans-I/O 协议层 API

`ruvia::http` 不执行任何 socket I/O、定时器或线程调度，全部协议状态机均以内存字节流（`std::string_view` / `std::span` / `http_io_buffer`）与强类型事件/计划对象交互。

### 1. 基础类型、缓冲与编码工具

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/borrowed_text.h"` | `class borrowed_text final` | 只读字符串借用视图，拒绝直接绑定临时拥有型字符串，但不延长底层文本生命周期；集合 option 使用 `std::span<const T>` 借用连续存储，调用方必须保持存储有效直到消费入口完成读取或拥有化，`span` 本身不阻止悬垂 |
| `"ruvia/http/port_number.h"` | `class port_number final` | 原始整数宽度受检的端口值；在格式化或原生 API 边界用 `value()` 显式解包 |
| `"ruvia/http/http_io_buffer.h"` | `class http_io_buffer final` | PMR 流式读写缓冲区：`prepare(n)` / `prepare_spare()`、`commit(n)`、`append(bytes)`、`data()` / `view()`、`consume(n)`、`clear_retaining_small()`；返回借用的四个入口只接受左值，缓冲区存储须覆盖异步 I/O 与视图消费 |
| `"ruvia/http/http_protocol_version.h"` / `"ruvia/http/http_known_method.h"` | `http_protocol_version`（`http10`, `http11`, `http2`）、`http_scheme`（`http`, `https`）、`http_known_method`、`classify_http_method`、`is_valid_http_method_token` | 协议版本、Scheme 与 HTTP 方法分类及幂等/安全性判断 |
| `"ruvia/http/http_status.h"` | `class http_status_code final`、`namespace ruvia::http_status`、`http_reason_phrase` | 强类型 HTTP 状态码（`100..599`）与标准状态码常量（`http_status::ok`、`not_found` 等） |
| `"ruvia/http/http_protocol_error.h"` | `class http_protocol_error final : public std::exception`、`enum class http_parse_error` | 零动态分配的协议错误异常（`http_protocol_error(status, code, message)`：携带 `http_status_code`、静态存储期的稳定 snake_case 机器码 `code()`（如 `missing_host`、`conflicting_content_length`）与定长诊断文本）与解析错误枚举 |
| `"ruvia/http/http_request_target.h"` | `http_authority_view`、`http_origin_view`、`parse_http_authority`、`is_valid_http_origin_form_target`、`is_valid_http_connect_authority`、`is_valid_http_serialized_origin` | RFC 9110 / RFC 3986 请求目标、Authority 与 Origin 语法校验和解析 |
| `"ruvia/http/http_target_origin.h"` | `decide_http_target_origin`、`http_target_origin`、`http_target_origin_connection`、`http_target_origin_action` | RFC 9110 §7.4 目标源判定：服务器提供连接事实（是否 TLS、对端是否可信网关），返回接受、需按证书校验的主机（DNS 名去掉末尾点 / IPv4 / IPv6 网络字节序）或 `421` 拒绝；明文连接上的 `https` 目标直接判为 misdirected，零分配 |
| `"ruvia/http/url_encoding.h"` / `"ruvia/http/http_hex.h"` / `"ruvia/http/http_base64.h"` | `decode_url_component`、`validate_url_encoding`、`visit_url_encoded_pairs`、`percent_encode_uri_reference`、`uri_paths_equivalent`、`compare_uri_paths`、`http_encode_lower_hex`、`http_base64_encode`、`http_base64_decode` | URL 百分号/表单编解码、RFC 3986 URI-reference 规范化编码（`decode_url_component` 与 `percent_encode_uri_reference` 无需变换时返回输入视图本身、不分配，否则把结果写入调用方提供的 `std::pmr::string` 并返回其视图；解码只在百分号编码非法时返回 `nullopt`）、路由使用的路径规范形式比较（字面 `/` 分段、段内按百分号解码后的字节比较，`%2F` 不作分隔符）、十六进制转换、标准与 URL-safe Base64 编解码 |

`decode_url_component()` 与 `percent_encode_uri_reference()` 的文本输入使用 `borrowed_text`：字符串字面量、`std::string_view` 和命名的 `std::string` / `std::pmr::string` 可直接传入，临时拥有型字符串（含 `const` 右值）在编译期拒绝。返回值可能借用原始输入，也可能借用输出 `storage`，两者的 owner 均须覆盖结果消费；不要根据某次内容是否需要转换来推断寿命。需要传入生成的字符串时先保存为命名 owner，需要独立结果时在 owner 仍有效期间复制视图；显式构造 `string_view` 不会延长 owner 寿命。原有零拷贝和输入／输出别名能力不变。

`http_status_code::from_value()`、`try_from_value()` 与 `is_valid_value()` 接受原始整数类型并在窄化前检查 `100..599`；超宽或负数不能通过截断变成有效状态码。浮点、枚举及其他依赖隐式转换的调用须先由调用方完成明确的整数转换和相应校验；不要为迁移预先窄化到 `uint16_t`，否则会丢失原始边界信息。

端口输入统一使用 `port_number`：包括 `http_origin_options::port_`、`http_connect_udp_target_view::port_`，以及 Web 的 HTTP / WebSocket 客户端、监听、数据库和 Redis 配置；可选端口仍保留 `std::optional`。合法整数可直接填入，构造前按原始宽度检查 `0..65535`，负数或超范围值抛出 `std::invalid_argument`；需要无异常探测时用 `port_number::try_from_value()`。浮点与枚举不隐式接受，也不提供到原始整数的隐式转换；不要在调用前窄化。零端口与缺省端口不同，各入口原有的非零限制不变；已解析的协议端口和系统观察结果仍可返回 `std::uint16_t`。

---

### 2. HTTP 首部、缓存、条件请求、范围与 Cookie

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_header.h"` / `"ruvia/http/http_field_syntax.h"` / `"ruvia/http/http_protocol_managed_fields.h"` | `class http_header final`、`class http_header_view final`、`is_valid_http_header_name`、`is_valid_http_header_value`、`is_http_ows_char`、`is_http_protocol_managed_request_field`、`is_valid_http_request_application_field`、`is_valid_http_response_field` | PMR 拥有型首部键值对、非拥有型首部视图、RFC 9110 字段语法校验、分帧/连接专用请求字段判定及按请求/响应方向区分的发送前单字段校验 |
| `"ruvia/http/http_accept_match.h"` / `"ruvia/http/http_accept_encoding.h"` | `class http_accept_match final`、`http_response_coding_selection_for(request)` | `Accept` 媒体类型/语言权重匹配与 `Accept-Encoding` 响应压缩算法协商 |
| `"ruvia/http/http_date.h"` / `"ruvia/http/http_cache.h"` | `format_http_date`、`parse_http_date`、`class cache_control final`、`parse_cache_control`、`class cache_control_field_parser final` | IMF-fixdate 日期格式化/解析与 `Cache-Control` 指令解析（`parse_cache_control` 解析单个字段值；重复字段行逐行 `update`，`finish` 取得结果） |
| `"ruvia/http/http_conditional_request.h"` / `"ruvia/http/http_byte_range.h"` / `"ruvia/http/http_representation_response_plan.h"` | `get_http_etag_preconditions`、`resolve_http_byte_range_set`、`plan_http_representation_response` | 条件请求（`If-Match` / `If-None-Match` / `If-Modified-Since` / `If-Unmodified-Since` / `If-Range`）与 `Range` 字节范围响应计划计算 |
| `"ruvia/http/cookies.h"` / `"ruvia/http/http_set_cookie.h"` | `struct cookie_options`、`parse_set_cookie`、`class set_cookie_plan final` | Cookie 属性校验（含 `__Secure-` / `__Host-` 前缀规则）、`Set-Cookie` 解析与序列化计划 |
| `"ruvia/http/http_priority.h"` | `class http_priority_urgency`、`struct http_priority`、`struct http_priority_fields`、`parse_http_priority`、`encode_http_priority` | RFC 9218 可扩展 HTTP 优先级首部与 HTTP/2 `PRIORITY_UPDATE` 帧编解码 |

`http_priority::urgency_` 与 `http_priority_fields::urgency_` 共用 `http_priority_urgency`，后者仍以 `std::optional` 区分缺省参数。按 [RFC 9218 §4.1](https://www.rfc-editor.org/rfc/rfc9218.html#section-4.1)，值域为 `0..7`、默认值为 `3`；本地整数输入在窄化前检查，非法值抛出 `std::invalid_argument`，无异常探测用 `try_from_value()`。不隐式接受浮点或枚举，也不隐式转换回整数；需要整数时用 `urgency_.value()`（可选字段用 `urgency_->value()`），不要预先窄化。接收端仍按 RFC 9218 §4 忽略越界或类型不符的参数，不把本地构造失败规则套用到对端字段。

当两个 `http_header` 均绑定 PMR 时，赋值保留目标的资源对象。移动赋值在资源等价时直接转移存储、不分配；`is_equal()` 只允许互相回收存储，不表示两个资源对象具有相同寿命，因此不会改为借用源资源。拷贝和移动构造仍沿用源对象的资源。

`is_valid_http_request_application_field(name, value)` 在应用配置或请求构造边界校验单个出站请求首部，返回 `bool`、`noexcept` 且不分配内存：检查字段名 token、规范化字段值（包括拒绝首尾 SP/HTAB 及非法控制字节），按大小写不敏感匹配拒绝 `Host`、`Content-Length`、`Transfer-Encoding`、`Trailer`、`Expect`、`Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Upgrade`，并复用请求 writer 的已知字段发送方语义校验（如 `Content-Type`、`Content-Encoding`、`Origin` 与 CORS 请求字段）。合法未知扩展和其空值仍允许；不引入应用配置政策。HTTP/1 与 HTTP/2 的 `TE` 差异已被托管字段边界排除，因此无版本参数。此接口不保留状态，字段间重复、完整消息约束仍由调用方及最终 writer 负责；不得用于响应首部或接收方校验，接收方仍按各协议入口的 recipient 规则处理。

`is_valid_http_response_field(name, value)` 声明在 `http_field_syntax.h`，同样返回 `bool`、`noexcept` 且不分配内存；检查通用名称/规范化字段值语法并复用响应发送方的 `Content-Type`、`Content-Encoding`、`Trailer` 语义校验。它不应用请求专属的 `Origin`、CORS、`Expect` 规则或请求托管字段黑名单，不用于接收方校验，也不替代响应写出入口的版本相关连接/分帧检查（如 `Connection`、`Upgrade`、`TE`）、重复字段检查及完整消息约束。应用禁改首部政策由调用方单独保留；此谓词返回 `true` 不表示字段在任意响应上下文中都可以发送。

---

### 3. 消息模型、内容压缩编解码、Multipart 与 SSE

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/http_request.h"` / `"ruvia/http/http_request_trailers.h"` | `class http_request final`、`make_parsed_http_request`、`class http_request_trailers final` | 请求头与请求体零拷贝视图描述符、大小写不敏感首部/Cookie 查找与请求 Trailer 校验收集 |
| `"ruvia/http/http_response.h"` / `"ruvia/http/http_interim_response.h"` | `class http_response final`、`class http_response_headers final`、`class http_interim_response_head final`、`plan_buffered_http_response_write` | 最终响应（`200..599`）与临时响应（`1xx`）模型，以及针对 `HEAD` / `204` / `205` / `304` 的响应体写计划。`head_metadata_body()` 将正文替换为描述 HEAD 所选表示的空正文；`has_head_metadata_body()` 查询该状态。显式 `Content-Length` 保留为 GET 表示的长度，缺失时不推断为零；重新设置正文会清除标记 |
| `"ruvia/http/http_content_coding.h"` / `"ruvia/http/http_content_codec.h"` / `"ruvia/http/http_content_encoder.h"` | `enum class http_content_coding { identity, gzip, deflate, brotli, zstd }`、`parse_http_content_coding_headers`、`request_content_coding`、`encode_http_content`、`decode_http_content`、`class http_content_encode_context final`、`class http_content_codec_error final`、`class http_content_encoder final` | 接收方 `Content-Encoding` 解析，单次缓冲、可复用缓冲上下文与流式增量的 `gzip` / `deflate` / `br` / `zstd` 内容压缩，以及带解压上限保护的解码器 |
| `"ruvia/http/multipart_parser.h"` | `parse_multipart_boundary`、`parse_multipart_body`、`class multipart_parser final` | 一次性与流式增量（`feed` / `poll`）的 `multipart/form-data` 解析器 |
| `"ruvia/http/sse.h"` | `struct sse_message final`、`format_sse_message` | Server-Sent Events 消息格式化与校验 |

`http_response::body_segment_count()` 枚举表示的存储片段，不等同于待发送的字节数。长度为零的文件范围仍返回一个 file segment，保留路径、句柄与 identity，调用方必须完成适用的文件验证，不能因 `send_body() == false` 丢弃它；普通空字节正文没有 segment。`send_body()` 仍只表示方法/状态允许且正文长度非零时需要传送 payload。

`encode_http_content` 的两个 overload（单个 coding，或按声明顺序依次编码的 coding 列表）与 `http_content_encode_context::encode` 统一返回 `std::optional<std::pmr::string>`：有值表示完整编码成功，`std::nullopt` 只表示结果超过 `max_encoded_bytes_`。预算为零只允许零字节表示；多 coding 按声明顺序编码，每个中间表示都受同一预算约束。输出使用 `options.resource_`；可复用上下文的状态使用构造时的资源，该资源必须覆盖上下文寿命，且上下文不可并发使用。成功、超预算或异常后上下文均可继续编码独立表示，不携带上一次的压缩历史。`decode_http_content` 接受单个 coding 或 `Content-Encoding` 给出的 coding 列表，列表按逆序解码，每个中间表示都受同一解码上限约束；超过上限的 identity 表示在分配前即被拒绝；`http_content_decode_error` 只描述表示本身的数据错误（`unsupported_coding`、`invalid_content`、`decoded_size_exceeded`）。编码、解码与增量编码器在编解码器自身无法运行（状态无法创建或重置、库内部错误）时统一抛出 `http_content_codec_error`，分配异常保留原始类型。接收方用 `parse_http_content_coding`（单个字段值）、`parse_http_content_coding_headers`（字段行序列或 `http_response_headers`）或 `request_content_coding`（已解析的请求）读取 `Content-Encoding`。

Web 缓冲响应用“小于原文”的预算尝试压缩，超预算是正常的“不划算”结果，仍回退 identity；请求拒绝 identity 时不设该预算，任何可接受的编码都优于 `406`。真实故障沿现有失败边界记录并翻译，不作为预算结果忽略。静态根预压缩同样只略过不划算的表示，真实编码异常使构建或刷新失败，不发布不完整的新快照。

---

### 4. HTTP/1.x 与 HTTP/2 Sans-I/O 状态机

#### 4.1 HTTP/1.0 & HTTP/1.1 状态机

- **服务端解析与序列化**：
  - `"ruvia/http/http1_server_request_parser.h"`：`http1_server_request_parse_state::parse_head(buffer, offset, resource)` 解析请求行、首部、连接复用计划（`http1_request_connection_plan`）与请求体分帧计划（`http1_request_body_plan`）。首行不包含 HTTP-version 词的明文输入（例如 TLS 记录、SSH banner）按非 HTTP 流量静默丢弃，不返回状态行；只有声明了版本号的非法请求行才回 400（RFC 9112 §2.2 的 400 为 SHOULD）。
  - `"ruvia/http/http1_chunked_body_decoder.h"`：`http1_chunked_body_decoder::decode(available)` 增量解码 `chunked` 请求体与 Trailer。Trailer 中的 obs-fold 由 `http1_chunked_body_decoder_config::obs_fold_` 决定：默认 `reject`（请求，服务端可回 `400`），`accept` 时按展开后的字段校验并原样发布字节，由使用方展开（RFC 9112 §5.2）。
  - `"ruvia/http/http1_server_semantics.h"` & `"ruvia/http/http1_response_head_plan.h"`：提交最终响应头与流式响应头分帧计划。临时、最终、`CONNECT` 与 `CONNECT-UDP` 响应头违反消息规则或超出头部大小上限时，统一抛出 `"ruvia/http/http1_response_head_error.h"` 的 `http1_response_head_error`（派生自 `std::invalid_argument`），表示应用或运行时缺陷而非对端错误。
  - `"ruvia/http/http_response_head_buffer.h"`：响应头缓冲保留 inline scratch 的两阶段写入能力。`stack_cursor(bound)` 只检查当前剩余空间，不占用独占 reservation；对象须存活且地址不变。`commit_stack(end)` 将 inline 已提交长度设到本对象数组内的端点（允许 one-past 和缩短长度），不再是 `noexcept`：已经 spill 到堆时抛 `std::logic_error`，空指针或其他对象的端点抛 `std::invalid_argument`，拒绝时保留已有内容。失败的 reserve 不会使仍处于 inline 状态的 cursor 失效；堆状态须先 `reset()` 再取得新的 inline cursor。
- **客户端写出与响应解析**：
  - `"ruvia/http/http1_client_request_writer.h"` & `"ruvia/http/http1_request_content_writer.h"`：生成客户端请求头与流式分块请求体，产出 `http1_client_exchange_state`。
  - `"ruvia/http/http1_client_response_parser.h"` & `"ruvia/http/http1_client_response_body_decoder.h"`：增量解析服务端 `1xx` 临时响应、最终响应头及定长/分块/EOF 定界响应体。客户端按接收方规则处理响应：首部与 Trailer 中的 obs-fold 均替换为空格（响应体解码器发布的 Trailer 块已展开，仅含折叠时从解码器的 PMR 分配一次）；`000..099` 与 `600..999` 状态码按 `500` 处理，`received_status()` 保留原值；HTTP/1.0 响应携带 `Transfer-Encoding` 时按分帧故障处理：忽略 transfer codings，回退到 `Content-Length` 或关闭定界，并在该响应后关闭连接（RFC 9112 §6.1，HEAD/204/304 同样关闭）；`1xx` 永不终止交换，其 `Connection` 选项不改变连接生命周期，最终响应才决定复用（RFC 9110 §15.2）；`Connection` 仅拒绝点名影响响应处理的字段（`Content-Length`、`Transfer-Encoding`、`Set-Cookie`），请求端接收方另拒绝 `Host`、`Authorization`、`Cookie`、`Origin`。发送方（客户端请求与服务端响应）不得发送点名 `Content-Length`、`Transfer-Encoding`、`Host` 等由协议层管理或面向全部接收方的字段的 `Connection` 选项（RFC 9110 §7.6.1），`TE`、`Upgrade`、`keep-alive` 与自定义逐跳字段仍可列出。

`http1_request_content_writer::chunk` 与 `http_datagram_write_plan` 是对应写入器／会话产出的只读分段快照，不再开放默认构造和可写的前缀缓冲、长度及正文成员。保留具名计划，通过 `prefix()`、`payload()`（请求块另有 `suffix()`）组成 scatter-gather 写入。`prefix()` 借用计划自身的内联存储，只允许左值调用；计划必须存活且不变，直到传输已消费前缀。`payload()` 仍借用原始输入，其寿命独立于计划，调用方同样须保留输入直到消费完成。复制／移动计划保留独立的内联前缀与原有正文借用，不分配、不复制正文。

HTTP/1 请求头与响应头解析的字段描述符共用内部存储（`parser/http_header_descriptor_storage.h`）：前 32 项内联，超出后整体迁入传入的 PMR（缺省为默认资源）并按实际字段数增长。服务端 `parsed_request_header_block` 与客户端 `http1_client_parsed_response_head` 均不按首部字节上限预留线程栈数组，也不增加字段数量限制；64 KiB 首部字节上限仍然生效。服务端请求视图零拷贝借用输入缓冲区，客户端最终响应头拥有字段数据，原始输入可在解析完成后释放。PMR 分配失败以 `std::bad_alloc` 传出，此时不发布 head_ready 或成功状态；释放资源后可用同一缓冲区与偏移重试，但部分元数据可能残留，不保证回滚。PMR 须存活至其拥有的描述符存储释放：服务端请求为 `http_request::reset()` 或请求对象析构，客户端响应头为 `take_headers()` 返回的向量或响应头析构。

请求转发使用 `"ruvia/http/http_client.h"` 的 `enum class http_request_field_source : std::uint8_t { generated, forwarded }`，不要把收到的字段当成新配置字段逐项送入 `is_valid_http_request_application_field`：

- HTTP/1 在 `http1_client_request_wire_policy::field_source_` 设置来源；`prepare`、`prepare_streaming`、两个 `prepare_connect` 重载（`http_origin_view` 隧道源，或 `example.test:443` 形式的 authority，经 `is_valid_http_connect_authority` 校验）和 `prepare_connect_udp` 均消费同一策略。HTTP/2 的 `http2_regular_request_head_view`、`http2_connect_request_head_view`、`http2_extended_connect_request_head_view` 均提供 `field_source_`。缺省 `generated` 保持新生成字段的严格语义；配置生成或重写字段须先按生成规则校验，不能借 `forwarded` 绕过配置边界。
- `forwarded` 只切换值语义：`Content-Type`、`Origin`、`Access-Control-Request-Method`、`Access-Control-Request-Headers` 原样保留，由最终应用解释；`Content-Encoding` 和 HTTP/1 `Upgrade` 容忍合法列表空成员（包括空列表），不要求重建列表，非空成员仍须符合 coding token / upgrade protocol 语法。未知但合法的 content coding 不因本地不支持解码而被丢弃。依据 [RFC 9110 §2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-2.2)，generate 不包括 forward；[§5.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.6.1) 区分发送方新生成列表和接收方空成员容忍义务。OPTIONS 携带正文须有 Content-Type 是 §9.3.7 的生成要求，转发已接收请求时不补造该字段；TRACE 禁止正文的消息约束不变。
- 来源不改变字段名/值字节与 CRLF 注入防护、字段节大小、方法/目标、Host/authority 一致性、正文分帧、Content-Length/Expect 所有权或 singleton 次数。即使接收端容忍重复 `User-Agent` 或条件日期，转发 writer 也不切换整套 recipient 规则，不额外放行这些重复字段。非法来源枚举拒绝，HTTP/1 返回 `invalid_header`，HTTP/2 返回 `invalid_message`，不写出请求或占用新流。
- 代理仍负责按 [RFC 9110 §7.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1) 移除/重建逐跳字段。`Connection`、`TE`、`Trailer` 及协议专属限制不放宽；HTTP/1 `Upgrade` 仍须有正确 `Connection: Upgrade`，响应切换仍须匹配实际 offer。HTTP/2 继续禁止 `Connection`、`Upgrade` 等逐跳字段（RFC 9113 §8.2.2）；CONNECT-UDP 的 Capsule-Protocol 与禁止表示/分帧字段规则仍生效。新生成的专用 WebSocket 握手与 PUSH_PROMISE 仍按 `generated` 检查。


#### 4.2 HTTP/2 与 HPACK 状态机（`"ruvia/http/http2_connection.h"`, `"ruvia/http/hpack.h"`）

`ruvia::http2_connection` 实现了符合 RFC 9113 / RFC 7541 / RFC 8441 的完整客户端与服务端 Sans-I/O HTTP/2 连接状态机：

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

独立 `hpack_decoder` 每次 `decode()` 处理一个完整字段块；回调只借用本次字段的名称和值，必须保持 decoder 的实现存活直到调用返回。同一实现处于字段块解码期间，再次 `decode()` 或调用 `set_max_dynamic_table_size()` 均抛出 `std::logic_error`，且不提交该嵌套操作；容量修改移到块之间，不同 decoder 仍可嵌套使用。这是库的状态独占契约，不是 RFC 的额外输入限制。回调异常不截断合法块的消费，动态表完成同步后再重抛；后续编码错误则回滚整个块，并优先保留最初的回调异常。

- **流控凭证 `http2_received_data_credit`**：`message_body_chunk()` 与 `tunnel_data()` 事件携带移动专属的流控凭证；当上层消费完缓冲数据后，通过析构凭证或调用 `conn.acknowledge(std::move(credit))` 触发连接与流级 `WINDOW_UPDATE` 帧发送，实现精确背压。
- **服务端请求租约 `http2_request_head_event`**：持有服务端流请求头的底层内存租约；请求处理完毕后调用 `conn.release(std::move(event))` 归还存储。
- **DATA 部分消费 `submit_data(stream_id, bytes, end_stream, max_payload_bytes)`**：返回 `http2_data_submit_result{consumed_bytes_, status_}`，本次只编码 `min(bytes.size(), max_payload_bytes, 连接窗口, 流窗口)` 字节；帧头开销另计。`complete` 只表示这次输入全部消费，不表示 socket 已写完；`backpressured` 可以消费零字节或一个前缀。协议层既不借用也不复制未消费后缀，调用方必须保存后缀并按实际消费量推进，取得输出预算或收到 `WINDOW_UPDATE` / `SETTINGS` 后显式重试。窗口更新不会自动产生 DATA，也没有旧的 queued/drained 队列通知。`expectation_pending` 和 `closed` 均消费零字节；前者等待 `100 Continue` 或显式 `release_request_content()`，后者停止发送。`pending_data_output_bytes()` 与输出批次 observer 仍用于已编码 DATA 和 socket 写完成记账；它们不能代替应用层输出预算。
- **长度与终结**：每次先验证完整输入及其终结意图是否符合 `Content-Length`，再预留该次允许编码的全部帧空间；非法长度或分配异常不提交半个前缀。长度计数只推进 `consumed_bytes_`。`END_STREAM` 仅在终结输入全部消费后提交，空终结输入即使窗口和 payload 许可为零也提交一帧空 DATA；结束后不能重复终结。所有正文都消费完后才调用 `finish_request()` / `finish_response()` 提交 trailers，保证 terminal HEADERS 排在正文之后；finish 不再排队等待未消费正文。
- **对端帧元数据 `receive_frame_metadata_`**：开启后（客户端与服务端均可），连接把已应用的非 ACK `SETTINGS`（保留线上顺序、未知项与重复项）、已应用的 `WINDOW_UPDATE`、`PRIORITY` 帧及 `HEADERS` 优先级字段，分别以 `received_settings()`、`received_window_update()`、`received_priority()` 事件按线上顺序与其他事件一同发布；`HEADERS` 的优先级事件先于该帧产生的请求头事件。关闭（默认）时不产生这些事件，也不做额外工作。请求伪首部的线上顺序始终由请求头事件的 `request_view().pseudo_header_order_` 提供。
- **优雅排空 `begin_drain()` / `finish_drain()` / `draining()` / `drain_final()`**：服务端按 RFC 9113 §6.8 两阶段排空——先发 `GOAWAY(2^31-1, NO_ERROR)` 与 `PING`，继续接纳在途的流；收到该 `PING` 的 ACK 或所有者调用 `finish_drain()` 后发出携带最后对端流 ID 的最终 `GOAWAY` 并拒绝之后的流（对端此间新开超过 128 条流时提前进入最终阶段，属产品上限）。RFC 只要求至少一个往返时间，不要求等到 ACK：所有者以 `finish_drain()` 为往返等待设定时间上限；未在排空时调用则直接以最终边界进入排空（适用于对端已结束输入、无法再开流）。收到对端合法 `GOAWAY` 时同样立即进入最终阶段（其发送方不得再开流），包括已在往返中的排空。`drain_final()` 为真后，所有者在剩余流结束时即可关闭连接。客户端角色一步发出最终 `GOAWAY`。
- **服务端 `CONNECT` 乐观数据**：接受/拒绝前收到的 `DATA` 以 `tunnel_data()` 事件发布，其流控凭证由所有者持有到接受隧道后消费；`submit_connect_response_head()` 接受后再发布 `tunnel_end()`（若请求半边已结束）。非 2xx 拒绝后到达的 `DATA` 丢弃并归还流控额度。RFC 8441 `:protocol = websocket` 的提前 `DATA` 同样保留（RFC 6455 §4.1 的等待规则只约束客户端）。非 2xx 拒绝响应后服务端以 `RST_STREAM(NO_ERROR)` 结束流。已建立隧道上的未知扩展帧被丢弃（RFC 9113 §5.5），`ALTSVC`/`ORIGIN` 帧也被忽略（RFC 7838 §4、RFC 8336 §2.1）。
- **客户端 `REFUSED_STREAM`**：客户端在收到任何响应头（含 `1xx` 临时响应与最终响应头）前收到 `RST_STREAM(REFUSED_STREAM)` 时发布 `request_unprocessed()` 而不是 `stream_closed()`（RFC 9113 §8.7：请求未被处理，可安全重试），连接保持可用；`http_client` 对无请求体的请求重试一次。
- **Server Push 并发**：`submit_push_request()` 在已打开的服务端推送流（预留状态不计入，RFC 9113 §5.1.2）达到对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 时返回 `http2_push_submit_error::stream_limit`，不发送 `PUSH_PROMISE`（RFC 9113 §8.4）。
- **分配失败与重试**：`submit_reset()` 的本地 reset 标记只在完整 `RST_STREAM` 入队后提交；因分配失败抛出时，不消耗已关闭流上仍允许的一次 late reset，可在资源恢复后对同一流重试。未完成首部块的 `CONTINUATION` 处理若因分配失败回滚，保留此前的片段与拒绝判定；对同一片段重试不会把本应拒绝的隧道上 `PUSH_PROMISE` 发布为成功事件。这是本库的异常恢复契约，不表示任意 `feed()` 调用或已发布事件都具备整体回滚保证。
- **`SETTINGS_ENABLE_CONNECT_PROTOCOL`**：服务端收到该参数不产生任何影响（RFC 8441 §3）；客户端仍校验服务端发送的取值与 1→0 回退。

---

### 5. WebSocket 与扩展隧道协议层

| 头文件 | 核心公开类型 / 函数 | 说明 |
| --- | --- | --- |
| `"ruvia/http/websocket_handshake.h"` / `"ruvia/http/websocket_client_negotiation.h"` / `"ruvia/http/http2_connection.h"` | `validate_websocket_handshake`、`make_websocket_server_handshake`、`struct websocket_server_handshake_options`、`class websocket_client_negotiation final`、`prepare_http1_websocket_client_request`、`validate_http1_websocket_client_response`、`submit_http2_websocket_client_request`、`validate_http2_websocket_client_response`、`enum class websocket_client_handshake_error` | WebSocket 服务端与客户端在 HTTP/1.1 Upgrade 及 HTTP/2 Extended CONNECT 下的握手校验、子协议选择与 `permessage-deflate` 扩展参数协商；两种 HTTP 版本共用同一服务端握手选项与同一客户端握手错误枚举。`websocket_client_negotiation` 的成员 `submit_http2_request` / `validate_response` 即以自身调用两个 HTTP/2 自由函数，`validate_fields` 只校验给定字段中服务端的子协议与扩展选择（不检查状态码与连接专用字段） |
| `"ruvia/http/websocket_connection.h"` / `"ruvia/http/websocket_protocol.h"` | `class websocket_connection final`、`enum class websocket_feed_status`、`struct websocket_event`、`enum class websocket_event_kind`、`struct websocket_deflater_config`、`validate_websocket_deflater_config` | Sans-I/O WebSocket 帧状态机与协议类型：支持增量输入 `feed`（默认构造的连接拥有自己的输入缓冲区，连接实现与全部协议缓冲分配自 `websocket_connection_options::resource_`；`max_buffered_input_bytes_` 限制等待 `next_event` 的字节，超限时返回 `websocket_feed_status::backpressured` 且不消费输入，连接不再活动时返回 `inactive`），也可从调用方的 `http_io_buffer` 读取输入（追加收到的字节后调用 `next_event`，全部协议缓冲分配自该缓冲区的 PMR，连接实现也分配自该 PMR，除非设置了 `resource_`）；事件轮询 `next_event`（`kind()` 返回 `websocket_event_kind`）覆盖消息重组、Ping/Pong、Close 与协议错误，帧与关闭提交 `submit_frame` / `submit_close`，以及 RFC 7692 `permessage-deflate` 压缩编解码（压缩级别与 memLevel 由 `websocket_deflater_config` 统一表示并校验）；关闭握手完成后服务端角色规划结束传输，客户端角色进入 `websocket_liveness_mode::awaiting_peer_eof`，丢弃后续输入并保持传输打开，直到 `notify_transport_eof()`（RFC 6455 §7.1.1 由服务端先关闭 TCP） |
| `"ruvia/http/http_connect_udp.h"` / `"ruvia/http/http_datagram.h"` | `encode_http_connect_udp_path`、`parse_http_connect_udp_path`、`validate_http_connect_udp_request`、`class http_capsule_decoder final`、`class http_datagram_session final` | RFC 9298 `CONNECT-UDP` 请求/目标校验与 RFC 9297 HTTP Capsule / Datagram 帧增量编解码 |

`websocket_connection::submit_close()` 接受 `websocket_close_code`；`from_value()` / `try_from_value()` 在整数窄化前校验，发送与接收的状态码判定共用该值类型。`websocket_close_status::{normal_closure, going_away, internal_error}` 提供现有生命周期使用的常量，其余支持值由工厂构造；范围、保留码和 Web 关闭配置的迁移规则见[WebSocket 关闭 API](streaming-websocket-client.md#32-ruviawebsocket-与-ruviawebsocket_message-api)。服务端主动发送客户端专属 `1010` 仍返回 `invalid_code`，UTF-8 原因与控制帧大小的拒绝语义不变。
