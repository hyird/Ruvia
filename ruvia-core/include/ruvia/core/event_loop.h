#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/disposition.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

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

namespace ruvia {

// The completion argument of async_start(). Void tasks complete with
// std::exception_ptr, which satisfies Asio's disposition concept directly;
// value-returning tasks complete with the value first and the failure second,
// passed by rvalue reference so over-aligned results are never passed by value
// on MSVC.
template <typename t_type>
    requires detail::asio_task_result<t_type>
using task_completion_result = std::conditional_t<std::is_void_v<t_type>, std::exception_ptr,
    std::variant<t_type, std::exception_ptr>>;

}  // namespace ruvia

namespace ruvia::detail {

// Owns the started task and the completion handler, allocated with the
// handler's associated allocator, until delivery on its associated executor.
template <typename t_type, typename handler_type>
class task_completion_state final {
public:
    using handler_allocator_type = asio::associated_allocator_t<handler_type>;
    using state_allocator_type = typename std::allocator_traits<handler_allocator_type>::template rebind_alloc<
        task_completion_state>;
    using state_allocator_traits_type = std::allocator_traits<state_allocator_type>;

    static task_completion_state* create(task<t_type>&& task_value, handler_type&& handler_value) {
        handler_allocator_type handler_allocator(asio::get_associated_allocator(handler_value));
        state_allocator_type state_allocator(handler_allocator);
        auto* state_value = state_allocator_traits_type::allocate(state_allocator, 1);
        try {
            state_allocator_traits_type::construct(state_allocator, state_value, std::move(task_value),
                std::forward<handler_type>(handler_value), std::move(handler_allocator));
        } catch (...) {
            state_allocator_traits_type::deallocate(state_allocator, state_value, 1);
            throw;
        }
        return state_value;
    }

    task_completion_state(
        task<t_type>&& task_value, handler_type&& handler_value, handler_allocator_type allocator_value)
        : task_(std::move(task_value)),
          handler_(std::forward<handler_type>(handler_value)),
          allocator_(std::move(allocator_value)) {}

    void start() {
        task_.coroutine_.promise().control_.set_completion(this, &task_completion_state::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state_value = static_cast<task_completion_state*>(raw);
        auto executor = asio::get_associated_executor(state_value->handler_);
        auto completion_allocator = state_value->allocator_;
        state_owner_type owner_value(state_value, state_deleter_type{state_allocator_type(state_value->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completion_allocator),
                                 [owner_value = std::move(owner_value)]() mutable {
                                     deliver(std::move(owner_value));
                                 }));
    }

private:
    struct state_deleter_type final {
        void operator()(task_completion_state* state_value) noexcept {
            state_allocator_traits_type::destroy(allocator_, state_value);
            state_allocator_traits_type::deallocate(allocator_, state_value, 1);
        }

        state_allocator_type allocator_;
    };

    using state_owner_type = std::unique_ptr<task_completion_state, state_deleter_type>;

    static void deliver(state_owner_type owner_value) {
        // The delivery owner outlives the result and every callback argument.
        // An owning completion handler can publish external readiness in its
        // destructor only after moved-from result storage has been reclaimed.
        auto handler = std::move(owner_value->handler_);
        owner_value->task_.prepare_direct_completion();
        auto result_value = [&owner_value]() -> task_completion_result<t_type> {
            try {
                if constexpr (std::is_void_v<t_type>) {
                    owner_value->task_.coroutine_.promise().result();
                    return {};
                } else {
                    return task_completion_result<t_type>(
                        std::in_place_index<0>, owner_value->task_.coroutine_.promise().result());
                }
            } catch (...) {
                if constexpr (std::is_void_v<t_type>) {
                    return std::current_exception();
                } else {
                    return task_completion_result<t_type>(std::in_place_index<1>, std::current_exception());
                }
            }
        }();
        owner_value->task_.finish_direct_completion();
        owner_value.reset();
        std::move(handler)(std::move(result_value));
    }

    task<t_type> task_;
    handler_type handler_;
    handler_allocator_type allocator_;
};

template <typename t_type>
    requires asio_task_result<t_type> && (!std::is_void_v<t_type>)
[[nodiscard]] asio::awaitable<t_type> await_non_void_task(task<t_type> task_value) {
    auto result_value = co_await async_start(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    co_return std::move(std::get<0>(result_value));
}

}  // namespace ruvia::detail

namespace ruvia {

// Starts a lazy task as an Asio asynchronous operation. The task starts
// inline on the calling thread; its completion is delivered through the
// handler's associated executor with a task_completion_result<T>, after the
// coroutine frame has been reclaimed. The operation state is allocated with
// the handler's associated allocator. Any completion token works: a callback,
// use_awaitable, use_future, deferred, or a custom token.
template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type>&& task_value, completion_token_type&& token) {
    task_value.prepare_direct_start();
    using completion_signature_type =
        std::conditional_t<std::is_void_v<t_type>, void(std::exception_ptr), void(task_completion_result<t_type>&&)>;
    return asio::async_initiate<completion_token_type, completion_signature_type>(
        [](auto&& handler, task<t_type> started_task) {
            using handler_type = std::decay_t<decltype(handler)>;
            auto* state_value = detail::task_completion_state<t_type, handler_type>::create(
                std::move(started_task), std::forward<decltype(handler)>(handler));
            state_value->start();
        },
        token, std::move(task_value));
}

// Adapts a lazy task for an Asio coroutine owner such as co_spawn. A failed
// task rethrows its exception from the awaitable. Void tasks use Asio's
// std::exception_ptr disposition, so use_awaitable produces the awaitable<void>
// directly without a wrapper coroutine frame.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    if constexpr (std::is_void_v<t_type>) {
        return async_start(std::move(task_value), asio::use_awaitable);
    } else {
        return detail::await_non_void_task(std::move(task_value));
    }
}

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields. Void completions carry
// only the error code: the result accessors and the two-argument factory exist
// only for a non-void result_type.
template <typename result_type>
class asio_completion final {
    using result_storage_type = std::conditional_t<std::is_void_v<result_type>, std::monostate, result_type>;

public:
    asio_completion(const asio_completion&) = default;
    asio_completion(asio_completion&&) = default;
    asio_completion& operator=(const asio_completion&) = delete;
    asio_completion& operator=(asio_completion&&) = delete;

