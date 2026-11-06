# AGENTS.md

默认使用中文沟通与编写文档，路径、命令、代码与标识符保留原文。
本文件定义项目的全局**工程准则**、**架构边界**、**编程范式**与**核心设计契约**，仅在全局规范或核心架构变化时更新，不作单点修复记事本。`README.md` 面向使用者提供最短接入入口（遵守第 9 节规范）；代码风格、命名与头文件分组统一遵守 `STYLE.md`。
适用范围：`ruvia-core`、`ruvia-http`、`ruvia-web`、`examples` 与 `tests`。

---

## 1. 工程准则

- **改动纪律**：修改前检查 `git status`，保留用户及并行任务的已有改动，不擅自回退、覆盖或夹带无关修改。
- **自主决策**：上下文与现有契约可推导时直接执行，仅在存在实质性歧义或重大权衡时问一个必要问题。
- **优先级准则**：正确性与并发安全第一，其次长期可维护性，最后是经实测证明的性能；围绕调用主链、不变量、失败路径与资源回收采用最小清晰方案。
- **少即是多（Less is More）**：能删则删、能简则简；新增设计只服务当前明确需求，至少两个真实调用方共享才提取抽象；严禁预设假想扩展点、单用法模板/策略/配置项、无状态包装或纯转发层，严禁将同步或非事务操作伪装成异步或事务。
- **遵循标准与借鉴权威**：严格遵守现行标准与 RFC，不以主观“更严格”代替标准；优先借鉴同领域权威项目（如 Pingora 等）的成熟机制，不闭门造车。
- **正交分层与高内聚**：模块依赖保持单向无环（DAG）；同职责代码保持内聚，大型状态机仅按正交阶段拆分实现并共享同一内部状态头（各实现片段显式包含自身直接依赖头），不拆碎片文件或胶水层。
- **语义化命名与注释**：接口明确表达所有权、生命周期与错误语义；名称直接反映对象、职责与动作，禁用无上下文缩写及 `Manager`、`Helper`、`Processor`、`Util`、`Data`、`handle` 等泛称；注释仅解释意图、约束、不变量与 RFC 条款，不复述代码。
- **单一权威实现**：同类能力统一入口，共用唯一主链、状态机与生命周期（服务端请求、子请求与 `test_app` 共用同一套准入、分发与尾部管线），不以复制代码、平行校验或无独立语义的中间 DTO 掩盖重复。
- **零技术债务与纯粹契约**：不为缩小 diff 塞入旁路或兼容开关；内部代码、协议、存储与缓存保持当前唯一契约，不引入无外部标准依据的内部版本号、代次后缀或带编号魔数。
- **公开 API 边界**：公开 API（各库 `include/` 下非 `detail` 符号）服务外部使用方，必须覆盖本库全部能力（`src` 与 `detail` 不承载仅库内可用的能力）；未经允许不改变公开 API 契约；内部重构一次性迁移全部调用方、测试与文档，彻底清除死代码与 `TODO`。

---

## 2. 架构分层与目录边界

| 库目录 | 公开 Target（顶层 `namespace ruvia`） | 职责与依赖边界 |
| :--- | :--- | :--- |
| `ruvia-core` | `ruvia::core` | 独立运行时底座：协程任务、事件循环、定时器、有界通道、连接槽租约调度、阻塞卸载池与 PMR 内存池；依赖 standalone Asio、Threads 与 mimalloc，不含 HTTP/Web、TLS、压缩或数据后端语义 |
| `ruvia-http` | `ruvia::http` | 独立 sans-I/O 协议层：HTTP/1.0、HTTP/1.1、HTTP/2、HPACK、内容编码、multipart、Range、条件请求、Cookie、WebSocket 与 Capsule 语义；不依赖 `core`、Asio、socket、定时器、OpenSSL 或 mimalloc |
| `ruvia-web` | `ruvia::web` | 应用框架与 I/O 驱动：路由、中间件、Model/ORM、静态文件、限流、Session、TLS/crypto、服务端/客户端网络驱动、隧道及数据库/Redis/JWT 接入；依赖 `ruvia::core` + `ruvia::http`，消费协议计划驱动 I/O，不复制协议判断或下沉 Web 语义 |

