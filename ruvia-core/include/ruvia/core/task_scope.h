#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"

namespace ruvia {

class task_scope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable worker_ref, matching the borrow-only hot-path rule.
    explicit task_scope(const worker_ref& worker_value);
    task_scope(worker_ref&&) = delete;
    task_scope(const worker_ref&&) = delete;
    // Destruction requires that no child is active and no join is pending;
    // violating that is a lifecycle contract violation and terminates. A
    // scope whose children all completed may be destroyed without a join; a
    // child failure nobody awaited is then routed to
    // report_unhandled_failure.
    ~task_scope();

    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;
    task_scope(task_scope&&) = delete;
    task_scope& operator=(task_scope&&) = delete;

    // Every operation is bound to this scope's stable address: child
    // completion nodes point back to it, and lazy join tasks borrow it.
    // Reject temporary scopes before either lifetime can be
    // created and keep even state/token access on the same explicit owner.
    void spawn(task<void> task) &;
    void spawn(task<void>) && = delete;
    // Stop control is worker-affine. Construction creates the stop state so
    // token() only observes it and never allocates or requests cancellation.
    void request_stop() & noexcept;
    void request_stop() && = delete;
    [[nodiscard]] stop_token token() const&;
    stop_token token() const&& = delete;
    [[nodiscard]] bool stop_requested() const& noexcept;
    bool stop_requested() const&& = delete;
    // Includes a spawn currently acquiring its child node.
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    [[nodiscard]] bool empty() const& noexcept {
        return size() == 0;
    }
    bool empty() const&& = delete;
    // One join: single-use, movable only before it is awaited. A cold join
    // does not close spawn admission; starting it claims the scope exactly
    // once and prevents further spawning. It waits for in-progress spawn
    // admission and child node retirement. The scope and worker must remain
    // alive until the started operation and all children have completed.
    class join_operation final {
    public:
        join_operation(const join_operation&) = delete;
        join_operation& operator=(const join_operation&) = delete;
        join_operation(join_operation&& other) noexcept;
        join_operation& operator=(join_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        void await_suspend(std::coroutine_handle<> continuation) noexcept;
        void await_resume();

    private:
        friend class task_scope;

        explicit join_operation(task_scope& scope) noexcept
            : scope_(&scope) {}

        task_scope* scope_;
        bool started_{false};
    };

    // Creation and starting the join both require the bound worker. The join
    // rethrows the first child failure; each later child failure not
    // equivalent to it (see equivalent_failures) is routed to
    // report_unhandled_failure when that child completes.
    [[nodiscard]] join_operation join() &;
    join_operation join() && = delete;

private:
    struct node_type;
    struct retiring_guard;

    struct task_scope_open_type final {};
    class task_scope_joining_type final {
    public:
        explicit task_scope_joining_type(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };
    struct task_scope_joined_type final {};

    static void child_complete(void* raw) noexcept;
    void finish(node_type* node) noexcept;
    void resume_join_if_idle() noexcept;
    void rethrow_failure();

    using lifecycle_type = std::variant<task_scope_open_type, task_scope_joining_type, task_scope_joined_type>;

    const worker_ref& worker_;
    stop_source stop_source_;
    std::size_t active_{0};
    retiring_guard* retiring_{nullptr};
    lifecycle_type lifecycle_;
    std::exception_ptr failure_{};
};

}  // namespace ruvia

namespace ruvia {

class scoped_capability_registration;
class worker_ref;

namespace detail {
class scoped_operation_registration;
}

// Worker-affine owner of borrowed capabilities and their lazy operations.
// Close discards cold frames before capability cleanup. Started operations must
// finish; close_and_join waits for their frames, leases and capability cleanup
// to retire, including when invoked reentrantly by a cleanup callback.
// bind_worker pins the scope to one worker: every operation in it then
// terminates when started, completed or retired cold on another thread.
class operation_scope final {
public:
    operation_scope() noexcept = default;
    ~operation_scope() {
        close();
    }