    [[nodiscard]] static asio_completion completed(std::error_code error_code) noexcept
        requires std::is_void_v<result_type>
    {
        return asio_completion(error_code, result_storage_type{});
    }

    [[nodiscard]] static asio_completion completed(std::error_code error_code, result_storage_type result_value)
        requires(!std::is_void_v<result_type>)
    {
        return asio_completion(error_code, std::move(result_value));
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return !error_code_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] const result_storage_type& result() const& noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    const result_storage_type& result() const&& = delete;

    [[nodiscard]] result_storage_type& result() & noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    result_storage_type& result() && = delete;

    [[nodiscard]] result_storage_type take_result() &&
        requires(!std::is_void_v<result_type>)
    {
        return std::move(result_);
    }

private:
    asio_completion(std::error_code error_code, result_storage_type&& result_value) noexcept(
        std::is_nothrow_move_constructible_v<result_storage_type>)
        : error_code_(error_code),
          result_(std::move(result_value)) {}

    std::error_code error_code_;
    [[no_unique_address]] result_storage_type result_;
};

// The single-use awaiter of one completion-based Asio operation, as returned
// by async_asio(). It owns the initiation and the completion state until the
// operation finishes, and is neither copyable, movable, nor addressable; an
// owner that keeps it beyond one await-expression constructs it in place
// (for example std::optional::emplace) from the initiation.
template <typename result_type, typename initiate_type>
class asio_completion_awaiter final {
public:
    explicit asio_completion_awaiter(initiate_type initiate)
        : initiate_(std::move(initiate)) {}
    asio_completion_awaiter(const asio_completion_awaiter&) = delete;
    asio_completion_awaiter& operator=(const asio_completion_awaiter&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // suspend_race_state records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        if constexpr (std::is_void_v<result_type>) {
            initiate_([this, continuation](std::error_code ec, auto&&...) mutable {
                if (state_.complete(asio_completion<void>::completed(ec))) {
                    continuation.resume();
                }
            });
        } else {
            initiate_([this, continuation](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>::completed(ec, std::move(result_value)))) {
                    continuation.resume();
                }
            });
        }
        return state_.suspend(continuation);
    }

    [[nodiscard]] asio_completion<result_type> await_resume() {
        return state_.take_value();
    }

private:
    initiate_type initiate_;
    detail::suspend_race_state<asio_completion<result_type>> state_;
};