- **目录与构建边界**：使用 C++20，源码仅置于三个库、`examples` 和 `tests`；各库自带 `CMakeLists.txt`、`include`、`src`（及 `src/pch.h`），`src` 最多一层职责目录，`ruvia-core/src` 保持扁平；根 `CMakeLists.txt` 统一管理依赖查找与 `ruviaConfig.cmake` 导出。
- **链接与安装闭包**：非 Windows 下 `ruvia-core` 以 `WHOLE_ARCHIVE` 链接启用 `override` 的静态 `mimalloc-static`，Windows 统一使用 MSVC 静态运行时与普通 mimalloc；可选特性（`MARIADB`、`POSTGRESQL`、`REDIS`、`JWT` 等）关闭时同步剔除对应源码与安装头目录，安装仅含启用特性的公开头及其真实依赖闭包。
- **跨库契约与最小包含闭包（Include Firewall）**：跨库只用非 `detail` 公开 API，严禁穿透内部头或物理路径；核心入口头（如 `context.h`）对独立子系统句柄优先使用前置声明，由消费方编译单元按需包含定义头。
- **Sans-I/O 协议核心与传输驱动解耦**：`ruvia-http` 仅在计算期借用输入视图，产出只含纯值状态与区间的解析结果与协议计划（不持有 socket、定时器、协程或文件 I/O；复合事件兼作派生视图的线性存储租约，流控信用以移动唯一、析构自动归还的线性令牌交付）；`ruvia-web` 消费协议计划驱动异步 I/O，直接复用构帧阶段已打开并验证身份的原生文件句柄（防 TOCTOU：内联小文件写出前二次校验身份，Windows `TransmitFile` 合并首帧响应头并按 256 KiB 分片刷新写超时，Linux 阻塞池预读经 `F_DUPFD_CLOEXEC` 复制独立 `fd` 并使用 `process_resource()`）。

---

## 3. 核心编程范式

- **组合、概念与单一主模板**：优先值语义、纯数据聚合与概念约束（`concept`），继承仅用于标准库与 Asio 强制契约；需同时支持 `T` 与 `void` 的泛型任务或等待物通过底层统一 `result()` 接口保持单一主模板，避免 `void` 偏特化失步。
- **具名控制流**：避免深层回调或 lambda 嵌套；lambda 仅承载短小局部回调，复杂分支与多阶段异步流程提取为具名函数或协程，明确捕获范围与生命周期。
- **类型驱动不变量**：互斥阶段与结果优先用 `std::variant` 建模，消除松散布尔标志与输出参数耦合；部分失败仍产出有效进度（如已传输字节数）时同时携带错误码与进度；结果类型固定构造期类别并 `= delete` 赋值运算符（需替换时用外层 `std::optional::emplace` 重建），防移动抛异常退化为无类别态。
- **数据错误与内部故障分层**：预期内的数据/格式/配额错误以值语义（枚举、`optional`、`variant`）返回，底层状态创建失败或 `std::bad_alloc` 直接抛出异常（严禁伪装成数据错误）；允许重复集合的唯一字段查找显式返回 `absent / found / repeated` 三态；有状态流处理器维护 `active / finished / failed` 三态，失败即锁入终态，仅经显式 `reset()` 复用工作内存。
- **编译期借用防线**：借用视图不延长宿主寿命；跨表达式留存的视图（`borrowed_text`、`borrowed_span`）在编译期拒绝绑定右值临时容器；视图入口 `= delete` `std::initializer_list` 重载（改用变参模板在栈帧构造局部数组转发）；内部存储访问器标注 `RUVIA_LIFETIMEBOUND`；被异步操作或回调借用地址的宿主限定左值引用（`&` / `const&`）并 `= delete` 右值重载；受控临时等待物同时 `= delete` 一元 `operator&`。
- **借用、提取与接管分离**：容器严格分离左值只读借用（`value() &` / `const&`，禁右值借用）与右值所有权提取（`take_value() &&`），构建器链式修改限定左值（`&`）；零拷贝接管右值容器的重载用 `std::same_as<T>` 精确约束以防字面量隐式构造临时容器；长寿命描述符按值接收拥有型容器，热路径选项使用受检借用视图并在入口返回前复制归一化。
- **原始域受检转换与饱和算术**：跨边界整数在窄化或转无符号前于原始域完成范围校验（`std::in_range`），位掩码单项查询校验非零 2 的幂；受检值类型以单一 `constexpr` 谓词为真值，提供抛异常构造（本地严格）与 `noexcept try_create`（线面容错）双入口，并断言平凡可拷贝与同底层尺寸；协议数值保留原始字面量精度（不隐式窄化 `double`）；容量与时长统一采用饱和运算或减法比较（`limit - used < delta`），正延迟向低精度超时转换一律向上取整（ceil）；超时与限流统一使用 `steady_clock`，日历时钟仅用于协议时间戳。
- **构造即就绪（RAII）与宿主地址固定**：对象构造完成即进入合法可用态，不设半初始化或二次 `init()`，中途失败按已构造计数逆序精确回滚；持有在途 I/O、侵入式节点或被借用 `this` 的宿主 `= delete` 拷贝与移动，需支持移动的门面句柄及其内嵌子门面直接绑定至地址稳定的独立状态块。
- **防重入摘除、终结前栈快照与析构反序**：重置或销毁持有回调与外部句柄的包装器时，先用 `std::exchange` 置空内部指针再移动或析构目标；触发可能销毁宿主自身的终结回调前将回调与状态全部快照至栈局部，调用后严禁再访问 `this`；成员按析构反序声明（最先解链阻断外部回调的放末位，最后注销的放首位）；侵入式子对象注册项 `= delete` 单参数拷贝/移动构造，仅提供带新宿主地址的双参数构造以强制 $O(1)$ 原位重绑。
- **两阶段事务化变更（Prepare-Commit-Cancel）**：`const` 方法零可观测副作用；多步变更或复合追加先在候选副本校验，或先一次性预留容量与回滚检查点，可失败步骤全部通过后再 `noexcept` 提交，失败完整回滚；带共享状态的流式解码器遇协议错误回滚当次变更，遇业务回调拒绝则继续消费完当前块以维持双端状态同步。
- **持久层一致性与短事务远程 I/O 隔离**：关键状态落库提交后才确认消息或发布通知，缓存不得掩盖持久层失败，提示队列溢出时通过递增恢复代次引导下游重建快照；严禁持有数据库事务或互斥锁等待远程网络 I/O，统一采用“短事务校验租约 → 释放锁执行远程调用 → 短事务按版本与租约 CAS 提交”。
- **自别名、嵌套自包含与 `uintptr_t` 区间安全**：可能触发扩容、压缩或释放旧存储的写入/替换操作（及 Base64 原地编解码），若输入视图可能借用目标容器自身存储，须先在独立缓冲或栈快照构造再替换；嵌套拥有型结构移动赋值前检测目标是否位于源存储内；校验外部指针是否落在内部数组区间时统一转 `std::uintptr_t` 无符号比较作差（防跨对象指针运算 UB）。
- **准入优先与失败退回负载**：调用非平凡构造或工厂前先在栈上快照宿主句柄，带工厂投递（`post_factory`）先取得有界队列准入槽位再同步调用工厂；按值投递或发送被拒时，必须经返回结构（`worker_delivery_result`）将未消费的原始负载完整退回调用方。
- **三阶段职责前移与无锁快照切换**：编译期完成静态查表、Schema/SQL 占位符校验与类型推导；启动期一次性完成配置聚合原子校验、不可变路由树与中间件组装、静态索引（记录 `has_sidecars`）及池槽预分配并封存只读（等长配置与运行态按平行数组同下标索引）；运行期动态刷新的只读索引在后台全量构建新快照（任一步失败整次放弃），各 worker 仅在请求间隙无锁原子切换。
- **单遍断点续扫与阶段内复用**：边界入口单遍完成词法、编码与转义扫描（标记 `literal / escaped`），杜绝“预扫描再二次遍历”或“格式化再解析”；跨多行列表字段以状态机零分配流式折叠；不相交区间集合单遍原地归并重叠/相邻区间；增量解析器在 `need_more` 分支保存已验证偏移防 $O(N^2)$ 重扫；同阶段可复用结果只取一次（如路由查找用位掩码剪枝复用已探测方法），局部变更只增量更新受影响目标。

