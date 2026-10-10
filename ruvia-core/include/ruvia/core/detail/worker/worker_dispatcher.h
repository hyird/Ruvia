#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

class worker_shutdown_listener {
public:
    virtual ~worker_shutdown_listener() = default;
    virtual void worker_stopping() noexcept = 0;
    virtual void worker_stopping_complete() noexcept {}
};

class worker_dispatcher final : public std::enable_shared_from_this<worker_dispatcher> {
public:
    worker_dispatcher(asio::io_context& io_context, std::size_t capacity);
    ~worker_dispatcher();

    worker_dispatcher(const worker_dispatcher&) = delete;
    worker_dispatcher& operator=(const worker_dispatcher&) = delete;

    [[nodiscard]] post_result_type post(move_only_function<void()> task);
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory);
    void defer(move_only_function<void()> task);
    [[nodiscard]] bool defer_if_attached(move_only_function<void()> task);
    void defer_or_terminate(move_only_function<void()> task) noexcept;
    void register_shutdown_listener(const std::shared_ptr<worker_shutdown_listener>& listener);
    // Runs after the first stopping batch has notified every listener, or now if
    // no batch is active. The active bit is published with accepting=false.
    void when_shutdown_notifications_complete(move_only_function<void()> callback);
    void when_idle(move_only_function<void()> callback);
    worker_timer_schedule_status schedule_timer(worker_timer_registration& registration,
        std::chrono::steady_clock::time_point deadline,
        move_only_function<void(worker_timer_outcome)> completion);
    void request_timer_cancellation(std::size_t slot, std::uint64_t generation, bool notify) noexcept;
    void cancel_timer(std::size_t slot, std::uint64_t generation, bool notify) noexcept;
    void stop_timers() noexcept;
    // Runs the owned/attached io_context with a thread-local worker identity so
    // worker-affine hot paths do not lock merely to prove their current worker.
    // External owners that run an attached context directly retain the safe
    // executor-based fallback in is_current().
    void run_context();
    // Invokes failure_handler exactly once for the first escaping handler
    // exception while worker identity is still active, then re-enters run() to
    // drain shutdown continuations. A successfully returning handler consumes
    // the failure; a throwing handler is rethrown only after the drain ends.
    void run_context(move_only_function<void(std::exception_ptr)> failure_handler);
    // Runs startup after worker identity is established and shutdown after the
    // context has drained but before that identity is cleared. Startup failures
    // enter the same first-failure path as handler failures. Shutdown is a
    // terminal cleanup hook and must not throw.
    void run_context(move_only_function<void()> startup_handler,
        move_only_function<void(std::exception_ptr)> failure_handler,
        move_only_function<void()> shutdown_handler);
    void close() noexcept;
    // Called after worker activity is serialized with teardown (by a joined
    // pool thread, an attached context's terminal handler, or its context
    // service). Handles remain safe terminal endpoints.
    void detach_context() noexcept;
    void wait_for_reservations() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;

private:
    using shutdown_listeners_type = std::pmr::vector<std::weak_ptr<worker_shutdown_listener>>;
    using idle_callbacks_type = std::pmr::vector<move_only_function<void()>>;
    struct shutdown_batch_type final {
        shutdown_listeners_type listeners_{process_resource()};
        bool active_{false};
    };

    [[nodiscard]] idle_callbacks_type take_idle_callbacks_locked();
    static void notify_idle(idle_callbacks_type callbacks) noexcept;
    [[nodiscard]] shutdown_batch_type begin_stopping(bool abandon_drain) noexcept;
    void notify_stopping(shutdown_batch_type batch) noexcept;
    void abandon_queued() noexcept;
    void publish(std::size_t index);
    void rollback_reserved(std::size_t index) noexcept;
    void release_abandoned_node(std::size_t index) noexcept;
    void drain();
    void arm_timer();
    void fire_timers();
    [[nodiscard]] bool has_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    // Deactivates an active slot, returns it to the free list, and hands back
    // its completion for the caller to invoke or destroy.
    [[nodiscard]] move_only_function<void(worker_timer_outcome)> release_timer_slot(std::size_t slot) noexcept;

    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia::detail