// Adapts a completion-based Asio operation for a Ruvia coroutine. The
// initiation receives one handler taking (std::error_code[, result_type]).
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(std::forward<initiate_type>(initiate));
}

}  // namespace ruvia

namespace ruvia {

class event_loop;

namespace detail {

struct event_loop_state;

// Routes an unobserved root-task failure through the loop's own failure sink,
// borrowed from the live loop state instead of copied into every task. A
// retired loop falls back to the unhandled-failure diagnostic.
void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept;

class root_task_completion final {
public:
    root_task_completion(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : worker_(std::move(worker_value)),
          loop_(std::move(loop_value)) {}

    root_task_completion(const root_task_completion&) = delete;
    root_task_completion& operator=(const root_task_completion&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        const std::lock_guard lock(mutex_);
        return owner_alive_;
    }

    [[nodiscard]] bool ready() const noexcept {
        const std::lock_guard lock(mutex_);
        return complete_;
    }

    void wait() const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return complete_; });
    }

    [[nodiscard]] bool wait_until(std::chrono::steady_clock::time_point deadline) const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        return completed_.wait_until(lock, deadline, [this] { return complete_; });
    }

    void abandon() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (!owner_alive_) {
                return;
            }
            owner_alive_ = false;
            if (complete_) {
                unobserved = failure_;
            }
        }
        report(std::move(unobserved));
    }

    void stage_failure(std::exception_ptr failure) noexcept {
        const std::lock_guard lock(mutex_);
        if (complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
    }

    // Publish only after the delivery owner has destroyed all intermediate
    // result objects and reclaimed their moved-from storage.
    void publish_completion() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            complete_ = true;
            if (!owner_alive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

    void complete_failure(std::exception_ptr failure) noexcept {
        stage_failure(std::move(failure));
        publish_completion();
    }

    [[nodiscard]] std::exception_ptr consume_failure() {
        wait();
        std::lock_guard lock(mutex_);
        if (!owner_alive_) {
            throw std::logic_error("root task result was already consumed");
        }
        owner_alive_ = false;
        return failure_;
    }

private:
    void require_foreign_thread() const {
        if (worker_.is_current()) {
            throw std::logic_error("cannot wait for a root task on its event loop");
        }
    }

    void report(std::exception_ptr failure) const noexcept {
        if (!failure) {
            return;
        }
        report_root_task_failure(loop_, std::move(failure));
    }

    worker_ref worker_;
    std::weak_ptr<event_loop_state> loop_;
    mutable std::mutex mutex_;
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool owner_alive_{true};
};

struct root_task_void_result final {};

template <typename t_type>
class root_task_state final {
public:
    root_task_state(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : completion_(std::move(worker_value), std::move(loop_value)) {}

    // The type-independent completion protocol; only the result is typed here.
    [[nodiscard]] root_task_completion& completion() noexcept {
        return completion_;
    }
    [[nodiscard]] const root_task_completion& completion() const noexcept {
        return completion_;
    }

    template <typename value_type>
        requires std::same_as<std::remove_cvref_t<value_type>, std::remove_cv_t<t_type>>
    void stage_value(value_type&& value) {
        value_.emplace(std::forward<value_type>(value));
    }

    void stage_value() noexcept
        requires std::is_void_v<t_type>
    {}

    t_type get() {
        auto failure = completion_.consume_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if constexpr (!std::is_void_v<t_type>) {
            struct retire_value final {
                std::optional<t_type>& value_;
                ~retire_value() {
                    value_.reset();
                }
            } retire{value_};
            return std::move(*value_);
        }
    }

private:
    root_task_completion completion_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<t_type>, root_task_void_result, std::optional<t_type>> value_;
};

}  // namespace detail

template <typename t_type>
class [[nodiscard]] root_task final {
public:
    root_task(const root_task&) = delete;
    root_task& operator=(const root_task&) = delete;

    root_task(root_task&& other) noexcept
        : state_(std::move(other.state_)) {}

    root_task& operator=(root_task&& other) noexcept {
        if (this != &other) {
            // The old result may own or replace the source during retirement.
            root_task incoming(std::move(other));
            state_.swap(incoming.state_);
        }
        return *this;
    }

    ~root_task() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->completion().valid();
    }

    // True once the task has completed and its result has not been consumed.
    [[nodiscard]] bool ready() const noexcept {
        return state_ && state_->completion().valid() && state_->completion().ready();
    }

    // Blocks a thread other than the loop's own until completion.
    void wait() const {
        require_state().completion().wait();
    }

    // Bounded wait(): returns whether the task completed in time. A relative
    // bound starts now; an absolute deadline that has passed only reports the
    // current state.
    [[nodiscard]] bool wait_for(operation_timeout timeout) const {
        return require_state().completion().wait_until(timeout.deadline_from(std::chrono::steady_clock::now()));
    }

    // Consumes the result, so only an rvalue extracts it:
    // std::move(root).get(). User result moves run without the state mutex
    // and may re-enter this root_task. The object must remain alive for the
    // entire synchronous call; replacing its owned state during a move is
    // supported.
    decltype(auto) get() && {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state_value = state_;
        struct retire_state final {
            std::shared_ptr<detail::root_task_state<t_type>>& state_;
            const std::shared_ptr<detail::root_task_state<t_type>>& expected_;
            ~retire_state() {
                if (state_ == expected_ && !expected_->completion().valid()) {
                    state_.reset();
                }
            }
        } retire{state_, state_value};
        if constexpr (std::is_void_v<t_type>) {
            state_value->get();
            return;
        } else {
            return state_value->get();
        }
    }
    void get() & = delete;

private:
    explicit root_task(std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : state_(std::move(state_value)) {}

    [[nodiscard]] const detail::root_task_state<t_type>& require_state() const {
        if (!state_) {
            throw std::logic_error("root task has no state");
        }
        return *state_;
    }

    void reset() noexcept {
        if (auto state_value = std::exchange(state_, nullptr)) {
            state_value->completion().abandon();
        }
    }

    std::shared_ptr<detail::root_task_state<t_type>> state_;
    friend class event_loop;
};

}  // namespace ruvia