---

## 4. 性能与内存治理

- **热路径零拷贝与同步快路径**：请求热路径不新增锁、原子争用、类型擦除或无用引用计数拷贝（连接级不变服务预构建跨流复用，上下文直接借用长寿命 `worker_handle` 与 `stop_token`，运行期回调借用使用双指针 `callback_ref`；显式阻塞卸载是共享所有权的唯一例外）；能同步完成的路径不创建协程帧（`as_awaitable(task<void>)` 直接复用 `exception_ptr` 签名）；空结果或零长响应不分配数据块、不占配额租约；有状态编解码器首遇压缩消息时惰性创建，无状态保留方向同步借用 worker 共享实例并当次重置；超时、扫描与取消注册优先采用内嵌侵入式节点。
- **显式分级内存池（PMR）**：分配器覆盖对象寿命，可选 `memory_resource*` 在入口一次性归一化随删除器携带（不篡改全局默认池，禁用隐式 `thread_local` 全局池）；进程级 `process_resource()` 供跨线程共享状态、跨请求复用谓词与上游分配（返回冷任务前归一化至目标 worker 池）；`worker_memory` 无锁 `pool()`（小对象池 + 受 `retained_block_bytes_` 预算约束的对齐块缓存）供连接、结果与后台任务使用；单次请求与握手用 `request_memory` 单调 Arena（子操作可 `fork()`）；C 压缩库分配经 `pmr_codec_allocation` 路由至 PMR 并在 C 回调边界桥接重抛分配异常。
- **栈小缓冲优化（SBO）与写时分配（Copy-on-Unescape）**：高频短数据与小型可调用对象优先采用栈 SBO（以 `variant` 建模内联栈态与溢出堆态，`constexpr` 大定长栈数组仅在 `std::is_constant_evaluated()` 分支全量清零）；多条目内联缓存采用共享连续字节池配 `(offset, size)` 索引；单线程有界环（`local_ring_queue`）启动期预构造槽位；AST 采用单一连续数组配下标偏移；未折叠、未转义切片直接零拷贝借用输入缓冲，仅对发生折叠或转义的字段按需分配存储（Copy-on-Unfold/Unescape）。
- **最短内存生命周期与长连接水位线**：异步入口返回前完成输入拥有化，构造冷任务后、进入 I/O 挂起前立即释放临时源容器；PMR 对象拷贝/移动构造继承源资源，赋值保留目标资源（同资源 `swap`，跨资源递归重绑定后 `swap`，纯标量经 `consteval` 短路）；异常抛出时将动态诊断负载提升至 `process_resource()`；长连接缓冲按水位线保留小容量（HTTP/1 读缓冲回落时缩容超限块，归还池保留 $\le 16\text{ KiB}$）、主动归还超限大块，并在空闲周期触发 `trim_when_idle`；读缓冲扩容免零初始化，区分扩容 `prepare(bytes)` 与仅回收前缀返回剩余空间的 `prepare_spare()`；流式缓冲 `consume()` 仅推进游标，待已消费前缀足以覆盖存活后缀时才 $O(1)$ 摊销前移压缩；整帧批量输出停在帧边界（空目标且同分配器时直接 $O(1)$ `swap` 存储，并在触发观察者回调前先提交游标）。
- **有界资源预算、双轨超时与防预分配 DoS**：队列、缓存、连接与输入缓冲强制执行两级实时内存预算（worker 级与单连接级，超限抛 `std::bad_alloc`；超连接上限在 TLS 握手前直接关 socket）与“阶段无活动超时（`idle / initial_read / payload_read / write`）+ 不可被慢速涓流刷新的绝对完成截止期”双轨超时；流式读取每次只返回非空块并以 `std::nullopt` 为唯一 EOF，每次读取与关闭独立计单次截止期；外部声明长度在分配前须受初始窗口底线（如 `min(content_length, 64 KiB)`）约束并按实际到达字节几何扩容，严禁一次性预分配；更窄作用域配额遵循“只紧不松”，多级限流独立叠加生效。

