#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/config.hpp>
#include <asio/execution_context.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#if defined(_WIN32)
#include <windows.h>
#endif

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
    void run(move_only_function<void()> startup_handler, move_only_function<void(std::exception_ptr)> failure_handler, move_only_function<void()> shutdown_handler);

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
        explicit lease_token_type(std::shared_ptr<event_loop_state> owner_value) noexcept :
            owner_(std::move(owner_value)) {}
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
    attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure);

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
    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure);

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
        return left.deadline_ > right.deadline_ || (left.deadline_ == right.deadline_ && left.sequence_ > right.sequence_);
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
    [[nodiscard]] worker_registration_status register_shutdown(worker_shutdown_registration& registration, std::weak_ptr<void> owner, const worker_shutdown_operations_type& operations);
    // Runs after the first stopping batch has notified every hook, or now if
    // no batch is active. The active bit is published with accepting=false.
    void when_shutdown_notifications_complete(move_only_function<void()> callback);
    void when_idle(move_only_function<void()> callback);
    worker_timer_schedule_status schedule_timer(worker_timer_registration& registration, std::chrono::steady_clock::time_point deadline, move_only_function<void(worker_timer_outcome)> completion);
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
    void run_context(move_only_function<void()> startup_handler, move_only_function<void(std::exception_ptr)> failure_handler, move_only_function<void()> shutdown_handler);
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
        explicit waiter_node_type(move_only_function<void()> callback_value) noexcept :
            callback_(std::move(callback_value)) {}

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
    [[nodiscard]] move_only_function<void(worker_timer_outcome)> take_timer(std::size_t slot, std::uint64_t generation) noexcept;

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

namespace ruvia {

namespace {

class runtime_lifecycle final {
public:
    [[nodiscard]] bool start() noexcept {
        auto expected = worker_runtime_state::ready;
        return state_.compare_exchange_strong(expected, worker_runtime_state::running, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool request_stop() noexcept {
        auto observed_value = state_.load(std::memory_order_acquire);
        while (observed_value == worker_runtime_state::ready || observed_value == worker_runtime_state::running) {
            if (state_.compare_exchange_weak(observed_value, worker_runtime_state::stopping, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void complete_stop() noexcept {
        auto expected = worker_runtime_state::stopping;
        (void)state_.compare_exchange_strong(expected, worker_runtime_state::stopped, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] worker_runtime_state state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<worker_runtime_state> state_{worker_runtime_state::ready};
};

// The first failure of a runtime owner, kept until the owner rethrows it or
// hands it to the unhandled-failure diagnostic. Later failures lose. The owner
// guards it with its own lifecycle lock so recording and waking its waiters
// stay one critical section.
class first_failure final {
public:
    // Returns true for the winning failure. `observed` records that a consumer
    // (such as a failure hook) already owns it.
    [[nodiscard]] bool record(std::exception_ptr failure, bool observed) noexcept {
        if (failure == nullptr || first_ != nullptr) {
            return false;
        }
        first_ = std::move(failure);
        observed_ = observed;
        return true;
    }

    [[nodiscard]] const std::exception_ptr& first() const noexcept {
        return first_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return first_ != nullptr;
    }

    // Returns the first failure, if any, for the caller to rethrow.
    [[nodiscard]] std::exception_ptr observe() noexcept {
        observed_ = true;
        return first_;
    }

    // Releases a first failure nobody observed; the caller must rethrow or
    // report it. A later failure may then become the next first one.
    [[nodiscard]] std::exception_ptr take_unobserved() noexcept {
        if (observed_) {
            return {};
        }
        return std::exchange(first_, nullptr);
    }

private:
    std::exception_ptr first_;
    bool observed_{false};
};
}  // namespace
class worker_runtime::impl_type final {
public:
    enum class phase_type : std::uint8_t { pending,
        reached,
        aborted };

    explicit impl_type(worker_runtime_options options) :
        io_context_(asio::config_from_concurrency_hint(options.io_policy_ == worker_io_policy::single_owner ? ASIO_CONCURRENCY_HINT_UNSAFE_IO : ASIO_CONCURRENCY_HINT_DEFAULT)),
        context_(io_context_, options.queue_capacity_, options.blocking_pool_),
        stop_(context_) {
        stop_.configure({
            .stop_admission_ =
                [this] {
            begin_stop_admission();
        },
            // The hook's target may reenter finalize() while it moves or dies.
            .finalize_ = [this] {
            auto hook = std::move(hooks_.finalize_);
            if (hook) {
                hook();
            }
        },
        });
    }

    // Returns true for the winning failure.
    [[nodiscard]] bool record_failure(std::exception_ptr failure, bool observed) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!failure_.record(failure, observed)) {
                return false;
            }
        }
        // Publish the cause before stopping another member wakes a group phase
        // waiter. The user's failure hook remains a separate notification.
        notify_group(std::move(failure));
        phase_changed_.notify_all();
        return true;
    }

    void bind_group(worker_group& group, worker_runtime& member) {
        const std::lock_guard lock(mutex_);
        if (group_ != nullptr || group_notifications_ != 0 || failure_.failed() || lifecycle_.state() != worker_runtime_state::ready) {
            throw std::logic_error("worker group member must be cold and belong to one group");
        }
        group_ = &group;
        group_member_ = &member;
    }

    void unbind_group(worker_group& group) noexcept {
        std::unique_lock lock(mutex_);
        if (group_ != &group) {
            std::terminate();
        }
        group_ = nullptr;
        group_member_ = nullptr;
        condition_.wait(lock, [this] {
            return group_notifications_ == 0;
        });
    }

    void notify_group(std::exception_ptr failure = {}) noexcept {
        worker_group* group = nullptr;
        worker_runtime* member = nullptr;
        {
            const std::lock_guard lock(mutex_);
            group = group_;
            if (group == nullptr) {
                return;
            }
            member = group_member_;
            ++group_notifications_;
        }
        // Group stop may reenter this runtime or notify other members. Keep no
        // runtime lock across it, and retire the observation before user hooks.
        if (failure) {
            group->member_failed(*member, std::move(failure));
        } else {
            group->member_stopping();
        }
        const std::lock_guard lock(mutex_);
        --group_notifications_;
        condition_.notify_all();
    }

    bool fail(std::exception_ptr failure) noexcept {
        const bool first = record_failure(failure, static_cast<bool>(hooks_.failure_));
        if (failure != nullptr) {
            request_stop();
        }
        if (first && hooks_.failure_) {
            try {
                hooks_.failure_(std::move(failure));
            } catch (...) {
                std::terminate();
            }
        }
        return first;
    }

    [[nodiscard]] std::exception_ptr unobserved_failure() noexcept {
        const std::lock_guard lock(mutex_);
        return failure_.take_unobserved();
    }

    void configure(worker_runtime_hooks hooks) {
        {
            const std::lock_guard lock(mutex_);
            if (configured_ || lifecycle_.state() != worker_runtime_state::ready) {
                throw std::logic_error("worker runtime hooks must be configured once before launch");
            }
            configured_ = true;
        }
        // Configuration precedes publication and launch. The reservation
        // rejects reentry; callable moves run without the runtime mutex.
        hooks_ = std::move(hooks);
    }

    // Owner thread, once, from the stop chain. A runtime with a loop first
    // drains the loop's retirement gate, which then resumes here through
    // finish_loop_stop_admission().
    void begin_stop_admission() {
        std::shared_ptr<detail::event_loop_state> loop;
        {
            const std::lock_guard lock(mutex_);
            loop = loop_;
            stop_admission_delegated_ = loop != nullptr;
        }
        if (loop) {
            loop->complete_stop_admission();
            return;
        }
        run_stop_admission();
    }

    void finish_loop_stop_admission() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!std::exchange(stop_admission_delegated_, false)) {
                return;
            }
        }
        run_stop_admission();
    }

    void run_stop_admission() noexcept {
        try {
            if (hooks_.stop_admission_) {
                hooks_.stop_admission_();
            } else {
                finalize();
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<detail::event_loop_state> loop(worker_runtime& owner) {
        {
            const std::lock_guard lock(mutex_);
            if (loop_) {
                return loop_;
            }
        }
        auto created = std::make_shared<detail::event_loop_state>(owner);
        created->failure_sink_ = [&owner](std::exception_ptr failure) {
            (void)owner.fail(std::move(failure));
        };
        {
            const std::lock_guard lock(mutex_);
            if (loop_) {
                return loop_;
            }
            loop_ = created;
        }
        // A worker that already stops never notifies the loop; retire its
        // admission directly.
        if (context_.worker().register_shutdown(created, &detail::event_loop_state::shutdown_registration_) != worker_registration_status::registered) {
            created->stop();
        }
        return created;
    }

    [[nodiscard]] bool start() {
        std::unique_lock lock(mutex_);
        if (start_called_) {
            throw std::logic_error("worker runtime can only be started once");
        }
        start_called_ = true;
        if (!lifecycle_.start()) {
            return false;
        }
        try {
            thread_ = std::thread([this] {
                run(true);
            });
        } catch (...) {
            const auto failure = std::current_exception();
            lock.unlock();
            (void)record_failure(failure, true);
            request_stop();
            std::rethrow_exception(failure);
        }
        return true;
    }

    void mark_ready() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ != phase_type::pending || lifecycle_.state() != worker_runtime_state::running) {
                return;
            }
            ready_ = phase_type::reached;
        }
        phase_changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(const phase_type& phase) {
        std::unique_lock lock(mutex_);
        if (lifecycle_.state() == worker_runtime_state::ready) {
            throw std::logic_error("worker runtime has not been launched");
        }
        phase_changed_.wait(lock, [&] {
            return phase != phase_type::pending || failure_.failed();
        });
        if (const auto failure = failure_.observe()) {
            std::rethrow_exception(failure);
        }
        return phase == phase_type::reached;
    }

    void request_serve() {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ == phase_type::pending) {
                throw std::logic_error("worker runtime must be ready before it serves");
            }
            if (ready_ != phase_type::reached || serve_requested_) {
                return;
            }
            serve_requested_ = true;
        }
        // A refused control means stopping already aborted the serving phase.
        (void)post_control([this] {
            apply_serve();
        });
    }

    void apply_serve() noexcept {
        if (lifecycle_.state() != worker_runtime_state::running) {
            return;
        }
        try {
            if (hooks_.serve_) {
                hooks_.serve_();
            }
        } catch (...) {
            (void)fail(std::current_exception());
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            if (serving_ != phase_type::pending || lifecycle_.state() != worker_runtime_state::running) {
                return;
            }
            serving_ = phase_type::reached;
        }
        phase_changed_.notify_all();
    }