namespace ruvia {

namespace detail {
struct event_loop_state;
class event_loop_stop_listener;
template <typename t_type>
struct root_task_launch_guard;
}  // namespace detail

// Thrown by event_loop::start() when the loop refuses a task: status()
// distinguishes a full admission queue from a stopping or retired loop. Also
// the failure of an admitted root task whose launch was discarded because
// the loop stopped before running it.
class event_loop_start_rejected final : public std::runtime_error {
public:
    [[nodiscard]] post_status status() const noexcept {
        return status_;
    }

private:
    friend class event_loop;
    template <typename>
    friend struct detail::root_task_launch_guard;

    explicit event_loop_start_rejected(post_status status);

    post_status status_;
};

// The outcome of event_loop::try_start(): accepted owns the started
// root_task (value()/take_value()); a rejection owns the unconsumed cold task
// (rejected()/take_rejected()) with its post_status.
template <typename t_type>
using event_loop_start_result =
    worker_delivery_result<task<t_type>, post_status, post_status::accepted, root_task<t_type>>;

namespace detail {

// drain() destroys remaining queue closures without invoking them when
// a posted task throws. Own that destructor path so wait()/get() cannot
// hang and an unobserved failure still reaches the loop sink.
template <typename t_type>
struct root_task_launch_guard final {
    std::optional<task<t_type>> task_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;

    root_task_launch_guard(task<t_type> task_value, std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : task_(std::move(task_value)),
          completion_(std::move(state_value)) {}
    root_task_launch_guard(const root_task_launch_guard&) = delete;
    root_task_launch_guard& operator=(const root_task_launch_guard&) = delete;
    root_task_launch_guard(root_task_launch_guard&& other) noexcept
        : task_(std::move(other.task_)),
          completion_(std::move(other.completion_)) {}
    root_task_launch_guard& operator=(root_task_launch_guard&&) = delete;
    ~root_task_launch_guard() {
        // A rejected or discarded launch still owns a cold frame.
        // Retire it before waking a caller that may reclaim its inputs.
        task_.reset();
        if (completion_ != nullptr) {
            completion_->completion().complete_failure(
                std::make_exception_ptr(event_loop_start_rejected(post_status::worker_stopping)));
        }
    }
    void release() noexcept {
        task_.reset();
        completion_.reset();
    }
};
template <typename t_type>
struct root_task_completion_handler final {
    using executor_type = asio::io_context::executor_type;

    executor_type executor_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;
    std::shared_ptr<void> retirement_lease_;
    bool delivered_{false};

