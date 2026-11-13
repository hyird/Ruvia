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
#if defined(_WIN32)
#include <windows.h>
#endif

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <typeinfo>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#ifdef _WIN32
#include <array>
#include <charconv>
#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <asio/error_code.hpp>
#endif
#if RUVIA_SANITIZER_ALLOCATOR
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#else
#include <mimalloc.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>

#include <asio/windows/stream_handle.hpp>
#else
#include <fcntl.h>
#include <unistd.h>

#include <asio/posix/stream_descriptor.hpp>
#endif
#ifdef __linux__
#include <sys/eventfd.h>
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

namespace ruvia {

namespace {

class runtime_lifecycle final {
public:
    [[nodiscard]] bool start() noexcept {
        auto expected = worker_runtime_state::ready;
        return state_.compare_exchange_strong(
            expected, worker_runtime_state::running, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool request_stop() noexcept {
        auto observed_value = state_.load(std::memory_order_acquire);
        while (observed_value == worker_runtime_state::ready || observed_value == worker_runtime_state::running) {
            if (state_.compare_exchange_weak(observed_value, worker_runtime_state::stopping, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void complete_stop() noexcept {
        auto expected = worker_runtime_state::stopping;
        (void)state_.compare_exchange_strong(
            expected, worker_runtime_state::stopped, std::memory_order_acq_rel, std::memory_order_acquire);
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

    explicit impl_type(worker_runtime_options options)
        : io_context_(
              asio::config_from_concurrency_hint(options.io_policy_ == worker_io_policy::single_owner
                                                     ? ASIO_CONCURRENCY_HINT_UNSAFE_IO
                                                     : ASIO_CONCURRENCY_HINT_DEFAULT)),
          context_(io_context_, options.queue_capacity_, options.blocking_pool_),
          stop_(context_) {
        stop_.configure({
            .stop_admission_ = [this] { begin_stop_admission(); },
            // The hook's target may reenter finalize() while it moves or dies.
            .finalize_ =
                [this] {
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
        if (group_ != nullptr || group_notifications_ != 0 || failure_.failed() ||
            lifecycle_.state() != worker_runtime_state::ready) {
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
        condition_.wait(lock, [this] { return group_notifications_ == 0; });
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
        created->failure_sink_ = [&owner](std::exception_ptr failure) { (void)owner.fail(std::move(failure)); };
        {
            const std::lock_guard lock(mutex_);
            if (loop_) {
                return loop_;
            }
            loop_ = created;
        }
        // A worker that already stops never notifies the loop; retire its
        // admission directly.
        if (context_.worker().register_shutdown(created, &detail::event_loop_state::shutdown_registration_) !=
            worker_registration_status::registered) {
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
            thread_ = std::thread([this] { run(true); });
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
            if (ready_ != phase_type::pending ||
                lifecycle_.state() != worker_runtime_state::running) {
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
        phase_changed_.wait(lock, [&] { return phase != phase_type::pending || failure_.failed(); });
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
        (void)post_control([this] { apply_serve(); });
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
            if (serving_ != phase_type::pending ||
                lifecycle_.state() != worker_runtime_state::running) {
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
            context_.run(
                [this, launch_startup] {
                    if (launch_startup && hooks_.startup_) {
                        hooks_.startup_();
                    }
                },
                [this](std::exception_ptr failure) noexcept { (void)fail(std::move(failure)); },
                [this] {
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
            thread_ = std::thread([this] { run(false); });
        }
        HANDLE duplicate{};
        if (!::DuplicateHandle(::GetCurrentProcess(), thread_.native_handle(),
                ::GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) {
            const auto error = ::GetLastError();
            if (!std::in_range<int>(error)) {
                throw std::runtime_error("worker thread handle duplication returned an out-of-range system error");
            }
            throw std::system_error(static_cast<int>(error), std::system_category(),
                "failed to duplicate worker thread wait handle");
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
            condition_.wait(lock, [this] { return !joining_ && !stop_dispatching_; });
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
                joining_thread = std::thread([this] { run(false); });
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

worker_runtime::worker_runtime(worker_runtime_options options)
    : impl_(std::make_unique<impl_type>(options)) {}

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
worker_runtime::thread_join_operation::thread_join_operation(worker_runtime& owner, worker_ref caller) noexcept
    : owner_(&owner),
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
    if (owner_->impl_->joining_ || (owner_->impl_->stop_dispatching_ &&
                                       owner_->impl_->stop_dispatch_thread_ == std::this_thread::get_id())) {
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

worker_runtime_context::worker_runtime_context(
    asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool)
    : io_context_(&io_context),
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

void worker_runtime_context::run(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    dispatcher_->run_context(
        std::move(startup_handler), std::move(failure_handler), std::move(shutdown_handler));
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

worker_stop_chain::worker_stop_chain(worker_runtime_context& context)
    : context_(context),
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
    worker_ref_access::defer_or_terminate(context_.worker(), [this] { apply_stop(); });
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
        stop_changed_.wait(lock, [this] { return !stop_dispatching_ && !joining_; });
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
    stop_changed_.wait(lock, [this] { return !stop_dispatching_; });
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
            loops_.push_back(std::make_shared<detail::event_loop_state>(
                options.queue_capacity_, options.blocking_pool_));
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
                    } },
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

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::make_shared<impl_type>(options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
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
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
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
void report_through_loop(const std::weak_ptr<detail::event_loop_state>& loop, std::exception_ptr failure,
    const char* context) noexcept {
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

    explicit external_context_attachment_service(asio::execution_context& context_value)
        : asio::execution_context::service(context_value) {}

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

external_context_claim::external_context_claim(asio::io_context& io_context)
    : service_(&asio::use_service<external_context_attachment_service>(io_context)) {
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

attached_context::attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool,
    move_only_function<void(std::exception_ptr)> failure)
    : claim_(io_context),
      runtime_(io_context, queue_capacity, pool),
      stop_(runtime_),
      failure_(std::move(failure)) {
    worker_ref_access::enable_external_runner(runtime_.worker());
}

// The callback owner is kept alive by the completion handler until both the
// returned task frame and its completion chain have retired.
class event_loop_stop_listener final : public std::enable_shared_from_this<event_loop_stop_listener> {
public:
    event_loop_stop_listener(worker_ref worker_value, asio::io_context::executor_type executor,
        move_only_function<task<void>()> callback_value, std::weak_ptr<event_loop_state> loop)
        : worker_(std::move(worker_value)),
          executor_(std::move(executor)),
          callback_(std::move(callback_value)),
          loop_(std::move(loop)) {}

    void install_shutdown_registration() {
        if (worker_.register_shutdown(shared_from_this(), &event_loop_stop_listener::shutdown_registration_) !=
            worker_registration_status::registered) {
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
        worker_ref_access::defer_or_terminate(worker_,
            [self = std::move(self)]() mutable noexcept { self->start_callback(std::move(self)); });
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
            async_start(std::move(*task),
                asio::bind_executor(executor_,
                    [self = std::move(self)](std::exception_ptr failure) mutable {
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

event_loop_state::event_loop_state(std::size_t queue_capacity, blocking_pool* pool)
    : context_ownership_(std::in_place_type<worker_runtime>,
          worker_runtime_options{.queue_capacity_ = queue_capacity, .blocking_pool_ = pool}),
      io_context_(&owned_runtime().io_context()),
      executor_(io_context_->get_executor()),
      runtime_(owned_runtime().context()) {}

event_loop_state::event_loop_state(worker_runtime& runtime)
    : context_ownership_(std::in_place_type<borrowed_runtime>, &runtime),
      io_context_(&runtime.io_context()),
      executor_(io_context_->get_executor()),
      runtime_(runtime.context()) {}

event_loop_state::event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool,
    move_only_function<void(std::exception_ptr)> failure)
    : context_ownership_(std::in_place_type<attached_context>, external_context, queue_capacity, pool, std::move(failure)),
      io_context_(std::addressof(external_context)),
      executor_(external_context.get_executor()),
      runtime_(std::get_if<attached_context>(&context_ownership_)->runtime_) {}

void event_loop_state::attach(const std::shared_ptr<event_loop_state>& self) {
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    attached.stop_.configure({
        // Only stop() requests this chain, so its gate is already sealing.
        // Joining it keeps the chain from finalizing before the gate drains.
        .stop_admission_ = [this] { complete_stop_admission(); },
        // Releasing the claim can destroy this state together with its chain;
        // the chain then releases its work guard on the still-running context.
        .finalize_ =
            [this, &attached] {
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
        report_unhandled_failure("attached event loop",
            std::make_exception_ptr(std::logic_error(
                "io_context destroyed while its attached event loop still had root tasks, stop "
                "callbacks, or cleanup pending; keep running the context until the attachment retires")));
        std::terminate();
    }
    stopping_.store(true, std::memory_order_release);
    attached.stop_.abandon();
    runtime_.detach();
    worker_ref_access::wait_for_reservations(runtime_.worker());
}
void event_loop_state::install_shutdown_registration() {
    if (runtime_.worker().register_shutdown(shared_from_this(), &event_loop_state::shutdown_registration_) !=
        worker_registration_status::registered) {
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
        (void)worker_value.defer_completion([state_value = std::move(self)] { state_value->finalize_stop(); });
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

event_loop_stop_registration::event_loop_stop_registration(
    std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept
    : listener_(std::move(listener_value)) {}

event_loop_start_rejected::event_loop_start_rejected(post_status status)
    : std::runtime_error(status == post_status::queue_full ? "event loop root task queue is full"
                                                           : "event loop is stopping"),
      status_(status) {}

event_loop_stop_registration::~event_loop_stop_registration() {
    reset();
}

event_loop_stop_registration& event_loop_stop_registration::operator=(
    event_loop_stop_registration&& other) noexcept {
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

event_loop::event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

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

        cleanup_control(std::shared_ptr<detail::event_loop_state> state_value,
            move_only_function<void()> task_input) noexcept
            : state_(std::move(state_value)),
              task_(std::move(task_input)) {}
        cleanup_control(const cleanup_control&) = delete;
        cleanup_control& operator=(const cleanup_control&) = delete;
        cleanup_control(cleanup_control&& other) noexcept
            : state_(std::move(other.state_)),
              task_(std::move(other.task_)) {}
        cleanup_control& operator=(cleanup_control&&) = delete;
        ~cleanup_control() {
            if (state_) {
                task_ = nullptr;
                state_->retirement_.finish();
            }
        }
    };
    return state_->runtime_.worker().defer_completion([control = cleanup_control(state_, std::move(task_value))]() mutable { control.task_(); });
}

event_loop_stop_registration event_loop::register_stop_callback(
    move_only_function<task<void>()> callback_value) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    if (!callback_value) {
        throw std::invalid_argument("event loop stop callback must be callable");
    }
    if (!state_->runtime_.worker().accepting()) {
        throw std::runtime_error("cannot register a stop callback on a stopping event loop");
    }
    auto listener_value = std::make_shared<detail::event_loop_stop_listener>(
        state_->runtime_.worker(), state_->executor_, std::move(callback_value), state_);
    listener_value->install_shutdown_registration();
    return event_loop_stop_registration(std::move(listener_value));
}

event_loop_attachment::event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

event_loop_attachment::~event_loop_attachment() {
    stop();
}

event_loop_attachment::event_loop_attachment(event_loop_attachment&& other) noexcept
    : state_(std::move(other.state_)) {}

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

event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options) {
    auto state_value = std::make_shared<detail::event_loop_state>(
        io_context, options.queue_capacity_, options.blocking_pool_, std::move(options.failure_));
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
    explicit current_worker_guard(const worker_dispatcher& worker_value)
        : previous_(std::exchange(g_current_worker, &worker_value)) {
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

worker_dispatcher::worker_dispatcher(asio::io_context& io_context, std::size_t capacity, ruvia::blocking_pool* pool)
    : io_context_(io_context),
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
        asio::post(io_context_, [self = shared_from_this()] { self->drain(); });
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

worker_registration_status worker_dispatcher::register_shutdown(worker_shutdown_registration& hook,
    std::weak_ptr<void> owner, const worker_shutdown_operations_type& operations) {
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

void worker_dispatcher::run_context(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    current_worker_guard current(*this);
    bool failure_delivered = false;
    std::exception_ptr deferred_failure;
    std::exception_ptr delivered_failure;
    const auto deliver_failure = [this, &failure_delivered, &deferred_failure, &delivered_failure, &failure_handler](
                                     std::exception_ptr failure) {
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
    pending_changed_.wait(lock, [this] { return pending_count_ == 0; });
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

worker_ref::worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept
    : dispatcher_(std::move(dispatcher)) {}

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
    return dispatcher_ ? dispatcher_->post(std::move(task_value))
                       : detail::worker_delivery_access::reject<post_result_type>(post_status::worker_stopping, std::move(task_value));
}

void worker_ref::defer_resume(std::coroutine_handle<> continuation) const noexcept {
    if (!dispatcher_) {
        std::terminate();
    }
    dispatcher_->defer_or_terminate([continuation] { continuation.resume(); });
}

worker_ref detail::worker_ref_access::make(
    const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept {
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

void detail::worker_ref_access::defer_or_terminate(
    const worker_ref& worker_value, move_only_function<void()> task_value) noexcept {
    if (!worker_value.dispatcher_) {
        std::terminate();
    }
    worker_value.dispatcher_->defer_or_terminate(std::move(task_value));
}

worker_registration_status worker_ref::link_shutdown(worker_shutdown_registration& registration,
    std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const {
    if (!dispatcher_) {
        return worker_registration_status::worker_stopping;
    }
    return dispatcher_->register_shutdown(registration, std::move(owner), operations);
}

void detail::worker_ref_access::when_shutdown_notifications_complete(
    const worker_ref& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe shutdown notifications on a stopped worker");
    }
    dispatcher->when_shutdown_notifications_complete(std::move(callback_value));
}

void detail::worker_ref_access::when_idle(
    const worker_ref& worker_value, move_only_function<void()> callback_value) {
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

worker_timer_schedule_status worker_ref::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) const& {
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
    if (dispatcher == nullptr || slot_state == nullptr ||
        slot_state->active_generation_.load(std::memory_order_acquire) != generation) {
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

void worker_timer_registration::bind(detail::worker_dispatcher& dispatcher, std::size_t slot,
    std::uint64_t generation, const detail::timer_slot& slot_state) noexcept {
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
    return slot_state_ != nullptr && generation_ != 0 &&
           slot_state_->active_generation_.load(std::memory_order_acquire) == generation_;
}

// Only a cold operation may move; a started one is referenced by its timer
// callback and must stay address-stable.
sleep_operation::sleep_operation(sleep_operation&& other) noexcept
    : worker_(std::exchange(other.worker_, nullptr)),
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
    const auto status = worker_->schedule_timer(registration_, deadline,
        [this](worker_timer_outcome value) {
            outcome_ = value;
            continuation_.resume();
        });
    if (status == worker_timer_schedule_status::worker_stopping) {
        outcome_ = worker_timer_outcome::cancelled;
        return false;
    }
    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_,
            [cancellation = registration_.cancellation()] { cancellation.cancel(); });
    }
    return true;
}

// A zero/negative duration never suspends and keeps the default expired
// outcome, so it reports elapsed unless stop was already requested.
timer_sleep_result sleep_operation::await_resume() const noexcept {
    return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested()
               ? timer_sleep_result::elapsed
               : timer_sleep_result::stop_requested;
}

sleep_operation sleep_for(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value) {
    return sleep_operation(worker_value, duration, std::move(stop_token_value));
}

sleep_operation sleep_until(const worker_ref& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) {
    return sleep_operation(worker_value, deadline, std::move(stop_token_value));
}
}  // namespace ruvia

namespace ruvia::detail {

worker_timer_schedule_status worker_dispatcher::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) {
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

void worker_dispatcher::request_timer_cancellation(
    std::size_t slot, std::uint64_t generation, bool notify) noexcept {
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
    while (!timers_.empty() &&
           !has_live_timer_entry(timers_.front().slot_, timers_.front().generation_)) {
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

move_only_function<void(worker_timer_outcome)> worker_dispatcher::take_timer(
    std::size_t slot_index, std::uint64_t generation) noexcept {
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
        const bool active =
            timer_slots_[index].active_generation_.load(std::memory_order_acquire) != 0;
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
            if (timers_.empty() || timers_.front().deadline_ > now ||
                timers_.front().sequence_ >= sequence_limit) {
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
    return generation != 0 && slot < timer_slots_.size() &&
           timer_slots_[slot].active_generation_.load(std::memory_order_acquire) == generation;
}

bool worker_dispatcher::has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].occupied_ &&
           timer_slots_[slot].generation_ == generation;
}

bool worker_dispatcher::has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept {
    return has_occupied_timer(slot, generation) && !timer_slots_[slot].cancel_requested_;
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::release_timer_slot(
    std::size_t slot_index) noexcept {
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

namespace ruvia {

struct task_scope::node_type {
    node_type(task_scope& owner_value, task<void> child_value)
        : scope_(owner_value),
          task_(std::move(child_value)) {}

    task_scope& scope_;
    task<void> task_;
};

struct task_scope::retiring_guard final {
    explicit retiring_guard(task_scope& scope) noexcept
        : scope_(&scope),
          previous_(scope.retiring_) {
        scope.retiring_ = this;
    }

    ~retiring_guard() {
        if (!destroyed_) {
            scope_->retiring_ = previous_;
        }
    }

    task_scope* scope_;
    retiring_guard* previous_;
    bool destroyed_{false};
};

task_scope::task_scope(const worker_ref& worker_value)
    : worker_(worker_value.valid()
                  ? worker_value
                  : throw std::invalid_argument("task scope requires a valid worker")) {}

task_scope::~task_scope() {
    if (active_ != 0 || std::holds_alternative<task_scope_joining_type>(lifecycle_)) {
        std::terminate();
    }
    for (auto* retiring = retiring_; retiring != nullptr; retiring = retiring->previous_) {
        retiring->destroyed_ = true;
    }
    // Every child finished but no join observed the outcome: the failure has
    // no other owner.
    if (std::holds_alternative<task_scope_open_type>(lifecycle_) && failure_ != nullptr) {
        report_unhandled_failure("task scope child", failure_);
    }
}

void task_scope::spawn(task<void> task_value) & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope spawn must run on its bound worker");
    }
    const auto join_started = [this] {
        return std::holds_alternative<task_scope_joining_type>(lifecycle_) ||
               std::holds_alternative<task_scope_joined_type>(lifecycle_);
    };
    if (join_started()) {
        throw std::logic_error("cannot spawn a task after task scope join started");
    }
    task_value.prepare_direct_start();

    // Admission must remain visible while the allocator can reenter join.
    ++active_;
    node_type* node_value = nullptr;
    try {
        node_value = new node_type(*this, std::move(task_value));
    } catch (...) {
        // Destroy the still-cold child before allowing a join to retire its owner.
        {
            task<void> discarded(std::move(task_value));
        }
        --active_;
        resume_join_if_idle();
        throw;
    }
    if (join_started()) {
        delete node_value;
        --active_;
        resume_join_if_idle();
        throw std::logic_error("cannot spawn a task after task scope join started");
    }

    node_value->task_.coroutine_.promise().control_.set_completion(node_value, &task_scope::child_complete);
    node_value->task_.start();
}

void task_scope::request_stop() & noexcept {
    stop_source_.request_stop();
}

stop_token task_scope::token() const& {
    return stop_source_.token();
}

bool task_scope::stop_requested() const& noexcept {
    return stop_source_.stop_requested();
}

std::size_t task_scope::size() const& noexcept {
    return active_;
}

task_scope::join_operation task_scope::join() & {
    if (!worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    return join_operation(*this);
}

// Only a cold operation may move; a started one has handed its continuation
// to the scope.
task_scope::join_operation::join_operation(join_operation&& other) noexcept
    : scope_(std::exchange(other.scope_, nullptr)) {
    if (other.started_) {
        std::terminate();
    }
}

bool task_scope::join_operation::await_ready() {
    if (scope_ == nullptr || started_) {
        throw std::logic_error("task scope join cannot be awaited");
    }
    if (!scope_->worker_.is_current()) {
        throw std::logic_error("task scope join must run on its bound worker");
    }
    if (std::holds_alternative<task_scope_joining_type>(scope_->lifecycle_) ||
        std::holds_alternative<task_scope_joined_type>(scope_->lifecycle_)) {
        throw std::logic_error("task scope can only be joined once");
    }
    started_ = true;
    if (scope_->active_ == 0 && scope_->retiring_ == nullptr) {
        scope_->lifecycle_.template emplace<task_scope_joined_type>();
        return true;
    }
    return false;
}

void task_scope::join_operation::await_suspend(std::coroutine_handle<> continuation) noexcept {
    scope_->lifecycle_.template emplace<task_scope_joining_type>(continuation);
}

void task_scope::join_operation::await_resume() {
    if (!std::holds_alternative<task_scope_joined_type>(scope_->lifecycle_)) {
        std::terminate();
    }
    scope_->rethrow_failure();
}

void task_scope::resume_join_if_idle() noexcept {
    if (active_ != 0 || retiring_ != nullptr) {
        return;
    }
    auto* joining = std::get_if<task_scope_joining_type>(&lifecycle_);
    if (joining == nullptr) {
        return;
    }
    auto continuation = joining->continuation();
    lifecycle_.template emplace<task_scope_joined_type>();
    // The joiner may destroy this scope inline.
    continuation.resume();
}

void task_scope::child_complete(void* raw) noexcept {
    auto* node_value = static_cast<node_type*>(raw);
    detail::worker_ref_access::defer_or_terminate(
        node_value->scope_.worker_, [node_value] { node_value->scope_.finish(node_value); });
}

void task_scope::finish(node_type* node_value) noexcept {
    bool notify_stop = false;
    node_value->task_.prepare_direct_completion();
    try {
        node_value->task_.coroutine_.promise().result();
    } catch (...) {
        if (failure_ == nullptr) {
            failure_ = std::current_exception();
            notify_stop = true;
        } else if (!equivalent_failures(std::current_exception(), failure_)) {
            // join rethrows only the first failure; later distinct ones have no other owner.
            report_unhandled_failure("additional task scope child", std::current_exception());
        }
    }
    if (notify_stop) {
        request_stop();
    }

    --active_;
    {
        retiring_guard retiring(*this);
        task<void> retired_task(std::move(node_value->task_));
        delete node_value;
        retired_task.finish_direct_completion();
        retired_task.reset();
        if (retiring.destroyed_) {
            return;
        }
    }
    resume_join_if_idle();
}

void task_scope::rethrow_failure() {
    if (failure_ != nullptr) {
        std::rethrow_exception(failure_);
    }
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

stop_state_ref stop_state::create() {
    return stop_state_ref(new stop_state());
}

void stop_state::destroy() noexcept {
    delete this;
}

void stop_state::request_stop() noexcept {
    std::unique_lock lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return;
    }
    flags_.fetch_or(requested_flag, std::memory_order_release);
    requester_ = std::this_thread::get_id();
    while (head_ != nullptr) {
        auto* const node = head_;
        head_ = node->next_;
        if (head_ != nullptr) {
            head_->previous_ = nullptr;
        }
        node->next_ = nullptr;
        running_ = node;
        lock.unlock();
        // The callback may destroy the node, and an unlink waiting on another
        // thread frees it once running_ moves on: the node is not touched again.
        node->invoke();
        lock.lock();
        running_ = nullptr;
        if (std::exchange(run_awaited_, false)) {
            run_epoch_.fetch_add(1, std::memory_order_relaxed);
            run_epoch_.notify_all();
        }
    }
}

bool stop_state::link(stop_callback_node& node, std::atomic<stop_registration_phase>& phase) noexcept {
    std::lock_guard lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return false;
    }
    node.linked_ = true;
    auto expected = stop_registration_phase::registering;
    if (!phase.compare_exchange_strong(expected, stop_registration_phase::registered,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        node.linked_ = false;
        if (expected != stop_registration_phase::reset_pending) {
            std::terminate();
        }
        return false;
    }
    node.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &node;
    }
    head_ = &node;
    return true;
}

void stop_state::unlink(stop_callback_node& node) noexcept {
    std::unique_lock lock(mutex_);
    if (head_ == &node || node.previous_ != nullptr) {
        if (node.previous_ != nullptr) {
            node.previous_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->previous_ = node.previous_;
        }
        node.previous_ = nullptr;
        node.next_ = nullptr;
        return;
    }
    // Never linked, or its run already returned.
    if (running_ != &node) {
        return;
    }
    // Destroyed from inside its own run: waiting would deadlock, and the
    // requester never touches the node after its run.
    if (requester_ == std::this_thread::get_id()) {
        return;
    }
    // The mutex orders the callback's effects before the observed running_ change.
    while (running_ == &node) {
        run_awaited_ = true;
        const auto epoch = run_epoch_.load(std::memory_order_relaxed);
        lock.unlock();
        run_epoch_.wait(epoch, std::memory_order_relaxed);
        lock.lock();
    }
}

}  // namespace detail

// Request deadlines create a source per request on every worker. The mimalloc
// resource has no shared pooling layer and outlives every observer, including
// tokens retired after the allocating worker has exited.
stop_source::stop_source()
    : state_(detail::stop_state::create()) {}

namespace {

[[nodiscard]] bool unique_parent(std::span<const stop_token> parents, std::size_t index) noexcept {
    if (!parents[index].stoppable()) {
        return false;
    }
    for (std::size_t previous = 0; previous != index; ++previous) {
        if (parents[previous] == parents[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t extra_parent_count(std::span<const stop_token> parents) noexcept {
    if (parents.size() <= 2) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            return 0;
        }
        count += unique_parent(parents, index);
    }
    return count > 2 ? count - 2 : 0;
}

}  // namespace

stop_group::stop_group(std::span<const stop_token> parents)
    : extra_parents_(extra_parent_count(parents)) {
    // Capture the state, never this: downstream callbacks may destroy the
    // group, including the parent registration currently invoking us.
    const auto register_parent = [this](const stop_token& parent, stop_registration& registration) {
        parent.register_callback(registration, [state = output_.state_] {
            state->request_stop();
        });
    };
    if (!extra_parents_.empty()) {
        source_.emplace();
        output_ = source_->token();
        std::size_t slot = 0;
        for (std::size_t index = 0; index != parents.size(); ++index) {
            if (!unique_parent(parents, index)) {
                continue;
            }
            if (slot < parents_.size()) {
                register_parent(parents[index], parents_[slot]);
            } else {
                register_parent(parents[index], extra_parents_[slot - parents_.size()]);
            }
            ++slot;
        }
        return;
    }
    const stop_token* first = nullptr;
    const stop_token* second = nullptr;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            output_ = parents[index];
            return;
        }
        if (unique_parent(parents, index)) {
            if (first == nullptr) {
                first = &parents[index];
            } else {
                second = &parents[index];
            }
        }
    }
    if (first == nullptr) {
        return;
    }
    if (second == nullptr) {
        output_ = *first;
        return;
    }
    source_.emplace();
    output_ = source_->token();
    register_parent(*first, parents_[0]);
    register_parent(*second, parents_[1]);
}

}  // namespace ruvia

namespace ruvia {

// Keeps a reentrant join from publishing retirement during frame/capability
// cleanup. Its registration is a value, not a frame-owner base class.
struct operation_scope::drain_guard final {
    explicit drain_guard(operation_scope& scope) noexcept {
        scope.link(registration_);
        registration_.phase_ = detail::scoped_operation_registration::phase::retiring;
    }

    ~drain_guard() {
        auto* scope = registration_.scope_;
        // Keep the drain registered while capability callbacks run: a join
        // started by one of them must not publish retirement prematurely.
        if (scope->head_ == &registration_ && registration_.next_ == nullptr &&
            scope->join_continuation_ == nullptr) {
            scope->expire_capabilities();
        }
        registration_.phase_ = detail::scoped_operation_registration::phase::expired;
        scope->unlink(registration_);
        // Join may retire this scope's owner inline. No further scope access.
        scope->resume_joiner();
    }

    detail::scoped_operation_registration registration_;
};

struct operation_scope::join_awaiter final {
    explicit join_awaiter(operation_scope& scope) noexcept
        : owner_(scope) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return owner_.head_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (owner_.head_ == nullptr || owner_.join_continuation_ != nullptr) {
            std::terminate();
        }
        owner_.join_continuation_ = continuation;
        return true;
    }

    void await_resume() const noexcept {}

    operation_scope& owner_;
};

void operation_scope::close() noexcept {
    if (expiring_capabilities_ || (!active_ && head_ == nullptr && capability_head_ == nullptr)) {
        return;
    }
    for (auto* operation = head_; operation != nullptr; operation = operation->next_) {
        if (operation->phase_ == detail::scoped_operation_registration::phase::running ||
            operation->phase_ == detail::scoped_operation_registration::phase::completing ||
            (active_ && operation->phase_ == detail::scoped_operation_registration::phase::retiring)) {
            std::terminate();
        }
    }
    // An inactive scope already has a drain owner. Reentrant cold-frame
    // cleanup may observe its retiring entries, but never unfinished work.
    if (!active_) {
        return;
    }
    expire_for_join();
}

void operation_scope::expire() noexcept {
    if (expiring_capabilities_ || !active_) {
        return;
    }
    expire_for_join();
}

task<void> operation_scope::close_and_join() & {
    if (join_started_) {
        if (!join_complete_) {
            throw std::logic_error("scoped operation scope can only be joined once");
        }
        co_return;
    }
    join_started_ = true;
    expire_for_join();
    if (head_ == nullptr) {
        expire_capabilities();
        join_complete_ = true;
        co_return;
    }
    co_await join_awaiter(*this);
}

void operation_scope::retire_cold_operations() noexcept {
    for (;;) {
        auto* operation = head_;
        while (operation != nullptr && operation->phase_ != detail::scoped_operation_registration::phase::cold) {
            operation = operation->next_;
        }
        if (operation == nullptr) {
            return;
        }
        operation->retire_frame();
        // Cleanup may remove sibling cold frames; search the live chain again.
    }
}

void operation_scope::expire_for_join() noexcept {
    drain_guard drain(*this);
    active_ = false;
    retire_cold_operations();
}

void operation_scope::expire_capabilities() noexcept {
    expiring_capabilities_ = true;
    while (capability_head_ != nullptr) {
        capability_head_->expire();
    }
    expiring_capabilities_ = false;
}

void operation_scope::resume_joiner() noexcept {
    if (head_ != nullptr || join_continuation_ == nullptr) {
        return;
    }
    auto continuation = std::exchange(join_continuation_, {});
    expire_capabilities();
    join_complete_ = true;
    continuation.resume();
}

void operation_scope::check_worker() const noexcept {
    if (worker_ != nullptr && !worker_->is_current()) {
        std::terminate();
    }
}

scoped_capability_registration::scoped_capability_registration(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept
    : target_(target),
      cleanup_(cleanup),
      active_(scope.active()) {
    if (active_) {
        link(scope);
    }
}

scoped_capability_registration::scoped_capability_registration(
    const scoped_capability_registration& other, void* target) noexcept
    : target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (active_ && other.scope_ != nullptr) {
        link(*other.scope_);
    }
}

scoped_capability_registration::scoped_capability_registration(
    scoped_capability_registration&& other, void* target) noexcept
    : scope_(other.scope_),
      previous_(other.previous_),
      next_(other.next_),
      target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->capability_head_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.target_ = nullptr;
    other.cleanup_ = nullptr;
    other.active_ = false;
}

scoped_capability_registration::~scoped_capability_registration() {
    unlink();
}

void scoped_capability_registration::require_active() const {
    if (!active_) {
        throw std::logic_error("scoped capability lifetime has expired");
    }
}

operation_scope& scoped_capability_registration::scope() const {
    require_active();
    return *scope_;
}

void scoped_capability_registration::bind(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept {
    if (scope_ != nullptr || active_) {
        std::terminate();
    }
    target_ = target;
    cleanup_ = cleanup;
    active_ = scope.active();
    if (active_) {
        link(scope);
    }
}

void scoped_capability_registration::link(operation_scope& scope) noexcept {
    scope_ = &scope;
    next_ = scope.capability_head_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    scope.capability_head_ = this;
}

void scoped_capability_registration::unlink() noexcept {
    if (scope_ == nullptr) {
        return;
    }
    if (previous_ != nullptr) {
        previous_->next_ = next_;
    } else {
        scope_->capability_head_ = next_;
    }
    if (next_ != nullptr) {
        next_->previous_ = previous_;
    }
    scope_ = nullptr;
    previous_ = nullptr;
    next_ = nullptr;
}

void scoped_capability_registration::expire() noexcept {
    const auto cleanup = cleanup_;
    auto* target = target_;
    active_ = false;
    unlink();
    // Cleanup may destroy this registration or remove sibling capabilities.
    // Never access the registration after calling its typed owner.
    if (cleanup != nullptr) {
        cleanup(target);
    }
}

void operation_scope::link(detail::scoped_operation_registration& operation) noexcept {
    operation.scope_ = this;
    operation.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &operation;
    }
    head_ = &operation;
}

void operation_scope::unlink(detail::scoped_operation_registration& operation) noexcept {
    if (operation.previous_ != nullptr) {
        operation.previous_->next_ = operation.next_;
    } else if (head_ == &operation) {
        head_ = operation.next_;
    }
    if (operation.next_ != nullptr) {
        operation.next_->previous_ = operation.previous_;
    }
    operation.scope_ = nullptr;
    operation.previous_ = nullptr;
    operation.next_ = nullptr;
}

namespace detail {

scoped_operation_registration::scoped_operation_registration(
    scoped_operation_registration&& other, void* new_frame_target) noexcept {
    if (other.phase_ == phase::running || other.phase_ == phase::retiring) {
        std::terminate();
    }
    if (other.phase_ == phase::unscoped) {
        return;
    }
    scope_ = other.scope_;
    previous_ = other.previous_;
    next_ = other.next_;
    phase_ = other.phase_;
    frame_target_ = other.frame_target_ != nullptr ? new_frame_target : nullptr;
    retire_cold_ = other.retire_cold_;
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->head_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.phase_ = phase::unscoped;
    other.clear_frame_binding();
}

scoped_operation_registration::~scoped_operation_registration() {
    if (scope_ != nullptr || phase_ == phase::running || phase_ == phase::retiring) {
        std::terminate();
    }
}

void scoped_operation_registration::bind(operation_scope& scope, void* target,
    void (*retire_cold)(void*) noexcept) noexcept {
    if (phase_ != phase::unscoped || retire_cold == nullptr || retire_cold_ != nullptr) {
        std::terminate();
    }
    if (!scope.active()) {
        scope.check_worker();
        phase_ = phase::retiring;
        retire_cold(target);
        phase_ = phase::expired;
        return;
    }
    phase_ = phase::cold;
    frame_target_ = target;
    retire_cold_ = retire_cold;
    scope.link(*this);
}

void scoped_operation_registration::clear_frame_binding() noexcept {
    frame_target_ = nullptr;
    retire_cold_ = nullptr;
}

void scoped_operation_registration::prepare_start() const {
    if (phase_ == phase::expired) {
        throw std::logic_error("capability operation scope has expired");
    }
    if (phase_ != phase::cold) {
        throw std::logic_error("capability operation can only be awaited once");
    }
    scope_->check_worker();
}

void scoped_operation_registration::start() noexcept {
    if (phase_ != phase::cold) {
        std::terminate();
    }
    phase_ = phase::running;
}

void scoped_operation_registration::prepare_completion() noexcept {
    if (phase_ != phase::running) {
        std::terminate();
    }
    scope_->check_worker();
    // The final suspend point permits owner relocation, not early retirement:
    // the registration stays linked until the frame has been destroyed.
    phase_ = phase::completing;
}

void scoped_operation_registration::complete() noexcept {
    if (phase_ != phase::completing) {
        std::terminate();
    }
    phase_ = phase::complete;
    clear_frame_binding();
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        scope->resume_joiner();
    }
}

void scoped_operation_registration::retire_frame() noexcept {
    if (phase_ == phase::running || phase_ == phase::completing || phase_ == phase::retiring) {
        std::terminate();
    }
    if (phase_ == phase::cold) {
        scope_->check_worker();
        phase_ = phase::retiring;
        const auto retire_cold = retire_cold_;
        auto* target = frame_target_;
        clear_frame_binding();
        retire_cold(target);
    }
    phase_ = phase::expired;
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        // Join may retire the owner inline; no further registration access.
        scope->resume_joiner();
    }
}

}  // namespace detail
}  // namespace ruvia

namespace ruvia {

namespace {

#ifdef _WIN32
// Asio reports socket and resolver errors in its own system category, whose
// message() formats Win32 codes in the ANSI code page like std::system_category().
[[nodiscard]] bool is_windows_system_category(const std::error_category& category) noexcept {
    return category == std::system_category() || category == asio::system_category();
}

[[nodiscard]] bool is_message_trailing_space(wchar_t value) noexcept {
    return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n';
}

// The Win32 system message for `value` in UTF-8, or empty when there is none.
[[nodiscard]] std::string windows_system_message(int value) {
    std::array<wchar_t, 1024> buffer;
    // MAX_WIDTH_MASK folds the message's line breaks into spaces.
    auto length = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, nullptr,
        static_cast<DWORD>(value), 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    while (length > 0 && is_message_trailing_space(buffer[length - 1])) {
        --length;
    }
    if (length == 0) {
        return {};
    }
    const auto wide_length = static_cast<int>(length);
    const auto size = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide_length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide_length, text.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return text;
}
#endif

// Reporting is loudest exactly when the process is least able to afford it: an
// allocation failure or a dead upstream fails every connection at once, and
// stderr is a synchronous write that can block the reporting thread when it is
// redirected to a slow or full file. A flood would then slow the very recovery
// it is describing, and bury the first failure -- the informative one -- under
// thousands of identical consequences.
//
// So the reporter emits at most burst lines per window and counts the rest.
// The count is not lost: the next line that gets through carries it.
constexpr std::size_t burst = 20;
constexpr auto window = std::chrono::seconds(1);

std::mutex& rate_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

struct rate_state final {
    std::chrono::steady_clock::time_point window_start_{};
    std::size_t emitted_{0};
    std::size_t suppressed_{0};
    bool started_{false};
};

rate_state& get_rate_state() noexcept {
    static rate_state state;
    return state;
}

// Decides whether this failure gets a line. Returns the number of failures
// suppressed since the last emitted line, to be reported alongside it.
struct rate_decision final {
    bool emit_{false};
    std::size_t suppressed_{0};
};

[[nodiscard]] rate_decision admit() noexcept {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(rate_mutex());
    auto& state_value = get_rate_state();

    if (!state_value.started_ || now - state_value.window_start_ >= window) {
        state_value.started_ = true;
        state_value.window_start_ = now;
        state_value.emitted_ = 0;
    }
    if (state_value.emitted_ >= burst) {
        ++state_value.suppressed_;
        return {};
    }
    ++state_value.emitted_;
    rate_decision decision;
    decision.emit_ = true;
    decision.suppressed_ = state_value.suppressed_;
    state_value.suppressed_ = 0;
    return decision;
}

// Holds the stdio lock of stderr, so the pieces of one line reach it together,
// without interleaving with any concurrent stdio writer.
class stream_lock final {
public:
    stream_lock() noexcept {
#if defined(_WIN32)
        _lock_file(stderr);
#else
        flockfile(stderr);
#endif
    }

    ~stream_lock() {
#if defined(_WIN32)
        _unlock_file(stderr);
#else
        funlockfile(stderr);
#endif
    }

    stream_lock(const stream_lock&) = delete;
    stream_lock& operator=(const stream_lock&) = delete;
};

void write_text(std::string_view text) noexcept {
    std::fwrite(text.data(), 1, text.size(), stderr);
}

void write_line(std::string_view context_value, std::string_view what, std::size_t occurrences,
    std::size_t suppressed) noexcept {
    char occurrences_text[32] = "";
    if (occurrences > 1) {
        std::snprintf(occurrences_text, sizeof(occurrences_text), " (x%zu)", occurrences);
    }
    char suppressed_text[48] = "";
    if (suppressed != 0) {
        std::snprintf(suppressed_text, sizeof(suppressed_text), " (+%zu suppressed)", suppressed);
    }
    // Views are written by their exact length: they need not be NUL-terminated
    // and may be longer than printf's int precision can express.
    const stream_lock lock;
    write_text("ruvia: ");
    write_text(context_value);
    write_text(" failed: ");
    write_text(what);
    write_text(occurrences_text);
    write_text(suppressed_text);
    write_text("\n");
}

}  // namespace

std::string error_code_text(const std::error_code& error) {
#ifdef _WIN32
    if (is_windows_system_category(error.category())) {
        auto text = windows_system_message(error.value());
        if (text.empty()) {
            std::array<char, std::numeric_limits<int>::digits10 + 3> digits;
            const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), error.value()).ptr;
            text.assign(error.category().name()).append(":").append(digits.data(), end);
        }
        return text;
    }
#endif
    return error.message();
}

std::string system_error_text(const std::system_error& error) {
#ifdef _WIN32
    if (is_windows_system_category(error.code().category())) {
        const std::string_view what(error.what());
        const auto native = error.code().message();
        auto text = error_code_text(error.code());
        if (!what.ends_with(native)) {
            return text;
        }
        std::string result(what.substr(0, what.size() - native.size()));
        result.append(text);
        return result;
    }
#endif
    return error.what();
}
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences) noexcept {
    if (exception == nullptr) {
        return;
    }
    const auto decision = admit();
    if (!decision.emit_) {
        return;
    }
    // Rethrowing is the only way to read the exception's message. A nested
    // failure here (a what() that allocates and fails) still leaves the
    // unknown-exception line below, so the report never disappears entirely.
    try {
        std::rethrow_exception(exception);
    } catch (const std::system_error& error) {
        // On Windows what() carries a system-category message in the ANSI code
        // page; the line must stay UTF-8.
        try {
            const auto text = system_error_text(error);
            write_line(context_value, text, occurrences, decision.suppressed_);
        } catch (...) {
            // Formatting failed to allocate; the native text still reports it.
            write_line(context_value, error.what(), occurrences, decision.suppressed_);
        }
    } catch (const std::exception& error) {
        write_line(context_value, error.what(), occurrences, decision.suppressed_);
    } catch (...) {
        write_line(context_value, "unknown exception", occurrences, decision.suppressed_);
    }
}

bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept {
    if (left == right) {
        return true;
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }
    // A caught exception may be a copy that dies with its catch block (MSVC's
    // rethrow_exception copies), so both messages are compared while both
    // handlers are still active; a what() pointer never leaves its catch.
    bool equivalent = false;
    try {
        std::rethrow_exception(left);
    } catch (const std::exception& left_error) {
        try {
            std::rethrow_exception(right);
        } catch (const std::exception& right_error) {
            equivalent = typeid(left_error) == typeid(right_error) &&
                         std::string_view(left_error.what()) == std::string_view(right_error.what());
        } catch (...) {
        }
    } catch (...) {
    }
    return equivalent;
}

}  // namespace ruvia

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RUVIA_SANITIZER_ALLOCATOR 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RUVIA_SANITIZER_ALLOCATOR 1
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif

namespace ruvia {

namespace {

[[nodiscard]] void* allocate_mimalloc(std::size_t bytes_value, std::size_t alignment) {
#if RUVIA_SANITIZER_ALLOCATOR
    // The system allocators may answer a zero-byte request with null.
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, (std::max)(alignment, alignof(void*)))) {
        return block;
    }
#else
    void* block = nullptr;
    if (::posix_memalign(&block, (std::max)(alignment, alignof(void*)), size) == 0 && block != nullptr) {
        return block;
    }
#endif
#else
    // mimalloc returns a unique, aligned block for zero bytes.
    if (void* block = mi_malloc_aligned(bytes_value, alignment)) {
        return block;
    }
#endif
    throw std::bad_alloc();
}

void free_mimalloc(void* block) noexcept {
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
#else
    mi_free(block);
#endif
}

}  // namespace

namespace detail {
void* task_frame_allocate(std::size_t bytes_value) {
    return allocate_mimalloc(bytes_value, alignof(std::max_align_t));
}

void task_frame_deallocate(void* pointer) noexcept {
    free_mimalloc(pointer);
}

void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment) {
    return allocate_mimalloc(bytes_value, alignment);
}

}  // namespace detail

void collect_thread_heap() noexcept {
#if !RUVIA_SANITIZER_ALLOCATOR
    // mi_collect() serves the calling thread's heap; forcing also purges the
    // shared arenas.
    mi_collect(true);
#endif
}
}  // namespace ruvia

namespace ruvia {

namespace {

// The lexical shape is checked here because the platform parsers behind
// asio::ip::make_address are not strict validators: Windows accepts
// `a.b.c.d:port`, `[v6]:port`, and fills in missing IPv4 octets. The address
// text must therefore already be a complete literal before it reaches asio.

[[nodiscard]] constexpr bool is_decimal_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool is_hex_digit(char value) noexcept {
    return is_decimal_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

// Exactly four decimal octets, each 1-3 digits without leading zeros and at
// most 255. Returns the bytes in network order on success.
[[nodiscard]] std::optional<asio::ip::address_v4::bytes_type> parse_ipv4_literal(std::string_view text) noexcept {
    asio::ip::address_v4::bytes_type bytes{};
    std::size_t position = 0;
    for (std::size_t octet = 0; octet < bytes.size(); ++octet) {
        if (octet != 0) {
            if (position >= text.size() || text[position] != '.') {
                return std::nullopt;
            }
            ++position;
        }
        std::size_t digits = 0;
        unsigned value = 0;
        while (position < text.size() && is_decimal_digit(text[position])) {
            if (digits == 3 || (digits == 1 && value == 0)) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<unsigned>(text[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0 || value > 255) {
            return std::nullopt;
        }
        bytes[octet] = static_cast<unsigned char>(value);
    }
    if (position != text.size()) {
        return std::nullopt;
    }
    return bytes;
}

// Hex digits, ':' and '.' (for an embedded IPv4 tail), optionally followed by
// one '%' and a non-empty zone that contains no further '%'; nothing else.
[[nodiscard]] bool is_ipv6_literal_shape(std::string_view text) noexcept {
    const auto zone = text.find('%');
    const auto address = text.substr(0, zone);
    if (address.find(':') == std::string_view::npos) {
        return false;
    }
    for (const char value : address) {
        if (!is_hex_digit(value) && value != ':' && value != '.') {
            return false;
        }
    }
    if (address.find('.') != std::string_view::npos) {
        const auto last_colon = address.rfind(':');
        if (last_colon == std::string_view::npos || !parse_ipv4_literal(address.substr(last_colon + 1)).has_value()) {
            return false;
        }
    }
    if (zone == std::string_view::npos) {
        return true;
    }
    const auto zone_name = text.substr(zone + 1);
    if (zone_name.empty()) {
        return false;
    }
    for (const unsigned char ch : zone_name) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '%' || ch == ':' || ch == '[' || ch == ']') {
            return false;
        }
    }
    if (zone_name.front() == '+' || zone_name.front() == '-') {
        return false;
    }
    if (is_decimal_digit(zone_name.front())) {
        if (zone_name.front() == '0') {
            return false;
        }
        std::uint64_t scope_value = 0;
        for (const char ch : zone_name) {
            if (!is_decimal_digit(ch)) {
                return false;
            }
            scope_value = scope_value * 10 + static_cast<std::uint64_t>(ch - '0');
            if (scope_value > std::numeric_limits<asio::ip::scope_id_type>::max()) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// Consumes the complete view, including checking for embedded NUL. Common
// address text uses stack storage; unusually long scope names use temporary heap
// storage that is released before returning the independent address value.
std::optional<asio::ip::address> parse_ip_address(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    if (const auto v4 = parse_ipv4_literal(text)) {
        return asio::ip::address(asio::ip::address_v4(*v4));
    }
    if (!is_ipv6_literal_shape(text)) {
        return std::nullopt;
    }
    std::error_code error;
    std::array<char, 64> terminated;
    const auto address = [&] {
        if (text.size() < terminated.size()) {
            std::memcpy(terminated.data(), text.data(), text.size());
            terminated[text.size()] = '\0';
            return asio::ip::make_address_v6(terminated.data(), error);
        }
        const std::string owned(text);
        return asio::ip::make_address_v6(owned.c_str(), error);
    }();
    if (error || (text.find('%') != std::string_view::npos && address.scope_id() == 0)) {
        return std::nullopt;
    }
    return asio::ip::address(address);
}

}  // namespace ruvia

namespace ruvia {

// Bounded single-thread FIFO ring. All slots are constructed at startup and stay
// constructed; no queue operation allocates afterwards. Payload assignment may
// allocate or throw independently of the queue. Callers that share it across
// threads must serialize every operation themselves.
//
// prepare_push reserves one unpublished slot: it cannot be prepared again until
// commit_push publishes it or cancel_push releases it. Direct prepared-slot
// assignment failure requires cancel before re-preparing; try_push cancels its
// hidden reservation before rethrowing. A front() borrow stays valid until pop.
template <typename value_type>
class local_ring_queue final {
public:
    explicit local_ring_queue(std::size_t capacity)
        : storage_(allocate_storage(capacity)) {}

    local_ring_queue(const local_ring_queue&) = delete;
    local_ring_queue& operator=(const local_ring_queue&) = delete;
    local_ring_queue(local_ring_queue&&) = delete;
    local_ring_queue& operator=(local_ring_queue&&) = delete;
    ~local_ring_queue() = default;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.get_deleter().capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return size_ == capacity();
    }

    [[nodiscard]] value_type* prepare_push() & noexcept {
        if (prepared_) {
            std::terminate();
        }
        if (size_ == capacity()) {
            return nullptr;
        }
        prepared_ = true;
        return storage_.get() + slot_index(size_);
    }
    value_type* prepare_push() && = delete;

    void commit_push() noexcept {
        if (!prepared_) {
            std::terminate();
        }
        prepared_ = false;
        ++size_;
    }

    void cancel_push() noexcept {
        prepared_ = false;
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept(std::is_nothrow_copy_assignable_v<value_type>) {
        return push_value(value);
    }

    [[nodiscard]] bool try_push(value_type&& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        return push_value(std::move(value));
    }

    [[nodiscard]] value_type* front() & noexcept {
        return size_ == 0 ? nullptr : storage_.get() + head_;
    }
    [[nodiscard]] const value_type* front() const& noexcept {
        return size_ == 0 ? nullptr : storage_.get() + head_;
    }
    value_type* front() && = delete;
    const value_type* front() const&& = delete;

    void pop() noexcept {
        if (size_ == 0) {
            std::terminate();
        }
        head_ = slot_index(1);
        --size_;
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        auto* slot = front();
        if (!slot) {
            return false;
        }
        value = std::move(*slot);
        pop();
        return true;
    }

private:
    struct storage_deleter final {
        std::size_t capacity_{0};
        std::size_t constructed_{0};

        void operator()(value_type* storage) const noexcept {
            std::destroy_n(storage, constructed_);
            std::allocator<value_type>{}.deallocate(storage, capacity_);
        }
    };

    using storage_owner = std::unique_ptr<value_type, storage_deleter>;

    [[nodiscard]] static storage_owner allocate_storage(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("ring queue capacity must be nonzero");
        }
        std::allocator<value_type> allocator;
        storage_owner storage(allocator.allocate(capacity), storage_deleter{capacity, 0});
        auto& constructed = storage.get_deleter().constructed_;
        while (constructed != capacity) {
            std::construct_at(storage.get() + constructed);
            ++constructed;
        }
        return storage;
    }

    // Physical index of the slot `offset` positions after the head; offset <= capacity.
    [[nodiscard]] std::size_t slot_index(std::size_t offset) const noexcept {
        const auto remaining = capacity() - head_;
        return offset >= remaining ? offset - remaining : head_ + offset;
    }

    template <typename input_type>
    [[nodiscard]] bool push_value(input_type&& value) noexcept(std::is_nothrow_assignable_v<value_type&, input_type&&>) {
        auto* slot = prepare_push();
        if (!slot) {
            return false;
        }
        if constexpr (std::is_nothrow_assignable_v<value_type&, input_type&&>) {
            *slot = std::forward<input_type>(value);
        } else {
            try {
                *slot = std::forward<input_type>(value);
            } catch (...) {
                cancel_push();
                throw;
            }
        }
        commit_push();
        return true;
    }

    storage_owner storage_;
    std::size_t head_{0};
    std::size_t size_{0};
    bool prepared_{false};
};

}  // namespace ruvia

namespace ruvia {

namespace {

[[nodiscard]] std::size_t resolve_thread_count(std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto hardware_threads = std::size_t{std::thread::hardware_concurrency()};
    const auto half_hardware_threads = (hardware_threads + 1) / 2;
    return std::clamp(half_hardware_threads, std::size_t{2}, std::size_t{8});
}

[[nodiscard]] std::size_t resolve_queue_capacity(
    std::size_t requested, std::size_t thread_count) {
    constexpr std::size_t default_per_thread_capacity = 64;
    if (requested == 0 &&
        thread_count > std::numeric_limits<std::size_t>::max() / default_per_thread_capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    const auto capacity = requested != 0 ? requested : thread_count * default_per_thread_capacity;
    if (thread_count > std::numeric_limits<std::size_t>::max() - capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    return capacity;
}

thread_local const void* g_current_blocking_pool = nullptr;
}  // namespace

std::string_view describe_blocking_status(blocking_status status) noexcept {
    switch (status) {
        case blocking_status::completed:
            return "completed";
        case blocking_status::queue_full:
            return "blocking pool queue is full";
        case blocking_status::pool_stopped:
            return "blocking pool is stopped";
        case blocking_status::worker_stopping:
            return "worker is stopping";
        case blocking_status::cancelled:
            return "blocking operation was cancelled";
        case blocking_status::timed_out:
            return "blocking operation timed out";
        case blocking_status::unavailable:
            return "worker has no blocking pool";
    }
    return "unknown";
}

// Only blocking_result::value() constructs this, after ruling out completed.
blocking_operation_rejected::blocking_operation_rejected(blocking_status status)
    : std::runtime_error(std::string(describe_blocking_status(status))),
      status_(status) {}

struct blocking_pool::impl_type final {
    struct node_type final {
        move_only_function<void()> task_;
        node_type* next_{nullptr};
    };

    explicit impl_type(const blocking_pool_options& options)
        : thread_count_(resolve_thread_count(options.thread_count_)),
          queue_capacity_(resolve_queue_capacity(options.queue_capacity_, thread_count_)),
          queue_(queue_capacity_),
          nodes_(),
          threads_() {
        // Reserved/queued submissions occupy at most queue_capacity_ nodes;
        // each thread may also hold one node while moving its callable out.
        nodes_.resize(queue_capacity_ + thread_count_);
        for (auto& node : nodes_) {
            node.next_ = free_head_;
            free_head_ = &node;
        }
    }

    void start(const std::shared_ptr<impl_type>& self) {
        threads_.reserve(thread_count_);
        try {
            for (std::size_t i = 0; i < thread_count_; ++i) {
                threads_.emplace_back([self] { self->run(); });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves construction.
            stop();
            join_threads();
            throw;
        }
    }

    void run() noexcept {
        struct current_pool_guard final {
            const void* previous_;
            explicit current_pool_guard(const impl_type* pool) noexcept
                : previous_(std::exchange(g_current_blocking_pool, pool)) {}
            ~current_pool_guard() {
                g_current_blocking_pool = previous_;
            }
        } current_guard(this);
        std::unique_lock lock(mutex_);
        for (;;) {
            condition_.wait(lock, [this] { return stopping_ || queued_ != 0; });
            if (stopping_) {
                // Stopping discards every task that has not already been
                // picked up. A worker must never start queued work after
                // the stop request becomes visible.
                return;
            }
            node_type* node = nullptr;
            if (!queue_.try_pop(node)) {
                std::terminate();
            }
            --queued_;
            ++running_;
            lock.unlock();

            move_only_function<void()> task = std::move(node->task_);
            try {
                task();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // submit() whose task threw with nobody to receive it.
                report_unhandled_failure("blocking pool task", std::current_exception());
            }
            // The task owns the completion it answered; destroy it here rather
            // than under the lock the next iteration takes.
            task = nullptr;

            lock.lock();
            node->next_ = free_head_;
            free_head_ = node;
            --running_;
            ++completed_;
        }
    }

    void stop() noexcept {
        // Only the first stop caller owns the drain. In particular, a queued
        // task's destructor may call stop() again; letting that call recurse
        // into the drain would grow the stack once per queued task.
        std::size_t dropped = 0;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            discarded_ += queued_;
            dropped = std::exchange(queued_, 0);
            if (dropped != 0) {
                stop_drain_thread_ = std::this_thread::get_id();
            }
        }
        condition_.notify_all();
        if (dropped == 0) {
            return;
        }

        // Once stopping is published no thread pops or pushes the ring again,
        // so the first stop caller owns the remaining slots. Destroy them
        // outside the mutex so callable destructors may reenter stats(),
        // stop(), or submit() safely without recursion.
        // Stopping permanently closes admission, so drained nodes need no recycling.
        node_type* node = nullptr;
        while (dropped-- != 0) {
            if (!queue_.try_pop(node)) {
                std::terminate();
            }
            node->task_ = nullptr;
        }
        {
            std::lock_guard lock(mutex_);
            stop_drain_thread_ = {};
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool is_current_thread() const noexcept {
        return g_current_blocking_pool == this;
    }

    void join_threads() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void join() {
        if (is_current_thread()) {
            throw std::logic_error("cannot join a blocking pool from one of its threads");
        }
        stop();
        std::vector<std::thread> joining_threads;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return !joining_; });
            if (!joined_) {
                joining_ = true;
                joining_threads.swap(threads_);
            }
        }
        for (auto& thread : joining_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        {
            std::unique_lock lock(mutex_);
            if (joining_) {
                joining_ = false;
                joined_ = true;
                condition_.notify_all();
            }
            condition_.wait(lock, [this] {
                return (stop_drain_thread_ == std::thread::id{} ||
                           stop_drain_thread_ == std::this_thread::get_id()) &&
                       reserved_ == 0;
            });
        }
    }

    void detach_threads() noexcept {
        std::vector<std::thread> detached_threads;
        {
            std::lock_guard lock(mutex_);
            detached_threads.swap(threads_);
        }
        for (auto& thread : detached_threads) {
            if (thread.joinable()) {
                thread.detach();
            }
        }
    }

    std::size_t thread_count_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // Queue operations move only pointers under mutex_. A claimed node stays
    // exclusive while user callable moves/destruction run outside the lock.
    local_ring_queue<node_type*> queue_;
    std::vector<node_type> nodes_;
    std::vector<std::thread> threads_;
    node_type* free_head_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    std::size_t running_{0};
    std::uint64_t completed_{0};
    std::uint64_t rejected_{0};
    std::uint64_t discarded_{0};
    bool stopping_{false};
    // A nonempty ID owns the stop drain, including destructor reentry.
    std::thread::id stop_drain_thread_{};
    bool joining_{false};
    bool joined_{false};
};

blocking_pool::blocking_pool(blocking_pool_options options)
    : impl_(std::make_shared<impl_type>(options)) {
    impl_->start(impl_);
}

blocking_pool::~blocking_pool() {
    stop();
    impl_->detach_threads();
}

std::size_t blocking_pool::thread_count() const noexcept {
    return impl_->thread_count_;
}

std::size_t blocking_pool::queue_capacity() const noexcept {
    return impl_->queue_capacity_;
}

blocking_pool_stats blocking_pool::stats() const noexcept {
    std::lock_guard lock(impl_->mutex_);
    return blocking_pool_stats{
        .queued_ = impl_->queued_,
        .running_ = impl_->running_,
        .completed_ = impl_->completed_,
        .rejected_ = impl_->rejected_,
        .discarded_ = impl_->discarded_,
    };
}

blocking_submit_result blocking_pool::submit(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("blocking pool requires a callable task");
    }
    const auto owner_ptr = impl_;
    auto& owner = *owner_ptr;
    impl_type::node_type* node = nullptr;
    {
        std::lock_guard lock(owner.mutex_);
        if (owner.stopping_) {
            ++owner.discarded_;
            return detail::worker_delivery_access::reject<blocking_submit_result>(
                blocking_submit_status::pool_stopped, std::move(task_value));
        }
        if (owner.queued_ + owner.reserved_ >= owner.queue_capacity_) {
            ++owner.rejected_;
            return detail::worker_delivery_access::reject<blocking_submit_result>(
                blocking_submit_status::queue_full, std::move(task_value));
        }
        node = owner.free_head_;
        owner.free_head_ = node->next_;
        ++owner.reserved_;
    }
    node->task_ = std::move(task_value);
    bool stopped = false;
    {
        std::lock_guard lock(owner.mutex_);
        stopped = owner.stopping_;
        if (stopped) {
            ++owner.discarded_;
        } else {
            --owner.reserved_;
            if (!owner.queue_.try_push(node)) {
                std::terminate();
            }
            ++owner.queued_;
        }
    }
    if (stopped) {
        // Stopping never recycles nodes, so the reserved node stays exclusive
        // until its callable has been handed back.
        auto rejected = detail::worker_delivery_access::reject<blocking_submit_result>(
            blocking_submit_status::pool_stopped, std::move(node->task_));
        bool notify = false;
        {
            std::lock_guard lock(owner.mutex_);
            notify = (--owner.reserved_ == 0);
        }
        if (notify) {
            owner.condition_.notify_all();
        }
        return rejected;
    }
    owner.condition_.notify_one();
    return detail::worker_delivery_access::accept<blocking_submit_result>();
}

void blocking_pool::stop() noexcept {
    const auto owner = impl_;
    owner->stop();
}

void blocking_pool::join() {
    const auto owner = impl_;
    owner->join();
}

}  // namespace ruvia

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    using acquire_result_type = worker_wait_result<std::size_t>;

    static constexpr std::size_t no_free_slot = static_cast<std::size_t>(-1);
    static constexpr std::size_t slot_leased = static_cast<std::size_t>(-2);

    // Intrusive FIFO of one pool_lease_scheduler. The scheduler is only ever
    // touched from its worker thread, so the queue needs no synchronization.
    class waiter_queue_type final {
    public:
        void enqueue(pool_lease_acquire& waiter) noexcept {
            waiter.previous_ = tail_;
            waiter.next_ = nullptr;
            waiter.queued_ = true;
            if (tail_ != nullptr) {
                tail_->next_ = std::addressof(waiter);
            } else {
                head_ = std::addressof(waiter);
            }
            tail_ = std::addressof(waiter);
        }

        void remove(pool_lease_acquire& waiter) noexcept {
            if (!waiter.queued_) {
                return;
            }
            if (waiter.previous_ != nullptr) {
                waiter.previous_->next_ = waiter.next_;
            } else {
                head_ = waiter.next_;
            }
            if (waiter.next_ != nullptr) {
                waiter.next_->previous_ = waiter.previous_;
            } else {
                tail_ = waiter.previous_;
            }
            waiter.previous_ = nullptr;
            waiter.next_ = nullptr;
            waiter.queued_ = false;
        }

        // Commit the freed slot to the next waiter before deferring its continuation.
        // Cancellation and closure cannot replace this result after the handoff.
        [[nodiscard]] bool commit_handoff(
            std::size_t index, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (head_ == nullptr) {
                return false;
            }
            continuation = head_->resolve(worker_wait_result<std::size_t>::with_value(index));
            return true;
        }

        [[nodiscard]] bool cancel(std::uint64_t id) noexcept {
            auto* waiter = find(id);
            std::coroutine_handle<> continuation;
            if (waiter == nullptr || !commit_cancellation(*waiter, continuation)) {
                return false;
            }
            if (continuation) {
                continuation.resume();
            }
            return true;
        }

        // Commit cancellation while still inside a stop callback, but let the
        // caller resume the coroutine only after that callback has returned.
        [[nodiscard]] bool commit_cancellation(
            pool_lease_acquire& waiter, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (!waiter.queued_) {
                return false;
            }
            continuation = waiter.resolve(worker_wait_status::cancelled);
            return true;
        }

        [[nodiscard]] bool expire(pool_lease_acquire& waiter, worker_wait_status status) noexcept {
            if (!waiter.queued_) {
                return false;
            }
            if (auto continuation = waiter.resolve(status)) {
                continuation.resume();
            }
            return true;
        }

        // Commit closure or shutdown for the entire current queue and resume
        // every waiter in a later worker turn, as release() does: closure runs
        // inside the owner's own teardown, which must finish before any waiter
        // observes it.
        void complete_all(const worker_ref& worker, worker_wait_status status) noexcept {
            while (head_ != nullptr) {
                if (auto continuation = head_->resolve(status)) {
                    worker.defer_resume(continuation);
                }
            }
        }

    private:
        [[nodiscard]] pool_lease_acquire* find(std::uint64_t id) const noexcept {
            auto* waiter = head_;
            while (waiter != nullptr) {
                if (waiter->waiter_id_ == id) {
                    return waiter;
                }
                waiter = waiter->next_;
            }
            return nullptr;
        }

        pool_lease_acquire* head_{nullptr};
        pool_lease_acquire* tail_{nullptr};
    };

    impl_type(std::size_t pool_size, const worker_ref& worker_value)
        : worker_(worker_value),
          next_free_(pool_size, no_free_slot),
          waiter_state_(std::make_shared<waiter_state_type>(worker_value)) {
        for (std::size_t i = 0; i < pool_size; ++i) {
            next_free_[i] = free_head_;
            free_head_ = i;
        }
        if (worker_.register_shutdown(waiter_state_, &waiter_state_type::shutdown_registration_) !=
            worker_registration_status::registered) {
            waiter_state_->stopping_ = true;
        }
    }

    ~impl_type() {
        if (reserved_acquires_ != 0) {
            std::terminate();
        }
    }

    struct waiter_state_type final : std::enable_shared_from_this<waiter_state_type> {
        explicit waiter_state_type(worker_ref worker_value) noexcept
            : worker_(std::move(worker_value)) {}

        // First member: unlinks from the worker registry after every other member is gone.
        worker_shutdown_registration shutdown_registration_;
        worker_ref worker_;
        waiter_queue_type queue_;
        // Worker-affine: set once the shutdown notification reaches the worker.
        bool stopping_{false};

        void stop_waiters() noexcept {
            stopping_ = true;
            queue_.complete_all(worker_, worker_wait_status::worker_stopping);
        }

        // The notification may arrive on the stopping thread; the queue is
        // touched only on the worker.
        void worker_stopping() noexcept {
            if (worker_.is_current()) {
                stop_waiters();
                return;
            }
            try {
                // A detached worker has already retired every suspended acquire.
                (void)worker_.defer_completion([retained = shared_from_this()] { retained->stop_waiters(); });
            } catch (...) {
                std::terminate();
            }
        }

        void request_cancellation(pool_lease_acquire& waiter, std::uint64_t waiter_id) {
            if (worker_.is_current()) {
                std::coroutine_handle<> continuation;
                if (!queue_.commit_cancellation(waiter, continuation) || !continuation) {
                    return;
                }
                worker_.defer_resume(continuation);
                return;
            }
            auto retained = shared_from_this();
            // A detached worker has already retired the owners of every
            // suspended acquire, so there is nothing left to cancel.
            (void)worker_.defer_completion([retained = std::move(retained), waiter_id] {
                (void)retained->queue_.cancel(waiter_id);
            });
        }
    };

    [[nodiscard]] std::optional<acquire_result_type> try_acquire(
        const std::optional<std::chrono::steady_clock::time_point>& deadline, const stop_token& stop_token_value) {
        if (!worker_.is_current()) {
            throw std::logic_error("pool lease acquire must run on its bound worker");
        }
        if (closing_) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::closed);
        }
        if (stop_token_value.stop_requested()) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::cancelled);
        }
        if (free_head_ == no_free_slot) {
            if (waiter_state_->stopping_ || !worker_.accepting() ||
                detail::worker_ref_access::timers_stopping(worker_)) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::worker_stopping);
            }
            if (deadline.has_value() && *deadline <= std::chrono::steady_clock::now()) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::timed_out);
            }
            return std::nullopt;
        }
        const auto slot = free_head_;
        free_head_ = std::exchange(next_free_[slot], slot_leased);
        return worker_wait_result<std::size_t>::with_value(slot);
    }

    [[nodiscard]] std::uint64_t next_waiter_id() noexcept {
        auto waiter_id = ++next_waiter_id_;
        if (waiter_id == 0) {
            waiter_id = ++next_waiter_id_;
        }
        return waiter_id;
    }

    void release(std::size_t slot) noexcept {
        if (slot >= next_free_.size() || next_free_[slot] != slot_leased) {
            std::terminate();
        }
        std::coroutine_handle<> continuation;
        if (!closing_ && waiter_state_->queue_.commit_handoff(slot, continuation)) {
            if (continuation) {
                worker_.defer_resume(continuation);
            }
            return;
        }
        next_free_[slot] = free_head_;
        free_head_ = slot;
    }

    [[nodiscard]] bool close() noexcept {
        if (closing_) {
            return false;
        }
        closing_ = true;
        waiter_state_->queue_.complete_all(worker_, worker_wait_status::closed);
        return true;
    }

    worker_ref worker_;
    std::vector<std::size_t> next_free_;
    std::size_t free_head_{no_free_slot};
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    std::uint64_t next_waiter_id_{0};
    bool closing_{false};
};

pool_lease_scheduler::pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<impl_type>(pool_size, worker_value);
}

pool_lease_scheduler::~pool_lease_scheduler() = default;

pool_lease_acquire pool_lease_scheduler::acquire(operation_options options) & {
    return pool_lease_acquire(*impl_, options.timeout_, std::move(options.stop_token_));
}

pool_lease_acquire::pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value,
    std::optional<operation_timeout> timeout, stop_token stop_token_value) noexcept
    : owner_(&owner_value),
      timeout_(timeout),
      stop_token_(std::move(stop_token_value)) {
    ++owner_->reserved_acquires_;
}

pool_lease_acquire::pool_lease_acquire(pool_lease_acquire&& other) noexcept
    : owner_(other.awaited_ ? nullptr : std::exchange(other.owner_, nullptr)),
      timeout_(other.timeout_),
      deadline_(other.deadline_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.awaited_) {
        std::terminate();
    }
}

pool_lease_acquire::~pool_lease_acquire() {
    if (owner_ != nullptr) {
        if (queued_) {
            cancel_registrations();
        }
        --owner_->reserved_acquires_;
    }
}

void pool_lease_acquire::cancel_registrations() noexcept {
    if (queued_ && owner_ != nullptr) {
        owner_->waiter_state_->queue_.remove(*this);
    }
    deadline_timer_.cancel();
    stop_registration_.reset();
}

void pool_lease_acquire::complete(worker_wait_result<std::size_t> result_value) noexcept {
    if (result_.has_value()) {
        std::terminate();
    }
    result_.emplace(result_value);
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_result<std::size_t> result_value) noexcept {
    cancel_registrations();
    complete(result_value);
    return std::exchange(continuation_, {});
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_status status) noexcept {
    return resolve(worker_wait_result<std::size_t>::with_status(status));
}

bool pool_lease_acquire::await_ready() {
    if (owner_ == nullptr || awaited_) {
        throw std::logic_error("pool lease acquire cannot be awaited");
    }
    awaited_ = true;
    if (timeout_.has_value()) {
        deadline_ = timeout_->deadline_from(std::chrono::steady_clock::now());
    }
    if (auto acquired = owner_->try_acquire(deadline_, stop_token_)) {
        result_.emplace(std::move(*acquired));
    }
    return result_.has_value();
}

bool pool_lease_acquire::await_suspend(std::coroutine_handle<> continuation) {
    // Nothing ran between await_ready and here, so the pool is still saturated.
    waiter_id_ = owner_->next_waiter_id();
    const auto waiter_id = waiter_id_;
    auto* const waiter_state = owner_->waiter_state_.get();
    waiter_state->queue_.enqueue(*this);

    // A deadline arms a worker-local timer owned by this acquire and cancelled
    // before it can be destroyed. Its expiry can unlink the waiter directly;
    // cross-thread stop notifications still use waiter IDs. A timer queue that
    // stops, before or after arming, ends the wait as worker_stopping.
    if (deadline_.has_value()) {
        auto status = worker_timer_schedule_status::worker_stopping;
        try {
            status = owner_->worker_.schedule_timer(deadline_timer_, *deadline_,
                [waiter_state, waiter = this](worker_timer_outcome outcome) noexcept {
                    (void)waiter_state->queue_.expire(*waiter, outcome == worker_timer_outcome::expired
                                                                   ? worker_wait_status::timed_out
                                                                   : worker_wait_status::worker_stopping);
                });
        } catch (...) {
            cancel_registrations();
            throw;
        }
        if (status == worker_timer_schedule_status::worker_stopping) {
            (void)resolve(worker_wait_status::worker_stopping);
            return false;
        }
    }

    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_,
            [waiter_state, waiter = this, waiter_id] { waiter_state->request_cancellation(*waiter, waiter_id); });
    }
    if (result_.has_value()) {
        return false;
    }
    continuation_ = continuation;
    return true;
}

worker_wait_result<std::size_t> pool_lease_acquire::await_resume() {
    if (!result_.has_value()) {
        std::terminate();
    }
    return *result_;
}

void pool_lease_scheduler::release(std::size_t slot) noexcept {
    impl_->release(slot);
}

bool pool_lease_scheduler::close() noexcept {
    return impl_->close();
}

bool pool_lease_scheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class worker_notification_wait_resource final {
public:
    [[nodiscard]] bool allocated() const noexcept {
        return allocation_ != nullptr;
    }
    void* allocate(std::size_t bytes, std::size_t alignment);
    void deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept;

private:
    static constexpr std::size_t buffer_size = 1024;
    static constexpr std::size_t max_alignment = 64;

    // Leave room to align the operation without over-aligning the resource or its owner.
    std::array<std::byte, buffer_size + max_alignment - 1> buffer_storage_{};
    void* allocation_{nullptr};
};

template <typename t_type>
class worker_notification_wait_allocator final {
public:
    using value_type = t_type;

    explicit worker_notification_wait_allocator(
        worker_notification_wait_resource& resource) noexcept
        : resource_(&resource) {}

    template <typename other_type>
    worker_notification_wait_allocator(
        const worker_notification_wait_allocator<other_type>& other) noexcept
        : resource_(other.resource_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) const {
        return static_cast<t_type*>(resource_->allocate(count * sizeof(t_type), alignof(t_type)));
    }

    void deallocate(t_type* pointer, std::size_t count) const noexcept {
        resource_->deallocate(pointer, count * sizeof(t_type), alignof(t_type));
    }

    template <typename other_type>
    [[nodiscard]] bool operator==(
        const worker_notification_wait_allocator<other_type>& other) const noexcept {
        return resource_ == other.resource_;
    }

private:
    template <typename>
    friend class worker_notification_wait_allocator;

    worker_notification_wait_resource* resource_;
};

#ifdef _WIN32
using native_wake_descriptor = HANDLE;
using native_wake_stream = asio::windows::stream_handle;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = nullptr;
#else
using native_wake_descriptor = int;
using native_wake_stream = asio::posix::stream_descriptor;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = -1;
#endif

enum class native_wake_signal : std::uint8_t {
    signaled,
    full,
    failed,
};

// The platform wake channel: a nonblocking sender and the receiver descriptor that
// the worker adopts into its Asio stream. Implemented by
// worker_notification_win32.cpp (named pipe) and worker_notification_posix.cpp
// (Linux eventfd or a POSIX self-pipe).
struct native_wake_pair final {
    native_wake_descriptor sender_{invalid_native_wake_descriptor};
    native_wake_descriptor receiver_{invalid_native_wake_descriptor};
#if !defined(_WIN32) && !defined(__linux__)
    // Keep a reader alive through close/notify races, preventing SIGPIPE until
    // all producers have retired and the pair itself can be destroyed.
    native_wake_descriptor receiver_guard_{invalid_native_wake_descriptor};
#endif

    native_wake_pair() = default;
    native_wake_pair(const native_wake_pair&) = delete;
    native_wake_pair& operator=(const native_wake_pair&) = delete;
    native_wake_pair(native_wake_pair&& other) noexcept;
    native_wake_pair& operator=(native_wake_pair&& other) noexcept;
    ~native_wake_pair();

    // Safe from any thread; writes one token without blocking.
    [[nodiscard]] native_wake_signal signal() const noexcept;
    void close_receiver() noexcept;
};

// startup_timeout bounds the Windows pipe connection; POSIX setup ignores it.
[[nodiscard]] native_wake_pair make_native_wake_pair(std::chrono::milliseconds startup_timeout);

[[noreturn]] void throw_native_wake_error(const char* operation, int error);

#ifdef _WIN32
void disconnect_native_receiver(native_wake_descriptor receiver) noexcept;
#else
// Consumes native wake tokens; returns whether a notification was pending.
[[nodiscard]] bool drain_native_receiver(native_wake_descriptor receiver, std::error_code& error) noexcept;
#endif

class worker_notification_state final {
public:
    worker_notification_state(asio::io_context& io_context, const worker_ref& worker,
        worker_notification_options options);
    ~worker_notification_state();

    worker_notification_state(const worker_notification_state&) = delete;
    worker_notification_state& operator=(const worker_notification_state&) = delete;

    [[nodiscard]] worker_notification_status notify() noexcept;
    [[nodiscard]] bool wait_ready();
    [[nodiscard]] bool begin_wait(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_notification_wait_status take_wait_result();
    void close();

private:
    static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = ~closed;

    [[nodiscard]] bool enter_producer() noexcept;
    void leave_producer() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    void require_worker() const;
    void ensure_receiver();
#ifndef _WIN32
    [[nodiscard]] bool drain_receiver(std::error_code& error) noexcept;
#endif
    void retire_receiver() noexcept;
    void complete_wait(const std::error_code& error, std::size_t bytes_transferred) noexcept;

    asio::io_context& io_context_;
    const worker_ref& worker_;
    native_wake_pair wake_;
#ifdef _WIN32
    std::array<char, 4096> read_buffer_{};
#endif
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<bool> pending_{false};
    worker_notification_wait_resource wait_resource_;
    std::optional<native_wake_stream> receiver_stream_;
    std::coroutine_handle<> wait_continuation_{};
    std::error_code wait_error_;
    worker_notification_wait_status wait_result_{worker_notification_wait_status::notified};
    bool wait_pending_{false};
};

}  // namespace ruvia::detail

#ifndef _WIN32

namespace ruvia::detail {

namespace {

void close_descriptor(int& descriptor) noexcept {
    const int owned = std::exchange(descriptor, -1);
    if (owned >= 0) {
        static_cast<void>(::close(owned));
    }
}

#ifndef __linux__
int configure_descriptor(int descriptor, int command, int value = 0) {
    for (std::size_t interrupted = 0; interrupted < 64; ++interrupted) {
        const int result = fcntl(descriptor, command, value);
        if (result >= 0) {
            return result;
        }
        if (errno != EINTR) {
            throw_native_wake_error("fcntl(worker notification pipe)", errno);
        }
    }
    throw_native_wake_error("fcntl(worker notification pipe)", EINTR);
}

void configure_pipe_descriptor(int descriptor) {
    const int descriptor_flags = configure_descriptor(descriptor, F_GETFD);
    static_cast<void>(configure_descriptor(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    const int status_flags = configure_descriptor(descriptor, F_GETFL);
    static_cast<void>(configure_descriptor(descriptor, F_SETFL, status_flags | O_NONBLOCK));
}
#endif

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, -1)),
      receiver_(std::exchange(other.receiver_, -1))
#ifndef __linux__
      ,
      receiver_guard_(std::exchange(other.receiver_guard_, -1))
#endif
{
}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
#ifndef __linux__
    std::swap(receiver_guard_, other.receiver_guard_);
#endif
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_receiver();
    close_descriptor(sender_);
#ifndef __linux__
    close_descriptor(receiver_guard_);
#endif
}

native_wake_signal native_wake_pair::signal() const noexcept {
#ifdef __linux__
    constexpr std::uint64_t token = 1;
#else
    constexpr char token = '\x01';
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto written = ::write(sender_, &token, sizeof(token));
        if (written == static_cast<ssize_t>(sizeof(token))) {
            return native_wake_signal::signaled;
        }
        if (written < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A full eventfd or self-pipe is already readable. The pending
            // latch was published before this write and is observed after
            // consuming native tokens, including when a read races this write.
            return native_wake_signal::full;
        }
        return native_wake_signal::failed;
    }
    return native_wake_signal::failed;
}

void native_wake_pair::close_receiver() noexcept {
    close_descriptor(receiver_);
}

bool drain_native_receiver(native_wake_descriptor receiver, std::error_code& error) noexcept {
#ifdef __linux__
    std::uint64_t counter = 0;
#else
    std::array<char, 4096> tokens;
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
#ifdef __linux__
        const auto bytes_read = ::read(receiver, &counter, sizeof(counter));
        if (bytes_read == static_cast<ssize_t>(sizeof(counter))) {
#else
        const auto bytes_read = ::read(receiver, tokens.data(), tokens.size());
        if (bytes_read > 0) {
#endif
            return true;
        }
        if (bytes_read < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        if (bytes_read < 0) {
            error = std::error_code(errno, std::system_category());
        } else if (bytes_read == 0) {
            error = std::make_error_code(std::errc::connection_reset);
        } else {
            error = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
    error = std::make_error_code(std::errc::interrupted);
    return false;
}

native_wake_pair make_native_wake_pair(std::chrono::milliseconds) {
    native_wake_pair pair;
#ifdef __linux__
    pair.sender_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender_ < 0) {
        throw_native_wake_error("eventfd", errno);
    }
    pair.receiver_ = fcntl(pair.sender_, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver_ < 0) {
        throw_native_wake_error("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
#else
    int descriptors[2]{-1, -1};
    for (std::size_t interrupted = 0;; ++interrupted) {
        if (::pipe(descriptors) == 0) {
            break;
        }
        if (errno != EINTR || interrupted == 63) {
            throw_native_wake_error("pipe(worker notification)", errno);
        }
    }
    pair.receiver_ = descriptors[0];
    pair.sender_ = descriptors[1];
    configure_pipe_descriptor(pair.receiver_);
    configure_pipe_descriptor(pair.sender_);
    pair.receiver_guard_ = configure_descriptor(pair.receiver_, F_DUPFD);
    configure_pipe_descriptor(pair.receiver_guard_);
#endif
    return pair;
}

}  // namespace ruvia::detail

#endif

#ifdef _WIN32

namespace ruvia::detail {

namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] bool valid_descriptor(HANDLE descriptor) noexcept {
    return descriptor != nullptr && descriptor != INVALID_HANDLE_VALUE;
}

void close_descriptor(HANDLE& descriptor) noexcept {
    const HANDLE owned = std::exchange(descriptor, nullptr);
    if (valid_descriptor(owned)) {
        static_cast<void>(CloseHandle(owned));
    }
}

class pipe_connect_operation final {
public:
    explicit pipe_connect_operation(HANDLE pipe)
        : pipe_(pipe) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            throw_native_wake_error("CreateEventW(named-pipe connect)", static_cast<int>(GetLastError()));
        }
        overlapped_.hEvent = event_;
    }

    ~pipe_connect_operation() {
        cancel_and_drain();
        close_descriptor(event_);
    }

    pipe_connect_operation(const pipe_connect_operation&) = delete;
    pipe_connect_operation& operator=(const pipe_connect_operation&) = delete;

    [[nodiscard]] bool start() {
        if (ConnectNamedPipe(pipe_, &overlapped_)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            return true;
        }
        if (error == ERROR_IO_PENDING) {
            pending_ = true;
            return false;
        }
        throw_native_wake_error("ConnectNamedPipe", static_cast<int>(error));
    }

    [[nodiscard]] static DWORD remaining_milliseconds(clock_type::time_point deadline_value) noexcept {
        const auto now = clock_type::now();
        if (now >= deadline_value) {
            return 0;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline_value - now).count();
        return static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 60'000));
    }

    void wait_until(clock_type::time_point deadline_value) {
        if (!pending_) {
            return;
        }
        const DWORD timeout = remaining_milliseconds(deadline_value);
        const DWORD wait_result = WaitForSingleObject(event_, timeout);
        if (wait_result == WAIT_TIMEOUT) {
            throw std::runtime_error("timed out connecting native worker notification pipe");
        }
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            throw_native_wake_error("WaitForSingleObject(named-pipe connect)", static_cast<int>(error));
        }

        DWORD bytes_transferred = 0;
        const BOOL completed = GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        pending_ = false;
        if (!completed) {
            throw_native_wake_error("GetOverlappedResult(named-pipe connect)", static_cast<int>(error));
        }
    }

private:
    void cancel_and_drain() noexcept {
        if (!pending_) {
            return;
        }
        if (!CancelIoEx(pipe_, &overlapped_) && GetLastError() != ERROR_NOT_FOUND) {
            std::terminate();
        }
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) {
            std::terminate();
        }
        DWORD bytes_transferred = 0;
        if (!GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE) &&
            GetLastError() != ERROR_OPERATION_ABORTED) {
            std::terminate();
        }
        pending_ = false;
    }

    HANDLE pipe_{};
    HANDLE event_{};
    OVERLAPPED overlapped_{};
    bool pending_{false};
};

struct local_security_descriptor final {
    PSECURITY_DESCRIPTOR value_{nullptr};

    ~local_security_descriptor() {
        if (value_ != nullptr) {
            static_cast<void>(LocalFree(value_));
        }
    }
};

[[nodiscard]] std::array<wchar_t, 80> make_pipe_name() {
    constexpr wchar_t prefix[] = L"\\\\.\\pipe\\Ruvia-WorkerNotification-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::array<std::uint8_t, 16> random_bytes{};
    if (BCryptGenRandom(nullptr, random_bytes.data(), static_cast<ULONG>(random_bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("BCryptGenRandom failed for native worker notification name");
    }

    std::array<wchar_t, 80> name{};
    std::size_t position = 0;
    for (std::size_t index = 0; index + 1 < std::size(prefix); ++index) {
        name[position++] = prefix[index];
    }
    for (const auto byte : random_bytes) {
        name[position++] = hex[byte >> 4];
        name[position++] = hex[byte & 0x0f];
    }
    return name;
}

void validate_pipe_client_process(HANDLE server_pipe) {
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(server_pipe, &client_process_id)) {
        throw_native_wake_error("GetNamedPipeClientProcessId", static_cast<int>(GetLastError()));
    }
    if (client_process_id != GetCurrentProcessId()) {
        throw std::runtime_error("native worker notification pipe connected to another process");
    }
}

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, nullptr)),
      receiver_(std::exchange(other.receiver_, nullptr)) {}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_descriptor(sender_);
    close_receiver();
}

native_wake_signal native_wake_pair::signal() const noexcept {
    constexpr char token = '\x01';
    DWORD bytes_written = 0;
    if (!WriteFile(sender_, &token, 1, &bytes_written, nullptr)) {
        // ERROR_NO_DATA and every other failed WriteFile result are not evidence
        // of a full pipe. Never turn a disconnect into a successful coalesce.
        return native_wake_signal::failed;
    }
    if (bytes_written == 1) {
        return native_wake_signal::signaled;
    }
    if (bytes_written == 0) {
        // With a synchronous PIPE_NOWAIT writer, TRUE plus zero bytes means
        // the byte-mode pipe buffer is full; its receiver already has data.
        return native_wake_signal::full;
    }
    return native_wake_signal::failed;
}

void native_wake_pair::close_receiver() noexcept {
    if (valid_descriptor(receiver_)) {
        disconnect_native_receiver(receiver_);
        close_descriptor(receiver_);
    }
}

void disconnect_native_receiver(native_wake_descriptor receiver) noexcept {
    if (!valid_descriptor(receiver) || DisconnectNamedPipe(receiver)) {
        return;
    }
    if (GetLastError() != ERROR_PIPE_NOT_CONNECTED) {
        std::terminate();
    }
}

native_wake_pair make_native_wake_pair(std::chrono::milliseconds startup_timeout) {
    native_wake_pair pair;
    const auto pipe_name = make_pipe_name();

    // Restrict access to the pipe owner's SID; the random name and later client-PID
    // check prevent another local process from impersonating this worker channel.
    local_security_descriptor security_descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &security_descriptor.value_, nullptr)) {
        throw_native_wake_error("ConvertStringSecurityDescriptorToSecurityDescriptorW",
            static_cast<int>(GetLastError()));
    }
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.lpSecurityDescriptor = security_descriptor.value_;
    security_attributes.bInheritHandle = FALSE;

    pair.receiver_ = CreateNamedPipeW(pipe_name.data(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &security_attributes);
    if (!valid_descriptor(pair.receiver_)) {
        const DWORD error = GetLastError();
        pair.receiver_ = nullptr;
        throw_native_wake_error("CreateNamedPipeW", static_cast<int>(error));
    }

    const auto deadline_value = clock_type::now() + startup_timeout;
    pipe_connect_operation connect_operation(pair.receiver_);
    if (connect_operation.start()) {
        // ERROR_PIPE_CONNECTED here precedes our CreateFile call: validate the peer,
        // then fail closed because this instance has no writer handle owned by us.
        validate_pipe_client_process(pair.receiver_);
        throw std::runtime_error(
            "native worker notification pipe connected before its writer handle was opened");
    }

    for (;;) {
        const DWORD remaining = pipe_connect_operation::remaining_milliseconds(deadline_value);
        if (!WaitNamedPipeW(pipe_name.data(), remaining)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT || error == ERROR_FILE_NOT_FOUND) {
                throw std::runtime_error("timed out waiting for native worker notification pipe");
            }
            throw_native_wake_error("WaitNamedPipeW", static_cast<int>(error));
        }

        pair.sender_ = CreateFileW(pipe_name.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (valid_descriptor(pair.sender_)) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && clock_type::now() < deadline_value) {
            continue;
        }
        if (error == ERROR_PIPE_BUSY || error == ERROR_SEM_TIMEOUT) {
            throw std::runtime_error("timed out opening native worker notification writer");
        }
        throw_native_wake_error("CreateFileW(named-pipe writer)", static_cast<int>(error));
    }

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pair.sender_, &mode, nullptr, nullptr)) {
        throw_native_wake_error("SetNamedPipeHandleState(PIPE_NOWAIT)", static_cast<int>(GetLastError()));
    }
    connect_operation.wait_until(deadline_value);
    validate_pipe_client_process(pair.receiver_);
    return pair;
}

}  // namespace ruvia::detail

#endif

namespace ruvia::detail {

void* worker_notification_wait_resource::allocate(std::size_t bytes_value, std::size_t alignment) {
    if (allocated() || bytes_value > buffer_size || alignment > max_alignment) {
        throw std::bad_alloc();
    }
    void* pointer = buffer_storage_.data();
    auto space = buffer_storage_.size();
    allocation_ = std::align(alignment, bytes_value, pointer, space);
    if (allocation_ == nullptr) {
        throw std::bad_alloc();
    }
    return allocation_;
}

void worker_notification_wait_resource::deallocate(
    void* pointer, std::size_t, std::size_t) noexcept {
    if (!allocated() || pointer != allocation_) {
        std::terminate();
    }
    allocation_ = nullptr;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void throw_native_wake_error(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

worker_notification_state::worker_notification_state(asio::io_context& io_context,
    const worker_ref& worker, worker_notification_options options)
    : io_context_(io_context),
      worker_(worker) {
    if (!worker_.valid() || !worker_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    if (options.startup_timeout_.value() <= std::chrono::milliseconds::zero() ||
        options.startup_timeout_.value() > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
    wake_ = make_native_wake_pair(options.startup_timeout_.value());
}

worker_notification_state::~worker_notification_state() {
    const auto activity = activity_.load(std::memory_order_acquire);
    if ((activity & count_mask) != 0 || wait_pending_ || wait_continuation_ ||
        receiver_stream_.has_value() || wait_resource_.allocated()) {
        std::terminate();
    }
    retire_receiver();
}

worker_notification_status worker_notification_state::notify() noexcept {
    if (!enter_producer()) {
        return worker_notification_status::closed;
    }
    struct producer_exit final {
        worker_notification_state& state_;
        ~producer_exit() {
            state_.leave_producer();
        }
    } producer_exit_value{*this};

    if (pending_.exchange(true, std::memory_order_acq_rel)) {
        return is_closed() ? worker_notification_status::closed : worker_notification_status::coalesced;
    }
    if (is_closed()) {
        return worker_notification_status::closed;
    }

    const auto signal = wake_.signal();
    if (is_closed()) {
        return worker_notification_status::closed;
    }
    switch (signal) {
        case native_wake_signal::signaled:
            return worker_notification_status::notified;
        case native_wake_signal::full:
            return worker_notification_status::coalesced;
        case native_wake_signal::failed:
            break;
    }
    std::terminate();
}

bool worker_notification_state::wait_ready() {
    require_worker();
    if (wait_pending_) {
        throw std::logic_error("worker notification permits one outstanding wait");
    }
    if (is_closed()) {
        wait_result_ = worker_notification_wait_status::closed;
        wait_error_.clear();
        return true;
    }
    ensure_receiver();
#ifdef _WIN32
    // Overlapped named-pipe reads are the readiness operation on IOCP. Even an
    // already-latched notification is consumed by the ensuing async_read_some.
    return false;
#else
    const bool received_value = drain_receiver(wait_error_);
    const bool latched = pending_.exchange(false, std::memory_order_acq_rel);
    if (wait_error_ || received_value || latched) {
        wait_result_ = worker_notification_wait_status::notified;
        return true;
    }
    return false;
#endif
}

bool worker_notification_state::begin_wait(std::coroutine_handle<> continuation) {
    require_worker();
    if (wait_pending_) {
        throw std::logic_error("worker notification permits one outstanding wait");
    }
    if (is_closed()) {
        wait_result_ = worker_notification_wait_status::closed;
        wait_error_.clear();
        return false;
    }
    // A signal arriving after await_ready remains readable by the native
    // async wait; polling the receiver again only adds an empty system call.

    wait_pending_ = true;
    wait_continuation_ = continuation;
    try {
        auto handler = asio::bind_allocator(
            worker_notification_wait_allocator<std::byte>(wait_resource_),
#ifdef _WIN32
            [this](const asio::error_code& error, std::size_t bytes_transferred) noexcept {
                complete_wait(error, bytes_transferred);
            }
#else
            [this](const asio::error_code& error) noexcept { complete_wait(error, 0); }
#endif
        );
#ifdef _WIN32
        receiver_stream_->async_read_some(asio::buffer(read_buffer_), std::move(handler));
#else
        receiver_stream_->async_wait(native_wake_stream::wait_read, std::move(handler));
#endif
    } catch (...) {
        wait_pending_ = false;
        wait_continuation_ = {};
        throw;
    }
    return true;
}

worker_notification_wait_status worker_notification_state::take_wait_result() {
    require_worker();
    if (wait_pending_) {
        std::terminate();
    }
    if (wait_error_) {
        const auto error = std::exchange(wait_error_, {});
        throw std::system_error(error, "worker notification wait");
    }
    return wait_result_;
}

void worker_notification_state::close() {
    require_worker();
    const auto old = activity_.fetch_or(closed, std::memory_order_acq_rel);
    if ((old & closed) != 0) {
        return;
    }

    if (receiver_stream_ && wait_pending_) {
        asio::error_code error;
        receiver_stream_->cancel(error);
        if (error) {
            std::terminate();
        }
#ifdef _WIN32
        // Keep the connected server HANDLE until IOCP reports the canceled read.
        return;
#else
        receiver_stream_->close(error);
        if (error) {
            std::terminate();
        }
        return;
#endif
    }
    retire_receiver();
}

bool worker_notification_state::enter_producer() noexcept {
    auto state_value = activity_.load(std::memory_order_acquire);
    for (;;) {
        if ((state_value & closed) != 0) {
            return false;
        }
        if ((state_value & count_mask) == count_mask) {
            std::terminate();
        }
        if (activity_.compare_exchange_weak(state_value, state_value + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void worker_notification_state::leave_producer() noexcept {
    const auto previous = activity_.fetch_sub(1, std::memory_order_release);
    if ((previous & count_mask) == 0) {
        std::terminate();
    }
}

bool worker_notification_state::is_closed() const noexcept {
    return (activity_.load(std::memory_order_acquire) & closed) != 0;
}

void worker_notification_state::require_worker() const {
    if (!worker_.is_current()) {
        throw std::logic_error("worker notification wait and close are worker-affine");
    }
}

void worker_notification_state::ensure_receiver() {
    if (receiver_stream_) {
        return;
    }
    try {
        receiver_stream_.emplace(io_context_);
        asio::error_code error;
        receiver_stream_->assign(wake_.receiver_, error);
        if (error) {
            throw std::system_error(error, "assign worker notification receiver");
        }
        wake_.receiver_ = invalid_native_wake_descriptor;
    } catch (...) {
        static_cast<void>(activity_.fetch_or(closed, std::memory_order_acq_rel));
        if (receiver_stream_) {
            asio::error_code ignored;
            if (receiver_stream_->is_open()) {
                receiver_stream_->close(ignored);
            }
            receiver_stream_.reset();
        }
        retire_receiver();
        throw;
    }
}

#ifndef _WIN32
bool worker_notification_state::drain_receiver(std::error_code& error) noexcept {
    if (!receiver_stream_ || !receiver_stream_->is_open()) {
        return false;
    }
    return drain_native_receiver(receiver_stream_->native_handle(), error);
}
#endif

void worker_notification_state::retire_receiver() noexcept {
    if (receiver_stream_) {
        asio::error_code error;
        if (receiver_stream_->is_open()) {
#ifdef _WIN32
            disconnect_native_receiver(receiver_stream_->native_handle());
#endif
            receiver_stream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiver_stream_.reset();
    }
    wake_.close_receiver();
}

void worker_notification_state::complete_wait(
    const std::error_code& error, std::size_t bytes_transferred) noexcept {
    if (!wait_pending_ || !wait_continuation_) {
        std::terminate();
    }
    wait_pending_ = false;
    const auto continuation = std::exchange(wait_continuation_, {});

    if (is_closed()) {
        wait_error_.clear();
        wait_result_ = worker_notification_wait_status::closed;
        retire_receiver();
        continuation.resume();
        return;
    }

    if (error) {
        wait_error_ = error;
#ifdef _WIN32
    } else if (bytes_transferred == 0) {
        wait_error_ = std::make_error_code(std::errc::connection_reset);
#endif
    } else {
        static_cast<void>(bytes_transferred);
#ifndef _WIN32
        static_cast<void>(drain_receiver(wait_error_));
#endif
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
    }
    wait_result_ = worker_notification_wait_status::notified;
    continuation.resume();
}

namespace {

// The only place a notification's state is built; every public constructor
// hands its runtime's io_context and worker_ref to this entry.
[[nodiscard]] std::unique_ptr<worker_notification_state>
make_worker_notification_state(
    asio::io_context& io_context, const worker_ref& worker, worker_notification_options options) {
    return std::make_unique<worker_notification_state>(io_context, worker, options);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

worker_notification::wait_operation::wait_operation(worker_notification& owner_value) noexcept
    : owner_(&owner_value) {}

bool worker_notification::wait_operation::await_ready() {
    return owner_->state_->wait_ready();
}

bool worker_notification::wait_operation::await_suspend(std::coroutine_handle<> continuation) {
    return owner_->state_->begin_wait(continuation);
}

worker_notification_wait_status worker_notification::wait_operation::await_resume() {
    return owner_->state_->take_wait_result();
}

worker_notification::worker_notification(state_pointer state) noexcept
    : state_(std::move(state)),
      wait_awaiter_(*this) {}

worker_notification::worker_notification(
    worker_runtime& runtime, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(runtime.io_context(), runtime.worker(), options)) {}

worker_notification::worker_notification(
    const event_loop& loop, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(loop.io_context(), loop.worker(), options)) {}
worker_notification::~worker_notification() = default;

worker_notification_status worker_notification::notify() noexcept {
    return state_->notify();
}

worker_notification::wait_operation& worker_notification::wait() & noexcept {
    return wait_awaiter_;
}

void worker_notification::close() {
    state_->close();
}

}  // namespace ruvia