    void abort_phases() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ == phase_type::pending) {
                ready_ = phase_type::aborted;
            }
            if (serving_ == phase_type::pending) {
                serving_ = phase_type::aborted;
            }
        }
        phase_changed_.notify_all();
    }

    void request_stop() noexcept {
        bool dispatch = false;
        {
            const std::lock_guard lock(mutex_);
            dispatch = lifecycle_.request_stop();
            if (dispatch) {
                stop_dispatching_ = true;
                stop_dispatch_thread_ = std::this_thread::get_id();
            }
        }
        if (dispatch) {
            // Publish independent stop before shutdown listeners can reenter
            // group stop. Its self-stop recursion closes admission through the
            // same nonwaiting path as every other repeat caller.
            notify_group();
        }
        // Losing the lifecycle transition does not imply its winner has
        // closed admission yet: it may still be notifying a group.
        // Every caller establishes the closed gate.
        context_.close();
        if (!dispatch) {
            return;
        }
        stop_.request_stop();
        abort_phases();
#if defined(_WIN32)
        thread_join_operation* waiters;
#endif
        {
            const std::lock_guard lock(mutex_);
            stop_dispatching_ = false;
            stop_dispatch_thread_ = {};
#if defined(_WIN32)
            waiters = std::exchange(thread_waiters_, nullptr);
#endif
        }
        condition_.notify_all();
#if defined(_WIN32)
        while (waiters) {
            auto* current = waiters;
            waiters = current->next_;
            current->next_ = nullptr;
            auto caller = current->caller_;
            const auto continuation = std::exchange(current->continuation_, {});
            caller.defer_resume(continuation);
        }
#endif
    }

    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        if (lifecycle_.state() != worker_runtime_state::running) {
            return false;
        }
        return stop_.post_control(std::move(control));
    }

    void finalize() noexcept {
        request_stop();
        stop_.finalize();
    }

    void run(bool launch_startup) noexcept {
        try {
            context_.run([this, launch_startup] {
                if (launch_startup && hooks_.startup_) {
                    hooks_.startup_();
                }
            }, [this](std::exception_ptr failure) noexcept {
                (void)fail(std::move(failure));
            }, [this] {
                if (hooks_.shutdown_) {
                    hooks_.shutdown_();
                }
            });
        } catch (...) {
            (void)record_failure(std::current_exception(), false);
        }
        // Detach before the context or domain owners can disappear. Unfinished
        // post factories own their stable endpoint, not this execution context.
        context_.detach();
        (void)lifecycle_.request_stop();
        abort_phases();
        lifecycle_.complete_stop();
    }

#if defined(_WIN32)
    std::uintptr_t prepare_thread_join() {
        if (context_.worker().is_current()) {
            throw std::logic_error("cannot join a worker runtime from its owner");
        }
        request_stop();
        const std::lock_guard lock(mutex_);
        if (joining_ || stop_dispatching_) {
            throw std::logic_error("cannot prepare a worker thread wait while joining or dispatching its stop hooks");
        }
        if (joined_) {
            return 0;
        }
        if (!thread_.joinable()) {
            thread_ = std::thread([this] {
                run(false);
            });
        }
        HANDLE duplicate{};
        if (!::DuplicateHandle(::GetCurrentProcess(), thread_.native_handle(), ::GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) {
            const auto error = ::GetLastError();
            if (!std::in_range<int>(error)) {
                throw std::runtime_error("worker thread handle duplication returned an out-of-range system error");
            }
            throw std::system_error(static_cast<int>(error), std::system_category(), "failed to duplicate worker thread wait handle");
        }
        return reinterpret_cast<std::uintptr_t>(duplicate);
    }
#endif

    void join() {
        if (context_.worker().is_current()) {
            throw std::logic_error("cannot join a worker runtime from its owner");
        }
        request_stop();
        std::thread joining_thread;
        bool drain_unstarted = false;
        {
            std::unique_lock lock(mutex_);
            if (stop_dispatching_ && stop_dispatch_thread_ == std::this_thread::get_id()) {
                throw std::logic_error("cannot join a worker runtime while dispatching its stop hooks");
            }
            // Wait for an external stop dispatch before establishing an owner
            // and draining its work, even when our stop lost its transition.
            condition_.wait(lock, [this] {
                return !joining_ && !stop_dispatching_;
            });
            if (joined_) {
                return;
            }
            joining_ = true;
            if (thread_.joinable()) {
                joining_thread = std::move(thread_);
            } else {
                drain_unstarted = true;
            }
        }
        if (drain_unstarted) {
            try {
                joining_thread = std::thread([this] {
                    run(false);
                });
            } catch (...) {
                // Thread creation failure does not waive accepted work or
                // owner-affine cleanup. Establish owner identity synchronously.
                (void)record_failure(std::current_exception(), false);
                run(false);
            }
        }
        if (joining_thread.joinable()) {
            joining_thread.join();
        }
        {
            const std::lock_guard lock(mutex_);
            joining_ = false;
            joined_ = true;
        }
        condition_.notify_all();
    }

    asio::io_context io_context_;
    worker_runtime_context context_;
    detail::worker_stop_chain stop_;
    runtime_lifecycle lifecycle_;
    worker_runtime_hooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable phase_changed_;
    std::thread thread_;
    first_failure failure_;
    phase_type ready_{phase_type::pending};
    phase_type serving_{phase_type::pending};
    bool serve_requested_{};
    bool configured_{};
    bool start_called_{};
    bool stop_dispatching_{};
    std::thread::id stop_dispatch_thread_{};
#if defined(_WIN32)
    thread_join_operation* thread_waiters_{};
#endif
    bool joining_{};
    bool joined_{};
    worker_group* group_{nullptr};
    worker_runtime* group_member_{nullptr};
    std::size_t group_notifications_{0};
    std::shared_ptr<detail::event_loop_state> loop_;
    bool stop_admission_delegated_{false};
};

worker_runtime::worker_runtime(worker_runtime_options options) :
    impl_(std::make_unique<impl_type>(options)) {}

worker_runtime::~worker_runtime() {
    impl_->request_stop();
    try {
        impl_->join();
    } catch (...) {
        // An owner cannot safely detach a running thread to recover self-join.
        std::terminate();
    }
    if (const auto failure = impl_->unobserved_failure()) {
        report_unhandled_failure("worker runtime", failure);
    }
}

void worker_runtime::configure(worker_runtime_hooks hooks) {
    impl_->configure(std::move(hooks));
}

void worker_runtime::bind_group(worker_group& group) {
    impl_->bind_group(group, *this);
}

void worker_runtime::unbind_group(worker_group& group) noexcept {
    impl_->unbind_group(group);
}

bool worker_runtime::start() {
    return impl_->start();
}

void worker_runtime::mark_ready() noexcept {
    impl_->mark_ready();
}

bool worker_runtime::wait_until_ready() {
    return impl_->wait_for(impl_->ready_);
}

void worker_runtime::request_serve() {
    impl_->request_serve();
}

bool worker_runtime::wait_until_serving() {
    return impl_->wait_for(impl_->serving_);
}

void worker_runtime::request_stop() noexcept {
    impl_->request_stop();
}

bool worker_runtime::fail(std::exception_ptr failure) noexcept {
    return impl_->fail(std::move(failure));
}

bool worker_runtime::post_control(move_only_function<void()> control) noexcept {
    return impl_->post_control(std::move(control));
}

void worker_runtime::finalize() noexcept {
    impl_->finalize();
}

void worker_runtime::join() {
    impl_->join();
}
#if defined(_WIN32)
worker_runtime::thread_join_operation::thread_join_operation(worker_runtime& owner, worker_ref caller) noexcept :
    owner_(&owner),
    caller_(std::move(caller)) {}

worker_runtime::thread_join_operation::~thread_join_operation() {
    if (started_ && !resumed_) {
        std::terminate();
    }
}

bool worker_runtime::thread_join_operation::await_ready() {
    if (!caller_.is_current()) {
        throw std::logic_error("worker thread join preparation must run on its caller worker");
    }
    if (owner_->worker().is_current()) {
        throw std::logic_error("cannot join a worker runtime from its owner");
    }
    owner_->request_stop();
    const std::lock_guard lock(owner_->impl_->mutex_);
    if (owner_->impl_->joining_ || (owner_->impl_->stop_dispatching_ && owner_->impl_->stop_dispatch_thread_ == std::this_thread::get_id())) {
        throw std::logic_error("cannot prepare a worker thread wait during join or its own stop dispatch");
    }
    started_ = true;
    return !owner_->impl_->stop_dispatching_;
}

bool worker_runtime::thread_join_operation::await_suspend(std::coroutine_handle<> continuation) {
    const std::lock_guard lock(owner_->impl_->mutex_);
    if (!owner_->impl_->stop_dispatching_) {
        return false;
    }
    continuation_ = continuation;
    next_ = owner_->impl_->thread_waiters_;
    owner_->impl_->thread_waiters_ = this;
    return true;
}

std::uintptr_t worker_runtime::thread_join_operation::await_resume() {
    resumed_ = true;
    return owner_->impl_->prepare_thread_join();
}

worker_runtime::thread_join_operation worker_runtime::prepare_thread_join(worker_ref caller) & {
    return thread_join_operation(*this, std::move(caller));
}
#endif

std::exception_ptr worker_runtime::take_failure() noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->failure_.observe();
}

std::exception_ptr worker_runtime::recorded_failure() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->failure_.first();
}

worker_runtime_state worker_runtime::state() const noexcept {
    return impl_->lifecycle_.state();
}

asio::io_context& worker_runtime::io_context() & noexcept {
    return impl_->io_context_;
}

const worker_ref& worker_runtime::worker() const& noexcept {
    return impl_->context_.worker();
}

event_loop worker_runtime::loop() & {
    return event_loop(impl_->loop(*this));
}

void worker_runtime::finish_loop_stop_admission() noexcept {
    impl_->finish_loop_stop_admission();
}

worker_runtime_context& worker_runtime::context() & noexcept {
    return impl_->context_;
}

worker_runtime_context::worker_runtime_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool) :
    io_context_(&io_context),
    dispatcher_(std::make_shared<detail::worker_dispatcher>(io_context, queue_capacity, pool)),
    worker_(detail::worker_ref_access::make(dispatcher_)) {}

worker_runtime_context::~worker_runtime_context() {
    detach();
}

asio::io_context& worker_runtime_context::io_context() const& noexcept {
    return *io_context_;
}

const worker_ref& worker_runtime_context::worker() const& noexcept {
    return worker_;
}

void worker_runtime_context::run() {
    dispatcher_->run_context({}, {}, {});
}

void worker_runtime_context::run(move_only_function<void()> startup_handler, move_only_function<void(std::exception_ptr)> failure_handler, move_only_function<void()> shutdown_handler) {
    dispatcher_->run_context(std::move(startup_handler), std::move(failure_handler), std::move(shutdown_handler));
}

void worker_runtime_context::close() noexcept {
    dispatcher_->close();
}

void worker_runtime_context::stop_timers() noexcept {
    dispatcher_->stop_timers();
}

void worker_runtime_context::detach() noexcept {
    dispatcher_->detach_context();
}
}  // namespace ruvia

namespace ruvia::detail {

worker_stop_chain::worker_stop_chain(worker_runtime_context& context) :
    context_(context),
    work_(asio::make_work_guard(context.io_context())) {}

void worker_stop_chain::configure(hooks_type hooks) noexcept {
    {
        const std::lock_guard lock(mutex_);
        std::swap(hooks_, hooks);
    }
}

void worker_stop_chain::request_stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
    }
    context_.close();
    try {
        // close() can lose the race to a dispatcher failure that is still
        // notifying listeners. Keep the work guard until all of them have
        // published their cleanup.
        worker_ref_access::when_shutdown_notifications_complete(context_.worker(), [this] {
            const std::lock_guard lock(mutex_);
            defer_stop_control_locked();
        });
    } catch (...) {
        std::terminate();
    }
}