    root_task_completion_handler(executor_type executor_value, std::shared_ptr<detail::root_task_state<t_type>> state_value,
        std::shared_ptr<void> lease_value) noexcept
        : executor_(std::move(executor_value)),
          completion_(std::move(state_value)),
          retirement_lease_(std::move(lease_value)) {}
    root_task_completion_handler(const root_task_completion_handler&) = delete;
    root_task_completion_handler& operator=(const root_task_completion_handler&) = delete;
    root_task_completion_handler(root_task_completion_handler&& other) noexcept
        : executor_(std::move(other.executor_)),
          completion_(std::move(other.completion_)),
          retirement_lease_(std::move(other.retirement_lease_)),
          delivered_(std::exchange(other.delivered_, false)) {}
    root_task_completion_handler& operator=(root_task_completion_handler&&) = delete;
    ~root_task_completion_handler() {
        if (delivered_) {
            completion_->completion().publish_completion();
        }
    }
    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }
    void operator()(task_completion_result<t_type>&& result_value) noexcept {
        try {
            if constexpr (std::is_void_v<t_type>) {
                if (result_value != nullptr) {
                    completion_->completion().stage_failure(std::move(result_value));
                } else {
                    completion_->stage_value();
                }
            } else if (result_value.index() == 1) {
                completion_->completion().stage_failure(std::get<1>(result_value));
            } else {
                completion_->stage_value(std::move(std::get<0>(result_value)));
            }
        } catch (...) {
            completion_->completion().stage_failure(std::current_exception());
        }
        delivered_ = true;
    }
};

}  // namespace detail

class event_loop_stop_registration final {
public:
    event_loop_stop_registration() noexcept = default;
    ~event_loop_stop_registration();

