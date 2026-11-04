# AGENTS.md

默认中文回复。文档用中文，路径、命令、标识符保留原文。
本文件只记录规范、边界与核心设计，不作记事本；仅在全局规范、架构边界或核心设计变化时更新，不记录单点修复与临时验证过程。`README.md` 是使用入口，遵守下文 README 规范；代码风格、命名、头文件分组与异常处理统一遵守 `STYLE.md`。
规范覆盖整个项目：`ruvia-core`、`ruvia-http`、`ruvia-web`、`examples` 和 `tests`。

## 工程原则与内聚解耦

- 修改前检查 `git status`，保留用户及并行任务的已有改动，不回滚或夹带无关修改。
- 正确性优先，其次长期可维护性与实测性能。围绕调用主链、状态不变量、失败路径和资源回收实现完整需求，采用最小、清晰、可验证的方案。
- 新增设计只服务现有调用方和明确需求，不预设扩展点；不为单一用法引入模板参数、策略、配置项或可插拔接口。两个及以上真实用法共享的逻辑才抽象，用具体类型和明确契约表达；禁止引入无职责转发包装，也不将同步操作或非事务操作伪装成异步流程或事务。
- 高内聚、低耦合、职责单一，模块依赖保持严格单向无环：
  - 同一领域状态机或协议阶段的代码保持内聚，避免拆成碎片文件或跨多层胶水转发；大型状态机仅在按正交阶段（如 `connect` / `read` / `write` / `heartbeat` / `close`）拆分 `.cpp` 或 `.inl` 时共享同一内部状态头，且每个实现片段必须显式包含自身直接使用的头文件，不依赖兄弟 `.inl` 或预编译头的隐式传递。
  - 新增同类能力统一入口，共用唯一主链、状态机和生命周期，共同逻辑只有一份权威实现，已有的公开等价形式在内部复用它；禁止通过复制代码、平行维护多套解析/校验逻辑、引入无独立语义的中间 Flag/DTO 搬运结构体、共享可变状态或薄包装掩盖重复，差异只在明确的策略或边界点处理。
  - 服务端请求、进程内子请求（subrequest）与内存测试门面（`test_app`）复用同一套权威请求准入（admission）、路由分发与响应尾部管线（CORS、内容编码、错误恢复），不维护平行分叉路径。
- 严格区分「编译期 → 启动期 → 请求期」三阶段职责：
  - 编译期完成 schema、路由元信息、模板化中间件配置、概念约束、SQL 字面量占位符与类型关系校验；
  - 启动期一次性完成配置与凭证加载校验、路由查找计划（动态路由树静态段直接以 `std::string_view` 借用不可变 `route_entry` 路径并在建树时一次性绑定参数名）、中间件链、静态目录全量索引（记录目录级 `has_sidecars` 等全局存在性特征）、outbound origin 池与监听矩阵构建，启动后封存为只读结构；
  - 请求期热路径只做无锁查表、确定性状态机推进与零拷贝 I/O。
- 对象生命周期与状态机不变量：
  - 构造即就绪（Construction Is Ready）：不引入半初始化状态或无意义的二次 `init()`；构造中途失败必须通过 RAII 完整回滚已获取的上游内存、描述符或注册项，不暴露半成品对象。
  - 宿主固定与值移动分离：持有地址稳定运行态、在途 I/O、侵入式节点或被回调/协程借用 `this` 的宿主类型必须显式 `= delete` 拷贝与移动；纯所有权转移的值或冷任务类型支持 `noexcept` 移动，移动后源对象保持可安全析构的确定性空状态，且移动操作须防范源与目标存储自包含重叠。
  - `const` 查询纯洁性、失败原子性与精简状态：移除未被状态机真正依赖或与外层队列重复的布尔标志与无意义的单布尔位域；当只读配置与 worker 局部运行态（如 SNI 主机名与 TLS 上下文）等长一一对应时，直接按平行数组同下标索引，禁止额外维护冗余的 `(view, ptr)` 查找向量；`const` 成员函数绝不修改可观测状态或夹带副作用；输入因背压、超限或格式错误被拒绝，或多步字符串/语句构建器（如 `procedure_append_guard`）中途抛出异常时，必须保持强异常安全原子性（零消费或自动 `resize` 截断回滚至初始长度，不破坏已排队输出与宿主不变量）。
- 作用域策略遵循“只紧不松（Narrower Scope May Only Tighten）”与独立叠加原则：更窄作用域的限制（如路由级 `body_limit`）只能收紧全局上限；多层声明的限流规则（全局、控制器、路由及同路由多重规则）各自独立计数并同时生效，不互相覆盖或混用计数器。
- 避免深层 lambda 嵌套：lambda 只承载短小局部回调，复杂控制流和多阶段异步流程提取为具名函数或协程，明确捕获范围、所有权与生命周期。
- 同一阶段内可安全复用的计算、解析与查询结果只获取一次（入口处一次性完成时钟/整数边界归一化并向下游传递受检类型；内部数值比较直接构造归一化视图如 `format_clock_decimal`，禁止“先格式化为文本再重新扫描解析”的回路；词法扫描阶段一次性完成 UTF-8 与转义校验并标记 `literal`/`escaped`，后续解码阶段对 `literal` 直接拷贝、对 `escaped` 按转义点分段解码而不二次扫描验证；JSON 对象/数组与请求首部准备采用按需单遍流式消费或按上界单遍写入，禁止“先预扫描计数/缓冲切片再二次遍历”；路由查找通过 `excluded_mask`/`initial_mask` 位掩码剪枝复用已探测方法结果，避免 `405`/`OPTIONS` 重复探测）；不跨状态变化、一致性边界或对象生命周期盲目缓存，也不为减少调用破坏重试和取消语义。支持局部变更的资源，在不破坏原子性、一致性和失效语义的前提下，优先只更新受影响目标，保留仍有效的运行态与缓存。
- 组合优于继承。继承仅用于标准库与 Asio 契约：异常层次、`std::pmr::memory_resource`、`std::error_category`、Asio service、实际使用的 `std::enable_shared_from_this` 和 `std::integral_constant` 谓词。框架扩展用概念和宏。
- 公开 API（各库 `include/` 下非 `detail` 的类型、函数、宏和配置项）已冻结，服务仓库外使用方，必须能调用本库提供的全部能力：`src` 与 `detail` 只承载实现细节，不承载只能在库内使用的能力。不得删除、收窄、改名、合并或改变现有公开 API 签名与契约。
- 后续即便要求破坏兼容性的重构或设计调整，也必须在不改变公开 API 的前提下仅针对内部实现（`src` 与 `detail`）进行；内部改造一次到位，彻底清理旧内部实现、无职责转发、死代码或临时 TODO。

## 分层与架构边界

|库目录|公开 Target（统一顶层 `namespace ruvia`）|职责与依赖边界|
|---|---|---|
|`ruvia-core`|`ruvia::core`|独立运行时底座：协程任务、事件循环、定时器、有界通道、连接槽租约调度（`pool_lease_scheduler`）、阻塞卸载池与 PMR 内存池；依赖 standalone Asio、Threads 与 mimalloc，不含任何 HTTP/Web、TLS、压缩或数据后端语义|
|`ruvia-http`|`ruvia::http`|独立 sans-I/O 协议引擎：HTTP/1.0、HTTP/1.1、HTTP/2、HPACK、内容编码（zlib/Brotli/zstd）、multipart、Range、条件请求、Cookie、WebSocket 握手/帧协议与 Capsule 数据报；不依赖 `core`、Asio、socket、定时器、OpenSSL 或 mimalloc|
|`ruvia-web`|`ruvia::web`|应用框架与 I/O 驱动：路由、中间件、Model/ORM、静态文件、限流、Session、TLS/crypto（OpenSSL 3.0+）、服务端/出站客户端网络驱动、HTTP/WebSocket 隧道及数据库/Redis/JWT 接入；依赖 `ruvia::core` + `ruvia::http`，消费 HTTP 层协议计划驱动 I/O，不复制协议判断或将 Web 语义下沉|

