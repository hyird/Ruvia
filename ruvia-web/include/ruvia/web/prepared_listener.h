#pragma once

#include <memory>

#include "ruvia/core/task.h"

namespace ruvia {
namespace detail {
class prepared_listener_state;
struct prepared_listener_access;
}  // namespace detail

// 拥有一批已绑定但尚未接入的监听事务。TCP 握手可能完成，但处理器不会执行。
// prepare 成功时 TLS、QUIC 与全部 worker 资源已就绪；同宿主的其他监听
// 控制等待该事务结束。多个端点须用 prepare_listeners 同批准备，不能在持有
// 票据时再 await 本宿主监听控制。先发布业务配置，再 activate()。
// 任一项准备失败、放弃票据或宿主关闭均回滚整批，不影响原有监听。
// 可跨线程移动、析构；activate/cancel 必须在 prepare 的 caller worker 上 await。
// 消耗型操作在调用时接管句柄；丢弃冷任务或析构句柄均请求回滚。
// 析构不阻塞；cancel() 等待回滚完成。caller 与宿主循环须驱动至操作完成。
class prepared_listener final {
public:
    prepared_listener(prepared_listener&&) noexcept;
    prepared_listener& operator=(prepared_listener&&) noexcept;
    prepared_listener(const prepared_listener&) = delete;
    prepared_listener& operator=(const prepared_listener&) = delete;
    ~prepared_listener();

    // 只提交已完成的准备；已关闭宿主或空句柄抛 logic_error。
    [[nodiscard]] task<void> activate() &&;
    [[nodiscard]] task<void> cancel() &&;
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit prepared_listener(std::shared_ptr<detail::prepared_listener_state>) noexcept;
    std::shared_ptr<detail::prepared_listener_state> state_;
    friend struct detail::prepared_listener_access;
};

}  // namespace ruvia