    event_loop_stop_registration(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration& operator=(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration(event_loop_stop_registration&&) noexcept = default;
    event_loop_stop_registration& operator=(event_loop_stop_registration&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;

private:
    explicit event_loop_stop_registration(
        std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept;

    std::shared_ptr<detail::event_loop_stop_listener> listener_;
    friend class event_loop;
};

// A stable handle to one Ruvia runtime and its Asio execution context. event_loop
// does not own a thread: event_loop_pool or event_loop_attachment owns and drives
// the runtime that produced it.
class event_loop final {
public:
    event_loop() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;

    // A retired pool loop or attachment is invalid; accessing its context or
    // obtaining a fresh executor then throws std::logic_error. Previously
    // obtained executors must not outlive the loop's physical context storage.
    [[nodiscard]] asio::io_context& io_context() const&;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    // The loop's worker endpoint: admission state, identity, posting, timers,
    // and the completion lane. Rvalue loops return a copy: the worker_ref must not
    // outlive its loop owner.
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    [[nodiscard]] worker_ref worker() const&& noexcept;

    // Reports a terminal runtime failure without throwing on the caller's
    // thread. Pool owners retain the first failure for join(); attachments
    // route it to their configured failure callback or the unhandled-failure
    // diagnostic, then retire without throwing from the host's run().
    void report_failure(std::exception_ptr failure) const noexcept;

    // Loop-level cold-path cleanup of an already owned resource; it belongs
    // to no admitted operation and is not new business work. Bypasses bounded
    // admission but takes retirement protection, so it can still return false
    // after retirement has begun. Accepted callbacks and their owned inputs
    // retire before the loop can release its execution context. Contrast
    // worker_ref::defer_completion(), the completion, cancellation, and
    // resume lane of an operation the worker already admitted.
    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] bool defer_cleanup(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.defer_cleanup_task(move_only_function<void()>(std::forward<fn_type>(fn)));
    }

    // Starts one lazy task on this loop. Accepted: the loop owns the started
    // coroutine until completion and the result holds its structured
    // completion owner; abandoning that root_task never destroys a suspended
    // frame, and an abandoned failure is routed to the loop failure sink.
    // Rejected: a full admission queue (queue_full) or a stopping or retired
    // loop (worker_stopping) returns the unconsumed cold task. Admission is
    // reserved before the task moves, so a rejection never consumes it. Throws
    // only for allocation failure.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] event_loop_start_result<t_type> try_start(task<t_type> task_value) const {
        using result_type = event_loop_start_result<t_type>;
        auto retirement_lease = acquire_root_lease();
        if (!retirement_lease) {
            return detail::worker_delivery_access::reject<result_type>(post_status::worker_stopping, std::move(task_value));
        }
        auto completion = std::make_shared<detail::root_task_state<t_type>>(worker(), state_);
        const auto bound_executor = executor();
        const auto status = worker().post_factory([&]() -> move_only_function<void()> {
            return [bound_executor, retirement_lease = std::move(retirement_lease),
                       launch = detail::root_task_launch_guard<t_type>(std::move(task_value), completion)]() mutable {
                try {
                    async_start(std::move(*launch.task_),
                        detail::root_task_completion_handler<t_type>(bound_executor, launch.completion_, std::move(retirement_lease)));
                    launch.release();
                } catch (...) {
                    auto failed_completion = std::move(launch.completion_);
                    launch.release();
                    failed_completion->completion().complete_failure(std::current_exception());
                }
            };
        });
        if (status != post_status::accepted) {
            return detail::worker_delivery_access::reject<result_type>(status, std::move(task_value));
        }
        return detail::worker_delivery_access::accept<result_type>(root_task<t_type>(std::move(completion)));
    }

    // The local strict form of try_start(): a rejection destroys the cold task
    // and throws event_loop_start_rejected carrying its status.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] root_task<t_type> start(task<t_type> task_value) const {
        auto started = try_start(std::move(task_value));
        if (!started.accepted()) {
            throw event_loop_start_rejected(started.status());
        }
        return std::move(started).take_value();
    }

    // Stop cleanup is asynchronous and joined before runtime retirement. The
    // callable owner remains address-stable until its returned task and
    // completion handler have both finished. Resetting, replacing or destroying
    // the registration only unregisters cleanup that has not started.
    // Cancelling an unstarted callback moves and destroys its target outside
    // the listener lock; those operations may request loop stop.
    template <typename fn_type>
        requires std::invocable<fn_type&> &&
                 std::same_as<std::invoke_result_t<fn_type&>, task<void>> &&
                 std::move_constructible<std::decay_t<fn_type>>
    [[nodiscard]] event_loop_stop_registration on_stop(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.register_stop_callback(
            move_only_function<task<void>()>(std::forward<fn_type>(fn)));
    }

private:
    explicit event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept;
    [[nodiscard]] event_loop_stop_registration register_stop_callback(
        move_only_function<task<void>()> callback) const;
    [[nodiscard]] std::shared_ptr<void> acquire_root_lease() const;
    [[nodiscard]] bool defer_cleanup_task(move_only_function<void()> task) const;

    std::shared_ptr<detail::event_loop_state> state_;
    friend class event_loop_pool;
    friend class event_loop_attachment;
    friend class worker_runtime;
};

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
    // Borrowed pool for run_blocking() and client host lookups on this loop;
    // it must outlive the attachment. Null leaves the loop without one.
    blocking_pool* blocking_pool_{nullptr};
    // Owns the receiver for each reported failure (including abandoned root
    // tasks and failed stop callbacks). Empty uses report_unhandled_failure.
    // Invoked synchronously, outside runtime locks, on the reporting thread:
    // normally the context thread, but report_failure() or abandoning an
    // already-completed root may report from another thread. Calls may overlap
    // or reenter; the receiver must synchronize any shared mutable state.
    // Requests attachment stop before invocation. The receiver may destroy
    // the attachment or loop handles; its target stays alive through invocation
    // and is retained until the last loop-state owner releases it. Borrowed
    // captures must remain alive that long; do not capture an owning loop.
    // Receiver exceptions go to report_unhandled_failure, never escape run(),
    // and do not interrupt retirement or recursively invoke this receiver.
    move_only_function<void(std::exception_ptr)> failure_{};
};

// Binds a worker to an io_context the caller owns. The caller drives that
// context on exactly one thread, either directly with io_context::run() or
// with event_loop_attachment::run(), which also installs Ruvia's worker
// identity. The attachment never calls io_context::stop() and never restarts
// or joins a context it does not own.
//
// Retirement runs the same stop/finalize sequence as an event_loop_pool loop.
// stop(), or destroying the attachment, closes admission at once, from any
// thread, including while another thread is inside run(). Stop callbacks,
// root tasks, and deferred cleanup then drain, timers are cancelled, and the
// worker retires on the context's thread. Until then the attachment holds
// work on the context, so the caller keeps running it: a plain run() returns
// once retirement and the caller's own work are done. valid() turns false
// when retirement completes, for callers whose own work keeps run() busy.
//
// Destroying the io_context before retirement completes is valid only while
// no root task, stop callback, or deferred cleanup is outstanding; the
// context's shutdown then retires the worker synchronously. Destroying it
// with such work outstanding violates this contract: the shutdown reports a
// std::logic_error to the unhandled-failure diagnostic and terminates,
// because that work is owned by handlers that will never run.
//
// After retirement, event_loop handles become invalid; io_context() and
// executor() throw std::logic_error. The external io_context remains owned
// by its caller.
class event_loop_attachment final {
public:
    ~event_loop_attachment();