void worker_stop_chain::finalize() noexcept {
    request_stop();
    const std::lock_guard lock(mutex_);
    if (finalization_requested_) {
        return;
    }
    finalization_requested_ = true;
    // A stop control that has not yet run consumes this request itself.
    if (stop_control_ran_ && !work_released_) {
        defer_stop_control_locked();
    }
}

bool worker_stop_chain::post_control(move_only_function<void()> control) noexcept {
    const std::lock_guard lock(mutex_);
    if (work_released_) {
        return false;
    }
    worker_ref_access::defer_or_terminate(context_.worker(), std::move(control));
    return true;
}

void worker_stop_chain::abandon() noexcept {
    std::optional<work_guard_type> work;
    {
        const std::lock_guard lock(mutex_);
        stop_requested_ = true;
        work_released_ = true;
        work.emplace(std::move(work_));
    }
    context_.stop_timers();
}

void worker_stop_chain::defer_stop_control_locked() noexcept {
    worker_ref_access::defer_or_terminate(context_.worker(), [this] {
        apply_stop();
    });
}

void worker_stop_chain::apply_stop() noexcept {
    if (!stop_applied_) {
        stop_applied_ = true;
        try {
            if (hooks_.stop_admission_) {
                hooks_.stop_admission_();
            } else {
                finalize();
            }
        } catch (...) {
            std::terminate();
        }
        context_.stop_timers();
    }
    // Declared before the hook so the work guard is released after it.
    std::optional<work_guard_type> work;
    move_only_function<void()> finalize_hook;
    {
        const std::lock_guard lock(mutex_);
        stop_control_ran_ = true;
        if (!finalization_requested_ || work_released_) {
            return;
        }
        work_released_ = true;
        work.emplace(std::move(work_));
    }
    // Only this finalizer owns the hook after work_released_ is published.
    // Its target's move/destructor may reenter finalize(), so move it unlocked.
    finalize_hook = std::move(hooks_.finalize_);
    try {
        if (finalize_hook) {
            finalize_hook();
        }
    } catch (...) {
        std::terminate();
    }
}

}  // namespace ruvia::detail

namespace ruvia {
worker_group::worker_group() = default;

worker_group::~worker_group() {
    for (auto* member : members_) {
        member->unbind_group(*this);
    }
}

void worker_group::add(worker_runtime& member) {
    const std::lock_guard lock(mutex_);
    if (sealed_ || stopping_) {
        throw std::logic_error("worker group members must be added before start or stop");
    }
    members_.push_back(&member);
    try {
        member.bind_group(*this);
    } catch (...) {
        members_.pop_back();
        throw;
    }
}

void worker_group::launch() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return;
            }
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        (void)join(launch_failure);
        std::rethrow_exception(launch_failure);
    }
}

bool worker_group::start() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return settle(false, "ready");
            }
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_ready(), "ready")) {
                return false;
            }
        }
        const bool reached = settle(true, "ready");
        if (reached) {
            const std::lock_guard lock(mutex_);
            ready_ = true;
        }
        return reached;
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

bool worker_group::serve() {
    {
        const std::lock_guard lock(mutex_);
        if (!ready_) {
            throw std::logic_error("worker group must be ready before it serves");
        }
        if (serve_requested_) {
            throw std::logic_error("worker group can only serve once");
        }
        serve_requested_ = true;
    }
    try {
        for (auto* member : members_) {
            if (stop_requested()) {
                return settle(false, "serving");
            }
            member->request_serve();
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_serving(), "serving")) {
                return false;
            }
        }
        return settle(true, "serving");
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

void worker_group::member_failed(worker_runtime& member, std::exception_ptr failure) noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
            failed_member_ = &member;
        }
    }
    request_stop();
}

void worker_group::member_stopping() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            member_stopped_ = true;
        }
    }
    request_stop();
}

void worker_group::request_stop() noexcept {
    bool dispatch = false;
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
            stop_dispatching_ = true;
            dispatch = true;
        }
    }
    // Publish group stop before notifying members, which can reenter it.
    // Every caller closes admission on all members; repeats are nonwaiting
    // even while the winning dispatch is notifying an earlier member.
    for (auto* member : members_) {
        member->request_stop();
    }
    if (!dispatch) {
        return;
    }
    const std::lock_guard lock(mutex_);
    stop_dispatching_ = false;
    // Notify before releasing the mutex: a waking join may retire this owner.
    stop_changed_.notify_all();
}

bool worker_group::stop_requested() const noexcept {
    const std::lock_guard lock(mutex_);
    return stopping_;
}

std::exception_ptr worker_group::join(std::exception_ptr primary) noexcept {
    request_stop();
    {
        std::unique_lock lock(mutex_);
        stop_changed_.wait(lock, [this] {
            return !stop_dispatching_ && !joining_;
        });
        if (joined_) {
            return join_failure_;
        }
        joining_ = true;
    }
    for (auto* member : members_) {
        try {
            member->join();
        } catch (...) {
            // join() only rejects a member's own thread, which can never
            // own the lifecycle; a stranded owner cannot be recovered.
            std::terminate();
        }
    }
    // The returned failure is settled before any report, so it is never also
    // reported here: the caller's primary, else the first member failure in
    // registration order.
    if (primary == nullptr) {
        for (auto* member : members_) {
            if (auto failure = member->recorded_failure(); failure != nullptr) {
                primary = std::move(failure);
                break;
            }
        }
    }
    // Workers commonly fail for one shared cause (an unreachable dependency),
    // so equivalent failures collapse into a single line with their count.
    // Each member's failure is read once with ownership: it is returned as the
    // primary, collapsed into it, or reported here.
    for (std::size_t index = 0; index != members_.size(); ++index) {
        const auto failure = members_[index]->take_failure();
        if (failure == nullptr || equivalent_failures(failure, primary)) {
            continue;
        }
        bool reported = false;
        for (std::size_t earlier = 0; earlier != index && !reported; ++earlier) {
            reported = equivalent_failures(members_[earlier]->recorded_failure(), failure);
        }
        if (reported) {
            continue;
        }
        std::size_t occurrences = 1;
        for (auto later = index + 1; later != members_.size(); ++later) {
            if (equivalent_failures(members_[later]->recorded_failure(), failure)) {
                ++occurrences;
            }
        }
        report_unhandled_failure("additional worker", failure, occurrences);
    }
    const std::lock_guard lock(mutex_);
    join_failure_ = std::move(primary);
    joining_ = false;
    joined_ = true;
    stop_changed_.notify_all();
    return join_failure_;
}

bool worker_group::launch(worker_runtime& member) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return false;
        }
    }
    // A concurrent stop (group-wide or of this member) can win the runtime's
    // launch transition; start() then reports it instead of launching.
    return member.start();
}

void worker_group::wait_for_stop_dispatch() noexcept {
    std::unique_lock lock(mutex_);
    stop_changed_.wait(lock, [this] {
        return !stop_dispatching_;
    });
}

void worker_group::rethrow_phase_failure(std::exception_ptr fallback) {
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    std::rethrow_exception(failure ? failure : std::move(fallback));
}

std::exception_ptr worker_group::observe_phase_failure() noexcept {
    std::exception_ptr failure;
    worker_runtime* member = nullptr;
    {
        const std::lock_guard lock(mutex_);
        failure = failure_;
        member = failed_member_;
    }
    if (member != nullptr) {
        // A phase waiter may be on a different, healthy member. Observe only
        // the exception actually delivered, preserving other failure reports.
        static_cast<void>(member->take_failure());
    }
    return failure;
}

bool worker_group::settle(bool reached, const char* phase) {
    bool was_stopping = false;
    {
        const std::lock_guard lock(mutex_);
        was_stopping = stopping_;
        if (reached && !stopping_ && !failure_) {
            return true;
        }
    }
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    bool member_stopped = false;
    {
        const std::lock_guard lock(mutex_);
        member_stopped = member_stopped_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (member_stopped || (!reached && !was_stopping)) {
        throw std::runtime_error(std::string("worker stopped before the group was ") + phase);
    }
    return false;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::size_t default_loop_count() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

}  // namespace

struct event_loop_pool::impl_type final {
    explicit impl_type(event_loop_pool_options options) {
        const auto count = options.loop_count_ == 0 ? default_loop_count() : options.loop_count_;
        if (options.queue_capacity_ == 0) {
            throw std::invalid_argument("event loop queue capacity must be greater than zero");
        }
        loops_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops_.push_back(std::make_shared<detail::event_loop_state>(options.queue_capacity_, options.blocking_pool_));
            loops_.back()->install_shutdown_registration();
        }
    }

    void bind_callbacks(const std::shared_ptr<impl_type>& self) {
        const std::weak_ptr<impl_type> weak_owner = self;
        for (const auto& loop : loops_) {
            const std::weak_ptr<detail::event_loop_state> weak_loop = loop;
            loop->failure_sink_ = [weak_owner, weak_loop](std::exception_ptr failure) noexcept {
                if (const auto owner = weak_owner.lock()) {
                    if (const auto locked_loop = weak_loop.lock()) {
                        owner->report_failure(*locked_loop, std::move(failure));
                        return;
                    }
                }
                report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            };
            auto* const stable_runtime = &loop->owned_runtime();
            stable_runtime->configure({
                .stop_admission_ = [weak_loop, stable_runtime] {
                if (const auto locked_loop = weak_loop.lock()) {
                    locked_loop->complete_stop_admission();
                } else {
                    // Construction rollback can destroy an unpublished
                    // state before its core owner drains this control.
                    stable_runtime->finalize();
                }
            },
            });
            group_.add(*stable_runtime);
        }
    }

    void report_failure(detail::event_loop_state& loop, std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        bool joined = false;
        {
            const std::lock_guard lock(failure_mutex_);
            joined = joined_;
        }
        if (joined) {
            report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            return;
        }
        if (!loop.owned_runtime().fail(failure)) {
            if (!equivalent_failures(failure, loop.recorded_failure())) {
                report_unhandled_failure("additional event loop failure", failure);
            }
        }
        stop();
    }

    void start() {
        group_.launch();
    }

    void stop() noexcept {
        group_.request_stop();
        for (const auto& loop : loops_) {
            loop->stop();
        }
    }

    [[nodiscard]] std::exception_ptr join() noexcept {
        auto failure = group_.join();
        for (const auto& loop : loops_) {
            detail::worker_ref_access::wait_for_reservations(loop->runtime_.worker());
        }
        const std::lock_guard lock(failure_mutex_);
        if (joined_) {
            return nullptr;
        }
        joined_ = true;
        return failure;
    }

    std::vector<std::shared_ptr<detail::event_loop_state>> loops_;
    worker_group group_;
    std::mutex failure_mutex_;
    bool joined_{false};
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options) :
    impl_(std::make_shared<impl_type>(options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    if (std::ranges::any_of(impl_->loops_, [](const auto& loop) {
        return loop->runtime_.worker().is_current();
    })) {
        std::terminate();
    }
    impl_->stop();
    if (const auto failure = impl_->join()) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever observed, and a destructor
        // cannot rethrow it further. Report rather than end here.
        report_unhandled_failure("event loop pool", failure);
    }
}

void event_loop_pool::start() {
    impl_->start();
}

void event_loop_pool::stop() noexcept {
    impl_->stop();
}

