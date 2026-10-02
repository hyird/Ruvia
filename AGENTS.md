# AGENTS.md

默认中文回复。
本文件只记录规范、边界与核心设计。
README 说明用法，STYLE.md 规定代码风格。

## 工程原则

- 保留用户已有改动。
- 优先正确性和长期可维护性。代码简单、清晰、易读易改。
- 分层清晰、职责单一、边界明确、低耦合。
- 同类能力统一抽象、入口、调用主链和生命周期。
- 共同逻辑只保留一份权威实现。差异在明确边界处理。
- 允许为明确收益破坏兼容性。迁移必须完整。
- 同步更新全部调用方、示例、文档和必要功能单测。
- 清除相关技术债务与历史遗留。不留旧实现、兼容旁路或临时 TODO。
- 接口明确所有权、生命周期和错误语义。用类型表达不变量。
- 遵守协议标准和 STYLE.md。避免过度设计。

## 分层与目录

```text
ruvia-core -> ruvia::core
ruvia-http -> ruvia::http
ruvia-web  -> ruvia::web
ruvia-web  -> ruvia-core + ruvia-http
```

- 使用 C++23。源码只放在三个库、examples 和 tests。
- 各库自带 CMakeLists.txt、include 和 src。
- 各库只拥有自己的源码和公开命名根。
- src 最多一层职责目录。core 的 src 保持扁平。
- 跨库只用非 detail 公开 API。禁止穿透内部头和物理路径。
- 安装只包含公开头的真实依赖闭包。
- 根 CMakeLists.txt 只负责全局构建与安装。
- core 是独立运行时底座。不含 HTTP/Web 语义。
- core 不公开依赖 TLS、压缩或数据后端。
- HTTP 是独立 sans-I/O 协议层。不依赖 core、Asio、socket 或 OpenSSL。
- HTTP 拥有消息、分帧、连接及 QUIC 协议语义。
- Web 拥有应用策略、路由、TLS/crypto 和网络驱动。
- ngtcp2 只归 HTTP。Web 仅经公开协议 API 驱动 QUIC。
- 不复制协议判断。不将 Web 能力下沉。

## 性能与内存

- 请求热路径以零拷贝、零抽象成本为目标。
- 默认热路径不新增锁、共享争用、类型擦除或无用拷贝。
- 默认热路径不新增共享所有权。显式卸载是唯一例外。
- 启动期允许一次性构建和动态分派。
- 优先值所有权与 RAII。借用不延长 owner 寿命。
- 框架动态存储使用所属 PMR。公开启动配置不要求用户提供 PMR。
- 请求和握手用 arena。独立操作、结果和临时数据用可回收 pool。
- 分配器覆盖对象寿命。结果持有存储直到析构。
- 异步入口返回前完成输入拥有化和资源归一化。
- 长连接不得累积临时分配。存活数据不得被后续操作回收。
- 零拷贝不得破坏所有权或借用有效期。

## 运行时与生命周期

- 生产 App 是进程级单例，通过 `ruvia::app()` 获取。
- App 不可自行构造、复制或移动。
- App 的配置与生命周期入口唯一。
- 每个业务 worker 独占一个 standalone Asio 事件循环。
- N 个业务 worker 另配一个网络接入线程。
- 阻塞池和信号线程另计。
- 网络线程接受 TCP，并驱动 HTTP/3 UDP 与 QUIC 传输。
- TCP 连接只交接一次，后续 I/O 由所属业务 worker 驱动。
- 每条 TCP 或 QUIC 连接固定绑定一个业务 worker。
- 同一连接的全部请求交给同一 worker。绑定关系保持至连接结束。
- worker 资源由统一 owner 管理。实例保持 worker-local。
- 跨线程交互走有界 mailbox。禁止直接操作连接状态。
- 调度入口与事件循环同属一个 owner。入口先于上下文退役。
- 请求期只借用稳定 worker 句柄。显式卸载可复制一次。
- 协程保持 lazy 和 structured ownership。
- 公开协程统一使用 `ruvia::Task<T>`。
- 未启动任务可丢弃。已启动任务必须完成。
- 取消后必须 await/join。禁止销毁挂起帧或静默 detach。
- teardown 先终止 I/O，再 join 全部后台操作。
- 跨线程 channel 完成双方退役和确认后才能销毁。
- 线性 lease 在任务启动时占用。不得并发或失败后复用。
- 阻塞或 CPU 密集工作必须显式卸载到常驻有界线程池。
- 卸载只携带自有数据。不借用请求或 worker 状态。
- 停机恢复等待者并丢弃迟到结果。不等待仍运行的卸载任务。
- 整体就绪后才能服务。启动失败不留下部分服务。
- 用户 hook 和 join 由生命周期调用线程执行。
- 其他线程只请求停止。资源由所属 owner 线程关闭。

## Web 核心设计

- controller 使用 CRTP 和宏自动注册。注册去重后封存。
- 路由只通过宏声明。不维护手工 controller 清单。
- controller 和中间件保持 per-worker。
- 进程只拥有一份不可变路由查找计划。
- worker 路由契约必须一致。路由和中间件链启动前构建。
- 路由冲突启动即报错。中间件继续调用为 single-shot。
- 上下文只暴露 typed capability。callback 由应用拥有。
- Model 使用编译期 schema。解析、校验和序列化职责分开。
- ORM 与直接访问保持独立路线。
- client 可独立绑定事件循环。应用上下文只提供便捷入口。
- outbound origin 启动前固定。请求期不建池。

## 验证与仓库规范

- 示例和测试按 target、协议层级归档。core 单测平铺。
- 复用根 build 和现有配置。不无故清缓存或全量重编译。
- 不修改父项目配置。Windows 使用 MSVC 和静态依赖/runtime。
- 只保留功能单测。不保留历史缺陷、结构或安装/API guard。
- 不新增长期 integration、conformance、benchmark 或 probe。
- 临时验证完成即清理。交付前运行相关最小验证。
- 生命周期改动验证成功、异常、取消和内存回收。
- 纯文档只做文档校验。性能结论必须有可复核依据。
- 准确说明验证范围和限制。
- build、依赖缓存及本地工具目录保持 ignored。