    event_loop_attachment(const event_loop_attachment&) = delete;
    event_loop_attachment& operator=(const event_loop_attachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    event_loop_attachment(event_loop_attachment&& other) noexcept;
    event_loop_attachment& operator=(event_loop_attachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event_loop loop() const noexcept;
    // Runs the attached context on the current thread with Ruvia's worker
    // identity installed. A callback may destroy the attachment during run();
    // the caller-owned context must remain alive until this call returns.
    void run();
    void stop() noexcept;

private:
    explicit event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept;

    std::shared_ptr<detail::event_loop_state> state_;
    friend event_loop_attachment attach_event_loop(asio::io_context&, event_loop_attachment_options);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller retains
// ownership of the context's unrelated work and stop/restart policy; stop()
// retires only Ruvia-managed work and never cancels unrelated operations.
[[nodiscard]] event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options = {});

}  // namespace ruvia

namespace ruvia {

class blocking_pool;

struct event_loop_pool_options final {
    std::size_t loop_count_{0};
    std::size_t queue_capacity_{1024};
    // One borrowed pool shared by every loop for run_blocking() and client
    // host lookups; it must outlive the pool. Null leaves the loops without one.
    blocking_pool* blocking_pool_{nullptr};
};

class event_loop_pool final {
public:
    explicit event_loop_pool(event_loop_pool_options options = {});
    ~event_loop_pool();

    event_loop_pool(const event_loop_pool&) = delete;
    event_loop_pool& operator=(const event_loop_pool&) = delete;
    event_loop_pool(event_loop_pool&&) = delete;
    event_loop_pool& operator=(event_loop_pool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish its managed cleanup.
    // If join() happens before start(), it creates short-lived owner threads to
    // drain accepted work and run stop callbacks. Loop handles become invalid
    // once joined. Calling join() from a pool worker throws logic_error before
    // stopping the pool.
    void join();

    [[nodiscard]] std::size_t loop_count() const noexcept;
    [[nodiscard]] event_loop loop(std::size_t index) const;
    [[nodiscard]] event_loop next_loop() noexcept;
    [[nodiscard]] event_loop loop_for(std::uint64_t key) const noexcept;
    [[nodiscard]] event_loop loop_for(std::string_view key) const noexcept;

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

}  // namespace ruvia

namespace ruvia {

class event_loop;
class worker_runtime;

struct worker_notification_options final {
    // Bounds the Windows native wake-pipe connection wait (at most one minute).
    // Synchronous setup system calls are not hard-real-time bounded.
    timeout_duration startup_timeout_{std::chrono::seconds(5)};
};

// coalesced means an existing pending notification covers this call;
// notifications are latched, not counted.
enum class worker_notification_status : std::uint8_t {
    notified,
    coalesced,
    closed,
};

enum class worker_notification_wait_status : std::uint8_t {
    notified,
    closed,
};

namespace detail {
class worker_notification_state;
}  // namespace detail

// A startup-created, address-stable, cross-thread latched wake channel for one
// worker runtime. The native wake pair is created at construction; the receiver
// Asio handle is adopted on the worker's first wait. The notification borrows
// its worker runtime or event loop, which must outlive it. Before destruction, every
// notify() call must have returned and no future call may begin. close()
// rejects new notifications but does not join producers. The owner worker must
// explicitly close the notification and join every suspended wait before its
// runtime stops. A cold, never-awaited instance can be destroyed directly.
class worker_notification final {
public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] worker_notification_wait_status await_resume();

    private:
        explicit wait_operation(worker_notification& owner_value) noexcept;

        worker_notification* owner_;
        friend class worker_notification;
    };

    explicit worker_notification(
        worker_runtime& runtime, worker_notification_options options = {});
    // Borrows the loop's worker; the loop must remain address-stable and alive.
    explicit worker_notification(
        const event_loop& loop, worker_notification_options options = {});
    worker_notification(event_loop&&, worker_notification_options = {}) = delete;
    worker_notification(const event_loop&&, worker_notification_options = {}) = delete;
    ~worker_notification();

