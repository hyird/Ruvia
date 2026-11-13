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
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

class blocking_pool;

// Owns the worker dispatcher endpoint and detaches escaped worker refs on teardown.
// The io_context remains owned by the runtime that supplied it, and the
// blocking pool, when given, by its owner; both must outlive this context.
class worker_runtime_context final {
public:
    // `pool` is borrowed for run_blocking() on this worker and must outlive it;
    // null leaves the worker without one.
    worker_runtime_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool = nullptr);
    ~worker_runtime_context();

    worker_runtime_context(const worker_runtime_context&) = delete;
    worker_runtime_context& operator=(const worker_runtime_context&) = delete;
    worker_runtime_context(worker_runtime_context&&) = delete;
    worker_runtime_context& operator=(worker_runtime_context&&) = delete;

    [[nodiscard]] asio::io_context& io_context() const& noexcept;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    const worker_ref& worker() const&& = delete;

    void run();
    void run(move_only_function<void()> startup_handler,
        move_only_function<void(std::exception_ptr)> failure_handler,
        move_only_function<void()> shutdown_handler);

    void close() noexcept;
    void stop_timers() noexcept;
    void detach() noexcept;

private:
    asio::io_context* io_context_;
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    worker_ref worker_;
};

}  // namespace ruvia

namespace ruvia::detail {

// The one stop/finalize sequence of a worker, shared by worker_runtime and by
// event loops attached to a caller-owned io_context.
//
// request_stop() closes dispatcher admission. Once the shutdown notification
// batch has completed, the owner thread runs the stop control: stop_admission_
// (or an immediate finalize() without one), then timer cancellation.
// finalize() makes the owner run finalize_ and then release the work guard
// that keeps the context running. The io_context is never stopped.
class worker_stop_chain final {
public:
    struct hooks_type final {
        move_only_function<void()> stop_admission_{};
        // The chain's last access to itself happens before this hook, which
        // may destroy the chain; the work guard is released after it returns.
        move_only_function<void()> finalize_{};
    };

    explicit worker_stop_chain(worker_runtime_context& context);

    worker_stop_chain(const worker_stop_chain&) = delete;
    worker_stop_chain& operator=(const worker_stop_chain&) = delete;
    worker_stop_chain(worker_stop_chain&&) = delete;
    worker_stop_chain& operator=(worker_stop_chain&&) = delete;

    // Once, before the chain is published or stopped.
    void configure(hooks_type hooks) noexcept;
    void request_stop() noexcept;
    // The first call wins; it also requests stop.
    void finalize() noexcept;
    // Cold-path owner control. Returns false once the work guard is released.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // The context is shutting down and will run no further handler: cancels
    // timers and releases the work guard without finalize_.
    void abandon() noexcept;

private:
    using work_guard_type = asio::executor_work_guard<asio::io_context::executor_type>;

    void defer_stop_control_locked() noexcept;
    void apply_stop() noexcept;

    worker_runtime_context& context_;
    std::mutex mutex_;
    work_guard_type work_;
    hooks_type hooks_;
    bool stop_requested_{};
    // At most one stop control is pending at a time, so finalize_ may destroy
    // the chain: finalize() defers another only after the first one has run.
    bool stop_control_ran_{};
    bool finalization_requested_{};
    bool work_released_{};
    // Owner thread only.
    bool stop_applied_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class external_context_attachment_service;
struct event_loop_state;

// Retirement gate of one loop: root tasks, stop callbacks, and cleanup each
// hold a pending count; finalize runs once the gate is sealed and drained.
class event_loop_retirement final {
public:
    event_loop_retirement() = default;

    [[nodiscard]] bool begin_stop(move_only_function<void()> finalize) noexcept {
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            if (sealed_ || finalized_) {
                return false;
            }
            std::swap(finalize_, finalize);
        }
        return true;
    }