void event_loop_pool::join() {
    if (std::ranges::any_of(impl_->loops_, [](const auto& loop) {
        return loop->runtime_.worker().is_current();
    })) {
        throw std::logic_error("cannot join an event loop pool from one of its workers");
    }
    impl_->stop();
    if (const auto failure = impl_->join()) {
        std::rethrow_exception(failure);
    }
}

std::size_t event_loop_pool::loop_count() const noexcept {
    return impl_->loops_.size();
}

event_loop event_loop_pool::loop(std::size_t index) const {
    if (index >= impl_->loops_.size()) {
        throw std::out_of_range("event loop index is out of range");
    }
    return event_loop(impl_->loops_[index]);
}

event_loop event_loop_pool::next_loop() noexcept {
    const auto index = impl_->next_index_.fetch_add(1, std::memory_order_relaxed);
    return event_loop(impl_->loops_[index % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::uint64_t key) const noexcept {
    return event_loop(impl_->loops_[key % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::string_view key) const noexcept {
    return loop_for(worker_selection_hash(key));
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Routes through the loop while it is alive, or to the unhandled-failure
// diagnostic under `context` once it has retired.
void report_through_loop(const std::weak_ptr<detail::event_loop_state>& loop, std::exception_ptr failure, const char* context) noexcept {
    if (const auto state_value = loop.lock()) {
        state_value->route_failure(std::move(failure), context);
        return;
    }
    report_unhandled_failure(context, std::move(failure));
}

}  // namespace

namespace detail {

class external_context_attachment_service final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;

    explicit external_context_attachment_service(asio::execution_context& context_value) :
        asio::execution_context::service(context_value) {}

    [[nodiscard]] bool claim() noexcept {
        const std::lock_guard lock(mutex_);
        if (claimed_) {
            return false;
        }
        claimed_ = true;
        return true;
    }

    void release_claim() noexcept {
        const std::lock_guard lock(mutex_);
        claimed_ = false;
    }

    void retain(std::shared_ptr<event_loop_state> state_value) noexcept {
        {
            const std::lock_guard lock(mutex_);
            state_.swap(state_value);
        }
    }

    void release_state(event_loop_state* state_value) noexcept {
        std::shared_ptr<event_loop_state> abandoned;
        {
            const std::lock_guard lock(mutex_);
            if (state_.get() != state_value) {
                return;
            }
            abandoned = std::move(state_);
            claimed_ = false;
        }
        abandoned.reset();
    }

private:
    void shutdown() override {
        std::shared_ptr<event_loop_state> state;
        {
            const std::lock_guard lock(mutex_);
            state = std::move(state_);
            claimed_ = false;
        }
        if (state) {
            // The context is entering Asio's service shutdown, so no new handler
            // may be queued here. Retire the worker before the context destroys
            // its scheduler. The state may still be held by event_loop/Attachment;
            // those handles become terminal and no longer expose a dangling
            // io_context reference.
            state->shutdown_external_context();
        }
    }

    std::mutex mutex_;
    bool claimed_{false};
    std::shared_ptr<event_loop_state> state_;
};

asio::execution_context::id external_context_attachment_service::id;

external_context_claim::external_context_claim(asio::io_context& io_context) :
    service_(&asio::use_service<external_context_attachment_service>(io_context)) {
    if (!service_->claim()) {
        throw std::invalid_argument("an io_context can have only one Ruvia event loop attachment");
    }
}

external_context_claim::~external_context_claim() {
    if (!retained_) {
        service_->release_claim();
    }
}

void external_context_claim::retain(std::shared_ptr<event_loop_state> state_value) noexcept {
    service_->retain(std::move(state_value));
    retained_ = true;
}

void external_context_claim::release(event_loop_state* state_value) noexcept {
    service_->release_state(state_value);
}

attached_context::attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure) :
    claim_(io_context),
    runtime_(io_context, queue_capacity, pool),
    stop_(runtime_),
    failure_(std::move(failure)) {
    worker_ref_access::enable_external_runner(runtime_.worker());
}

// The callback owner is kept alive by the completion handler until both the
// returned task frame and its completion chain have retired.
class event_loop_stop_listener final : public std::enable_shared_from_this<event_loop_stop_listener> {
public:
    event_loop_stop_listener(worker_ref worker_value, asio::io_context::executor_type executor, move_only_function<task<void>()> callback_value, std::weak_ptr<event_loop_state> loop) :
        worker_(std::move(worker_value)),
        executor_(std::move(executor)),
        callback_(std::move(callback_value)),
        loop_(std::move(loop)) {}

    void install_shutdown_registration() {
        if (worker_.register_shutdown(shared_from_this(), &event_loop_stop_listener::shutdown_registration_) != worker_registration_status::registered) {
            throw std::runtime_error("cannot register a stop callback on a stopping event loop");
        }
    }

    void cancel() noexcept {
        move_only_function<task<void>()> abandoned;
        std::shared_ptr<event_loop_state> active_loop;
        {
            const std::lock_guard lock(mutex_);
            if (started_ || cancelled_) {
                return;
            }
            cancelled_ = true;
            if (std::exchange(retirement_begun_, false)) {
                active_loop = std::move(active_loop_);
            }
        }
        // Cancellation owns the callback after publishing cancelled_; neither
        // the stop notification nor a deferred start may use it now.
        abandoned = std::move(callback_);
        abandoned = nullptr;
        if (active_loop) {
            active_loop->retirement_.finish();
        }
    }

    void worker_stopping() noexcept {
        auto loop = loop_.lock();
        if (!loop) {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || retirement_begun_ || started_) {
                return;
            }
            loop->retirement_.begin();
            active_loop_ = std::move(loop);
            retirement_begun_ = true;
        }
        auto self = shared_from_this();
        if (worker_.is_current()) {
            start_callback(std::move(self));
            return;
        }
        worker_ref_access::defer_or_terminate(worker_, [self = std::move(self)]() mutable noexcept {
            self->start_callback(std::move(self));
        });
    }

private:
    void report(std::exception_ptr failure) const noexcept {
        report_through_loop(loop_, std::move(failure), "event loop stop callback");
    }

    void start_callback(std::shared_ptr<event_loop_stop_listener> self) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || !retirement_begun_) {
                return;
            }
            started_ = true;
        }
        std::optional<task<void>> task;
        try {
            task.emplace(callback_());
        } catch (const std::bad_alloc&) {
            report(std::current_exception());
            std::terminate();
        } catch (...) {
            report(std::current_exception());
            callback_ = nullptr;
            auto active_loop = std::move(active_loop_);
            active_loop->retirement_.finish();
            return;
        }

        try {
            async_start(std::move(*task), asio::bind_executor(executor_, [self = std::move(self)](std::exception_ptr failure) mutable {
                if (failure != nullptr) {
                    self->report(std::move(failure));
                }
                self->callback_ = nullptr;
                auto active_loop = std::move(self->active_loop_);
                active_loop->retirement_.finish();
            }));
        } catch (...) {
            report(std::current_exception());
            std::terminate();
        }
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    asio::io_context::executor_type executor_;
    std::mutex mutex_;
    bool retirement_begun_{false};
    bool cancelled_{false};
    bool started_{false};
    move_only_function<task<void>()> callback_;
    std::weak_ptr<event_loop_state> loop_;
    std::shared_ptr<event_loop_state> active_loop_;
};

event_loop_state::event_loop_state(std::size_t queue_capacity, blocking_pool* pool) :
    context_ownership_(std::in_place_type<worker_runtime>, worker_runtime_options{.queue_capacity_ = queue_capacity, .blocking_pool_ = pool}),
    io_context_(&owned_runtime().io_context()),
    executor_(io_context_->get_executor()),
    runtime_(owned_runtime().context()) {}

event_loop_state::event_loop_state(worker_runtime& runtime) :
    context_ownership_(std::in_place_type<borrowed_runtime>, &runtime),
    io_context_(&runtime.io_context()),
    executor_(io_context_->get_executor()),
    runtime_(runtime.context()) {}

event_loop_state::event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure) :
    context_ownership_(std::in_place_type<attached_context>, external_context, queue_capacity, pool, std::move(failure)),
    io_context_(std::addressof(external_context)),
    executor_(external_context.get_executor()),
    runtime_(std::get_if<attached_context>(&context_ownership_)->runtime_) {}

void event_loop_state::attach(const std::shared_ptr<event_loop_state>& self) {
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    attached.stop_.configure({
        // Only stop() requests this chain, so its gate is already sealing.
        // Joining it keeps the chain from finalizing before the gate drains.
        .stop_admission_ =
            [this] {
        complete_stop_admission();
    },
        // Releasing the claim can destroy this state together with its chain;
        // the chain then releases its work guard on the still-running context.
        .finalize_ = [this, &attached] {
        runtime_.detach();
        attached.claim_.release(this);
    },
    });
    attached.claim_.retain(self);
}

void event_loop_state::shutdown_external_context() noexcept {
    // Only attach() retains a state with this service, so it is an attached loop.
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    retirement_.close_admission();
    if (retirement_.pending() != 0) {
        // Their frames and leases are owned by handlers that this shutdown
        // destroys without running; nothing can retire them safely.
        report_unhandled_failure("attached event loop", std::make_exception_ptr(std::logic_error("io_context destroyed while its attached event loop still had root tasks, stop callbacks, or cleanup pending; keep running the context until the attachment retires")));
        std::terminate();
    }
    stopping_.store(true, std::memory_order_release);
    attached.stop_.abandon();
    runtime_.detach();
    worker_ref_access::wait_for_reservations(runtime_.worker());
}
void event_loop_state::install_shutdown_registration() {
    if (runtime_.worker().register_shutdown(shared_from_this(), &event_loop_state::shutdown_registration_) != worker_registration_status::registered) {
        throw std::runtime_error("cannot register an event loop on a stopping worker");
    }
}

void event_loop_state::worker_stopping_complete() noexcept {
    stop();
}

void event_loop_state::route_failure(std::exception_ptr failure, const char* unbound_context) noexcept {
    if (failure_sink_) {
        failure_sink_(std::move(failure));
        return;
    }
    report_unhandled_failure(unbound_context, std::move(failure));
}

void event_loop_state::report_attached_failure(std::exception_ptr failure) noexcept {
    stop();
    auto& receiver = std::get_if<attached_context>(&context_ownership_)->failure_;
    if (!receiver) {
        report_unhandled_failure("attached event loop failure", std::move(failure));
        return;
    }
    try {
        receiver(std::move(failure));
    } catch (...) {
        report_unhandled_failure("attached event loop failure callback", std::current_exception());
    }
}

void event_loop_state::fail_teardown_start(std::exception_ptr failure) noexcept {
    route_failure(std::move(failure), "event loop teardown startup");
    std::terminate();
}

void event_loop_state::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    if (!retirement_.begin_stop([weak] {
        if (auto state_value = weak.lock()) {
            auto* const state_ptr = state_value.get();
            state_ptr->defer_finalize_stop(std::move(state_value));
        }
    })) {
        return;
    }
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.request_stop();
    } else if (auto* borrowed = std::get_if<borrowed_runtime>(&context_ownership_)) {
        borrowed->runtime_->request_stop();
    } else {
        owned_runtime().request_stop();
    }
}

void event_loop_state::complete_stop_admission() noexcept {
    stop();
    schedule_finalize_when_idle();
}