    worker_notification(const worker_notification&) = delete;
    worker_notification& operator=(const worker_notification&) = delete;
    worker_notification(worker_notification&&) = delete;
    worker_notification& operator=(worker_notification&&) = delete;

    // Safe from any thread. The method updates its atomic latch and performs a
    // nonblocking native write; an existing latch or proven full channel coalesces.
    [[nodiscard]] worker_notification_status notify() noexcept;

    // Returns the one stable awaiter; waiting is worker-affine and permits one
    // outstanding wait. Constructing or discarding a cold task around wait()
    // does not register an operation. A notification racing with registration
    // may resume asynchronously.
    [[nodiscard]] wait_operation& wait() & noexcept;
    wait_operation& wait() && = delete;

    // Worker-affine and nonblocking. A pending wait completes as closed after
    // Asio delivers and drains its cancellation completion.
    void close();

private:
    using state_pointer = std::unique_ptr<detail::worker_notification_state>;

    explicit worker_notification(state_pointer state) noexcept;

    state_pointer state_;
    wait_operation wait_awaiter_;
};

}  // namespace ruvia

namespace ruvia {
namespace detail {

template <typename t_type>
inline constexpr bool is_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept temporary_owning_char_string =
    is_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

template <typename range_type>
concept temporary_owning_byte_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, std::byte>;

}  // namespace detail

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <detail::temporary_owning_char_string string_type>
std::span<const std::byte> as_bytes(string_type&&) = delete;

[[nodiscard]] inline std::string_view as_chars(std::span<const std::byte> bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <detail::temporary_owning_byte_range range_type>
std::string_view as_chars(range_type&&) = delete;

}  // namespace ruvia

namespace ruvia {

// UTF-8 text describing an error code, for every Ruvia message that embeds one.
// On Windows, message() of std::system_category() and asio::system_category()
// is FormatMessageA output in the process ANSI code page (for example GBK), which
// would corrupt UTF-8 responses, JSON and logs; codes of those categories are
// therefore formatted with FormatMessageW and converted to UTF-8, or described
// as "<category>:<value>" when the system has no message. Every other category,
// and every category on POSIX, uses message() unchanged.
[[nodiscard]] std::string error_code_text(const std::error_code& error);

// what() of a std::system_error with its error-code text produced by
// error_code_text. The supported standard libraries build what() as
// "<what_arg>: <code().message()>"; when what() does not end with that
// message, only error_code_text(code()) is returned.
[[nodiscard]] std::string system_error_text(const std::system_error& error);
// The last resort for a failure that has nowhere else to go: a detached task or
// a callback threw, no application sink was configured to receive it, and no
// caller will ever rethrow it. Writing one line to stderr is the only remaining
// way to keep it observable.
//
// Discarding it instead would make the fault undiagnosable, and terminating
// would turn one local failure into a whole-process one. Every layer routes its
// unowned failures here so a Ruvia process never fails silently, and so the
// diagnostic looks the same wherever it came from:
//
//     ruvia: <context> failed: <what>
//
// `context` names the failing unit ("web connection", "event loop stop callback").
// Writing is best effort -- stderr may be closed, full, or redirected -- but
// nothing beyond this point could report the failure either way.
//
// A lifecycle owner that rethrows one failure among several (application::run)
// reports only the ones it does not rethrow; the caller owns the rethrown one.
//
// Rate limited: a fault that fails every connection at once would otherwise
// bury the first, informative failure under its consequences, and a blocked
// stderr would slow the recovery it is describing. Excess reports are counted
// and the count travels with the next line that gets through, so the volume is
// bounded without the flood becoming invisible. An application that wants every
// failure should install its layer's sink instead of relying on this.
//
// `occurrences` > 1 reports that many equivalent failures (see
// equivalent_failures) as one line, so one root cause shared by several units
// is not repeated once per unit.
//
// A std::system_error is described by system_error_text,
// so the line stays UTF-8 on Windows.
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences = 1) noexcept;

// True for the same exception object, or for two std::exception objects of the
// same dynamic type with the same what(): the same fault seen by several units.
[[nodiscard]] bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept;

}  // namespace ruvia

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> parse_ip_address(std::string_view text);
// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace ruvia
