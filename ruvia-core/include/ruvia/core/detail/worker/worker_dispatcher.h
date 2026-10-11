#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

class worker_dispatcher;

// The worker whose run_context() is executing on this thread, or nullptr. A
// plain thread-local read: worker-affine paths use it to skip the dispatcher
// mutex once they know they run on their own worker.
[[nodiscard]] const worker_dispatcher* current_worker_dispatcher() noexcept;

// A shutdown listener is an intrusive node in its worker's registry. Linking and
// unlinking are O(1) under the registry mutex and never allocate or scan. The
// node keeps the registry alive, so a listener can unlink from its destructor
// after the derived owner has already released its own worker handle; the
// listener may die on any thread, so the registry stays mutex-protected.
class worker_shutdown_listener {
public:
    worker_shutdown_listener() noexcept = default;
    worker_shutdown_listener(const worker_shutdown_listener&) = delete;
    worker_shutdown_listener& operator=(const worker_shutdown_listener&) = delete;
    virtual ~worker_shutdown_listener();

    virtual void worker_stopping() noexcept = 0;
    virtual void worker_stopping_complete() noexcept {}

private:
    friend class worker_dispatcher;

    std::shared_ptr<worker_dispatcher> registry_;
    std::weak_ptr<worker_shutdown_listener> self_;
    worker_shutdown_listener* previous_{nullptr};
    worker_shutdown_listener* next_{nullptr};
    bool linked_{false};
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
    // On this worker's own run_context() thread the callable goes straight to
    // the io_context without the dispatcher mutex or type erasure; elsewhere it
    // takes the locked defer() path. Dispatch failure is terminal.
    template <typename fn_type>
    void defer_or_terminate(fn_type&& fn) noexcept {
        try {
            if (running_on_worker()) {
                asio::post(io_context_, std::forward<fn_type>(fn));
            } else {
                defer(move_only_function<void()>(std::forward<fn_type>(fn)));
            }
        } catch (...) {
            std::terminate();
        }
    }
    // Throws std::runtime_error once the worker stops accepting.
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
    // Rethrows the first escaping handler exception after the drain ends.
    void run_context();
    // Invokes failure_handler exactly once for the first escaping handler
    // exception while worker identity is still active, then re-enters run() to
    // drain shutdown continuations. A successfully returning handler consumes
    // the failure; a throwing handler is rethrown only after the drain ends.
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
    // service). Handles remain safe terminal endpoints. It never runs inside
    // run_context(): the owned runtime detaches after run_context() returns,
    // and attached contexts are driven without worker identity. The lock-free
    // on-worker paths rely on this.
    void detach_context() noexcept;
    void wait_for_reservations() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;

private:
    friend class worker_shutdown_listener;
    using idle_callbacks_type = std::pmr::vector<move_only_function<void()>>;

    // True only inside this worker's run_context(), where the io_context is
    // alive and detach_context() cannot run concurrently (see its contract).
    [[nodiscard]] bool running_on_worker() const noexcept {
        return current_worker_dispatcher() == this && attached();
    }
    [[nodiscard]] idle_callbacks_type take_idle_callbacks_locked();
    static void notify_idle(idle_callbacks_type callbacks) noexcept;
    // Returns whether this call opened the (single) stopping batch.
    [[nodiscard]] bool begin_stopping(bool abandon_drain) noexcept;
    void notify_stopping(bool batch_active) noexcept;
    void unlink_shutdown_listener(worker_shutdown_listener& listener) noexcept;
    void abandon_queued() noexcept;
    void schedule_drain_locked();
    // Claims one free node against capacity; the caller owns it until it
    // publishes or releases it.
    [[nodiscard]] post_status reserve(std::size_t& index) noexcept;
    void push_ready_locked(std::size_t index) noexcept;
    void publish(std::size_t index);
    void release_abandoned_node(std::size_t index) noexcept;
    void drain();
    void arm_timer();
    void fire_timers();
    [[nodiscard]] bool has_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    // Deactivates an active slot, returns it to the free list, and hands back
    // its completion for the caller to invoke or destroy.
    [[nodiscard]] move_only_function<void(worker_timer_outcome)> release_timer_slot(std::size_t slot) noexcept;

    asio::io_context& io_context_;
    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};

template <typename fn_type>
void worker_handle_access::defer_or_terminate(const worker_handle& worker, fn_type&& fn) noexcept {
    if (!worker.dispatcher_) {
        std::terminate();
    }
    worker.dispatcher_->defer_or_terminate(std::forward<fn_type>(fn));
}

}  // namespace ruvia::detail