- 使用 C++20。源码只放在三个库、`examples` 和 `tests`。
- 各库自带 `CMakeLists.txt`、`include` 和 `src`，只拥有自己的源码、预编译头（`src/pch.h`）和公开命名根。`src` 最多一层职责目录；`ruvia-core/src` 保持扁平。
- 跨库只用非 `detail` 公开 API，严禁穿透内部头、跨组件 `detail` 头或相对物理路径。需要跨库共享的通用辅助能力由所属下层库公开提供。
- 公开头文件保持最小包含闭包（Include Firewall）：核心入口头（如 `context.h`）对独立子系统句柄优先使用前置声明，由实际消费该能力的编译单元按需包含定义头，避免全量头文件耦合传染。
- sans-I/O 与网络驱动边界：
  - `ruvia-http` 只产出确定性的解析结果、协议计划（表示响应计划、首部校验、重定向改写计划、帧编码与流控预算消费）和状态机转换，不持有 socket、定时器、协程挂起或执行系统文件 I/O（文件响应体仅通过不透明的 `http_response_file_identity` 与 `uintptr_t + release` 函数指针封装的 `http_response_file_handle` 传递已打开原生句柄）；
  - `ruvia-web` 严格消费 `ruvia-http` 的协议计划驱动读写、超时、重连与跨源策略，直接复用构帧时已打开的同一原生文件句柄发送文件响应（避免二次解析路径及 TOCTOU 替换风险），不在 Web 层二次解析协议语法或绕过协议状态机拼接报文。
- 构建、链接与安装闭包：
  - 根 `CMakeLists.txt` 统一管理第三方依赖查找与 `ruviaConfig.cmake` 的 `find_dependency` 导出，确保构建期与安装消费期依赖完全一致。
  - 非 Windows 下 `ruvia-core` 以 `WHOLE_ARCHIVE` 链接启用 `override` 的静态 `mimalloc-static`（消费方不得混用其他全局分配器替换）；Windows 统一使用 MSVC 静态运行时（`MultiThreaded$<$<CONFIG:Debug>:Debug>`）与普通 mimalloc。
  - 可选特性（`MARIADB`、`POSTGRESQL`、`REDIS`、`JWT`）关闭时，同步剔除对应实现源码与安装头文件目录（`db/`、`redis/`、`auth/`）；安装只包含启用特性的公开头及其真实传递引用的同组件 `detail` 头文件闭包。

## 安全、类型防线与生产鲁棒性

- 歧义即拒绝（Fail Closed）：消息边界、目标路由、编码格式或访问权限有歧义时一律拒绝，不猜测或容错放行。无根路径、点段逃逸、空字节、非法百分比编码及非 `http(s)` 的 absolute-form 目标按安全边界直接拒绝；连接无法承载目标 origin 时按 RFC 9110 §7.4 返回 421。
- 编译期借用边界、左值限定与悬垂防护：
  - 跨当前表达式留存的零拷贝文本或连续序列视图（如配置项、消息首部、Cookie 属性、路由/查询选项、WebSocket/隧道参数、字节转换 `as_bytes` / `as_chars`），必须通过 `borrowed_text`、`borrowed_span` 等受检视图类型在编译期拒绝绑定临时拥有型容器（rvalue `std::string`、`std::vector`、`std::initializer_list` 等）；返回对象内部存储借用的访问器标注 `RUVIA_LIFETIMEBOUND`；`move_only_function` 的引用返回签名严格要求目标直接返回兼容引用，禁止从按值返回或隐式转换临时量生成悬垂引用；
  - 被异步操作、子协程节点、定时器或取消注册借用自身地址的宿主（如 `task_scope`、`slot_pool`、`worker_memory`、`request_memory`、`session`、`env`），其构造参数（如拒绝右值 `worker_handle&&`）与成员操作（如 `spawn`、`join`、`allocator`、`resource`、`fork`、`data`、`get`）必须使用左值引用限定（`&` / `const&`）并显式 `= delete` 右值重载，从编译期阻断临时宿主悬垂。
- 原始整数域受检转换、时钟单调性与防溢出：
  - 公开辅助函数与边界入口对任意输入安全，检查边界、溢出及输入输出别名（如 `http_base64_encode` 通过计算正向/反向分组分界点支持任意偏移内存重叠与原地编码，解码严格区分必选填充与禁止填充）；数字文本解析（`parse_integer`）严格拒绝空白、前导 `+` 与尾随字符，且格式错误优先于溢出报告；
  - 端口、状态码、`Content-Length`、HPACK 索引与动态表容量、协议字节预算（`protocol_byte_limit`）、查询行数、Redis 批量计数、WebSocket 关闭码、HTTP 优先级 urgency 及超时/容量参数，必须在窄化或转为无符号类型前的**原始整数域**完成符号与范围校验（如 `std::in_range`），严禁隐式转换导致负数缠绕为超大无符号上限或高位截断改变目标语义；
  - 可配置时长和字节大小参与运算时统一使用饱和算术或减法分段比较（`worker_timer_saturating_duration_cast` 保持整数全因子精确换算，浮点转换将 `NaN` 映射为 `0`、无穷大饱和至边界）；所有超时、限流窗口、连接扫描与耗时统计统一使用单调时钟（`std::chrono::steady_clock`），日历时钟仅用于协议时间戳（HTTP Date、Cookie、JWT），防止系统时钟回拨导致超时失效。
- 配置、会话、静态文件与 TLS 安全：
  - 配置或持久化数据损坏必须显式报错，不得用默认值静默掩盖；类型化环境变量查找（`env::get<T>`）严格区分变量未设置（返回 `std::nullopt`，允许 `.value_or()` 回退）与已设置但格式非法（立即抛出 `std::invalid_argument`，防止拼写错误被默认值静默掩盖）。
  - 缓存键或命名空间须完整区分影响结果或访问权限的资源、表示（如编码变体）及身份维度；允许调用方自定义时，公开契约须说明隔离与失效责任。
  - 限流对客户端地址做规范化归并（无作用域 IPv6 按 `/64` 聚合、IPv4-mapped IPv6 归并至对应 IPv4、作用域 IPv6 包含数值 scope），防止同网段地址变形绕过配额。
  - 服务端 `session` 仅接受定长合法格式 ID，每次非空写入在响应头 commit 时自动轮换 ID 防会话固定攻击，替换已过期或被吊销会话返回 409；一旦响应头（含流式、WebSocket 或 `CONNECT` 握手头）开始发送，任何会话写操作立即抛出 `std::logic_error`。
  - 静态文件服务在构造期归一化根目录并建立不可变内存索引（支持按预算常驻高频小文件），默认拒绝隐藏路径段（`.` 开头或含 `/.`）、符号链接越界与非普通文件；非常驻文件在打开读取时校验底层身份（device、inode、mtime），磁盘文件被修改时严格 fail closed 报错拒绝以防按旧长度发送截断/错位内容；后台刷新在 worker 外全量构建并在请求间隙原子切换，构建中途任一文件系统操作失败则完整保留旧快照。
  - 代理转发首部（`Forwarded` / `X-Forwarded-*`）仅在可信代理（`trusted_proxy_config`）下采信（同时存在时 `X-Forwarded-*` 优先于 `Forwarded`，合并同名多行并从右向左逆序定位首个不可信跳作为客户端边界，且仅采信该边界跳上的 `proto` 以防客户端在左侧注入伪造 IP 或 HTTPS）；CORS 校验遇到畸形或重复/多值 `Origin` 与 `Access-Control-Request-Method` 时绝不生成跨源授权首部（严禁从多值 `Origin` 中挑选其一放行），动态跨源响应自动追加对应 `Vary` 令牌防缓存污染；Cookie 签名、JWT 验证等安全凭证对比使用常量时间比较，JWT 强制校验满足 RFC 7518 的 HMAC 最小密钥字节长度并默认要求 `exp` 过期声明。
  - TLS 安全基线（RFC 9846 / RFC 9525）：证书、私钥与 CA 在注册时一次性加载校验，私钥口令副本在返回前主动擦除（cleanse）；仅协商 TLS 1.2（强制 ECDHE + AEAD，拒绝重协商）或 TLS 1.3；证书主机名严格匹配 `subjectAltName`（`DNS-ID` 最左完整标签通配、`IP` 精确匹配，禁用 `Common Name`）；连接发出的 TLS 记录总数受 `max_outgoing_records_` 硬上限约束（`KeyUpdate` 不重置计数，达限即终止连接以符合 RFC 9846 §5.5）；Ticket 密钥定期轮换并销毁过期密钥，跨 worker 会话恢复严格绑定原 `server_name`。