    operation_scope(const operation_scope&) = delete;
    operation_scope& operator=(const operation_scope&) = delete;
    operation_scope(operation_scope&&) = delete;
    operation_scope& operator=(operation_scope&&) = delete;

    void close() noexcept;
    void expire() noexcept;
    [[nodiscard]] task<void> close_and_join() &;
    task<void> close_and_join() && = delete;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool has_pending_operations() const noexcept {
        return head_ != nullptr;
    }
    // Borrows the worker_ref, which must outlive this scope (the same rule
    // as task_scope). Bind once, before any operation is registered.
    void bind_worker(const worker_ref& worker) & noexcept {
        if (worker_ != nullptr || head_ != nullptr || capability_head_ != nullptr || !active_) {
            std::terminate();
        }
        worker_ = &worker;
    }
    void bind_worker(const worker_ref&) && = delete;
    void bind_worker(worker_ref&&) = delete;
    void bind_worker(const worker_ref&&) = delete;

private:
    friend class detail::scoped_operation_registration;
    friend class scoped_capability_registration;
    struct drain_guard;
    struct join_awaiter;
    void check_worker() const noexcept;
    void retire_cold_operations() noexcept;
    void expire_for_join() noexcept;
    void expire_capabilities() noexcept;
    void resume_joiner() noexcept;
    void link(detail::scoped_operation_registration& operation) noexcept;
    void unlink(detail::scoped_operation_registration& operation) noexcept;

    detail::scoped_operation_registration* head_{nullptr};
    scoped_capability_registration* capability_head_{nullptr};
    const worker_ref* worker_{nullptr};
    std::coroutine_handle<> join_continuation_{};
    bool active_{true};
    bool expiring_capabilities_{false};
    bool join_started_{false};
    bool join_complete_{false};
};

// Allocation-free value registration. An owner must explicitly rebind the
// cleanup target when copying or moving; ordinary copy/move is forbidden.
// Declare it after the payload so destruction unlinks before payload teardown.
class scoped_capability_registration final {
public:
    scoped_capability_registration() noexcept = default;
    scoped_capability_registration(operation_scope& scope, void* target,
        void (*cleanup)(void*) noexcept) noexcept;
    scoped_capability_registration(const scoped_capability_registration& other, void* target) noexcept;
    scoped_capability_registration(scoped_capability_registration&& other, void* target) noexcept;
    ~scoped_capability_registration();

    scoped_capability_registration(const scoped_capability_registration&) = delete;
    scoped_capability_registration& operator=(const scoped_capability_registration&) = delete;
    scoped_capability_registration(scoped_capability_registration&&) = delete;
    scoped_capability_registration& operator=(scoped_capability_registration&&) = delete;

    void require_active() const;
    [[nodiscard]] operation_scope& scope() const;
    void bind(operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept;

private:
    friend class operation_scope;
    void link(operation_scope& scope) noexcept;
    void unlink() noexcept;
    void expire() noexcept;

    operation_scope* scope_{nullptr};
    scoped_capability_registration* previous_{nullptr};
    scoped_capability_registration* next_{nullptr};
    void* target_{nullptr};
    void (*cleanup_)(void*) noexcept {nullptr};
    bool active_{false};
};

template <typename t_type>
[[nodiscard]] task<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value) {
    task_value.bind_scope(scope);
    return task_value;
}
}  // namespace ruvia

namespace ruvia {

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the worker_ref is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class worker_signal final {
    enum class await_state_type : std::uint8_t {
        idle,
        linked,
        scheduled,
    };

public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        // Only a cold operation may move; a linked waiter must stay address-stable.
        wait_operation(wait_operation&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {
            if (other.started_) {
                std::terminate();
            }
        }
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (state_ != await_state_type::idle) {
                std::terminate();
            }
            release_reservation();
        }

        [[nodiscard]] bool await_ready() {
            if (signal_ == nullptr || started_) {
                throw std::logic_error("worker signal wait cannot be awaited");
            }
            started_ = true;
            try {
                signal_->require_current_worker();
            } catch (...) {
                release_reservation();
                throw;
            }
            if (!signal_->pending_) {
                return false;
            }
            signal_->pending_ = false;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation_ = value;
            next_ = signal_->waiters_;
            state_ = await_state_type::linked;
            signal_->waiters_ = this;
            return true;
        }
        void await_resume() noexcept {
            release_reservation();
        }

    private:
        explicit wait_operation(worker_signal& signal) noexcept
            : signal_(&signal) {
            ++signal.reserved_waits_;
        }
        void release_reservation() noexcept {
            if (signal_ != nullptr) {
                --signal_->reserved_waits_;
                signal_ = nullptr;
            }
        }
        worker_signal* signal_;
        wait_operation* next_{nullptr};
        std::coroutine_handle<> continuation_{};
        await_state_type state_{await_state_type::idle};
        bool started_{false};
        friend class worker_signal;
    };

private:
    using awaiter_type = wait_operation;

public:
    explicit worker_signal(const worker_ref& worker_value)
        : worker_(require_worker(worker_value)) {}
    worker_signal(worker_ref&&) = delete;
    worker_signal(const worker_ref&&) = delete;