---

## 5. 并发运行时与生命周期

- **Share-Nothing 模型与平台监听**：生产 `application` 为进程单例（`ruvia::app()`），`core` 是线程、事件循环与 worker 生命周期的唯一 owner（`current_worker_guard` 禁单线程嵌套运行；附着外部 `io_context` 时经自定义 `service` 独占 `claim()` 并在 `shutdown()` 自动退役）；每业务 worker 独占 standalone Asio 事件循环并独立绑定全部监听端点，连接从 accept 到关闭全程留在 origin worker，不设跨线程连接交接；POSIX 使用 `SO_REUSEADDR + SO_REUSEPORT` 直接 accept，Windows 各 worker 独立绑定后在共享监听矩阵上发起原生 `AcceptEx`，跨 worker 完成只向 origin 的预留有界完成槽发布唤醒，严禁跨线程处理或销毁 origin 的 socket 与 PMR。
- **有界预分配调度、独立控制通道与锁外执行**：跨线程调度与有界通道在构造期预分配节点（`worker_dispatcher` 预分配 $2 \times \text{capacity}$ 节点，有界 `channel` 合并统计 `reserved_ + queued_`）；已准入操作的取消、延迟清理（`defer_cleanup`）与挂起恢复（`defer_resume`）必须走独立控制/完成通道，不受业务队列满阻塞；跨线程唤醒（`worker_notification`）原子合并锁存并采用内嵌单槽零分配等待（Windows 保持管道 `HANDLE` 至 IOCP 取消完成）；互斥锁内仅交换指针或整链摘取，**用户回调执行、对象移动与析构严格在互斥锁外进行**；同 worker 内的槽位移交与唤醒统一延迟至下一轮事件循环恢复防栈递归；跨线程仅传独立所有权数据或不可变快照。
- **惰性协程与结构化并发**：公开协程统一使用 `ruvia::task<t_type>`（仅右值 `co_await`），冷任务移至最终宿主后才标运行态；挂起与完成竞争由 `suspend_race_state` 原子裁决（禁在真正挂起前内联恢复），交付外部回调前先销毁回收协程帧，超对齐局部变量置独立堆存储；已启动任务取消后必须 `co_await / join`，严禁销毁挂起帧或静默 detach；`root_task` 被放弃时经存活与完成标志互斥握手将未消费异常路由至 failure sink（先销毁 moved-from 中间结果再 `publish_completion()`）；`task_scope` 分配前先递增 `active_`，`operation_scope` 关闭时先销毁未启动冷任务再执行能力清理回调并挂载栈 `drain_guard` 防重入提前唤醒；投递携冷任务或等待者的闭包须挂载析构守卫防队列排空时等待端永久挂起。
- **多源取消与单一定时器最小堆**：`stop_token` 引用进程池分配的 `stop_state`，多源取消由地址稳定的 `stop_group` 显式拥有，选项派生替换用户 token 而不累加；`request_stop()` 在锁外执行回调并经栈 `destroyed` 指针防回调自毁后悬垂写回（异线程解链阻塞等待回调返回，同线程自解链立即完成）；跨线程取消经持有原子可分离指针与 `operation_id` / 代际计数（`slot + generation`）的轻量代理投递回所属 worker；每 worker 由单一 `steady_timer` 与最小堆驱动全部定时任务（跨线程无通知取消直接 CAS 置零代际免唤醒）；受锁与定时器/取消源共同驱动的等待槽须在持锁期间完成结果转移、置零代际解链并唤醒，且将 `stop_registration` 声明为末位成员。
- **连接池流水线与脏连接淘汰**：连接池（`slot_pool / pool_lease_scheduler`）保持 worker-local 且预分配槽位；共享流水线在存在更早独占请求或队首已放弃（`stalled`）时停止并入防饥饿，无进展超时（`stretch_start_`）跨读者交接连续计量，非读者取消经 `abandon` 由后续读者静默吸收孤儿结果，读者在 `hand_off` 前顺带推进已就绪跟随者；借出槽位仅在协议流排空、空闲干净、未关 `autocommit` 且无未决事务时才可归还复用；任何超时、取消、I/O 异常、半关闭或未捕获退出必须在释放租约前立即关闭并淘汰底层连接（`db_slot_socket` 复制独立句柄并在建连等待恢复后即刻关闭，`close_slot` 遇活跃等待时待栈展开后再先关 `wait_socket_`、后关驱动连接）。
- **显式阻塞卸载（`blocking_pool`）**：阻塞或 CPU 密集任务显式卸载至预分配节点的常驻有界线程池，锁外执行移动、调用与析构；卸载任务只携自有数据，严禁触碰 worker 状态或在池内等待同一池的子任务；严格区分队列满拒绝（`queue_full`）与停机丢弃（`pool_stopped`），停机恢复等待者并丢弃迟到结果。
- **两阶段启动屏障与分阶段停机**：重复监听端点直接拒绝；`worker_group` 按 `start()`（独立绑定并发布 Windows 共享矩阵）→ `serve()`（全员 barrier 后发起 accept）推进，任一失败逆序回滚全部资源；停机按「停止接入（POSIX 关监听器，Windows 取消 `AcceptEx` 并保持矩阵与 IOCP 至转递消费完毕）→ 有界排空（关空闲连接，在 `drain_timeout` 内完成在途请求，HTTP/2 两阶段 `GOAWAY`，WebSocket `1001` 关闭，`connection_drain` 热路径无锁原子计数且 `wait_until` 按 $\le 1\text{h}$ 分段）→ 强制关闭 → 先停 I/O 再 join 后台」推进；`join()` 修改运行态前先校验调用线程不属于目标 worker 集合防自等待死锁，停机清理钩子严禁抛异常。