    [[nodiscard]] bool try_begin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    [[nodiscard]] bool try_begin_cleanup() noexcept {
        const std::lock_guard lock(mutex_);
        if (finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    void begin() noexcept {
        if (!try_begin()) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<void> try_acquire(const std::shared_ptr<event_loop_state>& owner) {
        {
            const std::lock_guard lock(mutex_);
            if (finalized_ || root_admission_closed_ || sealed_) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::make_shared<lease_token_type>(owner);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            take_finalize_locked(finalize);
        }
        if (finalize) {
            finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void close_admission() noexcept {
        move_only_function<void()> abandoned;
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            sealed_ = true;
            finalized_ = true;
            std::swap(abandoned, finalize_);
        }
    }

    void seal() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            take_finalize_locked(finalize);
        }
        if (finalize) {
            finalize();
        }
    }

private:
    // Both drain and seal claim finalization under the gate mutex; the caller
    // invokes the claimed callback only after releasing that mutex.
    void take_finalize_locked(move_only_function<void()>& finalize) noexcept {
        if (sealed_ && pending_ == 0 && !finalized_) {
            finalized_ = true;
            finalize = std::move(finalize_);
        }
    }

    struct lease_token_type final {
        explicit lease_token_type(std::shared_ptr<event_loop_state> owner_value) noexcept
            : owner_(std::move(owner_value)) {}
        ~lease_token_type();
        std::shared_ptr<event_loop_state> owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool root_admission_closed_{false};
    bool sealed_{false};
    bool finalized_{false};
    move_only_function<void()> finalize_;
};

// The single Ruvia attachment claim on a caller-owned io_context.
class external_context_claim final {
public:
    explicit external_context_claim(asio::io_context& io_context);
    ~external_context_claim();

    external_context_claim(const external_context_claim&) = delete;
    external_context_claim& operator=(const external_context_claim&) = delete;

    // Hands the state to the context service, which retires it at the latest
    // when the context shuts down.
    void retain(std::shared_ptr<event_loop_state> state_value) noexcept;
    // Releases the retained state and the claim once the state has retired.
    void release(event_loop_state* state_value) noexcept;

private:
    external_context_attachment_service* service_;
    bool retained_{false};
};

// A caller-owned io_context bound to one loop: its claim, the worker endpoint,
// and the same worker stop chain that a pool loop's worker_runtime owns.
struct attached_context final {
    attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool,
        move_only_function<void(std::exception_ptr)> failure);

    external_context_claim claim_;
    worker_runtime_context runtime_;
    worker_stop_chain stop_;
    move_only_function<void(std::exception_ptr)> failure_;
};

// A standalone worker_runtime that exposes its loop through
// worker_runtime::loop(); the runtime outlives every handle to the loop.
struct borrowed_runtime final {
    worker_runtime* runtime_;
};

// One loop's runtime and retirement. A pool loop owns a worker_runtime; an
// attached loop owns the same worker stop chain over a caller-owned context;
// a borrowed loop gates the stop admission of a standalone worker_runtime.
// Both stop through stop(): it seals the retirement gate on the stopping
// thread and requests the worker stop chain, whose owner-thread stop control
// re-enters stop(); the drained gate finalizes the chain on the owner.
struct event_loop_state final : std::enable_shared_from_this<event_loop_state> {
    using context_ownership_type = std::variant<worker_runtime, attached_context, borrowed_runtime>;

    event_loop_state(std::size_t queue_capacity, blocking_pool* pool);
    explicit event_loop_state(worker_runtime& runtime);
    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool,
        move_only_function<void(std::exception_ptr)> failure);

    [[nodiscard]] worker_runtime& owned_runtime() noexcept {
        return *std::get_if<worker_runtime>(&context_ownership_);
    }
    // Pool loops only: the runtime's first failure, without observing it.
    [[nodiscard]] std::exception_ptr recorded_failure() noexcept {
        return owned_runtime().recorded_failure();
    }

    // Attached loops only: hooks the stop chain and hands the state to the
    // context service, which retires it at the latest when the context shuts
    // down.
    void attach(const std::shared_ptr<event_loop_state>& self);
    void install_shutdown_registration();

    void worker_stopping_complete() noexcept;

    // Delivers to the bound failure sink; only a state whose sink is not yet
    // bound (pool construction rollback) reports under `unbound_context`.
    void route_failure(std::exception_ptr failure, const char* unbound_context) noexcept;
    void report_attached_failure(std::exception_ptr failure) noexcept;
    [[noreturn]] void fail_teardown_start(std::exception_ptr failure) noexcept;

    void stop() noexcept;
    void complete_stop_admission() noexcept;
    void finalize_stop() noexcept;
    // Asio service shutdown of an attached context: retires the worker before
    // the context destroys its scheduler.
    void shutdown_external_context() noexcept;

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    context_ownership_type context_ownership_;
    asio::io_context* const io_context_;
    asio::io_context::executor_type executor_;
    worker_runtime_context& runtime_;
    std::atomic_bool stopping_{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Root tasks and stop callbacks borrow it through the loop state
    // instead of copying it.
    move_only_function<void(std::exception_ptr)> failure_sink_;
    event_loop_retirement retirement_;

private:
    void schedule_finalize_when_idle() noexcept;
    void defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept;
};

inline event_loop_retirement::lease_token_type::~lease_token_type() {
    owner_->retirement_.finish();
}

}  // namespace ruvia::detail

// The dispatcher's state, declared here because two translation units own parts
// of it: worker_dispatcher.cpp runs the queue and the worker's lifecycle, while
// worker_timers.cpp runs the timer heap that shares its mutex.

namespace ruvia {
class blocking_pool;
}  // namespace ruvia

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
    bool cancel_requested_{false};
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
    // `pool` is borrowed: offloads from this worker run there, and it must
    // outlive the dispatcher. Null leaves the worker without one.
    worker_dispatcher(asio::io_context& io_context, std::size_t capacity, ruvia::blocking_pool* pool);
    ~worker_dispatcher();

    worker_dispatcher(const worker_dispatcher&) = delete;
    worker_dispatcher& operator=(const worker_dispatcher&) = delete;