    ~worker_signal() {
        if (waiters_ != nullptr || scheduled_waiters_ != 0 || reserved_waits_ != 0) {
            std::terminate();
        }
    }

    worker_signal(const worker_signal&) = delete;
    worker_signal& operator=(const worker_signal&) = delete;

    // A cold single-use awaiter reserves the signal until completion or destruction.
    // Creation and starting the wait both require the bound worker.
    [[nodiscard]] wait_operation wait() & {
        require_current_worker();
        return wait_operation(*this);
    }
    wait_operation wait() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Wakes every waiter, or leaves one pending wake when none waits. Like
    // wait(), it must run on the bound worker and throws std::logic_error
    // elsewhere. A failed resume dispatch after waiters were detached has no
    // recoverable owner and terminates.
    void notify();

private:
    [[nodiscard]] static const worker_ref& require_worker(const worker_ref& worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return worker_value;
    }

    void require_current_worker() const {
        if (!worker_.is_current()) {
            throw std::logic_error("worker signal operation must run on its worker");
        }
    }

    void resume_scheduled(awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept;

    // The owning session/connection keeps its stable worker_ref alive until
    // every signal waiter and scheduled resumption has joined.
    const worker_ref& worker_;
    awaiter_type* waiters_{nullptr};
    std::size_t scheduled_waiters_{0};
    std::size_t reserved_waits_{0};
    bool pending_{false};
};

inline void worker_signal::notify() {
    require_current_worker();

    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next_value = waiter->next_;
        const auto continuation = waiter->continuation_;
        waiter->next_ = nullptr;
        waiter->state_ = await_state_type::scheduled;
        ++scheduled_waiters_;
        // A detached intrusive node has no recoverable owner. Dispatch failure
        // is terminal instead of silently stranding the continuation. On the
        // worker's own thread this small fixed callable is posted directly.
        detail::worker_ref_access::defer_or_terminate(
            worker_, [this, waiter, continuation] { resume_scheduled(waiter, continuation); });
        waiter = next_value;
    }
}

inline void worker_signal::resume_scheduled(
    awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.is_current() || waiter->state_ != await_state_type::scheduled ||
        waiter->continuation_ != continuation || scheduled_waiters_ == 0) {
        std::terminate();
    }
    --scheduled_waiters_;
    waiter->continuation_ = {};
    waiter->state_ = await_state_type::idle;
    continuation.resume();
}

}  // namespace ruvia