---

## 6. 安全与健壮性规范

- **歧义拒绝（Fail-Closed）与 421 Origin 保护**：无根路径、点段逃逸、空字节、非法或二次百分比编码及非 `http(s)` 的 `absolute-form` 目标直接拒绝；连接无法承载目标 origin（拒绝明文 `https` 目标、`IPvFuture` 及尾随点 IPv4 如 `"127.0.0.1."`，栈上解码 `reg-name` 中 `unreserved` 百分比字节后核对 TLS 证书）时按 RFC 9110 §7.4 返回 `421`；跨源重定向自动剥离认证凭据与非目标源 Cookie，改写为无正文方法时同步剥离原请求正文与表示首部。
- **配置完整性与命名空间隔离**：类型化配置查找（`env::get<T>`）严格区分“未配置（返回 `std::nullopt`）”与“配置非法或已执行迁移被篡改（立即抛异常阻断启动，严禁用默认值掩盖）”；缓存键与命名空间完整区分资源、表示（如编码变体）、协议及连接身份维度；限流归并同网段地址（无作用域 IPv6 按 `/64`、IPv4-mapped 归并 IPv4），开放寻址探测遇过期槽仅记录首个可回收槽并继续探测至空槽或目标 key。
- **可信代理、跨源安全与常量时间比对**：转发首部（`Forwarded / X-Forwarded-*`）仅在可信代理下采信，从右向左逆序取首个不可信跳为客户端边界；Auto-HTTPS 308 重定向保留空查询 `?` 区分并强制附加 `Cache-Control: private` 防缓存投毒；CORS 遇畸形或多值 `Origin` 绝不生成授权首部，动态跨源响应自动追加 `Vary`；敏感凭证采用常量时间比对（签名 Cookie 前置 4B 大端 `name_len` 防边界碰撞，$\le 512\text{B}$ 栈内联完成 HMAC），JWT 强制校验 RFC 7518 最小密钥长度并默认要求 `exp`。
- **TLS 安全基线（RFC 9846 / RFC 9525）**：证书与私钥注册时先完成 SNI 去重预检，经临时 `scratch` `SSL_CTX` 一次性读取提取句柄供各 worker 内存安装（替换 `PEM_def_callback` 为非交互回调防阻塞 stdin，口令用毕 cleanse）；仅协商 TLS 1.2（ECDHE + AEAD，禁重协商）或 TLS 1.3；入站/出站/DB 驱动强制按 SAN（`DNS-ID` 最左完整标签通配、`IP` 精确匹配，禁 `Common Name`）校验身份，无跳过开关且禁降级明文；服务端 mTLS 强制绑定非空 `session_id_context`，客户端证书 DN 经内存 `BIO` 按 RFC 2253 无截断渲染；单连接发出记录总数受 `max_outgoing_records_` 硬上限约束（`KeyUpdate` 不重置，达限阻断写出）；Ticket 密钥仅在相邻周期保留 `previous_`，析构时 `OPENSSL_cleanse` 擦除，跨 worker 恢复严格绑定原 `server_name`。
- **跨平台 UTF-8 与错误脱敏**：对外文本与系统路径统一使用合法 UTF-8（拒绝 `\0`，Windows 路径经 `MB_ERR_INVALID_CHARS` 严格宽化、系统错误经 `FormatMessageW` 转 UTF-8、截断时回退半个多字节序列保完整码点）；对外报错与日志严格脱敏，禁回显内部异常栈、`what()`、SQL、宿主路径或上游细节，真实凭据严禁入库、入制品或入日志。
- **故障隔离与兜底限流上报**：正常断开、停机或排空不计为故障；入站传输自身 I/O 失败直接终止连接，不伪造错误响应；未捕获后台/回调异常统一路由至带限流与等价根因合并计数（`equivalent_failures`）的 `noexcept` 通道 `report_unhandled_failure`（owner 重抛首个异常时其余独立异常逐一上报），严禁静默吞掉；单次请求、流或连接错误在局部边界内隔离，缓冲预算耗尽与阻塞池满统一映射为 `503 Service Unavailable`。