- 跨平台 UTF-8 与输出脱敏：对外文本及传递给 OpenSSL / 数据库驱动的文件路径统一使用合法 UTF-8，禁止在 Windows 下经 ANSI 代码页窄化；Windows 下 `std::system_category()` 与 `asio::system_category()` 错误消息必须经 `FormatMessageW` 转为 UTF-8（`error_code_text` / `system_error_text`），防止本地 ANSI 编码污染 JSON 与日志；受限长度文本或校验诊断按字节上限截断时必须回退半个多字节序列以保持 UTF-8 码点完整。对外错误响应不回显内部异常、SQL、宿主路径或上游细节，服务端脱敏后记录诊断原因；日志、代码与制品不得包含真实凭据。
- 故障隔离、不变量恢复与兜底限流汇聚：对端正常断开、自身停机或排空引起的中止属于正常结束，不按故障上报或计数；真实异常必须上报，不得静默吞掉。请求入站传输自身的 I/O 失败（如发送 `100 Continue` 或临时响应头时对端已断开）直接穿透回会话层终止连接，绝不转入应用错误处理器伪造无法送达的响应；路由异常捕获边界通过单次 `rethrow_exception` 一次性提取错误码与响应头元数据（如 `Accept-Encoding`、限流 `Retry-After`）。无调用方重抛的后台/回调异常统一路由至 `report_unhandled_failure`（内置速率限制并将被抑制计数随下一条放行诊断一并输出，通过 `equivalent_failures` 将多单元观察到的同一根因合并为单行汇报；生命周期 owner 重抛其一时仅上报未重抛的其余异常）。单次请求、单个 HTTP/2 流、单条连接或单次后端调用的超时、取消与协议错误严格隔离在当前作用域内回收，不得破坏 worker 状态机或升级为进程故障；可恢复异常捕获边界保留的对象必须维持完整不变量。

## 协议实现与审查

