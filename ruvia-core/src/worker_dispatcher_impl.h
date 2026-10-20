#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

// The dispatcher's state, declared here because two translation units own parts
// of it: worker_dispatcher.cpp runs the queue and the worker's lifecycle, while
// worker_timers.cpp runs the timer heap that shares its mutex.

namespace ruvia::detail {

inline constexpr std::size_t no_timer_slot = static_cast<std::size_t>(-1);

// One pending deadline in the heap. `slot` and `generation` identify the
// registration it belongs to, so a cancelled entry is recognised as stale
// instead of being searched for and removed.
struct timer_entry final {
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t sequence_{0};
    std::size_t slot_{no_timer_slot};
    std::uint64_t generation_{0};
};

// A registration slot, reused through a free list; `generation` invalidates every
// heap entry that referred to a previous occupant. Slots never move or die
// before the dispatcher, so a bound registration may read `active_generation_`
// from any thread: it holds the occupant's generation while the timer is
// pending and 0 otherwise, and is released before any completion runs.
struct timer_slot final {
    std::uint64_t generation_{0};
    std::atomic<std::uint64_t> active_generation_{0};
    std::size_t next_free_{no_timer_slot};
    bool occupied_{false};
    move_only_function<void(worker_timer_outcome)> completion_;
};

// Heap order: earliest deadline first, ties broken by registration order.
struct timer_entry_later final {
    bool operator()(const timer_entry& left, const timer_entry& right) const noexcept {
        return left.deadline_ > right.deadline_ ||
               (left.deadline_ == right.deadline_ && left.sequence_ > right.sequence_);
    }
};

// The worker whose run_context() is executing on this thread, or nullptr. A
// plain thread-local read: worker-affine paths use it to skip the dispatcher
// mutex once they know they run on their own worker.
[[nodiscard]] const worker_dispatcher* current_worker_dispatcher() noexcept;

// The process-wide worker id source.
[[nodiscard]] worker_id_type next_worker_dispatcher_id() noexcept;

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
    // the io_context without the dispatcher mutex; elsewhere it takes the
    // locked defer() path. Dispatch failure is terminal.
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
    // Links `hook`, a member of the object `owner` aliases, for both stop
    // phases. Throws std::runtime_error once the worker stops accepting.
    void register_shutdown_hook(
        worker_shutdown_hook& hook, std::weak_ptr<void> owner, const worker_shutdown_hook::operations_type& operations);
    // Runs after the first stopping batch has notified every hook, or now if
    // no batch is active. The active bit is published with accepting=false.
    void when_shutdown_notifications_complete(move_only_function<void()> callback);
    void when_idle(move_only_function<void()> callback);
    worker_timer_schedule_status schedule_timer(worker_timer_registration& registration,
        std::chrono::steady_clock::time_point deadline,
        move_only_function<void(worker_timer_outcome)> completion);
    // From any thread. A notified cancellation of a still-live timer delivers
    // `cancelled` inline from the handler a foreign thread posts, or deferred
    // when the caller is already running on this worker.
    void request_timer_cancellation(std::size_t slot, std::uint64_t generation, bool notify) noexcept;
    void stop_timers() noexcept;
    // Runs the owned/attached io_context with a thread-local worker identity so
    // worker-affine hot paths do not lock merely to prove their current worker.
    // External owners that run an attached context directly retain the safe
    // executor-based fallback in is_current().
    // Invokes failure_handler exactly once for the first escaping handler
    // exception while worker identity is still active, then re-enters run() to
    // drain shutdown continuations. A successfully returning handler consumes
    // the failure; a throwing handler, or the first failure when no handler is
    // given, is rethrown only after the drain ends.
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
    friend class worker_shutdown_hook;
    static constexpr std::size_t no_node = static_cast<std::size_t>(-1);
    using idle_callbacks_type = std::pmr::vector<move_only_function<void()>>;

    struct node_type final {
        move_only_function<void()> task_;
        std::size_t next_{no_node};
    };

    [[nodiscard]] static std::size_t checked_queue_capacity(std::size_t capacity) {
        if (capacity == 0 || capacity > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::invalid_argument("worker queue capacity is out of range");
        }
        return capacity;
    }

    // True only inside this worker's run_context(), where the io_context is
    // alive and detach_context() cannot run concurrently (see its contract).
    [[nodiscard]] bool running_on_worker() const noexcept {
        return current_worker_dispatcher() == this && attached();
    }
    [[nodiscard]] idle_callbacks_type take_idle_callbacks_locked();
    static void notify_idle(idle_callbacks_type callbacks) noexcept;
    // Returns whether this call opened the (single) stopping batch.
    [[nodiscard]] bool begin_stopping(bool abandon_drain) noexcept;
    [[nodiscard]] bool begin_stopping_locked(bool abandon_drain) noexcept;
    void notify_stopping(bool batch_active) noexcept;
    void unlink_shutdown_hook(worker_shutdown_hook& hook) noexcept;
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
    void fail_timers(std::string_view context_value, std::exception_ptr failure) noexcept;
    [[nodiscard]] bool has_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    [[nodiscard]] bool has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    // Deactivates an active slot, returns it to the free list, and hands back
    // its completion for the caller to invoke or destroy.
    [[nodiscard]] move_only_function<void(worker_timer_outcome)> release_timer_slot(std::size_t slot) noexcept;
    // Removes a live timer from the queue and hands back its completion; empty
    // when the timer already expired or was cancelled.
    [[nodiscard]] move_only_function<void(worker_timer_outcome)> take_timer(
        std::size_t slot, std::uint64_t generation) noexcept;

    asio::io_context& io_context_;
    std::unique_ptr<asio::steady_timer> timer_;
    std::size_t capacity_;
    std::pmr::vector<node_type> nodes_;
    mutable std::mutex mutex_;
    std::condition_variable pending_changed_;
    std::size_t free_head_{no_node};
    std::size_t ready_head_{no_node};
    std::size_t ready_tail_{no_node};
    std::size_t ready_count_{0};
    std::size_t pending_count_{0};
    std::size_t active_count_{0};
    idle_callbacks_type idle_waiters_{process_resource()};
    worker_id_type worker_id_{0};
    std::atomic_bool accepting_{true};
    std::atomic_bool context_attached_{true};
    bool drain_scheduled_{false};
    bool abandon_drain_{false};
    bool shutdown_notification_active_{false};
    idle_callbacks_type shutdown_notification_waiters_{process_resource()};
    // Registered shutdown hooks in registration order. A separate mutex keeps
    // registration and unlinking off the admission lock.
    std::mutex hooks_mutex_;
    worker_shutdown_hook* hooks_head_{nullptr};
    worker_shutdown_hook* hooks_tail_{nullptr};
    std::pmr::vector<timer_entry> timers_;
    std::pmr::deque<timer_slot> timer_slots_;
    std::size_t free_timer_slot_{no_timer_slot};
    std::uint64_t next_timer_sequence_{0};
    std::uint64_t timer_generation_{0};
    // The deadline the asio timer waits for while `timer_armed_`; every live
    // heap entry is due no earlier, so later schedules need not re-arm.
    std::chrono::steady_clock::time_point armed_deadline_{};
    bool timer_armed_{false};
    bool dispatching_timers_{false};
    std::chrono::steady_clock::time_point timer_round_deadline_{};
    std::atomic_bool timers_stopping_{false};
    std::size_t stale_timer_count_{0};
};

}  // namespace ruvia::detail