---

## 7. 协议实现与审查规范

- **标准遵循与主链审查**：协议实现严格遵循现行 RFC、更新及勘误（HTTP 参照 RFC 9110/9111/9112/9113，TLS 1.3 参照 RFC 9846）；区分 MUST/SHOULD/MAY 与收发/代理义务，不把发送方 MUST NOT 当作接收方拒绝规则；构建请求组件独占协议托管首部并区分本端生成（`generated`）与代理转发（`forwarded`）；沿「请求解析 → 正文分帧与解码 → 响应计划与序列化 → 网络驱动与回收」主链审查，协议义务注释标注 RFC 条款。
- **HTTP/1.x 语义与增量分帧**：核对 `Host` 与请求目标（禁 `userinfo`，区分缺省/空/显式端口，`absolute-form / authority-form` 强制绑定 `Host` 至 `target.authority_`，请求头超限按是否仍在 `request-target` 内区分 `414` 与 `431`，`HTTP/1.1..1.9` 归一化为 `http11`）；同时收到 `Upgrade` 与 `100-continue` 时按 RFC 9110 §7.8 先发 `100 Continue`；客户端写入器原子决定 `100-continue` 门控并移交自包含 `http1_client_exchange_state`，流式请求正文散集写出且定长须 `commit_finish()` 校验总长（未完成前终止须 `abort()`），`awaiting_continue` 期间收到最终响应或 HTTP/1.0 响应含 `Transfer-Encoding` 时响应后强制关连，客户端将 `600..999` 映射为 `500`，非 `HEAD` 的 `205` 消费分帧字节但跳过传输解压且不产出正文；增量解析扫到裸 `LF` 立即拒绝，`chunked` 须消费终止零块，传输编码栈逆序级联且末级直写目标缓冲（单层零暂存），multipart 用 3B 前缀重叠与填充双游标断点续扫；流水线仅允许无正文且匹配缓冲路由的后续请求并入，并在响应头实际写出后发射访问日志；明文非 HTTP 流量静默丢弃；响应 `Trailer` 先构造借用证明，响应头分帧使用互斥变体（禁向 HTTP/1.0 发 `chunked`），未设显式长度的 `HEAD` 响应省略 `Content-Length`，秒级单缓冲 `http_date_cache` 供 HTTP/1 与 HTTP/2 共用。
- **条件请求、缓存、协商、Range 与 Cookie**：条件请求遵循 RFC 9110 §13.2.2 优先级，`If-Match / If-None-Match` 要求 `"*"` 独占单行，重复 `If-Modified-Since` 忽略，未来 `mtime` 钳位至当前秒并取消 `If-Range` 强验证器资格；`Cache-Control` 保留首见 `delta-seconds` 并支持 `quoted-pair` 解转义；`Accept-Encoding` 区分缺失与空值，`Content-Encoding` 限制层数（超限 `415`）与中间块上限并在阶段间立即释放（Zstd 锁定 `windowLog = 23` 即 8 MiB），应用编码自动弱化强 `ETag` 为 `W/` 并合并 `Vary: Accept-Encoding`，流式压缩延迟至 `activate` 借用编码器；`Range` 原地合并重叠/相邻区间，多段 `Range` 遇已编码表示按 RFC 9110 §14.6 回退完整表示；`Set-Cookie` 先受检求长，按 `(name, domain, path, partitioned)` 四元键原地替换去重，校验 `__Secure- / __Host- / SameSite=None`，删除用 Epoch 0 `Expires`，客户端 Jar 400 天封顶。
- **HTTP/2 与优先级（RFC 9113 / RFC 9218）**：`SETTINGS` 先在候选副本校验无溢出后再提交；`DATA` 先预留容量再扣连接窗口，丢弃帧立即归还连接窗口，移除流时 `flush_window_debt` 保留未释放信用防连接窗口萎缩，闭合流忽略零增量 `WINDOW_UPDATE`；HPACK 入站用 4-bit `consteval` Huffman 表，动态表先构造独立条目再驱逐防 Indexed-Name 自别名，首部块支持异常回滚，移除等待 `CONTINUATION` 的流转入 `discarded_header_stream_` 维持同步，出站 `HEADERS + CONTINUATION` 经 `checkpoint()` 原子回滚且敏感首部标 `never_indexed`；严格区分流级与连接级错误，内置未排空 `PING / SETTINGS`（1000）、Rapid-Reset 净差值（1000，防 CVE-2023-44487）、畸形请求头流（32）与空闲流 `PRIORITY_UPDATE` 防洪上限；两阶段 `GOAWAY` 限制 RTT 期新流数并保证 `last_stream_id` 单调不增，环形历史推进已重置奇数流水位线忽略迟到帧；已建立隧道收 `PUSH_PROMISE` 先同步 HPACK 再发双 `RST_STREAM`；入站正文队列采用双缓冲拼接配合线性流控信用令牌，未决流租约释放登记至定长账本 `http2_stream_lease_ledger` 随流控确认解锁。
- **全双工会话、WebSocket、SSE 与隧道**：全双工会话（WebSocket、HTTP/2 流、CONNECT / Capsule 隧道、流式响应）保持单向线性通道互斥（心跳超时仅在读活跃期驱动，写超期独立判定失活；HTTP/1 按需启用 `cancel_on_peer_eof()`）；SSE 区分 `data` 缺省与空串并单遍预留多行容量；`CONNECT-UDP`（RFC 9298/9297）强制 `Capsule-Protocol: ?1`（仅 `SP` 环绕），未知 Capsule 零缓冲流式跳过；WebSocket（RFC 6455/8441）校验子协议唯一性，HTTP/2 下仅当 `END_STREAM` 落在 `2xx HEADERS` 上时拒绝握手，未分片单帧零拷贝借用，出站以单次 `reserve` 为事务边界，写通道经 `websocket_write_lane` 仅在消息边界间轮询控制帧，`permessage-deflate` 遇 `BFINAL=1` 调 `inflateResetKeep` 且禁压缩时绕过字典更新，拒绝 `1005 / 1006` 伪关闭码；任一方向出错或关闭协同唤醒排空另一方向，连接关闭执行 2 秒有界 Lingering Close（TLS 单向刷出 `close_notify`，TCP 读取丢弃最多 4 MiB 未读输入防 RST）。

