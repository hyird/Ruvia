#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "ruvia/core/worker_post_types.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

using worker_id_type = std::uint64_t;

namespace detail {
class worker_dispatcher;
class worker_shutdown_listener;
struct worker_handle_access;
}  // namespace detail

class worker_handle {
public:
    worker_handle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;

    // Worker-affine timer registration. This stable handle must outlive the
    // borrowed registration; destruction unregisters without a late callback.
    void schedule_timer(worker_timer_registration& registration,
        std::chrono::steady_clock::time_point deadline,
        move_only_function<void(worker_timer_outcome)> completion) const&;
    void schedule_timer(worker_timer_registration&,
        std::chrono::steady_clock::time_point,
        move_only_function<void(worker_timer_outcome)>) const&& = delete;

    // Reserves bounded admission before invoking the factory, so the caller
    // can transfer ownership only for an accepted post.
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory) const;

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        if constexpr (detail::move_only_function_borrow_safe_input<void(), fn_type>) {
            return post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const worker_handle snapshot = *this;
            return snapshot.post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        }
    }

private:
    explicit worker_handle(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept;
    [[nodiscard]] post_result_type post_task(move_only_function<void()> task) const;

    // Owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    friend struct detail::worker_handle_access;
};

namespace detail {

struct worker_handle_access {
    [[nodiscard]] static worker_handle make(
        const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept;
    static void defer(const worker_handle& worker, move_only_function<void()> task);
    [[nodiscard]] static bool defer_if_attached(
        const worker_handle& worker, move_only_function<void()> task);
    static void defer_or_terminate(
        const worker_handle& worker, move_only_function<void()> task) noexcept;
    static void register_shutdown_listener(
        const worker_handle& worker, const std::shared_ptr<worker_shutdown_listener>& listener);
    static void when_shutdown_notifications_complete(
        const worker_handle& worker, move_only_function<void()> callback);
    static void when_idle(const worker_handle& worker, move_only_function<void()> callback);
    static void wait_for_reservations(const worker_handle& worker) noexcept;
};

}  // namespace detail

}  // namespace ruvia