namespace ruvia {

enum class timer_sleep_result : std::uint8_t {
    elapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- when a stop_token was passed -- whatever else owns
    // that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    stop_requested,
};

class sleep_operation;

// Suspend the current coroutine on its bound worker for `duration`, or until
// `deadline`. The result distinguishes a normal elapsed delay from
// cancellation caused by worker shutdown (stop_timers): a sleep pending at
// that point, or started after the worker's timer queue stopped, returns
// stop_requested without throwing. The duration is the sleep's target, not
// an operation bound (operation_timeout): sleep_for(d) is sleep_until(now +
// d), so a non-positive duration, like a deadline that has passed, elapses
// without suspending. Durations saturate, so an effectively unbounded delay such as
// hours::max() waits for stop instead of overflowing.
//
// The sleep is also cut short when `stop_token_value` is stopped. A
// framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token, so anything a request can await takes
// one. The stop callback may run on a thread that is not this worker, and
// cancelling a worker timer is worker-owned state, so cancellation is deferred
// onto the worker and validated there -- a stop that arrives after the sleep
// already finished is a no-op rather than a use-after-free.
//
// The worker is borrowed for the lifetime of the returned operation: the
// caller must keep its address-stable worker_ref alive until the operation
// completes or is dropped cold. This is the same borrow-only boundary the
// request hot path applies everywhere else; a temporary worker_ref would dangle in
// the cold operation.
[[nodiscard]] sleep_operation sleep_for(
    const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value = {});
sleep_operation sleep_for(worker_ref&&, timeout_duration, stop_token = {}) = delete;
sleep_operation sleep_for(const worker_ref&&, timeout_duration, stop_token = {}) = delete;
[[nodiscard]] sleep_operation sleep_until(const worker_ref& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value = {});
sleep_operation sleep_until(
    worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;
sleep_operation sleep_until(
    const worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;

// One sleep on the caller's worker: single-use, movable only before it is
// awaited, and started on the borrowed worker by co_await. An empty stop token
// makes it a plain timer wait; a stoppable one also cancels the timer when stop
// is requested.
class sleep_operation final {
public:
    sleep_operation(const sleep_operation&) = delete;
    sleep_operation& operator=(const sleep_operation&) = delete;
    sleep_operation(sleep_operation&& other) noexcept;
    sleep_operation& operator=(sleep_operation&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] timer_sleep_result await_resume() const noexcept;

private:
    friend sleep_operation sleep_for(const worker_ref&, timeout_duration, stop_token);
    friend sleep_operation sleep_until(
        const worker_ref&, std::chrono::steady_clock::time_point, stop_token);

    sleep_operation(const worker_ref& worker_value, timeout_duration duration,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(duration),
          stop_token_(std::move(stop_token_value)) {}

    sleep_operation(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(deadline),
          stop_token_(std::move(stop_token_value)) {}

    const worker_ref* worker_;
    std::variant<timeout_duration, std::chrono::steady_clock::time_point> target_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_outcome outcome_{worker_timer_outcome::expired};
    bool started_{false};
    worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

}  // namespace ruvia

// Offloading blocking work off a worker thread.
//
// A Ruvia worker is one thread driving one io_context, and every connection it
// accepted is dispatched on that thread. A handler that blocks -- hashing a
// password, calling a synchronous third-party SDK, rendering a template,
// touching a slow file -- freezes every other connection on that worker for as
// long as it blocks. Nothing about the coroutine machinery can hide that: the
// thread is simply not running the event loop any more.
//
// A blocking_pool is the escape hatch: a fixed set of ordinary threads with a
// bounded queue. run_blocking() hands the callable to that pool, suspends the
// calling coroutine, and resumes it on its own worker once the result comes
// back -- so the worker keeps serving other connections meanwhile.
//
// The callable runs on a foreign thread and outlives nothing: it must own
// everything it touches (capture by value or move), and it must not touch the
// context or any other worker-owned state. Nothing in C++
// enforces that -- the pool cannot see through a lambda's captures -- and a
// worker that stops while a task is still running does not wait for it, so a
// captured reference into a request is a use-after-free waiting to happen.
//
// A pool task must not wait on the same pool. The threads are a fixed set, so
// tasks that block until other tasks finish can occupy every one of them at
// once and deadlock -- the queue would drain only once a running task returns,
// and none can. Submitting without waiting is fine; waiting is not.

namespace ruvia {

struct blocking_pool_options final {
    // 0 selects half of std::thread::hardware_concurrency(), clamped to 2..8.
    // Blocking work benefits from some oversubscription on small machines, but
    // the default must not create one process thread per logical CPU on large
    // hosts. Set an explicit value when the workload has different needs.
    std::size_t thread_count_{0};
    // Queued tasks waiting for a free thread. 0 selects thread_count * 64.
    // The queue is bounded on purpose: an unbounded one converts an overloaded
    // pool into unbounded memory growth and unbounded latency, and hides the
    // overload from the caller that could still shed load.
    std::size_t queue_capacity_{0};
};

enum class blocking_submit_status : std::uint8_t {
    accepted,
    queue_full,
    pool_stopped,
};

// A rejected submission returns the callable it did not run.
using blocking_submit_result =
    worker_delivery_result<move_only_function<void()>, blocking_submit_status, blocking_submit_status::accepted>;

struct blocking_pool_stats final {
    // Tasks waiting for a free thread, and tasks a thread is running right now.
    std::size_t queued_{0};
    std::size_t running_{0};
    // Tasks that ran to completion, whether or not the callable threw.
    std::uint64_t completed_{0};
    // Tasks refused because no thread and no queue slot were free. This is the
    // sizing signal: a growing count means thread_count or queue_capacity is too
    // small for the offered load.
    std::uint64_t rejected_{0};
    // Tasks that never ran because the pool was stopping -- dropped from the
    // queue by stop(), or submitted after it. Shutdown accounting, kept apart
    // from `rejected` so it cannot be mistaken for overload.
    std::uint64_t discarded_{0};
};

// Fixed threads, bounded queue. Construction starts the threads. Destruction
// stops accepting work, discards queued tasks, and detaches threads that are
// already running a callable; those callables may finish after the pool object
// is gone. Call join() explicitly when the owner must wait for every thread.
// Copy and move are deleted: submitters hold a reference.
class blocking_pool final {
public:
    explicit blocking_pool(blocking_pool_options options = {});
    ~blocking_pool();

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;
    blocking_pool(blocking_pool&&) = delete;
    blocking_pool& operator=(blocking_pool&&) = delete;

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t queue_capacity() const noexcept;
    [[nodiscard]] blocking_pool_stats stats() const noexcept;

    // Safe from any thread. Admission is reserved before the callable moves
    // into the queue; a rejection (queue_full, or pool_stopped, including a
    // stop that lands during that move) returns the callable untouched in the
    // result, so the caller decides where it is destroyed. Callable moves and
    // destruction may query stats(), submit(), or stop(). Admission includes
    // submissions whose callable move has not finished.
    [[nodiscard]] blocking_submit_result submit(move_only_function<void()> task);

    // Stops accepting, discards tasks that have not started, and wakes the
    // threads. Tasks already running are not interrupted -- a blocking call
    // cannot be -- but nothing waits for their queued successors. Idempotent
    // and safe from any thread.
    void stop() noexcept;
    // Stops the pool and waits for its threads to leave their loops. Joining
    // from one of this pool's own threads would deadlock and throws logic_error
    // before changing the pool's state.
    // The destructor deliberately does not join already-running callables.
    void join();

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

enum class blocking_status : std::uint8_t {
    // The callable ran to completion. It may still have thrown: that is a
    // result, not a rejection.
    completed,
    // The pool's queue was full. The callable never ran.
    queue_full,
    // The pool was stopped before a thread picked the task up. Never ran.
    pool_stopped,
    // The worker stopped while the task was outstanding, so there is nobody
    // left to deliver a result to. The callable may or may not have run; its
    // result, if any, was discarded on the pool thread.
    worker_stopping,
    // The caller's stop token fired first. The callable may still be running;
    // its eventual result is discarded just like a timed-out result.
    cancelled,
    // The caller stopped waiting first. The callable keeps running -- a
    // blocking call cannot be interrupted -- and its result is discarded when
    // it finishes. This releases the caller's request, not the pool thread.
    timed_out,
    // The worker's event loop was given no blocking pool. The callable never ran.
    unavailable,
};

[[nodiscard]] std::string_view describe_blocking_status(blocking_status status) noexcept;

// Thrown when a blocking operation produced no result of its own: the pool
// refused it or the worker went away. A callable's own exception is rethrown
// unchanged instead -- it is the operation's result.
class blocking_operation_rejected final : public std::runtime_error {
public:
    [[nodiscard]] blocking_status status() const noexcept {
        return status_;
    }

private:
    template <typename>
    friend class blocking_result;

    explicit blocking_operation_rejected(blocking_status status);

    blocking_status status_;
};

namespace detail {

template <typename t_type>
struct blocking_payload final {
    using value_storage_type =
        std::conditional_t<std::is_void_v<t_type>, std::monostate, std::optional<t_type>>;

    blocking_status status_{blocking_status::completed};
    [[no_unique_address]] value_storage_type value_{};
    std::exception_ptr error_;
};

// Guards the one-shot half a submitted task owns. If the task is destroyed
// without running -- a stopped pool discards its queue -- the guard still
// answers the waiting coroutine instead of leaving it suspended forever.
// Answering or moving releases the completion, which is the answered marker:
// an empty completion rejects without effect. Holding nothing else keeps the
// submitted closure within move_only_function's inline storage.
template <typename t_type>
class blocking_completion_guard final {
public:
    explicit blocking_completion_guard(one_shot_completion<blocking_payload<t_type>> completion) noexcept
        : completion_(std::move(completion)) {}

    blocking_completion_guard(const blocking_completion_guard&) = delete;
    blocking_completion_guard& operator=(const blocking_completion_guard&) = delete;
    blocking_completion_guard(blocking_completion_guard&& other) noexcept = default;
    blocking_completion_guard& operator=(blocking_completion_guard&&) = delete;

    ~blocking_completion_guard() {
        blocking_payload<t_type> payload;
        payload.status_ = blocking_status::pool_stopped;
        // Nothing above this can receive a failure, and a rejected completion is
        // the normal case (the receiver may be gone), so the result is dropped.
        (void)completion_.complete(std::move(payload));
    }

    void answer(blocking_payload<t_type>&& payload_value) {
        const auto completion = std::move(completion_);
        try {
            (void)completion.complete(std::move(payload_value));
        } catch (...) {
            blocking_payload<t_type> failure;
            failure.error_ = std::current_exception();
            // An error-only payload has no result value to move, so it remains transportable
            // even when moving the callable's result was what failed above.
            (void)completion.complete(std::move(failure));
        }
    }

private:
    one_shot_completion<blocking_payload<t_type>> completion_;
};

}  // namespace detail

// A blocking operation's value, exception, or rejection before execution.
// The outcome category is fixed after construction. Moving a failed result
// preserves its failure in the source as well; it must not become a successful
// result without a value. Replace an owning optional by emplacing a new result.
// with_value(), with_error() and with_status() build the same outcomes for a
// composed offload.
template <typename t_type>
class blocking_result final {
public:
    blocking_result(const blocking_result&) = delete;
    blocking_result& operator=(const blocking_result&) = delete;
    blocking_result(blocking_result&& other) noexcept(
        std::is_nothrow_move_constructible_v<detail::blocking_payload<t_type>>)
        requires std::is_move_constructible_v<detail::blocking_payload<t_type>>
        : payload_(std::move(other.payload_)) {
        if (payload_.error_ != nullptr) {
            other.payload_.error_ = payload_.error_;
        }
    }
    blocking_result& operator=(blocking_result&&) = delete;

    [[nodiscard]] blocking_status status() const noexcept {
        return payload_.status_;
    }

    // True when execution and result delivery both completed without error.
    [[nodiscard]] bool completed() const noexcept {
        return payload_.status_ == blocking_status::completed && payload_.error_ == nullptr;
    }

    // Execution or result delivery failed; value() rethrows the exception.
    [[nodiscard]] bool failed() const noexcept {
        return payload_.error_ != nullptr;
    }

    [[nodiscard]] const std::exception_ptr& error() const& noexcept {
        return payload_.error_;
    }
    const std::exception_ptr& error() const&& = delete;

    // Rethrows the stored error, or throws blocking_operation_rejected if the
    // operation was rejected.
    decltype(auto) value() const& {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return static_cast<const t_type&>(*payload_.value_);
        }
    }

    void value() && = delete;
    void value() const&& = delete;

    decltype(auto) value() & {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return static_cast<t_type&>(*payload_.value_);
        }
    }

    t_type take_value() && {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return std::move(*payload_.value_);
        }
    }

    // The callable completed with this value.
    template <typename u_type>
        requires(!std::is_void_v<t_type> && std::constructible_from<t_type, u_type&&>)
    [[nodiscard]] static blocking_result with_value(u_type&& value) {
        blocking_result result_value(blocking_status::completed);
        result_value.payload_.value_.emplace(std::forward<u_type>(value));
        return result_value;
    }

    [[nodiscard]] static blocking_result with_value() noexcept
        requires std::is_void_v<t_type>
    {
        return blocking_result(blocking_status::completed);
    }

    // Execution or result delivery failed with this non-null exception.
    [[nodiscard]] static blocking_result with_error(std::exception_ptr error) {
        if (error == nullptr) {
            throw std::invalid_argument("blocking error result requires an exception");
        }
        blocking_result result_value(blocking_status::completed);
        result_value.payload_.error_ = std::move(error);
        return result_value;
    }

    // The operation produced no result of its own; every status but completed.
    [[nodiscard]] static blocking_result with_status(blocking_status status) {
        if (status == blocking_status::completed) {
            throw std::invalid_argument("blocking status result requires a rejection status");
        }
        return blocking_result(status);
    }

private:
    explicit blocking_result(blocking_status status) noexcept {
        payload_.status_ = status;
    }

    void rethrow_if_not_completed() const {
        if (payload_.error_ != nullptr) {
            std::rethrow_exception(payload_.error_);
        }
        if (payload_.status_ != blocking_status::completed) {
            throw blocking_operation_rejected(payload_.status_);
        }
        if constexpr (!std::is_void_v<t_type>) {
            if (!payload_.value_.has_value()) {
                std::terminate();
            }
        }
    }

    detail::blocking_payload<t_type> payload_;
};

namespace detail {

// A completed payload always holds a value or an error.
template <typename t_type>
[[nodiscard]] blocking_result<t_type> completed_blocking_result(blocking_payload<t_type>&& payload_value) {
    if (payload_value.error_ != nullptr) {
        return blocking_result<t_type>::with_error(std::move(payload_value.error_));
    }
    if constexpr (std::is_void_v<t_type>) {
        return blocking_result<t_type>::with_value();
    } else {
        return blocking_result<t_type>::with_value(std::move(*payload_value.value_));
    }
}

}  // namespace detail

// Runs `fn` on the blocking pool the worker's event loop borrows and resumes
// the caller on `worker` with a typed result. Queue pressure, a worker without
// a pool, shutdown, cancellation and timeout remain inspectable statuses; the
// callable's own exception is retained in the result.
//
// The returned task must be awaited on `worker` -- that is the thread the
// coroutine resumes on and the only thread the result is touched from;
// awaiting it elsewhere throws std::logic_error before `fn` is submitted. `fn`
// is moved into the pool and runs on a foreign thread: it must own everything
// it uses (see the file header).
//
// options.timeout_ bounds the caller, not the work: a blocking call cannot be
// interrupted, so the pool thread stays occupied until `fn` returns. Use it to
// keep one wedged dependency from pinning a request -- its connection, its
// arena, its leases -- indefinitely. A relative timeout starts when the task
// starts; an absolute one shares a deadline with the caller's other phases.
// options.stop_token_ cancels the wait the same way.
//
// Never throws for a rejection; the status says what happened. A worker
// without a pool, a stop token that is already stopped, or a deadline that has
// passed rejects without submitting `fn`. Use std::move(result).take_value()
// to turn a rejection into an exception and to rethrow the callable's own
// exception.
template <typename fn_type>
[[nodiscard]] auto run_blocking(worker_ref worker_value, operation_options options, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    using result_type = std::invoke_result_t<fn_type&>;
    using payload = detail::blocking_payload<result_type>;
    using outcome_type = blocking_result<result_type>;
    const operation_deadline deadline(options.timeout_);
    auto stop_token_value = std::move(options.stop_token_);
    static_assert(std::is_void_v<result_type> || std::is_move_constructible_v<result_type>,
        "a blocking callable's result travels back to the worker by move");

    // Every outcome the wait would decide without the callable is decided
    // first, so a misplaced, cancelled, or expired call never occupies a pool
    // thread or queue slot for a result nobody receives.
    if (!worker_value.is_current()) {
        throw std::logic_error("blocking work must be awaited on its bound worker");
    }
    auto* const pool = worker_value.blocking_pool();
    if (pool == nullptr) {
        co_return outcome_type::with_status(blocking_status::unavailable);
    }
    if (stop_token_value.stop_requested()) {
        co_return outcome_type::with_status(blocking_status::cancelled);
    }
    if (deadline.expired()) {
        co_return outcome_type::with_status(blocking_status::timed_out);
    }
    if (!worker_value.accepting()) {
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    }

    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::optional<std::pair<one_shot_completion<payload>, one_shot_receiver<payload>>> channel;
    try {
        channel.emplace(make_one_shot<payload>(std::move(worker_value)));
    } catch (const std::runtime_error&) {
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    }
    auto& [completion, receiver] = *channel;
    const auto submitted =
        pool->submit([guard = detail::blocking_completion_guard<result_type>(std::move(completion)),
                         call = std::move(fn)]() mutable {
            payload payload_value;
            try {
                if constexpr (std::is_void_v<result_type>) {
                    call();
                } else {
                    payload_value.value_.emplace(call());
                }
            } catch (...) {
                payload_value.error_ = std::current_exception();
            }
            guard.answer(std::move(payload_value));
        });
    if (!submitted.accepted()) {
        // The returned closure is destroyed here; its guard answers the
        // one-shot with pool_stopped, which nobody awaits any more.
        co_return outcome_type::with_status(submitted.status() == blocking_submit_status::queue_full
                                                ? blocking_status::queue_full
                                                : blocking_status::pool_stopped);
    }

    try {
        auto waited = co_await receiver.wait({.timeout_ = deadline.timeout(), .stop_token_ = std::move(stop_token_value)});
        if (waited.has_value()) {
            auto& payload_value = waited.value();
            if (payload_value.status_ != blocking_status::completed) {
                co_return outcome_type::with_status(payload_value.status_);
            }
            co_return detail::completed_blocking_result(std::move(payload_value));
        }
        if (waited.status() == worker_wait_status::timed_out) {
            co_return outcome_type::with_status(blocking_status::timed_out);
        }
        if (waited.status() == worker_wait_status::cancelled) {
            co_return outcome_type::with_status(blocking_status::cancelled);
        }
        // Closed cannot happen -- this coroutine owns the receiver and is the only
        // waiter -- so anything else is the worker going away under the operation.
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    } catch (...) {
        // Payload delivery now moves the result on the receiving worker.
        co_return outcome_type::with_error(std::current_exception());
    }
}

template <typename fn_type>
[[nodiscard]] auto run_blocking(worker_ref worker_value, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return run_blocking(std::move(worker_value), operation_options{}, std::move(fn));
}

}  // namespace ruvia