---

## 8. Web 与数据访问核心设计

- **控制器、中间件与 Per-Worker 状态**：`controller` 为不继承框架基类的 `final` 类，经 `RUVIA_ROUTES_BEGIN / END` 宏自动注册封存；中间件为满足 `ruvia::middleware` 概念的 `final` 类，`next` 经三态控制块（`fresh / invoked / expired`，单次 arena 合并分配）强制 single-shot，`use_at` 按百分比解码并归一化尾斜杠后的完整路径段匹配；`controller`、中间件与 `use_worker_state` 保持 per-worker（按依赖逆序启停回滚），全局共享唯一不可变路由计划，`test_app` 首次请求驱动真实能力启停（失败进入粘性 `failed` 态）；上下文仅暴露 typed capability。
- **三层响应元数据、流式提交与会话/静态文件**：响应元数据分为消耗型 `draft()`（原位移动接管）、缓冲修改 `edit_response()` 与跨替换 `policy()`（安全首部写入 `policy` 层，HSTS 仅限 `https`），替换不继承旧字段；`http_response_headers` 优先匹配静态表或连续视图零分配，同长原地覆写，跨表合并优先走无交集快路径；一旦 `output_committed`（流式/升级），下游异常必须强制重抛以关闭连接，严禁将截断流当作完整响应；服务端 `session` 在响应头 commit 时自动轮换 ID（连续 `clear()` 保留首次 `old_id_`，发送后禁改）；静态文件构造期建立不可变内存索引，禁二次百分比解码（防 `"%252E%252E"` 逃逸），打开非常驻文件时校验 `device / inode / mtime` 防篡改。
- **编译期 Model Schema 与 ORM**：`model_type` 采用编译期 schema，解析、校验与序列化分离；必填、可选（`RUVIA_OPTIONAL_FIELD`）、可空（`RUVIA_NULLABLE`）、默认值（`RUVIA_DEFAULT`，仅作用于缺失字段）与“不修改 vs 清空”语义正交独立；拒绝 `bool`/字符充当整数，无转义非字符串标量表单直接原地解析视图；ORM 与直接查询路线独立，映射结果递归拥有数据，分页关联集合加载（`prepare_entity_read`）先按根主键子查询选定分页根 ID 并复用外层 `WITH` 绑定。
- **数据库驱动、事务与迁移**：建连前异步预解析地址列表（PostgreSQL 生成等长 `host / hostaddr` 平行列表跳过 libpq 同步 DNS，首次 poll 前等待可写，TLS 后核验 `PQsslInUse == 1`）；预处理缓存 `db_statement_cache` 实行“二次命中才 prepare”与 128 条代际 LRU 淘汰；PG 在 `PQflush` 期间同步调 `PQconsumeInput` 防死锁，陈旧语句（`26000 / 0A000`）仅在事务外透明重跑一次，Pipeline 流式查询用 `PQsendPipelineSync` 复位 `SingleRowMode`；MariaDB 拒绝 `LOCAL INFILE`，读毕立即释放 >64 KiB 宽列缓冲，丢弃未缓冲 `MYSQL_RES` 前先置空 `handle` 防阻塞；事务独占连接且禁读写共享查询缓存（失败先释放缓冲再异步 `ROLLBACK`），`db_stream_result` 在四个出口精确归还连接一次；编译期 `fixed_string` SQL 校验 NUL、方言与占位符数；迁移限单条语句、校验历史篡改且只追加。
- **Redis 与查询结果缓存（`RUVIAQC2`）**：常规池与独立阻塞命令池物理隔离；配置 `username_` 时无口令也发三元 `AUTH <username> ""`；写缓冲按 $4\times$ 请求字节摊销保留，无流水线余量且超 `maxbuf` 时立即 `sdsRemoveFreeSpace`；有界阻塞命令按“服务端时长 + 宽限期”武装截止期，订阅连接先 `link` 入链再 `arm` 取消令牌；RediSearch `NUMERIC` 校验绝对值 $\le 2^{53}-1$ 并保留 `__ruvia_` 影子前缀，ORM Lua 脚本写前先校验类型、归属标记与必填字段；`RUVIAQC2` 查询缓存对八元组作用域算 SHA-256，按 `db_cache_shape` 分键隔离失效，解码 `reserve` 前强制校验 `columns <= remaining / 8` 与 `count <= remaining / (columns + 1)` 防预分配 DoS，遇取消或停机立即上抛。
- **出站客户端（`http_client / websocket_client`）**：可独立绑定事件循环，启动前固定 outbound origin；构造期预解析字面量 IP 并支持 Happy Eyeballs（RFC 8305，250ms 阶梯交错并发），TCP 策略应用失败直接报错；强制执行两级结果内存预算（`max_retained_bytes_ / max_in_flight_bytes_`，超限立即调用 `discard_response_body()` 释放已缓冲前缀），`request_timeout_` 覆盖从取槽到响应体排空全周期；`ORIGIN / Alt-Svc` 通告仅作只读观测且独立引用计数延迟自毁；重定向按 RFC 3986 §5.2.4 保留空路径段，已复用 HTTP/1 连接遇 `io_error` 仅在“幂等方法、非流式、非隧道、未收响应头且未放弃”时透明重试一次。