    [[nodiscard]] post_result_type post(move_only_function<void()> task);
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory);
    // Queues `task` on the io_context; false, dropping it, once the context
    // was detached. Off the worker the mutex orders the post before
    // detach_context(), after which the context may be destroyed.
    [[nodiscard]] bool defer_if_attached(move_only_function<void()> task);
    // On this worker's own run_context() thread the callable goes straight to
    // the io_context without the dispatcher mutex or type erasure; elsewhere it
    // takes defer_if_attached(). Dispatch failure is terminal.
    template <typename fn_type>
    void defer_or_terminate(fn_type&& fn) noexcept {
        try {
            if (running_on_worker()) {
                asio::post(io_context_, std::forward<fn_type>(fn));
            } else if (!defer_if_attached(move_only_function<void()>(std::forward<fn_type>(fn)))) {
                std::terminate();
            }
        } catch (...) {
            std::terminate();
        }
    }
    // Links `registration`, a member of the object `owner` aliases, for both
    // stop phases; worker_stopping once the worker stops accepting.
    [[nodiscard]] worker_registration_status register_shutdown(worker_shutdown_registration& registration,
        std::weak_ptr<void> owner, const worker_shutdown_operations_type& operations);
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
    // service). Worker refs remain safe terminal endpoints. It never runs inside
    // run_context(): the owned runtime detaches after run_context() returns,
    // and attached contexts are driven without worker identity. The lock-free
    // on-worker paths rely on this.
    void detach_context() noexcept;
    void wait_for_reservations() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    [[nodiscard]] bool timers_stopping() const noexcept {
        return timers_stopping_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool is_current() const noexcept;
    void enable_external_runner() noexcept {
        external_runner_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] ruvia::blocking_pool* blocking_pool() const noexcept {
        return blocking_pool_;
    }

private:
    friend class ruvia::worker_shutdown_registration;
    static constexpr std::size_t no_node = static_cast<std::size_t>(-1);
    struct waiter_node_type final {
        explicit waiter_node_type(move_only_function<void()> callback_value) noexcept
            : callback_(std::move(callback_value)) {}

        move_only_function<void()> callback_;
        waiter_node_type* next_{nullptr};
    };
    struct waiter_list_type final {
        waiter_node_type* head_{nullptr};
        waiter_node_type* tail_{nullptr};

        void push_back(waiter_node_type* node) noexcept {
            node->next_ = nullptr;
            if (tail_ == nullptr) {
                head_ = node;
            } else {
                tail_->next_ = node;
            }
            tail_ = node;
        }

        [[nodiscard]] waiter_node_type* take_all() noexcept {
            tail_ = nullptr;
            return std::exchange(head_, nullptr);
        }
    };

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
    [[nodiscard]] waiter_node_type* take_idle_callbacks_locked() noexcept;
    static void notify_waiters(waiter_node_type* head, bool invoke = true) noexcept;
    // Returns whether this call opened the (single) stopping batch.
    [[nodiscard]] bool begin_stopping(bool abandon_drain) noexcept;
    [[nodiscard]] bool begin_stopping_locked(bool abandon_drain) noexcept;
    void notify_stopping(bool batch_active) noexcept;
    void unlink_shutdown(worker_shutdown_registration& registration) noexcept;
    void abandon_queued() noexcept;
    void schedule_drain_locked();
    // Claims one free node against capacity; the caller owns it until it
    // publishes or releases it.
    [[nodiscard]] post_status reserve(std::size_t& index) noexcept;
    void push_ready_locked(std::size_t index) noexcept;
    void publish(std::size_t index);
    void release_abandoned_node(std::size_t index) noexcept;
    void release_abandoned_chain(std::size_t index) noexcept;
    void drain();
    void arm_timer();
    void fire_timers();
    void fail_timers(std::string_view context_value, std::exception_ptr failure) noexcept;
    [[nodiscard]] bool has_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    [[nodiscard]] bool has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept;
    [[nodiscard]] bool has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept;
    void note_stale_timer() noexcept;
    void pop_stale_timer_top() noexcept;
    // Gives an empty heap's storage back when it outgrew the startup
    // reservation, so a timer burst is not retained for the worker's lifetime.
    void release_drained_timer_heap() noexcept;
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
    ruvia::blocking_pool* const blocking_pool_;
    std::vector<node_type> nodes_;
    mutable std::mutex mutex_;
    std::condition_variable pending_changed_;
    std::size_t free_head_{no_node};
    std::size_t ready_head_{no_node};
    std::size_t ready_tail_{no_node};
    std::size_t ready_count_{0};
    std::size_t pending_count_{0};
    std::size_t active_count_{0};
    waiter_list_type idle_waiters_{};
    worker_id_type worker_id_{0};
    std::atomic_bool accepting_{true};
    std::atomic_bool context_attached_{true};
    std::atomic_bool external_runner_{false};
    bool drain_scheduled_{false};
    bool abandon_drain_{false};
    bool shutdown_notification_active_{false};
    waiter_list_type shutdown_notification_waiters_{};
    // Registered shutdown hooks in registration order. A separate mutex keeps
    // registration and unlinking off the admission lock.
    std::mutex hooks_mutex_;
    worker_shutdown_registration* hooks_head_{nullptr};
    worker_shutdown_registration* hooks_tail_{nullptr};
    std::vector<timer_entry> timers_;
    std::deque<timer_slot> timer_slots_;
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