void event_loop_state::schedule_finalize_when_idle() noexcept {
    if (!runtime_.worker().valid()) {
        return;
    }
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    try {
        worker_ref_access::when_idle(runtime_.worker(), [weak] {
            if (const auto idle_state = weak.lock()) {
                idle_state->retirement_.seal();
            }
        });
    } catch (...) {
        if (!runtime_.worker().valid()) {
            return;
        }
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept {
    const auto worker_value = runtime_.worker();
    try {
        // A detached context ran shutdown_external_context(), which
        // already abandoned the stop chain this would finalize.
        (void)worker_value.defer_completion([state_value = std::move(self)] {
            state_value->finalize_stop();
        });
    } catch (...) {
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::finalize_stop() noexcept {
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.finalize();
    } else if (auto* borrowed = std::get_if<borrowed_runtime>(&context_ownership_)) {
        borrowed->runtime_->finish_loop_stop_admission();
    } else {
        owned_runtime().finalize();
    }
}

void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept {
    report_through_loop(loop, std::move(failure), "unobserved event-loop root task");
}

}  // namespace detail

event_loop_stop_registration::event_loop_stop_registration(std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept :
    listener_(std::move(listener_value)) {}

event_loop_start_rejected::event_loop_start_rejected(post_status status) :
    std::runtime_error(status == post_status::queue_full ? "event loop root task queue is full" : "event loop is stopping"),
    status_(status) {}

event_loop_stop_registration::~event_loop_stop_registration() {
    reset();
}

event_loop_stop_registration& event_loop_stop_registration::operator=(event_loop_stop_registration&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Commit the incoming listener before retiring a callback that may own
    // the source registration or reenter this destination.
    event_loop_stop_registration retired(std::move(other));
    listener_.swap(retired.listener_);
    return *this;
}

bool event_loop_stop_registration::valid() const noexcept {
    return listener_ != nullptr;
}

void event_loop_stop_registration::reset() noexcept {
    if (auto listener = std::move(listener_)) {
        listener->cancel();
    }
}

event_loop::event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept :
    state_(std::move(state_value)) {}

bool event_loop::valid() const noexcept {
    return worker().valid();
}

asio::io_context& event_loop::io_context() const& {
    if (!state_ || !state_->runtime_.worker().valid()) {
        if (state_) {
            throw std::logic_error("event loop execution context is detached");
        }
        throw std::logic_error("cannot access a default-constructed event loop");
    }
    return *state_->io_context_;
}

asio::io_context::executor_type event_loop::executor() const {
    static_cast<void>(io_context());
    return state_->executor_;
}

const worker_ref& event_loop::worker() const& noexcept {
    if (state_) {
        return state_->runtime_.worker();
    }
    static const worker_ref empty_worker;
    return empty_worker;
}

worker_ref event_loop::worker() const&& noexcept {
    return worker();
}

void event_loop::report_failure(std::exception_ptr failure) const noexcept {
    if (!failure) {
        return;
    }
    if (const auto state_value = state_) {
        state_value->route_failure(std::move(failure), "event loop failure");
    } else {
        report_unhandled_failure("invalid event loop failure", std::move(failure));
    }
}

std::shared_ptr<void> event_loop::acquire_root_lease() const {
    if (!state_) {
        return {};
    }
    return state_->retirement_.try_acquire(state_);
}

bool event_loop::defer_cleanup_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("event loop cleanup must be callable");
    }
    if (!state_ || !state_->retirement_.try_begin_cleanup()) {
        return false;
    }
    struct cleanup_control final {
        // Destroy inputs and finish the retirement gate before releasing the
        // context owner. Do not rely on lambda capture member destruction order.
        std::shared_ptr<detail::event_loop_state> state_;
        move_only_function<void()> task_;

        cleanup_control(std::shared_ptr<detail::event_loop_state> state_value, move_only_function<void()> task_input) noexcept :
            state_(std::move(state_value)),
            task_(std::move(task_input)) {}
        cleanup_control(const cleanup_control&) = delete;
        cleanup_control& operator=(const cleanup_control&) = delete;
        cleanup_control(cleanup_control&& other) noexcept :
            state_(std::move(other.state_)),
            task_(std::move(other.task_)) {}
        cleanup_control& operator=(cleanup_control&&) = delete;
        ~cleanup_control() {
            if (state_) {
                task_ = nullptr;
                state_->retirement_.finish();
            }
        }
    };
    return state_->runtime_.worker().defer_completion([control = cleanup_control(state_, std::move(task_value))]() mutable {
        control.task_();
    });
}

event_loop_stop_registration event_loop::register_stop_callback(move_only_function<task<void>()> callback_value) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    if (!callback_value) {
        throw std::invalid_argument("event loop stop callback must be callable");
    }
    if (!state_->runtime_.worker().accepting()) {
        throw std::runtime_error("cannot register a stop callback on a stopping event loop");
    }
    auto listener_value = std::make_shared<detail::event_loop_stop_listener>(state_->runtime_.worker(), state_->executor_, std::move(callback_value), state_);
    listener_value->install_shutdown_registration();
    return event_loop_stop_registration(std::move(listener_value));
}

event_loop_attachment::event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept :
    state_(std::move(state_value)) {}

event_loop_attachment::~event_loop_attachment() {
    stop();
}

event_loop_attachment::event_loop_attachment(event_loop_attachment&& other) noexcept :
    state_(std::move(other.state_)) {}

bool event_loop_attachment::valid() const noexcept {
    return state_ != nullptr && state_->runtime_.worker().valid();
}

event_loop event_loop_attachment::loop() const noexcept {
    return event_loop(state_);
}

void event_loop_attachment::run() {
    auto state_value = state_;
    if (!state_value) {
        throw std::logic_error("cannot run an invalid event loop attachment");
    }
    if (!state_value->runtime_.worker().valid()) {
        throw std::logic_error("event loop execution context is detached");
    }
    state_value->runtime_.run();
}

void event_loop_attachment::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

event_loop_attachment attach_event_loop(asio::io_context& io_context, event_loop_attachment_options options) {
    auto state_value = std::make_shared<detail::event_loop_state>(io_context, options.queue_capacity_, options.blocking_pool_, std::move(options.failure_));
    const std::weak_ptr<detail::event_loop_state> weak_state = state_value;
    // root_task uses this sink directly; it must not call event_loop_state::route_failure,
    // which would route back through this same sink.
    state_value->failure_sink_ = [weak_state](std::exception_ptr failure) {
        if (const auto locked_state = weak_state.lock()) {
            locked_state->report_attached_failure(std::move(failure));
        } else {
            report_unhandled_failure("attached event loop failure after retirement", std::move(failure));
        }
    };
    state_value->install_shutdown_registration();
    state_value->attach(state_value);
    return event_loop_attachment(std::move(state_value));
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

std::atomic<worker_id_type> g_next_worker_id{1};
thread_local const worker_dispatcher* g_current_worker = nullptr;

class current_worker_guard final {
public:
    explicit current_worker_guard(const worker_dispatcher& worker_value) :
        previous_(std::exchange(g_current_worker, &worker_value)) {
        if (previous_ != nullptr && previous_ != &worker_value) {
            g_current_worker = previous_;
            throw std::logic_error("one thread cannot run multiple Ruvia workers concurrently");
        }
    }

    ~current_worker_guard() {
        g_current_worker = previous_;
    }

    current_worker_guard(const current_worker_guard&) = delete;
    current_worker_guard& operator=(const current_worker_guard&) = delete;

private:
    const worker_dispatcher* previous_;
};

}  // namespace

const worker_dispatcher* current_worker_dispatcher() noexcept {
    return g_current_worker;
}

worker_id_type next_worker_dispatcher_id() noexcept {
    return g_next_worker_id.fetch_add(1, std::memory_order_relaxed);
}

worker_dispatcher::worker_dispatcher(asio::io_context& io_context, std::size_t capacity, ruvia::blocking_pool* pool) :
    io_context_(io_context),
    timer_(std::make_unique<asio::steady_timer>(io_context)),
    capacity_(checked_queue_capacity(capacity)),
    blocking_pool_(pool),
    nodes_(),
    worker_id_(next_worker_dispatcher_id()),
    timers_(),
    timer_slots_() {
    // At most `capacity_` nodes are pending (reserved or ready) and at most
    // `capacity_` belong to the batch drain() is running, since a batch is
    // the ready list it spliced. Twice the capacity therefore keeps the
    // free list non-empty whenever admission has room.
    nodes_.resize(capacity_ * 2);
    for (std::size_t index = 0; index + 1 < nodes_.size(); ++index) {
        nodes_[index].next_ = index + 1;
    }
    free_head_ = 0;
    timers_.reserve(capacity);
    // Timer slots are address-stable (deque); they grow on demand.
}

worker_dispatcher::~worker_dispatcher() {
    notify_waiters(idle_waiters_.take_all(), false);
    notify_waiters(shutdown_notification_waiters_.take_all(), false);
}

}  // namespace ruvia::detail