---

## 9. 文档与验证交付规范

- **`README.md` 规范**：只作使用入口（定位、三库职责与 CMake target、前置条件与平台要求、最短快速开始、示例链接与许可证），不作 API 手册或内部实现文档；同一事实以 `CMakeLists.txt` 与 `vcpkg.json` 为准只维护一处，仅在影响入门接入或功能选择时同步更新。

| 验证维度 | 规范与门槛 |
| :--- | :--- |
| **本地构建与测试** | 复用根 `build/`，按影响范围并行构建（`cmake --build build --config Debug -j <cores>`）、运行单测并走通真实 E2E 改动路径；不无故清缓存或全量重编译 |
| **临时验证产物** | 严禁占用 `/tmp`（防内存文件系统 OOM）；临时源码、smoke 程序、日志与中间产物统一置于根 `build/` 下，验证后立即清理 |
| **单元测试边界** | 只经非 `detail` 公开 API 验证行为、边界、状态转换与错误语义；保持确定性与内存隔离，不测内部实现、源码文本、单纯转发、无意义默认值或 mock 回声 |
| **缺陷与数据后端** | 缺陷先复现后验证并补充公开 API 回归；生命周期改动覆盖成功、异常、取消与内存回收；SQL、迁移与 Redis 变更在真实后端验证（含事务回滚） |
| **代码格式与 CI 门槛** | LF 行尾，C++ 源码经 `clang-format --dry-run --Werror` 检查；Linux Musl（`clang-cross`）与 Windows MSVC（`x64-windows-static`）均以 C++20、`Release`、`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` 全功能通过全部测试 |
| **纯文档与性能实测** | 纯文档改动只做文档校验；性能结论必须在专用主机同条件交错实测，微基准不代表端到端吞吐 |

- **仓库卫生与交付闭环**：示例与测试按 target 和协议层级归档，`core` 单测平铺；无关改动导致失败时保留现场并如实报告；每轮修改验证后仅提交并推送本任务改动（不夹带无关修改、不强推、不擅自操作 Tag/Release），推送后不等待 CI 完成（未取结果时标注“CI 未核对”）。