- HTTP/1.0、HTTP/1.1、HTTP/2、TLS、WebSocket、HTTP 隧道（含 CONNECT-UDP / Capsule）、缓存、条件请求、范围请求、压缩、DNS 等协议及其扩展的实现必须符合适用的现行 RFC，以正文、适用更新及已验证勘误为准。HTTP 参照 RFC 9110/9111/9112/9113，TLS 1.3 参照 [RFC 9846](https://www.rfc-editor.org/info/rfc9846/)（取代 RFC 8446）。
- 区分 MUST、SHOULD、MAY 及发送方、接收方、中间代理的义务，不把发送方的 MUST NOT 自动变成接收方的拒绝规则。代应用构建请求的组件独占协议托管首部（`Host`、`Content-Length`、`Transfer-Encoding`、`Trailer`、`Expect`、`Connection`、`Keep-Alive`、`Proxy-Connection`、`TE`、`Upgrade`），拒绝应用层直接传入这些首部或首尾含 SP/HTAB 的字段值；出站首部显式区分本端生成（`generated`）与代理转发（`forwarded`，RFC 9110 §2.2：保留收到的 `Content-Type`、`Origin` 与 CORS 请求值并容忍 `Content-Encoding` / `Upgrade` 中的合法空成员，但绝不放宽字段字节安全、单例首部计数或写入器所有权，且按 §7.6.1 剥离/重建逐跳首部）。按规范忽略、容忍或规范化输入；未知但合法的扩展不等于格式错误，不支持的能力按协议处理。
- 涉及协议义务的代码注释与变更说明应标注具体适用的 RFC 条款；内部约束不得冒充 RFC 要求，偏离 `SHOULD` 须说明理由。
- 校验限于协议语法、分帧、路由、安全边界和明确资源限制，不以“更严格”代替正确性。产品限制单独说明，不冒充 RFC 要求，也不为字段数、分块数或消息数另设无依据限制。
- 沿「请求解析 → 正文分帧与解码 → 响应计划与序列化 → 网络驱动与生命周期回收」完整主链审查，不只检查孤立函数。
- HTTP/1 与通用表示语义：
  - 重点核对 `Host` 与请求目标（`http_authority_view` 拒绝 `userinfo` 并严格区分端口缺省、空端口与显式数值端口；默认端口未知即 `0` 时，缺省/空端口绝不与显式 `:0` 相等，非法 authority 永不相等）、`Content-Length` / `Transfer-Encoding` 冲突与优先级、重复字段与列表语义，以及 `HEAD`、1xx、204、205、304、`CONNECT`、`Upgrade`、`Expect: 100-continue` 的特殊行为（客户端无正文禁止发 `100-continue`；HTTP/1.0 接收方忽略 `100-continue` 但保留 `unsupported` 扩展事实；服务端同时收到 `Upgrade` 与 `100-continue` 时按 RFC 9110 §7.8 必须在 `101` 前先发送 `100 Continue`；通用 1xx 临时响应排除 `101`，`101` 仅由专用协议升级驱动边界生成；若 1xx 含 `Connection: close`，按 RFC 9112 在刷出后立即关闭连接；`CONNECT` 与 `TRACE` 接受显式零长度分帧但拒绝非空正文，而 `OPTIONS` 正文要求 `Content-Type` 仅约束发送方 RFC 9110 §9.3.7，入站准入不因缺失 `Content-Type` 拒绝）；
  - HTTP/1 客户端请求准备阶段由写入器原子决定 `Expect: 100-continue` 发射与正文门控（`continue_gated`，门控等待超时归属 I/O 运行时策略），并将进入线面计划的协议事实打包为自包含状态（`http1_client_exchange_state`）移交响应解析器，使响应解析彻底脱离调用方请求方法与首部存储的生命周期且常规请求零分配；流式请求正文写入（`http1_request_content_writer`）通过栈内联 `prefix`、零拷贝借用 `payload` 与常量 `suffix` 生成散集（scatter-gather）分片，定长正文也必须经 `commit_finish()` 校验总长度精确匹配（上传完成前收到最终响应或传输失败必须 `abort()` 而非发隐式结束帧）；携带正文的客户端请求必须在通知外部已完整写出预备请求正文后才可接受 `101`，且 `1xx` 的 `allow_reuse` 仅允许当前交换继续等待最终响应而不提前归还连接池；客户端响应头保留原始状态码（`received_status()`）但将收到的 `600..999` 按 RFC 9110 §15 映射为 `500` 处理，且 `HEAD` 响应（含 `205`）的 `Content-Length` 仅作元数据而不作为接收正文长度；
  - 增量处理覆盖任意切分、EOF、输出缓冲不足、chunk 扩展、trailer、消费量及流水线残余字节（HTTP/1 流水线仅在后续请求无请求正文且匹配缓冲响应路由时才允许在前序响应待写出期间继续执行 `http1_request_follows_held_output`）；multipart 增量边界查找维护最多 3 字节前缀重叠（`search_cursor`）与传输填充（`padding_cursor`）双游标断点续扫（未结束 preamble 末尾的 `"\r\n-"` 暂不计入配额直至下一字节消歧）；请求体分帧计划（`http1_request_body_plan`）仅由解析器构造（`chunked` 请求体即便解码后为空也必须消费终止零块才算消费完成，未消费完请求体按 RFC 9112 §9.3 严禁复用连接）；请求连接持久性计划（`http1_request_connection_plan`）在解析时确立初始倾向，后续策略只能单向收紧为关闭（`require_close()`）；明文监听器在请求行失败时仅当首行含 HTTP 版本标识才回 `400`，对非 HTTP 流量直接静默丢弃并关闭连接；逗号列表与分号参数统一复用唯一引号感知扫描器（`http_find_unquoted_delimiter`，识别 `quoted-string` 与 `\"` 转义），但 `Cookie` 解析按 RFC 6265 严禁赋予 `"` 引号包裹语义；
  - 首部与 `Trailer` 节字节预算（`http_field_section_budget`）统一按规范化行开销（`name + value + 4`，与收到的 OWS 长度无关）及终止空行计入 `max_http_header_bytes`（计费失败保持原预算不变）；请求 `Trailer`（`http_request_trailers`）独立于初始请求头存储并按全有或全无追加，禁止字段（RFC 9110 §6.5.1）在显式追加或 HTTP/2 下直接拒绝，而在 HTTP/1 接收时计入字节预算后按 RFC 9112 §7.1.2 丢弃不保留；
  - 响应 `Trailer` 与响应头计划：响应 `Trailer` 通过 `validate_http_response_trailers` 先行构造借用证明（`http_response_trailer_section`）；最终响应头分帧（`http1_response_head_plan`）使用互斥变体（缓冲定长、`chunked` 流、关闭定界流，关闭定界流剥离应用层 `Content-Length`/`Transfer-Encoding`），流式响应对绝不允许正文的状态码（`1xx`/`204`/`205`/`304`）拒绝 `trailer` 意图，对其他不支持 trailer 的分帧（如 HTTP/1.0 关闭定界，严禁向 HTTP/1.0 发送 `chunked`）则静默丢弃 trailer；强制标记关闭连接（`http1_mark_connection_close`）时合并重复 `Connection` 首部并保留应用声明的自定义逐跳字段名；响应头序列化（`append_http1_response_head`）优先保留显式 `Date`（未设置时复用缓存时间）且从同一十进制 token 测量并写出 `Content-Length`；
  - 条件请求、缓存与内容协商：条件请求遵循 RFC 9110 §13.2.2 优先级，重复的 `If-Modified-Since` / `If-Unmodified-Since` 首部行按无效日期条件直接忽略（不取末行生效），日期形式的 `If-Range` 要求强验证器且时间戳精确相等；HTTP-date 解析兼容 `IMF-fixdate`、RFC 850 与 `asctime`，写出严格固定为 29 字节 `IMF-fixdate`；`Cache-Control` 跨多行首部作为单一指令列表增量解析，重复的新鲜度 delta-seconds 指令严格保留首次出现的值；`Accept` 匹配（`http_accept_match`）跨多行零分配累积，高特异性覆盖低特异性（含 `q=0` 排除，非空参数即使在 `q` 后也增加特异性，token 模式拒绝未知参数）；`Accept-Encoding`（RFC 9110 §12.5.3）区分首部缺失（接受任意编码）与显式空值（仅接受 `identity`），`gzip` 与 `x-gzip` 共享权重，`identity` 默认权重为 `1000`（除非被 `*;q=0` 排除）；`Range` 解析使用有界定长零分配集合并按 RFC 9110 §14.2 / §15.3.7.2 自动合并重叠或相邻区间（保留首成员接收顺序，不重复发送表示字节）；携带 `Content-Encoding` 的表示遇到多段 `Range` 时按 RFC 9110 §8.4 / §14.6 回退完整表示（单段仍返回 206），`If-Range` 命中授权的 206 按 RFC 9110 §15.3.7 精简非必需表示首部；
  - Cookie 语义与 `Set-Cookie`（RFC 6265 / RFC 6265bis）：强制校验 `__Secure-`（需 `Secure`）、`__Host-`（需 `Secure`、`Path=/` 且无 `Domain`）与 `SameSite=None`（需 `Secure`）；删除 Cookie 使用 Epoch 0 `Expires` 而非 `Max-Age=0`（符合 RFC 6265 §4.1.1 非零首数字约束）；400 天寿命上限仅在客户端 Cookie Jar 封顶而不限制服务端发送；`Set-Cookie` 解析（RFC 6265 §5.2）零分配借用响应头（`Expires` 独立遵循 cookie-date 语法）；请求 `Cookie` 拼接（`append_cookie_request_pair`）支持输入借用目标缓冲（扩容后重定位偏移并先写键值对再写前导 `"; "`）；URI 引用修复（`percent_encode_uri_reference`）保留合法百分比三元组不二次编码（RFC 3986 §2.4），无需转义时零分配返回原视图，需转义且输入与输出缓冲存在地址别名时安全处理。
- HTTP/2 与 HTTP 优先级：
  - 重点核对连接前言与 `SETTINGS` 同步、流状态机与并发/流控窗口、HPACK 动态表上限与伪首部/普通首部顺序（首部列表按 RFC 9113 §6.5.2 包含伪首部每字段 `+32` 字节计费）、帧长度与填充/优先级约束、`GOAWAY` / `RST_STREAM` / `PING` 及 Extended `CONNECT`（RFC 8441），严格区分流级错误与连接级错误（RFC 9113 §5.4）；
  - 出站 HPACK 编码不插入动态表条目（容量缩减直接置零）；`enable_push_` 与 `ORIGIN` 帧（RFC 8336，仅限认证 TLS 连接，禁用 `h2c` 或显式代理；解码时忽略语法非法的单个 origin 但对长度截断条目拒绝整帧）仅限客户端启用，接受 `PUSH_PROMISE` 不自动确立传输权威，驱动在消费推送响应前须对照已认证连接校验推送 URI 的 origin；`ALTSVC` 帧（RFC 7838）在流 0 携带显式 origin，在请求流携带空 origin 以继承请求 origin；
  - HTTP 优先级（RFC 9218 / RFC 8941）本端构造严格校验 `0..7`，对端字典解析忽略未知成员、参数类型错误与越界值（按线序后出现的成员覆盖前值，缺省响应参数保持未设置以便代理合并）；接收数据流控信用（`http2_received_data_credit`）为线性令牌（显式 `acknowledge()` 或析构均无异常归还精确字节，支持同流 `merge()`；入站请求体队列 `http2_sans_io_body_queue` 采用 `queued_chunk_` / `active_chunk_` 双缓冲拼接以保证 `enqueue` 不使已借出的 `pop()` 视图失效，仅在字节成功追加后才接管信用并在上层消费完毕后经 `release_active_credits()` 归还窗口）；请求头事件（`http2_request_head_event`）兼具流存储租约职责，若同批次输入已携带正文则读取为 `no_content` 以按 RFC 9110 §10.1.1 省去冗余 `100 Continue`；致命连接错误后仍允许无帧释放已持有的请求租约与 DATA 额度，释放失败由后续推进自动重试。
- 全双工与长生命周期会话（WebSocket、HTTP/2 流、CONNECT / Capsule 隧道、流式传输）：
  - 读、写、心跳保活与关闭握手共享会话状态但保持单向线性通道互斥（`body_reader`、`response_stream_writer`、`http_capsule_stream` 均在操作启动时独占对应读或写通道，未启动的冷操作允许并存；`http_capsule_stream` 严禁与原始隧道读写混用，未知 Capsule 类型原样交付上层忽略）；SSE 消息（`sse_message`）区分 `data_` 缺省（不发射 `data` 行，不触发客户端派发 `MessageEvent`）与存在空串（发射 `data:` 派发空负载事件），并强制校验合法 UTF-8 与 `event`/`id` 无非法换行；`CONNECT-UDP`（RFC 9298 / RFC 9297）本端发送强制携带唯一 `Capsule-Protocol: ?1`，接收端将非布尔、`?0` 或重复字段按缺省忽略并拒绝 204/205/206 响应；Capsule 增量解码器对不保留的类型或未知 Context ID 直接零缓冲流式跳过（不受 `max_capsule_length_` 限制，但短于 Context ID 的畸形 `DATAGRAM` 仍保留报错），`feed_one` 在首个完整保留 Capsule 处停止并将未消费后缀归还调用方；
  - 响应完成不等于请求上传完成（如客户端流式请求收到响应 `END_STREAM` 仅关闭接收半边，发送半边仍须显式完成或重置）；复用或排空连接前必须满足请求与响应双向的消息边界和生命周期契约。后续字节归属不明时关闭连接，不继续解析；非 2xx `CONNECT` 的关闭遵守 RFC 9931；
  - WebSocket 握手、读写并发与关闭（RFC 6455 / RFC 8441）：子协议集合（`websocket_subprotocol_set`）对本端配置与客户端发送强制要求 token 唯一且超限拒绝（不截断），服务端接收则容忍并跳过重复提供项；客户端握手由版本无关的 `websocket_client_negotiation` 统一复用子协议与 `permessage-deflate` 选择校验（HTTP/1.1 校验 `101` 与 `Sec-WebSocket-Accept`；HTTP/2 Extended `CONNECT` 校验 `2xx`、无连接特定首部，且仅当 `END_STREAM` 直接落在 `2xx` `HEADERS` 帧上时拒绝握手，若落在后续 `DATA` 帧上则按 RFC 8441 §5 视为已建立隧道的正常关闭）；服务端与客户端复用同一套 123B 定长内联关闭原因（`websocket_peer_close_info`）与写通道让行原语（`websocket_write_lane`：写通道仅在完整消息边界之间轮询挂起读通道的控制帧，绝不打断在途完整消息，且读等待抛异常时自动清除 `reader_waiting_` 并唤醒写方防死锁）；禁用单帧压缩（`compress_ = false`）时同步绕过压缩字典更新以防侧信道泄漏；`websocket_close_code` 严格限制在合法线面状态码空间（拒绝 `1005`/`1006` 等本地伪状态码）；服务端刷出响应 Close 后立即计划关闭传输，客户端则保持传输打开等待对端 EOF（期间丢弃残余输入；客户端 `close()` 独占读/写/关闭三通道且退役前先锁定首个 `abort_reason_`，运行中的 HTTP/2 会话保持底层传输至 `join()` 有界刷出 `GOAWAY` 后才关闭）；
  - 任一方向发生错误、超时或半关闭时，必须协同唤醒并取消或排空另一方向的挂起操作，确保全部读写与定时器操作完成后才释放会话与底层传输。
- TLS 关闭、TCP Lingering Close（RFC 9112 §9.6 / §9.8）与 HTTP/2 排空：关闭连接的 HTTP/1 最终响应与以 `GOAWAY` 结束的 HTTP/2 会话执行有界 lingering close（TLS 发送 `close_notify`，TCP 半关闭发送端，在有界时间与字节预算内排空对端未读输入），防止未读输入触发 TCP RST 抹掉最终响应；空闲、请求头超时与排空关闭等待请求的连接发送 TLS `close_notify`，写超时、传输错误与强制停机直接关闭；HTTP 消息完整性依赖连接关闭时，不得将未认证的 TLS 截断当作完整消息（而已升级的 TLS 隧道字节流按 4 KiB 分块写入并立即 `flush` 密文，且将对端未发 `close_notify` 的 TCP EOF 视为隧道正常结束）。充当中间代理时按协议处理逐跳字段，不得丢失需要保留的重复字段语义。
- 缺陷结论以 RFC 条款和公开行为复现为证据；说明审查覆盖范围，不以已有测试通过宣称“绝对无 BUG”。

## 性能与内存治理

- 请求热路径以零拷贝、零抽象成本为目标：不新增锁、原子争用、类型擦除、共享所有权或无用拷贝（会话/连接级不变的服务绑定如 HTTP/2 `base_services_` 在连接构造期一次性预构建并跨流复用，热路径上下文与服务句柄优先按 `const&` 传递并在目标对象内就地绑定，避免每流/每请求按值拷贝派生临时副本）；显式阻塞卸载是共享所有权的唯一例外。
- 同步快路径优先：常规路径不创建可避免的协程帧或中间缓冲（如 `as_awaitable(task<void>)` 直接复用 `std::exception_ptr` 签名生成 `asio::awaitable<void>` 而不额外分配包装协程帧），能同步完成的步骤（如缓存/静态索引命中、目录无预压缩伴随文件 `has_sidecars` 时的零路径拼接短路、无阻塞解析、就绪连接槽获取、无需阻塞编码或异步错误恢复的响应准备）直接同步完成；启动期一次性完成构建和预计算（如静态文件全量内存索引，确保请求期零 `stat` 系统调用）。
- 侵入式零分配调度与按需惰性分配：worker 内的连接超时跟踪、周期扫描节点（`connection_scanner`）、槽位等待队列与取消注册优先使用内嵌于连接或协程栈帧的地址稳定侵入式节点；取消状态与非必选帧元数据仅在首次被观察或启用时按需分配，未观察路径零开销。
- 栈上小缓冲优化（SBO）、按需拥有化（Copy-on-Unfold/Unescape）与挂起前提前释放：
  - 高频有界短数据与回调（如 `move_only_function` 满足尺寸/对齐且 `nothrow_move_constructible` 的可调用目标、JWT 段解码、已校验的 `multipart_boundary`、小批量键视图数组、流式摘要 `cache_key_hasher` 的栈内联缓冲快路径、原生路径视图在栈定长数组上完成扩展名 ASCII 小写归一化与 MIME 查表）优先使用栈上或内联定长缓冲，小输入零堆分配直接单次完成计算，仅超限时才惰性分配动态上下文或 PMR 存储；多条目内联缓存（如 TLS 证书主机校验缓存）优先采用共享紧凑连续字节池配合 `(offset, size)` 索引，避免稀疏最大长度二维数组浪费缓存行；单线程有界环（`local_ring_queue`）在启动期一次性构造全部槽位，运行期通过 `prepare_push` / `commit_push` / `cancel_push` 两阶段协议实现零分配入队与异常回滚；
  - 协议解析（如 `multipart_part`、首部字段、无百分比编码的路由参数与查询键值）对未折叠、未转义的原始切片直接以 `std::string_view` 借用调用方缓冲，仅对发生折叠展开或转义解码的字段分配 PMR 存储（已解压请求体缓存为 `decoded_body` 供同一请求内重复读取零拷贝复用）；拥有型 `http_header` 检查长度求和溢出并在单次分配中连续存放名与值（已绑定资源的实例赋值时始终保留目标 `memory_resource*`）；`http_request` 仅拥有紧凑首部描述符块而借用底层协议输入，在输入存储退役前复用实例须调用 `reset()` 清空描述符与借用视图；WebSocket 未压缩且无掩码帧支持零拷贝发送（`submit_borrowed_frame`），无上下文接管的 `permessage-deflate` 方向同步复用共享编解码器；
  - 异步操作在构造冷任务完成参数拥有化后，调用方应在 `co_await` 进入网络 I/O 挂起前立即释放不再使用的临时源容器，缩短并发挂起期的内存驻留。
- 预算、背压、防压缩炸弹与防慢速攻击：
  - 框架的长期队列、缓存、连接池等待队列和外部输入驱动的缓冲，须明确资源预算及背压、拒绝或淘汰机制；重试须可取消且受生命周期约束；
  - 每个 worker 强制执行不可禁用的两级实时入站缓冲上限（worker 级与单连接级，覆盖缓冲/流式请求体、隧道读取与 WebSocket 会话，含容量与重分配重叠），超限抛出 `std::bad_alloc`；超出连接上限的新连接在 TLS 握手与协议探测前直接关闭 socket；达到单连接累计请求上限时 HTTP/2 按 RFC 9113 §6.8 两阶段 `GOAWAY` 优雅排空；
  - 流式解码与帧接收（如 WebSocket `feed()` 与 HTTP 内容解压）同时约束待消费输入缓冲上限与解压后消息/正文上限，拒绝时保持零消费且不破坏已排队输出；`Content-Encoding` 限制最大编码层数（超限按 415 拒绝），多层编解码对每个中间表示均强制施加字节上限并在下一阶段完成前立即释放上一阶段缓冲（`gzip` 多成员与 `zstd` 多帧消费至输入末尾，Brotli 拒绝尾随字节，Zstandard 强制执行 RFC 9659 HTTP 窗口上限；可复用 `http_content_encode_context` 在每次调用及异常后彻底重置状态以防跨表示残留）；
  - 服务端连接超时区分基于最后成功 I/O 的阶段无活动超时（`idle`、`initial_read`、`payload_read`、`write`：新连接首个请求及已收到部分字节的请求头按 `reading_initial` 计，已服务过请求的复用连接在等待下一请求首字节期间按 `idle` 计）与不可被慢速涓流刷新的绝对阶段完成截止期（`initial_read_completion`、`payload_read_completion`），排空回调受固定关闭宽限期兜底，防止慢速连接长期滞留。
- sans-I/O HTTP/2 DATA 提交只消费流控窗口和调用方输出额度允许的前缀，未消费后缀始终归调用方；协议层不代存阻塞正文。已编码输出与 socket 写完成仍分别记账。
- 显式 Worker 内存模型与 PMR 分级：
  - 优先值所有权与 RAII。借用视图不延长 owner 寿命，零拷贝不得破坏借用有效期；分配器覆盖对象寿命，结果持有存储直到析构；
  - 框架动态存储通过显式传递的 `worker_memory` / `request_memory` 管理（禁止使用 `thread_local` 隐式全局池），公开启动配置不要求用户提供 PMR；
  - `worker_memory` 无内部同步锁，直接对接进程级线程安全 `process_resource()`（mimalloc 上游，不替换全局默认 PMR，`construct_pmr_object` 在构造抛异常时立即按原尺寸/对齐归还已分配内存）：小对象走 worker 常驻小对象池，中等大块走受 `retained_block_bytes_` 预算约束的对齐精确尺寸块缓存，超出预算立即归还上游；
  - 单次请求和握手使用 `request_memory`（`monotonic_buffer_resource` arena），优先复用连接预留的初始缓冲块，派生独立子操作时通过 `fork()` 获取同 worker 上游的独立 arena；长生命周期连接状态、结果、池化数据以及无请求 arena 的后台投递任务（`web_worker_context`）使用 `worker_memory` 可回收 `pool()`（后台任务分配的存储须在任务作用域结束前于本 worker 销毁）；
  - 异步入口返回前完成输入拥有化和资源归一化（如跨请求复用的查询谓词暂存于进程级 PMR，生成异步操作前归一化至目标 worker 内存池）；拥有型 PMR 集合（如 `entity_rows`）跨不同 `memory_resource` 移动赋值时必须先在目标资源中递归重绑定全部元素后再交换，防止浅拷贝或分配器失配（通过 `consteval` 编译期递归判定字段是否含 PMR 资源 `entity_type_has_resource`，对不含 PMR 资源的纯标量或 `optional<标量>` 直接编译期短路）；
  - 长连接不得累积临时分配，存活数据不得被后续操作提前回收；流或连接内建暂存缓冲（如 HTTP/2 流局部首部/Trailer 编码块、Redis 连接写缓冲与网络输入缓冲 `http_io_buffer`）优先原位复用，使用后若容量超过保留阈值（`http_retained_pmr_string_bytes`）则主动归还大块存储，否则仅清空内容保留小容量；HTTP/1 连接读缓冲（`connection_read_buffer`）在连接中途待处理字节回落时缩容超 `max_http_header_bytes` 的缓冲，归还有界工作集池时保留 ≤16 KiB 容量以免大首部请求反复扩缩，且流水线合并写出通过 `http1_deferred_access_log` 在对应响应头真正写入 socket 后才发射访问日志；`http_io_buffer` 为读取扩容时不零初始化待覆写字节，`consume()` 仅推进游标保持既有可读视图有效，`prepare()` 仅在已消费前缀足以覆盖存活字节时才前移压缩（否则几何扩容摊销开销），多命令流水线追加先校验字节求和溢出并依赖几何扩容保持摊销 $O(\text{total})$，严禁按块重复搬移或逐命令精确 `reserve(size + delta)` 导致 $O(N^2)$ 重分配；worker 在完整扫描周期无活跃交换与 I/O 活动时触发 `trim_when_idle`，归还缓存大块并排空当前线程的协程帧缓存。

## 运行时、连接池与生命周期

线程与连接：

- 生产 `application` 是进程级单例，通过 `ruvia::app()` 获取，不可自行构造、复制或移动；配置与生命周期入口唯一。
- `core` 是线程、事件循环、调度入口和通用 worker 生命周期的唯一 owner（`current_worker_guard` 严禁单线程并发嵌套运行多个 worker；附着外部 `asio::io_context` 时通过自定义 Asio `service` 强制独占 `claim()` 并在 `service::shutdown()` 阶段自动退役 worker 状态，防止句柄暴露悬垂 `io_context&`）。`event_loop_pool` 与 Web 组合复用同一承载，Web 不另建线程、事件循环或启停主链。
- 每个业务 worker 独占一个 standalone Asio 事件循环，在本 worker 上独立 bind/listen 全部监听端点；n 个业务 worker 不另配网络接入线程（阻塞池和信号线程另计）。
- Web 只拥有 worker-local 业务状态、能力、监听器、连接 I/O 和接入/退役策略，通过生命周期扩展点接入 `core`。待接受 socket 从创建起归发起 accept 的 origin worker，全部请求、TCP I/O、TLS、协议定时器和连接生命周期均留在该 worker，不设独立 acceptor、跨 worker 连接交接或连接交接队列。
- 平台监听模型：
  - POSIX（Linux 等）：使用 `SO_REUSEADDR` + `SO_REUSEPORT`，缺失或设置失败则启动失败；每个 worker 直接接受本 worker 的监听器。
  - Windows：每个 worker 使用 `SO_REUSEADDR` 独立绑定监听 socket，全部 ready 后在共享监听句柄矩阵上发起原生 `AcceptEx`，不使用单监听回退（不保证严格均衡，也不阻止其他进程复用端口）。监听 IOCP 收到属于其他 worker 的完成时，只向 origin worker 的预留有界完成槽（容量由 worker 与监听器矩阵确定，不占用普通业务队列）发布结果并唤醒，禁止在当前线程处理或销毁 origin 的 socket、业务状态及 PMR。
- 跨线程调度、唤醒与通道：
  - 跨线程业务调度走有界队列（`worker_dispatcher` 构造期预分配 $2 \times \text{capacity}$ 节点池，`drain()` 锁内整链摘取 `ready` 队列并在锁外批量执行，确保有界准入未满时 `free` 链表永非空且运行期零堆分配），禁止直接操作其他 worker 的连接或状态；`post_factory` 先预留有界准入再同步执行工厂，非平凡 `post` 在调用用户构造/移动前先保存 `worker_handle` 快照以防重入退役清空原句柄；调度入口与事件循环同属一个 owner，入口先于上下文退役；请求期只借用稳定 worker 句柄，显式卸载可复制一次；
  - 跨线程唤醒（`worker_notification`）采用原子锁存与非阻塞原生写，已存在未决锁存时自动合并（`coalesced`）避免唤醒风暴，析构前须确保生产者静默且 owner worker 已显式 `close()` 并 `join` 挂起等待；worker 内侵入式唤醒（`worker_signal`）在构造冷等待操作时即递增 `reserved_waits_` 且仅限未启动态移动，存在未决或预留等待时析构直接 `std::terminate()`；
  - 已持有资源的冷路径异步清理使用 `event_loop::defer_cleanup` 获取退役保护租约，绕过普通业务队列有界准入但保证在事件循环释放物理上下文前执行完毕；已接纳挂起协程的延迟恢复（`defer_resume`）不受有界队列满拒绝，在本 worker 线程上零类型擦除直达事件循环下一轮次；
  - 跨 worker 有界 `channel` 在构造期预分配槽位，预留（`reserved_`）与已入队（`queued_`）合并计入容量上限防超发；单次异步交付（`one_shot`）将停机钩子作为首个成员以确保最后解链，并在互斥锁外执行结果值的移动提取或析构；投递结果（`worker_delivery_result`）在因队列满、关闭或停机被拒时完整保留并向调用方退回未消费的值或可调用对象，等待结果（`worker_wait_result`）在构造时固定结果类别且禁用赋值；跨 worker 投递的数据须持有独立所有权，确需共享可变数据时须明确同步与一致性语义，优先使用不可变快照。

协程、结构化并发、租约与连接池：

- 协程保持 lazy 和 structured ownership。公开协程统一使用单一模板 `ruvia::task<t_type>`（`void` 通过 `task_promise<void>` 复用同一主模板而不单独偏特化，且仅允许右值 `&&` 执行 `co_await`）；异步发起与协程挂起的双向竞争统一由 `suspend_race_state` 裁决（严禁在协程真正挂起前从 `await_suspend` 内联恢复 continuation，且负载移动抛异常时回滚至原状态）；超对齐（over-aligned）局部变量必须存放在独立拥有的堆存储中，禁止直接置于协程栈帧以规避编译器帧对齐缺陷。
- 未启动任务可丢弃；已启动任务必须完成。取消后必须 `co_await` / `join`，禁止销毁挂起帧或静默 detach。`event_loop::start` 启动的 `root_task` 由事件循环持有协程帧直至完成（严禁在所属事件循环线程上同步 `wait()` 自锁，且须在销毁可能借用调用方 PMR 的 moved-from 中间结果对象后才 `publish_completion()`），放弃 `root_task` 句柄绝不销毁挂起帧，未消费异常统一路由至循环的 failure sink。
- 结构化并发（`task_scope` 与 `operation_scope`）：`task_scope` 绑定地址稳定的左值 `worker_handle`，单次 `join()` 启动后立即封闭 `spawn` 准入并等待在途准入及全部子任务退役（析构时若有活跃子任务或未决 `join` 则 `std::terminate()`，未经 `join` 消费的已完成子任务异常转发至 `report_unhandled_failure`）；`operation_scope` 在关闭时先退役冷任务帧，`close_and_join()` 等待已启动帧、线性租约及能力清理回调全部完成（含清理回调重入），且 `scoped_capability_registration` 须声明在宿主负载成员之后以确保先解链再析构负载。
- `stop_token` 只引用一个取消状态（`stop_state` 由进程级 mimalloc 分配，支持最末持有者在任意线程释放；`request_stop()` 在锁外执行回调，跨线程解链等待运行中回调返回而回调自解链绝不自等待）；多源取消由地址稳定的 `stop_group` 显式拥有，其寿命覆盖使用者完成和 join。`operation_options` 派生（`with_options`）替换明确提供的用户 token（不隐式累加），而超时覆盖只允许收紧（tighten）已有截止期；客户端关闭仍通过自身生命周期独立终止操作。外部线程触发的 `stop_token` 回调不得直接操作 worker 局部定时器或连接槽，须经可分离的 `worker_cancellation_target`（按非零递增 64 位 `operation_id` 寻址，宿主析构时 `detach` 原子置空）或带代际计数（`slot + generation`）的 `worker_timer_cancellation` 投递回所属 worker 线程执行（每个 worker 由单一底层 `asio::steady_timer` 与最小堆驱动全部业务定时器，跨线程无通知取消直接 CAS 置零 `active_generation_` 免唤醒；定时器队列停止后 `schedule_timer` 返回 `worker_stopping`，不存储回调且不绑定注册项），且带取消的等待操作（如 `sleep_operation`）须将 `stop_registration` 声明为末位成员以确保先拆卸回调再销毁定时器与借用句柄。
- 线性 lease 在任务启动时占用，不得并发或失败后复用。
- 连接池与脏连接回收契约（`slot_pool` / `pool_lease_scheduler`）：
  - 数据库、Redis 与出站客户端连接池均保持 worker-local 且地址稳定，槽位在构造期预分配，统一管理租约排队、按 operation id 寻址的取消与单槽截止期定时器；`release()` 在返回前将空闲槽位所有权即刻移交排队等待者（后续取消或 `close()` 不得推翻该已提交获取），但等待者协程延迟至下一轮 worker 事件循环恢复，严禁在 `release()` 内联同步恢复；
  - 借出连接槽仅在操作正常完成，或错误明确承诺底层协议流已排空、连接处于空闲态且不在未决事务中（如 `reusable_connection()` 为真）时，才允许归还复用或转交等待者；
  - 任何超时、取消、网络 I/O 异常、半关闭、协议失步或事务内未捕获退出，必须在释放租约前立即关闭并淘汰该底层连接，严禁将未排空或携带残留事务/会话状态的脏连接放回池中。
- 阻塞或 CPU 密集工作显式卸载到常驻有界线程池（`blocking_pool`）：
  - 卸载任务运行在外部线程，只携带自有数据（按值或移动捕获），严禁触碰 `context`、请求 arena 或任何 worker 局部状态；
  - 池内任务严禁同步等待或 `co_await` 同一阻塞池的子任务完成，防止固定线程耗尽导致自锁死锁；
  - 统计与错误语义严格区分队列满导致的过载拒绝（`rejected` / `queue_full`）与停机导致的丢弃（`discarded` / `pool_stopped`）；停机恢复等待者并丢弃迟到结果，不等待仍在运行的卸载任务。

启动与停机：

- 监听配置中重复端点必须拒绝，不能因端口复用静默成功。`worker_group` 按 `start()`（全员就绪屏障）→ `serve()`（全员服务屏障）→ `request_stop()` → `join(primary)` 推进：各 worker 在准备阶段独立绑定监听，全部 ready 后发布 Windows 共享监听矩阵，全部启动 hook 和服务 barrier 成功后才发起 accept；任一阶段失败、成员异常或成员提前自停均立即关闭全组准入并回滚全部监听与资源，不留下部分服务；`join(primary)` 幂等返回主故障（调用方传入的 `primary` 或按注册顺序的首个成员故障）供重抛，其余成员故障折叠等价项后上报。
- 停机分阶段：停止接入 → 排空（关闭空闲和未完成请求头的连接，已接纳请求在 `drain_timeout` 内完成，HTTP/2 发送 `GOAWAY` 排空，WebSocket 以 1001 发起关闭）→ 强制关闭剩余连接 → 最终退役。
- 停止接入先封闭 accept 投递并取消重试：POSIX 关闭本 worker 监听器唤醒 pending accept；Windows 精确取消每个 pending `AcceptEx`，保持共享监听句柄和全部 IOCP owner 存活，直到原生完成已转递、全部 origin 消费完成且通知与定时器已 join，才由各 owner 关闭自身监听器并 finalize。冷启动取消及未启动 worker 不等待不存在的接收器。
- teardown 先终止 I/O，再 join 全部后台操作。`worker_runtime_hooks` 中除 `startup_` 与 `serve_` 外，其余停机/清理钩子（`stop_admission_`、`finalize_`、`failure_`、`shutdown_`）严禁抛出异常，且 `finalize_` 允许其目标在移动或析构时重入。外部生产者静默、跨线程 channel 双方退役确认及后台任务完成前，`core` 保持 owner 事件循环存活；若 `event_loop_pool::join()` 在 `start()` 前被调用，则创建短生命周期临时 owner 线程排空已接纳任务并运行停止回调（在池自身 worker 线程上调用 `join()` 立即抛 `std::logic_error`）。用户 hook 和 `join` 由生命周期调用线程执行。

## Web 核心设计

- `controller` 是不继承框架类型的 `final` 类，由 `RUVIA_ROUTES_BEGIN(type)` … `RUVIA_ROUTES_END` 宏声明路由并在 `main` 前自动注册；注册去重后封存，不维护手工清单。
- 中间件是满足 `ruvia::middleware` 概念的 `final` 类型，不继承框架基类；继续调用（`next`）通过三态控制块（`fresh` / `invoked` / `expired`）强制 single-shot 并在当前中间件栈帧退出后立即失效以防逃逸。前缀作用域中间件（`use_at`）按百分比解码并归一化尾斜杠后的完整路径段匹配（如 `/admin` 覆盖 `/%61dmin/x` 但不匹配 `/administrator`），归属关系在启动期编译进路由计划，仅跨边界的参数化路由在分发期检查请求路径。
- `controller`、中间件与 `use_worker_state` 注册的状态均保持 per-worker。进程只拥有一份不可变路由查找计划，各 worker 路由契约必须一致；路由和中间件链在启动前构建，冲突启动即报错。内存测试门面（`test_app`）在首次 `request()` 时封闭配置并驱动真实 worker 能力启停（禁止初始化重入，启动失败进入粘性 `failed` 态并在后续调用时原样重抛首次异常）。
- 上下文（`context` 与 `web_worker_context`）只暴露 typed capability（`web_worker_context` 直接借用 worker 的客户端注册表与停止令牌，启动后台任务零所有权复制，worker 退役前 `join` 全部已投递任务）；callback 由应用拥有。
- 响应元数据显式分为下一次构造消耗的 `draft()`、当前缓冲响应的 `edit_response()` 和交付时应用的跨替换 `policy()`；构造响应时通过 `take_draft_or_new()` 原位移动接管 `draft` 实例并仅在未显式设置时补充默认 `Content-Type`（消除新建空响应再拷贝 `draft` 字段的开销），替换不自动继承旧响应字段。流式生产者先 `commit()` 并区分正文阶段与正常完成，正常完成不得伪装成取消或中止。
- `model_type` 使用编译期 schema，解析、校验和序列化职责分离：必填与可空独立（`RUVIA_OPTIONAL_FIELD` 仅允许字段缺失，允许 `null` 须显式声明 `RUVIA_NULLABLE`，`RUVIA_DEFAULT` 仅作用于缺失的可选字段）；数值转换拒绝 `bool`/字符充当整数并执行精确整数与窄化浮点范围检查（表单解析对非字符串标量字段在无转义时直接原地解析视图而不分配 `std::pmr::string`）；跨分配器拥有化支持递归重绑定（`rebind_for_model`），移动操作检测源与目标地址重叠防止自包含结构移动时先销毁自身存储。
- ORM 与直接查询保持独立公开路线，不混用内部实现。映射结果的标量及容器元素必须递归拥有数据；借用字段读取只在显式持有原始结果的路线提供。数值查询谓词保持字面量精度（不将 `double` 隐式窄化为 `float`）。
- 数据库与 Redis 韧性：
  - 事务外查询/执行可按驱动契约共享流水线连接（PostgreSQL 常规 `query` / `execute` 每连接维护有界服务端命名预处理语句集合，流式查询以无名语句运行，`db_migrator` 使用独立专用连接；MariaDB 单遍扫描分类独占语句与存储过程调用，强制拒绝 `LOCAL INFILE` 请求以防服务端读取宿主本地文件，仅当参数二进制绑定与字面量语义完全等价时才走预处理绑定，并在行集读毕后立即释放超 64 KiB 的宽列缓冲），事务内独占连接租约；`db_transaction` 与 `db_stream_result` 的操作借用堆上地址稳定的内部状态（移动外层句柄不使冷任务或运行中帧失效），并发启动重叠操作抛出 `std::logic_error` 但绝不中止当前已持有事务的在途语句；涉及数据库事务与其他外部 I/O 的流程须明确一致性边界和失败补偿，避免持有数据库事务或锁等待无关远程操作；
  - 编译期 SQL 字面量（`fixed_string`）在编译期拒绝 NUL 字节、推导或验证数据库方言（MariaDB `?` vs PostgreSQL `$n`，正确跳过注释、字符串与 dollar-quoted 标签/含 `$` 标识符）并断言占位符数与实参个数严格匹配；查询编译器对 PostgreSQL 绑定参数默认发送为无类型（由上下文推导），仅在多态函数参数、全绑定参数算术/位运算/NULL 谓词/`COALESCE`/`NULLIF`/`GREATEST`/`LEAST` 或 `int4` 专属槽位按表生成精确 `::type` 转换；运行期 PostgreSQL 字符串参数严禁包含内嵌 NUL 或超出 `INT_MAX` 长度，内联 `double` 参数强制校验有限值（`std::isfinite`）；
  - 单次 SQL 执行与迁移（`db_migration`）严格限制为单条语句，迁移 ID 在启动期做大小写不敏感去重校验；PostgreSQL 迁移默认将 DDL 与迁移记录写入置于同一事务（仅不支持事务的 DDL 显式声明 `unwrapped`），且必须识别已执行迁移内容被篡改；存储过程异常 `SQLSTATE` 强制要求 5 位大写字母数字且严禁使用代表成功的 `"00000"`；
  - Redis 连接池隔离、Pub/Sub 与批处理：同一别名下常规请求/响应池与按需惰性建连的独立阻塞命令池（`blocking_pool_size_per_worker_`）物理隔离，防止阻塞命令耗尽常规连接；有界阻塞命令在命令写出时按“服务端阻塞时长 + 宽限期”武装截止期（不因槽位获取或建连耗时缩减等待窗口），无限期阻塞等待强制要求显式 `stop_token` 或操作超时；独立订阅连接（`redis_subscription_connection`）跳过普通池空闲探测，构造时必须先 `link` 入链使其可按 ID 寻址后再 `arm` `stop_token`（应对已触发 token 的内联取消）；RESP 响应同时受总字节（`max_reply_bytes_`）与数组嵌套深度（`max_array_depth_`）约束；RediSearch `NUMERIC` 索引与谓词强制校验有限值且绝对值 $\le 2^{53}-1$（防 `double` 整数精度丢失），并保留 `__ruvia_` 影子字段前缀；批处理（`redis_batch`）在返回前完成参数拥有化，事务（`transaction`）严禁含阻塞命令或同步屏障，非事务 `pipeline` 仅允许携带具备明确超时或可取消 `stop_token` 的 `WAIT` / `WAITAOF` 屏障；
  - 数据库查询结果缓存（Redis 承载）须共享同一操作的剩余截止期与组合取消令牌（`stop_group`）：遇到取消或停机（`cancelled` / `closing`）时必须立即上抛，不得被容错开关静默吞掉；截止期超时统一按查询超时处理。
- 出站客户端（`http_client` / `websocket_client`）：
  - 可独立绑定事件循环，应用上下文只提供便捷入口；outbound origin 启动前固定，请求期不建池，传输层在构造期预解析 IP 字面量端点（`literal_endpoint_`，连接时直接跳过 DNS Resolver）并支持 Happy Eyeballs（RFC 8305：以首个解析地址族为首交错去重 IPv6/IPv4 端点，按 250ms 阶梯并发尝试并在首个连接胜出或取消时立即关闭其余在途 socket），且已启用的客户端 TCP 套接字策略（如 `no_delay` / `keep_alive`）应用失败时建连直接失败而不静默降级；
  - 强制执行两级出站结果内存预算（已保留结果 `max_retained_bytes_` 与在途分配 `max_in_flight_bytes_`，空 `http_client_response_bytes` 不持有存储且不占用保留配额；全量收集 `collect_all_` 发现响应体超限时立即丢弃并释放已缓冲前缀 `discard_response_body()`，防止无用半截数据滞留占用预算）；`request_timeout_` 覆盖从获取连接槽到响应体被慢速消费者完整排空的全周期，超时立即失败并销毁连接；
  - 连接通告（`ORIGIN` / `Alt-Svc`）仅作有界只读观测，绝不自动改变已注册 origin、连接归并、TLS 身份或缓存策略；其底层内存池（`http_client_advertisement_memory`）独立跟踪引用计数与保留字节，客户端退役后延迟至全部外部持有的通告对象在所属 worker 释放完毕才自毁；
  - 出站重定向由 `ruvia-http` 生成 RFC 9110 §15.4 方法/正文改写计划及无 DNS 同源判定（IPv6 字面量按二进制地址比较），跨源凭证剥离与重定向次数上限由 `ruvia-web` I/O 驱动强制执行；已复用的 HTTP/1 连接遭遇 `io_error` 时，仅在同时满足“幂等方法、非流式上传、非隧道、尚未收到任何 `1xx` 或最终响应头且未被放弃”时才透明重试一次；客户端隧道（`http_client_tunnel`）在双端正常完成后允许幂等 `finish()`，而 `abort_malformed()` 即使在 `complete_` 已标记后仍强制携带 `protocol_error` 通知连接池废弃连接/流。

## README 内容规范

- `README.md` 只作库的使用入口：项目定位、三个库的职责与公开 CMake target、构建与接入、最小上手、示例导航和许可证；不是 API 手册、RFC 清单或完整功能目录。
- 说明构建前置条件（CMake、C++20 编译器、`VCPKG_ROOT`）、平台要求（Linux glibc、Windows MSVC `x64-windows-static`）及可选功能开关边界。所有权、生命周期和协议约束只保留首次正确接入必需的通用规则；单项 API 的参数、边界、异常与移动语义写在公开头或现有示例中。
- 快速开始给出可执行的最短路径，明确适用平台、shell 和必要前置条件，不混用不同环境语法。用法以仓库内可运行示例为准，只保留最小片段并链接对应文件，不复制完整源码。
- 只描述当前已实现且可验证的公开行为，不把规划、推测或未经验证的兼容性、性能结论写成现有能力；不收录内部实现（如内存池、缓存、调度细节）、工程规范、变更流水、排障过程、临时验证记录或逐项 bug 修复说明。
- 选项、依赖版本、默认值、target 和路径以当前代码与构建配置（`CMakeLists.txt`、`vcpkg.json`）为准，同一事实只列一处。修改后先判断是否影响入门步骤、构建接入或功能选择；仅影响这些入口信息时才同步更新 `README.md` 并核对命令、相对链接和说明，不得以“同步文档”为由向 `README.md` 追加细节。

## 验证与仓库规范

|范围|验证与约束|
|---|---|
|本地构建与测试|复用根 `build/`，按影响范围并行构建目标（`cmake --build build --config Debug -j <cores>`）、运行相关单测并实际执行改动路径；不无故清缓存或全量重编译|
|临时验证产物|禁止占用 `/tmp`，避免内存文件系统导致 OOM；临时源码、smoke 程序、日志和中间产物统一放在仓库根 `build/` 下，验证结束及时清理|
|单元测试边界|只经非 `detail` 公开 API 验证行为、边界、状态转换、优先级与错误语义；保持确定性、隔离性和全套运行安全，不测试内部实现、源码文本、安装、API guard、错误措辞、单纯转发、无意义的默认值、mock 回声或仅断言不抛异常|
|缺陷与生命周期|缺陷先复现再确认修复，可确定性复现的公开 API 缺陷补充功能回归；生命周期改动覆盖成功、异常、取消和内存回收。不新增长期 integration、conformance、benchmark 或 probe，临时验证完成即清理|
|数据后端验证|SQL、迁移与 Redis 行为变更按影响范围在真实后端执行相关操作；涉及事务时核对失败与回滚。后端不可用时说明未验证的范围和原因|
|格式与行尾|源文件使用 LF（见 `.gitattributes`）；改动的 C++ 源码用 `clang-format` 格式化并检查（`--dry-run --Werror`）|
|CI 门槛|Linux Musl（`x64-linux-musl`、`clang-cross`）与 Windows MSVC（`x64-windows-static`、静态 runtime）均以 C++20、`Release`、`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON`、全功能配置构建并通过全部测试；构建工具由 `mise` 管理，`clang-format` 检查及双平台构建测试全部通过后才清理旧运行记录；如实区分已通过、失败、未运行和待 CI 验证|
|纯文档与性能|纯文档本地只做文档校验，不主动运行代码构建和测试；性能测试只在专用主机运行，结论必须可复核。对比时统一条件并交错运行，微基准不代表端到端吞吐|

- 示例和测试按 target、协议层级归档，`core` 单测平铺；`build/`、依赖缓存及本地工具目录保持 ignored，不修改父项目配置。
- 无关改动导致构建或测试失败时保留现场、报告具体失败及验证限制，不借机扩大修改范围。
- 每轮修改结束汇报结果，自动提交并推送到任务对应的分支；只提交本任务改动，不夹带用户或并行任务的修改，不强推或回滚无关提交，并如实报告提交、推送与 CI 状态。除非用户特别说明，否则不创建、修改、删除或重发 Git Tag 与 Release。推送后不等待 CI 完成，不以 CI 结果阻塞交付；未取得结果时标注“CI 未核对”。
- HTTP/1、HTTP/2 在 `main` 分支维护。