namespace ruvia {

worker_shutdown_registration::~worker_shutdown_registration() {
    if (registry_) {
        registry_->unlink_shutdown(*this);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

void worker_dispatcher::schedule_drain_locked() {
    if (!drain_scheduled_) {
        asio::post(io_context_, [self = shared_from_this()] {
            self->drain();
        });
        drain_scheduled_ = true;
    }
}

void worker_dispatcher::push_ready_locked(std::size_t index) noexcept {
    nodes_[index].next_ = no_node;
    if (ready_tail_ == no_node) {
        ready_head_ = index;
    } else {
        nodes_[ready_tail_].next_ = index;
    }
    ready_tail_ = index;
    ++ready_count_;
}

post_status worker_dispatcher::reserve(std::size_t& index) noexcept {
    std::lock_guard lock(mutex_);
    if (!context_attached_ || !accepting_) {
        return post_status::worker_stopping;
    }
    if (pending_count_ == capacity_) {
        return post_status::queue_full;
    }
    index = free_head_;
    free_head_ = nodes_[index].next_;
    ++pending_count_;
    return post_status::accepted;
}

// Moving a task into its node runs the callable's own move constructor, which
// may reenter this worker (post, close, detach). Admission and publication are
// therefore two critical sections with the move between them, never under the
// mutex; the reserved node counts against capacity throughout.
post_result_type worker_dispatcher::post(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    std::size_t index = no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return detail::worker_delivery_access::reject<post_result_type>(status, std::move(task_value));
    }
    nodes_[index].task_ = std::move(task_value);
    publish(index);
    return detail::worker_delivery_access::accept<post_result_type>();
}

post_status worker_dispatcher::post_factory(move_only_function<move_only_function<void()>()> factory) {
    if (!factory) {
        throw std::invalid_argument("worker post factory requires a callable");
    }
    std::size_t index = no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return status;
    }
    try {
        auto task_value = factory();
        if (!task_value) {
            throw std::invalid_argument("worker post factory produced an empty task");
        }
        nodes_[index].task_ = std::move(task_value);
    } catch (...) {
        release_abandoned_node(index);
        throw;
    }
    publish(index);
    return post_status::accepted;
}

void worker_dispatcher::publish(std::size_t index) {
    std::unique_lock lock(mutex_);
    if (!abandon_drain_ && context_attached_) {
        try {
            schedule_drain_locked();
        } catch (...) {
            lock.unlock();
            release_abandoned_node(index);
            throw;
        }
        push_ready_locked(index);
        return;
    }
    lock.unlock();
    release_abandoned_node(index);
}

void worker_dispatcher::release_abandoned_node(std::size_t index) noexcept {
    // The caller exclusively claimed this pending node. Move its payload out
    // before returning the slot; user destruction can reenter admission or
    // reconcile higher-level work, and must run unlocked before idle callbacks.
    auto abandoned = std::move(nodes_[index].task_);
    waiter_node_type* callbacks = nullptr;
    bool all_pending_released = false;
    {
        std::lock_guard lock(mutex_);
        nodes_[index].next_ = free_head_;
        free_head_ = index;
        all_pending_released = (--pending_count_ == 0);
        callbacks = take_idle_callbacks_locked();
    }
    abandoned = nullptr;
    notify_waiters(callbacks);
    if (all_pending_released) {
        pending_changed_.notify_all();
    }
}

void worker_dispatcher::release_abandoned_chain(std::size_t index) noexcept {
    // The spliced chain is exclusively owned here; each node stays pending
    // until release_abandoned_node returns it.
    while (index != no_node) {
        const auto next = nodes_[index].next_;
        release_abandoned_node(index);
        index = next;
    }
}

bool worker_dispatcher::defer_if_attached(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    // Neither path keeps the dispatcher alive for the handler: the handler
    // only runs the task, and the io_context it is queued on outlives it.
    if (running_on_worker()) {
        asio::post(io_context_, std::move(task_value));
        return true;
    }
    // Off the worker, the mutex orders this post before detach_context(),
    // after which the context may be destroyed.
    std::lock_guard lock(mutex_);
    if (!context_attached_) {
        return false;
    }
    asio::post(io_context_, std::move(task_value));
    return true;
}

worker_registration_status worker_dispatcher::register_shutdown(worker_shutdown_registration& hook, std::weak_ptr<void> owner, const worker_shutdown_operations_type& operations) {
    // begin_stopping() clears accepting under the admission mutex before
    // notify_stopping() first takes this mutex on the same thread, so a
    // registration that still observes accepting here is linked before the
    // stopping batch walks the list.
    std::lock_guard lock(hooks_mutex_);
    if (hook.registry_) {
        throw std::logic_error("worker shutdown registration is already linked");
    }
    if (!accepting_) {
        return worker_registration_status::worker_stopping;
    }
    hook.operations_ = &operations;
    hook.registry_ = shared_from_this();
    hook.owner_ = std::move(owner);
    hook.previous_ = hooks_tail_;
    hook.next_ = nullptr;
    hook.linked_ = true;
    if (hooks_tail_ != nullptr) {
        hooks_tail_->next_ = &hook;
    } else {
        hooks_head_ = &hook;
    }
    hooks_tail_ = &hook;
    return worker_registration_status::registered;
}

void worker_dispatcher::unlink_shutdown(worker_shutdown_registration& hook) noexcept {
    std::lock_guard lock(hooks_mutex_);
    if (!hook.linked_) {
        return;
    }
    if (hook.previous_ != nullptr) {
        hook.previous_->next_ = hook.next_;
    } else {
        hooks_head_ = hook.next_;
    }
    if (hook.next_ != nullptr) {
        hook.next_->previous_ = hook.previous_;
    } else {
        hooks_tail_ = hook.previous_;
    }
    hook.previous_ = nullptr;
    hook.next_ = nullptr;
    hook.linked_ = false;
}

void worker_dispatcher::when_shutdown_notifications_complete(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("shutdown notification callback requires a callable");
    }
    auto node = std::make_unique<waiter_node_type>(std::move(callback_value));
    {
        std::lock_guard lock(mutex_);
        if (shutdown_notification_active_) {
            shutdown_notification_waiters_.push_back(node.release());
            return;
        }
    }
    callback_value = std::move(node->callback_);
    node.reset();
    callback_value();
}

void worker_dispatcher::when_idle(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("worker idle callback requires a callable");
    }
    auto node = std::make_unique<waiter_node_type>(std::move(callback_value));
    bool detached = false;
    {
        std::lock_guard lock(mutex_);
        if (!context_attached_) {
            detached = true;
        } else if (pending_count_ != 0 || active_count_ != 0) {
            idle_waiters_.push_back(node.release());
            return;
        }
    }
    callback_value = std::move(node->callback_);
    node.reset();
    if (detached) {
        throw std::runtime_error("worker execution context is detached");
    }
    callback_value();
}

worker_dispatcher::waiter_node_type* worker_dispatcher::take_idle_callbacks_locked() noexcept {
    if (pending_count_ == 0 && active_count_ == 0) {
        return idle_waiters_.take_all();
    }
    return nullptr;
}

void worker_dispatcher::notify_waiters(waiter_node_type* head, bool invoke) noexcept {
    while (head != nullptr) {
        std::unique_ptr<waiter_node_type> current(head);
        head = current->next_;
        if (!invoke) {
            continue;
        }
        try {
            current->callback_();
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::run_context(move_only_function<void()> startup_handler, move_only_function<void(std::exception_ptr)> failure_handler, move_only_function<void()> shutdown_handler) {
    current_worker_guard current(*this);
    bool failure_delivered = false;
    std::exception_ptr deferred_failure;
    std::exception_ptr delivered_failure;
    const auto deliver_failure = [this, &failure_delivered, &deferred_failure, &delivered_failure, &failure_handler](std::exception_ptr failure) {
        notify_stopping(begin_stopping(true));
        abandon_queued();
        if (!failure_delivered) {
            failure_delivered = true;
            delivered_failure = failure;
            try {
                if (failure_handler) {
                    failure_handler(failure);
                } else {
                    deferred_failure = std::move(failure);
                }
            } catch (...) {
                deferred_failure = std::current_exception();
            }
        } else if (!equivalent_failures(failure, delivered_failure)) {
            // Only the first failure is delivered; later ones would vanish.
            report_unhandled_failure("additional worker failure", failure);
        }
        stop_timers();
    };

    try {
        if (startup_handler) {
            startup_handler();
        }
    } catch (...) {
        deliver_failure(std::current_exception());
    }
    for (;;) {
        try {
            io_context_.run();
            break;
        } catch (...) {
            deliver_failure(std::current_exception());
        }
    }
    if (shutdown_handler) {
        try {
            shutdown_handler();
        } catch (...) {
            std::terminate();
        }
    }
    if (deferred_failure != nullptr) {
        std::rethrow_exception(deferred_failure);
    }
}

void worker_dispatcher::close() noexcept {
    notify_stopping(begin_stopping(false));
}

void worker_dispatcher::detach_context() noexcept {
    std::vector<timer_entry> abandoned_timers;
    std::unique_ptr<asio::steady_timer> detached_timer;
    {
        std::lock_guard lock(mutex_);
        if (!context_attached_) {
            return;
        }
        accepting_ = false;
        context_attached_ = false;
        abandon_drain_ = true;
        detached_timer = std::move(timer_);
    }
    {
        // Detached hooks are never notified; their destructors find them
        // unlinked.
        std::lock_guard lock(hooks_mutex_);
        auto* hook = std::exchange(hooks_head_, nullptr);
        hooks_tail_ = nullptr;
        while (hook != nullptr) {
            auto* next = hook->next_;
            hook->previous_ = nullptr;
            hook->next_ = nullptr;
            hook->linked_ = false;
            hook = next;
        }
    }

    // The caller guarantees no worker-thread timer activity runs concurrently
    // with detach_context: event_loop_pool joins its threads first, an attached
    // loop invokes this from its terminal context handler, and the external
    // context service invokes it while the context is shutting down. The timer
    // heap is therefore exclusively owned here. All user-owned closures are
    // destroyed outside the mutex so a destructor that releases another worker
    // primitive cannot deadlock. Slots stay in place: bound registrations may
    // still read their activity until the dispatcher itself is destroyed.
    timers_stopping_.store(true, std::memory_order_release);
    abandoned_timers.swap(timers_);
    stale_timer_count_ = 0;
    timer_armed_ = false;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (timer_slots_[index].occupied_) {
            static_cast<void>(release_timer_slot(index));
        }
    }
    detached_timer.reset();
    abandon_queued();
}

void worker_dispatcher::wait_for_reservations() noexcept {
    std::unique_lock lock(mutex_);
    pending_changed_.wait(lock, [this] {
        return pending_count_ == 0;
    });
}

bool worker_dispatcher::attached() const noexcept {
    return context_attached_.load(std::memory_order_acquire);
}

asio::io_context::executor_type worker_dispatcher::executor() const {
    const std::lock_guard lock(mutex_);
    if (!context_attached_) {
        throw std::logic_error("worker execution context is detached");
    }
    return io_context_.get_executor();
}

bool worker_dispatcher::begin_stopping(bool abandon_drain) noexcept {
    std::lock_guard lock(mutex_);
    return begin_stopping_locked(abandon_drain);
}

bool worker_dispatcher::begin_stopping_locked(bool abandon_drain) noexcept {
    if (abandon_drain) {
        drain_scheduled_ = false;
        abandon_drain_ = true;
    }
    if (!accepting_) {
        return false;
    }
    accepting_ = false;
    shutdown_notification_active_ = true;
    return true;
}

void worker_dispatcher::notify_stopping(bool batch_active) noexcept {
    if (!batch_active) {
        return;
    }
    // No hook can join once accepting is false, so the batch is the registry
    // itself. Each step pins the owner of the hook it notifies, which keeps
    // that node linked while the mutex is released for the callback; an owner
    // already being destroyed is skipped. References are dropped outside the
    // mutex because the last one runs the hook's unlink.
    struct pinned_hook final {
        worker_shutdown_registration* hook_{nullptr};
        std::shared_ptr<void> owner_;
    };
    const auto first_live = [](worker_shutdown_registration* hook) noexcept {
        pinned_hook pinned;
        while (hook != nullptr && !(pinned.owner_ = hook->owner_.lock())) {
            hook = hook->next_;
        }
        pinned.hook_ = hook;
        return pinned;
    };
    using operations_type = worker_shutdown_operations_type;
    const auto notify_each = [this, &first_live](operations_type::phase_type operations_type::* phase) noexcept {
        pinned_hook current;
        {
            std::lock_guard lock(hooks_mutex_);
            current = first_live(hooks_head_);
        }
        while (current.owner_) {
            if (const auto callback = current.hook_->operations_->*phase) {
                callback(current.owner_.get());
            }
            pinned_hook next;
            {
                std::lock_guard lock(hooks_mutex_);
                next = first_live(current.hook_->next_);
            }
            current = std::move(next);
        }
    };
    notify_each(&operations_type::stopping_);
    notify_each(&operations_type::stopping_complete_);

    waiter_node_type* waiters = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!shutdown_notification_active_) {
            std::terminate();
        }
        shutdown_notification_active_ = false;
        waiters = shutdown_notification_waiters_.take_all();
    }
    notify_waiters(waiters);
}

void worker_dispatcher::abandon_queued() noexcept {
    std::size_t index = no_node;
    {
        std::lock_guard lock(mutex_);
        index = std::exchange(ready_head_, no_node);
        ready_tail_ = no_node;
        ready_count_ = 0;
    }
    release_abandoned_chain(index);
}

bool worker_dispatcher::is_current() const noexcept {
    if (g_current_worker != nullptr || !external_runner_.load(std::memory_order_acquire)) {
        return g_current_worker == this;
    }
    std::lock_guard lock(mutex_);
    return context_attached_ && io_context_.get_executor().running_in_this_thread();
}

bool worker_dispatcher::accepting() const noexcept {
    return accepting_.load(std::memory_order_acquire);
}

worker_id_type worker_dispatcher::id() const noexcept {
    return context_attached_.load(std::memory_order_acquire) ? worker_id_ : 0;
}

void worker_dispatcher::drain() {
    // Each handler runs one spliced batch and then returns to the io_context.
    // Work published meanwhile is left to a freshly posted handler, so I/O
    // completions, timers, and deferred continuations interleave with a busy
    // queue instead of starving behind it.
    std::size_t batch_head = no_node;
    std::size_t batch_count = 0;
    bool abandon = false;
    waiter_node_type* callbacks = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (abandon_drain_) {
            drain_scheduled_ = false;
            abandon = true;
        } else if (ready_count_ == 0) {
            drain_scheduled_ = false;
        } else {
            batch_head = std::exchange(ready_head_, no_node);
            batch_count = std::exchange(ready_count_, 0);
            ready_tail_ = no_node;
            pending_count_ -= batch_count;
            active_count_ += batch_count;
        }
        if (abandon || batch_count == 0) {
            callbacks = take_idle_callbacks_locked();
        }
    }
    if (abandon) {
        notify_waiters(callbacks);
        abandon_queued();
        return;
    }
    if (batch_count == 0) {
        notify_waiters(callbacks);
        return;
    }

    std::size_t index = batch_head;
    std::size_t batch_tail = no_node;
    std::size_t completed = 0;
    while (index != no_node) {
        auto& node = nodes_[index];
        const auto next = node.next_;
        try {
            node.task_();
        } catch (...) {
            node.task_ = nullptr;
            bool batch_active = false;
            {
                // The completed prefix through the failed node returns to
                // the free list; the unstarted remainder is pending again
                // and is released below like queued work. The remainder can
                // push pending past capacity, so admission closes in this
                // same critical section before any reservation observes it.
                std::lock_guard lock(mutex_);
                node.next_ = free_head_;
                free_head_ = batch_head;
                active_count_ -= batch_count;
                pending_count_ += batch_count - completed - 1;
                batch_active = begin_stopping_locked(true);
            }
            notify_stopping(batch_active);
            release_abandoned_chain(next);
            abandon_queued();
            waiter_node_type* failure_callbacks = nullptr;
            {
                std::lock_guard lock(mutex_);
                failure_callbacks = take_idle_callbacks_locked();
            }
            notify_waiters(failure_callbacks);
            throw;
        }
        node.task_ = nullptr;
        ++completed;
        batch_tail = index;
        index = next;
    }

    {
        // The finished batch returns to the free list. A post failure leaves
        // drain unscheduled and escapes as a worker failure, whose handling
        // abandons the ready list.
        std::lock_guard lock(mutex_);
        nodes_[batch_tail].next_ = free_head_;
        free_head_ = batch_head;
        active_count_ -= batch_count;
        drain_scheduled_ = false;
        abandon = abandon_drain_;
        if (!abandon && ready_count_ != 0) {
            schedule_drain_locked();
        }
        callbacks = take_idle_callbacks_locked();
    }
    notify_waiters(callbacks);
    if (abandon) {
        abandon_queued();
    }
}

}  // namespace ruvia::detail

namespace ruvia {

worker_ref::worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept :
    dispatcher_(std::move(dispatcher)) {}

bool worker_ref::valid() const noexcept {
    return dispatcher_ && dispatcher_->attached();
}

bool worker_ref::accepting() const noexcept {
    return dispatcher_ && dispatcher_->accepting();
}

bool worker_ref::is_current() const noexcept {
    return dispatcher_ && dispatcher_->is_current();
}

worker_id_type worker_ref::id() const noexcept {
    return dispatcher_ ? dispatcher_->id() : 0;
}

blocking_pool* worker_ref::blocking_pool() const noexcept {
    return dispatcher_ ? dispatcher_->blocking_pool() : nullptr;
}

asio::io_context::executor_type worker_ref::executor() const {
    if (!dispatcher_) {
        throw std::logic_error("worker has no execution context");
    }
    return dispatcher_->executor();
}

worker_ref detail::worker_ref_access::current() noexcept {
    const auto* dispatcher = detail::current_worker_dispatcher();
    if (dispatcher == nullptr) {
        return {};
    }
    return worker_ref(std::const_pointer_cast<detail::worker_dispatcher>(dispatcher->shared_from_this()));
}

post_result_type worker_ref::post_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ ? dispatcher_->post(std::move(task_value)) : detail::worker_delivery_access::reject<post_result_type>(post_status::worker_stopping, std::move(task_value));
}

void worker_ref::defer_resume(std::coroutine_handle<> continuation) const noexcept {
    if (!dispatcher_) {
        std::terminate();
    }
    dispatcher_->defer_or_terminate([continuation] {
        continuation.resume();
    });
}

worker_ref detail::worker_ref_access::make(const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept {
    return worker_ref(dispatcher);
}

bool worker_ref::defer_completion(move_only_function<void()> completion) const {
    if (!completion) {
        throw std::invalid_argument("worker completion requires a callable task");
    }
    // The completion's move may release the last owner of this worker_ref.
    const auto dispatcher = dispatcher_;
    return dispatcher && dispatcher->defer_if_attached(std::move(completion));
}

void detail::worker_ref_access::defer_or_terminate(const worker_ref& worker_value, move_only_function<void()> task_value) noexcept {
    if (!worker_value.dispatcher_) {
        std::terminate();
    }
    worker_value.dispatcher_->defer_or_terminate(std::move(task_value));
}

worker_registration_status worker_ref::link_shutdown(worker_shutdown_registration& registration, std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const {
    if (!dispatcher_) {
        return worker_registration_status::worker_stopping;
    }
    return dispatcher_->register_shutdown(registration, std::move(owner), operations);
}

void detail::worker_ref_access::when_shutdown_notifications_complete(const worker_ref& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe shutdown notifications on a stopped worker");
    }
    dispatcher->when_shutdown_notifications_complete(std::move(callback_value));
}

void detail::worker_ref_access::when_idle(const worker_ref& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe idleness on a stopped worker");
    }
    dispatcher->when_idle(std::move(callback_value));
}

void detail::worker_ref_access::wait_for_reservations(const worker_ref& worker_value) noexcept {
    if (worker_value.dispatcher_) {
        worker_value.dispatcher_->wait_for_reservations();
    }
}

bool detail::worker_ref_access::timers_stopping(const worker_ref& worker_value) noexcept {
    return !worker_value.dispatcher_ || worker_value.dispatcher_->timers_stopping();
}

void detail::worker_ref_access::enable_external_runner(const worker_ref& worker_value) noexcept {
    if (worker_value.dispatcher_) {
        worker_value.dispatcher_->enable_external_runner();
    }
}

worker_timer_schedule_status worker_ref::schedule_timer(worker_timer_registration& registration, std::chrono::steady_clock::time_point deadline_value, move_only_function<void(worker_timer_outcome)> completion) const& {
    if (!dispatcher_) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    return dispatcher_->schedule_timer(registration, deadline_value, std::move(completion));
}

post_status worker_ref::post_factory(move_only_function<move_only_function<void()>()> factory) const {
    const auto dispatcher = dispatcher_;
    return dispatcher ? dispatcher->post_factory(std::move(factory)) : post_status::worker_stopping;
}

}  // namespace ruvia

// The dispatcher's timer heap: registering a deadline, cancelling one from any
// thread, re-arming the single asio timer, and firing everything due on the
// worker.

namespace ruvia {

void worker_timer_cancellation::cancel() const noexcept {
    if (dispatcher_ != nullptr && generation_ != 0) {
        dispatcher_->request_timer_cancellation(slot_, generation_, true);
    }
}

worker_timer_registration::~worker_timer_registration() {
    unregister(false);
}

void worker_timer_registration::cancel() noexcept {
    unregister(false);
}

void worker_timer_registration::cancel_and_notify() noexcept {
    unregister(true);
}

void worker_timer_registration::unregister(bool notify) noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto* slot_state = std::exchange(slot_state_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr || slot_state == nullptr || slot_state->active_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    if (!notify && detail::current_worker_dispatcher() != dispatcher) {
        auto expected = generation;
        if (!const_cast<detail::timer_slot*>(slot_state)->active_generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
    dispatcher->request_timer_cancellation(slot, generation, notify);
}

bool worker_timer_registration::registered() const noexcept {
    return dispatcher_ != nullptr;
}

worker_timer_cancellation worker_timer_registration::cancellation() const& {
    if (dispatcher_ == nullptr || generation_ == 0) {
        throw std::logic_error("worker timer registration is not active");
    }
    return worker_timer_cancellation(*dispatcher_, slot_, generation_);
}

void worker_timer_registration::bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation, const detail::timer_slot& slot_state) noexcept {
    dispatcher_ = &dispatcher;
    slot_state_ = &slot_state;
    slot_ = slot;
    generation_ = generation;
}

void worker_timer_registration::release() noexcept {
    dispatcher_ = nullptr;
    slot_state_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

bool worker_timer_registration::pending() const noexcept {
    // Pairs with the release store that deactivates the slot on its own worker
    // before expiry or cancellation delivers a completion.
    return slot_state_ != nullptr && generation_ != 0 && slot_state_->active_generation_.load(std::memory_order_acquire) == generation_;
}

// Only a cold operation may move; a started one is referenced by its timer
// callback and must stay address-stable.
sleep_operation::sleep_operation(sleep_operation&& other) noexcept :
    worker_(std::exchange(other.worker_, nullptr)),
    target_(other.target_),
    stop_token_(std::move(other.stop_token_)) {
    if (other.started_) {
        std::terminate();
    }
}

bool sleep_operation::await_ready() {
    if (worker_ == nullptr || started_) {
        throw std::logic_error("sleep operation cannot be awaited");
    }
    if (!worker_->is_current()) {
        throw std::logic_error("sleep must run on its bound worker");
    }
    started_ = true;
    if (stop_token_.stop_requested()) {
        return true;
    }
    if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
        return duration->value() <= std::chrono::milliseconds::zero();
    }
    return std::get<std::chrono::steady_clock::time_point>(target_) <= std::chrono::steady_clock::now();
}

// A stopped timer queue resolves immediately with the outcome stop_timers()
// would have delivered to a pending sleep.
bool sleep_operation::await_suspend(std::coroutine_handle<> continuation) {
    continuation_ = continuation;
    const auto deadline = [&] {
        if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
            return deadline_after(duration->value());
        }
        return std::get<std::chrono::steady_clock::time_point>(target_);
    }();
    const auto status = worker_->schedule_timer(registration_, deadline, [this](worker_timer_outcome value) {
        outcome_ = value;
        continuation_.resume();
    });
    if (status == worker_timer_schedule_status::worker_stopping) {
        outcome_ = worker_timer_outcome::cancelled;
        return false;
    }
    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_, [cancellation = registration_.cancellation()] {
            cancellation.cancel();
        });
    }
    return true;
}

// A zero/negative duration never suspends and keeps the default expired
// outcome, so it reports elapsed unless stop was already requested.
timer_sleep_result sleep_operation::await_resume() const noexcept {
    return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested() ? timer_sleep_result::elapsed : timer_sleep_result::stop_requested;
}

sleep_operation sleep_for(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value) {
    return sleep_operation(worker_value, duration, std::move(stop_token_value));
}

sleep_operation sleep_until(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) {
    return sleep_operation(worker_value, deadline, std::move(stop_token_value));
}
}  // namespace ruvia

namespace ruvia::detail {

worker_timer_schedule_status worker_dispatcher::schedule_timer(worker_timer_registration& registration, std::chrono::steady_clock::time_point deadline_value, move_only_function<void(worker_timer_outcome)> completion) {
    if (!is_current()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (registration.scheduling_) {
        throw std::logic_error("worker timer registration is already active");
    }
    registration.scheduling_ = true;
    struct scheduling_guard final {
        bool& scheduling_;
        ~scheduling_guard() {
            scheduling_ = false;
        }
    } guard{registration.scheduling_};
    if (registration.dispatcher_ != nullptr) {
        // The previous binding may belong to another worker; its slot state is
        // the only part of that worker read here.
        if (registration.pending()) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }
    // Detachment also stops the timer queue, so one check covers both.
    if (timers_stopping_.load(std::memory_order_acquire)) {
        return worker_timer_schedule_status::worker_stopping;
    }
    // New due entries belong to the next dispatch round. Place them after all
    // entries due at this round's snapshot, even if their requested deadline
    // is earlier, so the sequence gate cannot hide other pending expirations.
    if (dispatching_timers_) {
        deadline_value = std::max(deadline_value, timer_round_deadline_);
    }

    std::size_t slot_index = free_timer_slot_;
    if (slot_index == no_timer_slot) {
        slot_index = timer_slots_.size();
        timer_slots_.emplace_back();
    } else {
        free_timer_slot_ = timer_slots_[slot_index].next_free_;
    }
    auto& slot = timer_slots_[slot_index];
    if (++slot.generation_ == 0) {
        ++slot.generation_;
    }
    slot.next_free_ = no_timer_slot;
    slot.cancel_requested_ = false;
    // The target's move may stop timers reentrantly. Keep this reserved slot
    // unpublished until the move completes so stop_timers() cannot retire it
    // while its callback is still being constructed.
    slot.completion_ = std::move(completion);
    if (timers_stopping_.load(std::memory_order_acquire)) {
        static_cast<void>(release_timer_slot(slot_index));
        return worker_timer_schedule_status::worker_stopping;
    }
    slot.occupied_ = true;
    slot.active_generation_.store(slot.generation_, std::memory_order_release);
    try {
        timers_.push_back(timer_entry{
            .deadline_ = deadline_value,
            .sequence_ = next_timer_sequence_++,
            .slot_ = slot_index,
            .generation_ = slot.generation_,
        });
    } catch (...) {
        // Roll the slot back; the moved-in completion is destroyed uninvoked.
        static_cast<void>(release_timer_slot(slot_index));
        throw;
    }
    std::ranges::push_heap(timers_, timer_entry_later{});
    const auto generation = slot.generation_;
    registration.bind(*this, slot_index, generation, slot);
    // The armed wait already covers every deadline at or after it.
    if (!dispatching_timers_ && (!timer_armed_ || deadline_value < armed_deadline_)) {
        try {
            arm_timer();
        } catch (...) {
            // Arming cancelled the previous wait before failing. Withdraw the
            // new entry so the caller sees a clean failure, then re-arm for the
            // entries that were pending before this call.
            registration.release();
            static_cast<void>(take_timer(slot_index, generation));
            try {
                arm_timer();
            } catch (...) {
                fail_timers("worker timer arm", std::current_exception());
            }
            throw;
        }
    }
    return worker_timer_schedule_status::scheduled;
}

void worker_dispatcher::request_timer_cancellation(std::size_t slot, std::uint64_t generation, bool notify) noexcept {
    if (current_worker_dispatcher() != this) {
        std::lock_guard lock(mutex_);
        if (timers_stopping_.load(std::memory_order_acquire) || !context_attached_) {
            return;
        }
        if (!io_context_.get_executor().running_in_this_thread()) {
            try {
                // A stopped io_context can be restarted; queue the cancellation
                // so a destroyed registration cannot leave a live slot that
                // expires after that restart. The handler already runs fresh on
                // the worker, exactly like fire_timers(), so it delivers the
                // completion inline instead of posting a second hop.
                asio::post(io_context_, [self = shared_from_this(), slot, generation, notify] {
                    auto completion = self->take_timer(slot, generation);
                    if (notify && completion) {
                        completion(worker_timer_outcome::cancelled);
                    }
                });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    if (!notify) {
        static_cast<void>(take_timer(slot, generation));
        return;
    }
    if (!has_occupied_timer(slot, generation)) {
        return;
    }
    auto& slot_state = timer_slots_[slot];
    if (slot_state.cancel_requested_) {
        return;
    }
    slot_state.cancel_requested_ = true;
    note_stale_timer();
    // The caller is running synchronously on the worker; delivering its own
    // completion here would re-enter it, so the completion is posted. Deferring
    // slot extraction lets a subsequent registration.cancel() or destruction on
    // this worker withdraw the callback before the posted turn runs.
    defer_or_terminate([self = shared_from_this(), slot, generation] {
        auto completion = self->take_timer(slot, generation);
        if (completion) {
            completion(worker_timer_outcome::cancelled);
        }
    });
}

void worker_dispatcher::pop_stale_timer_top() noexcept {
    while (!timers_.empty() && !has_live_timer_entry(timers_.front().slot_, timers_.front().generation_)) {
        std::ranges::pop_heap(timers_, timer_entry_later{});
        timers_.pop_back();
        if (stale_timer_count_ != 0) {
            --stale_timer_count_;
        }
    }
}

void worker_dispatcher::note_stale_timer() noexcept {
    ++stale_timer_count_;
    if (stale_timer_count_ == timers_.size()) {
        // Nothing live remains: drop the heap and the armed wait so an idle
        // worker holds no timer work.
        timers_.clear();
        release_drained_timer_heap();
        stale_timer_count_ = 0;
        if (timer_armed_ && timer_) {
            ++timer_generation_;
            timer_armed_ = false;
            timer_->cancel();
        }
    } else {
        pop_stale_timer_top();
        if (stale_timer_count_ >= 64 && stale_timer_count_ * 2 >= timers_.size()) {
            std::erase_if(timers_, [this](const timer_entry& entry_value) {
                return !has_live_timer_entry(entry_value.slot_, entry_value.generation_);
            });
            std::ranges::make_heap(timers_, timer_entry_later{});
            stale_timer_count_ = 0;
        }
    }
}

void worker_dispatcher::release_drained_timer_heap() noexcept {
    // Slots stay: registrations keep their addresses and (slot, generation)
    // pairs across threads, so only the heap, which nothing references, shrinks.
    // The heap regrows geometrically on the next burst.
    if (timers_.empty() && timers_.capacity() > capacity_) {
        std::vector<timer_entry>().swap(timers_);
    }
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::take_timer(std::size_t slot_index, std::uint64_t generation) noexcept {
    if (!has_occupied_timer(slot_index, generation)) {
        return {};
    }
    const bool active = has_timer(slot_index, generation);
    const bool already_stale = timer_slots_[slot_index].cancel_requested_;
    auto completion = release_timer_slot(slot_index);
    if (!already_stale) {
        note_stale_timer();
    }
    return active ? std::move(completion) : move_only_function<void(worker_timer_outcome)>{};
}

void worker_dispatcher::stop_timers() noexcept {
    if (timers_stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ++timer_generation_;
    timer_armed_ = false;
    if (timer_) {
        timer_->cancel();
        // The timer object is bound to the worker io_context service.  A
        // worker_ref may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        timer_.reset();
    }

    timers_.clear();
    release_drained_timer_heap();
    stale_timer_count_ = 0;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (!timer_slots_[index].occupied_) {
            continue;
        }
        const bool active = timer_slots_[index].active_generation_.load(std::memory_order_acquire) != 0;
        auto completion = release_timer_slot(index);
        try {
            if (active && completion) {
                completion(worker_timer_outcome::cancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::arm_timer() {
    ++timer_generation_;
    const auto generation = timer_generation_;
    if (!timer_) {
        return;
    }
    if (timer_armed_) {
        timer_->cancel();
        timer_armed_ = false;
    }
    pop_stale_timer_top();
    if (timers_stopping_.load(std::memory_order_acquire) || timers_.empty()) {
        release_drained_timer_heap();
        return;
    }
    armed_deadline_ = timers_.front().deadline_;
    timer_->expires_at(armed_deadline_);
    timer_->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->timer_generation_) {
            return;
        }
        self->timer_armed_ = false;
        self->fire_timers();
    });
    // Initiation may allocate and throw; only a successful wait is armed.
    timer_armed_ = true;
}

void worker_dispatcher::fire_timers() {
    std::exception_ptr failure;
    dispatching_timers_ = true;
    // One pass fires only what was due when it began. A completion that
    // schedules a deadline already in the past (a periodic timer whose work
    // outlasts its interval) waits for the re-armed wait, so I/O, deferred work,
    // and stop controls run between rounds instead of starving behind them.
    const auto now = std::chrono::steady_clock::now();
    timer_round_deadline_ = now;
    const auto sequence_limit = next_timer_sequence_;
    try {
        for (;;) {
            pop_stale_timer_top();
            if (timers_.empty() || timers_.front().deadline_ > now || timers_.front().sequence_ >= sequence_limit) {
                break;
            }
            std::ranges::pop_heap(timers_, timer_entry_later{});
            auto entry_value = std::move(timers_.back());
            timers_.pop_back();
            const bool active = has_timer(entry_value.slot_, entry_value.generation_);
            auto completion = release_timer_slot(entry_value.slot_);
            if (active && completion) {
                completion(worker_timer_outcome::expired);
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // Restore the queue before propagating a user callback failure. An external
    // io_context owner may catch it and continue running the remaining timers.
    dispatching_timers_ = false;
    try {
        arm_timer();
    } catch (...) {
        fail_timers("worker timer re-arm", std::current_exception());
        if (!failure) {
            throw;
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Without an armed wait nothing pending could ever fire. Stopping the queue
// delivers every pending completion as cancelled instead of stalling silently.
void worker_dispatcher::fail_timers(std::string_view context_value, std::exception_ptr failure) noexcept {
    report_unhandled_failure(context_value, std::move(failure));
    stop_timers();
}

bool worker_dispatcher::has_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].active_generation_.load(std::memory_order_acquire) == generation;
}

bool worker_dispatcher::has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].occupied_ && timer_slots_[slot].generation_ == generation;
}

bool worker_dispatcher::has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept {
    return has_occupied_timer(slot, generation) && !timer_slots_[slot].cancel_requested_;
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::release_timer_slot(std::size_t slot_index) noexcept {
    auto& slot = timer_slots_[slot_index];
    slot.occupied_ = false;
    slot.cancel_requested_ = false;
    auto completion = std::move(slot.completion_);
    slot.next_free_ = free_timer_slot_;
    free_timer_slot_ = slot_index;
    slot.active_generation_.store(0, std::memory_order_release);
    return completion;
}

}  // namespace ruvia::detail
