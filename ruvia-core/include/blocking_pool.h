#pragma once

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

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "delivery_result.h"
#include "lifetimebound.h"
#include "move_only_function.h"
#include "one_shot.h"
#include "operation_options.h"
#include "task.h"
#include "wait_result.h"
#include "worker_ref.h"
#include "worker_wait.h"

namespace ruvia {

template <typename>
class blocking_result;

namespace detail {
struct blocking_pool_access;

template <typename t_type>
struct blocking_payload;

template <typename t_type>
[[nodiscard]] blocking_result<t_type> completed_blocking_result(blocking_payload<t_type>&& payload_value);
}  // namespace detail

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
    // queue by request_stop(), or posted after it. Shutdown accounting, kept apart
    // from `rejected` so it cannot be mistaken for overload.
    std::uint64_t discarded_{0};
};

// Fixed threads, bounded queue. Construction starts the threads. Destruction
// stops accepting work, discards queued tasks, joins the idle threads, and
// detaches threads that are already running a callable; those callables may
// finish after the pool object is gone. Call join() explicitly when the owner
// must wait for every thread.
// Copy and move are deleted: posters hold a reference.
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
    // into the queue; a rejection (queue_full, or stopping, including a
    // stop that lands during that move) returns the callable untouched in the
    // result, so the caller decides where it is destroyed. Callable moves and
    // destruction may query stats(), post(), or request_stop(). Admission
    // includes posts whose callable move has not finished.
    [[nodiscard]] post_result<> post(move_only_function<void()> task);

    // Stops accepting, discards tasks that have not started, and wakes the
    // threads. Tasks already running are not interrupted -- a blocking call
    // cannot be -- but nothing waits for their queued successors. Idempotent
    // and safe from any thread.
    void request_stop() noexcept;
    // Stops the pool and waits for its threads to leave their loops. Throws
    // logic_error before changing the pool's state if the caller runs on one
    // of its threads or owns an unfinished post reservation for this pool.
    // The destructor deliberately does not join already-running callables.
    void join();

private:
    friend struct detail::blocking_pool_access;

    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

enum class blocking_status : std::uint8_t {
    // Execution and result delivery both succeeded.
    completed,
    // Execution or result delivery raised an exception.
    failed,
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
    using value_storage_type = std::conditional_t<std::is_void_v<t_type>, std::monostate, std::optional<t_type>>;

    blocking_status status_{blocking_status::completed};
    [[no_unique_address]] value_storage_type value_{};
    std::exception_ptr error_;
};

template <typename t_type>
struct blocking_offload_state final : one_shot_state<blocking_payload<t_type>> {
    blocking_offload_state(worker_ref worker_value, operation_deadline deadline, stop_token stop_token_value) :
        one_shot_state<blocking_payload<t_type>>(std::move(worker_value)),
        deadline_(deadline),
        stop_token_(std::move(stop_token_value)) {}

    [[nodiscard]] bool try_start() noexcept {
        auto lock = this->lock_state();
        if (!std::holds_alternative<one_shot_pending>(this->lifecycle_)) {
            return false;
        }
        if (!this->worker_.accepting()) {
            this->lifecycle_.template emplace<one_shot_worker_stopping>();
            this->wait_slot_.complete(wait_status::worker_stopping);
            return false;
        }
        // Check the same bounds as the wait, even if the worker has not
        // dispatched the timer or resumed the caller yet.
        if (stop_token_.stop_requested() || deadline_.expired()) {
            return false;
        }
        started_ = true;
        return true;
    }

    [[nodiscard]] bool started() noexcept {
        auto lock = this->lock_state();
        return started_;
    }

    [[nodiscard]] operation_options wait_options() const noexcept {
        return {.timeout_ = deadline_.timeout(), .stop_token_ = stop_token_};
    }

private:
    const operation_deadline deadline_;
    const stop_token stop_token_;
    bool started_{false};
};

// If the pool discards the queued task, answer the waiter. A rejected claim
// leaves the wait to its own stop, timeout or worker shutdown. The closure
// owns only one shared state in addition to the callable.
template <typename t_type>
class blocking_completion_guard final {
public:
    explicit blocking_completion_guard(std::shared_ptr<blocking_offload_state<t_type>> state) noexcept :
        state_(std::move(state)) {}

    blocking_completion_guard(const blocking_completion_guard&) = delete;
    blocking_completion_guard& operator=(const blocking_completion_guard&) = delete;
    blocking_completion_guard(blocking_completion_guard&& other) noexcept = default;
    blocking_completion_guard& operator=(blocking_completion_guard&&) = delete;

    ~blocking_completion_guard() {
        if (state_ == nullptr) {
            return;
        }
        blocking_payload<t_type> payload;
        payload.status_ = blocking_status::pool_stopped;
        answer(std::move(payload));
    }

    [[nodiscard]] bool try_start() noexcept {
        if (!state_->try_start()) {
            state_.reset();
            return false;
        }
        return true;
    }

    void answer(blocking_payload<t_type>&& payload_value) {
        const auto completion = one_shot_access::completion<blocking_payload<t_type>>(std::move(state_));
        try {
            (void)completion.complete(std::move(payload_value));
        } catch (...) {
            blocking_payload<t_type> failure;
            failure.status_ = blocking_status::failed;
            failure.error_ = std::current_exception();
            // An error-only payload has no result value to move, so it remains transportable
            // even when moving the callable's result was what failed above.
            (void)completion.complete(std::move(failure));
        }
    }

private:
    std::shared_ptr<blocking_offload_state<t_type>> state_;
};

}  // namespace detail

// A blocking operation's value, exception, or rejection before execution.
// The outcome category is fixed after construction. Moving a failed result
// preserves its failure in the source as well; it must not become a successful
// result without a value. Replace an owning optional by emplacing a new result.
// from_value(), from_error() and from_status() build the same outcomes for a
// composed offload.
template <typename t_type>
class blocking_result final {
public:
    blocking_result(const blocking_result&) = delete;
    blocking_result& operator=(const blocking_result&) = delete;
    blocking_result(blocking_result&& other) noexcept(std::is_nothrow_move_constructible_v<detail::blocking_payload<t_type>>)
        requires std::is_move_constructible_v<detail::blocking_payload<t_type>>
        :
        payload_(std::move(other.payload_)) {
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
        return status() == blocking_status::completed;
    }

    // Execution or result delivery failed; value() rethrows the exception.
    [[nodiscard]] bool failed() const noexcept {
        return status() == blocking_status::failed;
    }

    [[nodiscard]] const std::exception_ptr& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_.error_;
    }
    const std::exception_ptr& error() const&& = delete;

    // Rethrows the stored error, or throws blocking_operation_rejected if the
    // operation was rejected.
    template <typename self_type = t_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] const self_type& value() const& RUVIA_LIFETIMEBOUND {
        rethrow_if_not_completed();
        return *payload_.value_;
    }

    void value() const&
        requires std::is_void_v<t_type>
    {
        rethrow_if_not_completed();
    }

    template <typename self_type = t_type>
        requires(!std::is_void_v<self_type>)
    const self_type& value() const&& = delete;
    void value() const&&
        requires std::is_void_v<t_type>
    = delete;

    [[nodiscard]] t_type take_value() &&
        requires(!std::is_void_v<t_type>)
    {
        rethrow_if_not_completed();
        return std::move(*payload_.value_);
    }

    void take_value() &&
        requires std::is_void_v<t_type>
    {
        rethrow_if_not_completed();
    }

    // The callable completed with this value.
    template <typename u_type>
        requires(!std::is_void_v<t_type> && std::constructible_from<t_type, u_type&&>)
    [[nodiscard]] static blocking_result from_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>) {
        blocking_result result_value(blocking_status::completed);
        result_value.payload_.value_.emplace(std::forward<u_type>(value));
        return result_value;
    }

    [[nodiscard]] static blocking_result from_value() noexcept
        requires std::is_void_v<t_type>
    {
        return blocking_result(blocking_status::completed);
    }

    // Execution or result delivery failed with this non-null exception.
    [[nodiscard]] static blocking_result from_error(std::exception_ptr error) {
        if (error == nullptr) {
            throw std::invalid_argument("blocking error result requires an exception");
        }
        blocking_result result_value(blocking_status::failed);
        result_value.payload_.error_ = std::move(error);
        return result_value;
    }

    // The operation produced no result of its own; rejection statuses only.
    [[nodiscard]] static blocking_result from_status(blocking_status status) {
        if (status == blocking_status::completed || status == blocking_status::failed) {
            throw std::invalid_argument("blocking status result requires a rejection status");
        }
        return blocking_result(status);
    }

private:
    friend blocking_result detail::completed_blocking_result<t_type>(detail::blocking_payload<t_type>&&);

    explicit blocking_result(blocking_status status) noexcept {
        payload_.status_ = status;
    }

    explicit blocking_result(detail::blocking_payload<t_type>&& payload_value) noexcept(std::is_nothrow_move_constructible_v<detail::blocking_payload<t_type>>) :
        payload_(std::move(payload_value)) {}

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

struct blocking_pool_access final {
    [[nodiscard]] static post_status post_factory(blocking_pool& pool, move_only_function<move_only_function<void()>()> factory);
};

// A completed payload always holds a value or an error.
template <typename t_type>
[[nodiscard]] blocking_result<t_type> completed_blocking_result(blocking_payload<t_type>&& payload_value) {
    if (payload_value.error_ != nullptr) {
        return blocking_result<t_type>::from_error(std::move(payload_value.error_));
    }
    return blocking_result<t_type>(std::move(payload_value));
}

template <typename t_type>
inline constexpr bool is_reference_wrapper_v = false;
template <typename u_type>
inline constexpr bool is_reference_wrapper_v<std::reference_wrapper<u_type>> = true;

}  // namespace detail

template <typename fn_type>
concept blocking_callable = (!std::is_reference_v<fn_type>) && (!detail::is_reference_wrapper_v<std::remove_cv_t<fn_type>>) && std::move_constructible<fn_type> && std::invocable<fn_type&> && (!std::is_reference_v<std::invoke_result_t<fn_type&>>) && (std::is_void_v<std::invoke_result_t<fn_type&>> || std::move_constructible<std::invoke_result_t<fn_type&>>);

namespace detail {

template <blocking_callable fn_type>
[[nodiscard]] auto run_blocking_impl(worker_ref worker_value, fn_type fn, operation_options options, bool join_started) -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    using result_type = std::invoke_result_t<fn_type&>;
    using payload = blocking_payload<result_type>;
    using outcome_type = blocking_result<result_type>;
    const operation_deadline deadline(options.timeout_);
    auto stop_token_value = std::move(options.stop_token_);

    // Every outcome the wait would decide without the callable is decided
    // first, so a misplaced, cancelled, or expired call never occupies a pool
    // thread or queue slot for a result nobody receives.
    if (!worker_value.is_current()) {
        throw std::logic_error("blocking work must be awaited on its bound worker");
    }
    auto* const pool = worker_value.blocking_pool();
    if (pool == nullptr) {
        co_return outcome_type::from_status(blocking_status::unavailable);
    }
    if (stop_token_value.stop_requested()) {
        co_return outcome_type::from_status(blocking_status::cancelled);
    }
    if (deadline.expired()) {
        co_return outcome_type::from_status(blocking_status::timed_out);
    }
    if (!worker_value.accepting()) {
        co_return outcome_type::from_status(blocking_status::worker_stopping);
    }

    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::shared_ptr<blocking_offload_state<result_type>> state;
    try {
        state = make_worker_wait_state<blocking_offload_state<result_type>>(std::move(worker_value), deadline, std::move(stop_token_value));
    } catch (const std::runtime_error&) {
        co_return outcome_type::from_status(blocking_status::worker_stopping);
    }
    // The receiver keeps the state alive after transferring the other owner to
    // the pool. The two sides use the same absolute deadline and stop state.
    auto& execution = *state;
    auto receiver = one_shot_access::receiver<payload>(state);
    const auto posted = blocking_pool_access::post_factory(*pool, [&] {
        return move_only_function<void()>([guard = blocking_completion_guard<result_type>(std::move(state)), call = std::move(fn)]() mutable {
            if (!guard.try_start()) {
                return;
            }
            payload payload_value;
            try {
                if constexpr (std::is_void_v<result_type>) {
                    call();
                } else {
                    payload_value.value_.emplace(call());
                }
            } catch (...) {
                payload_value.status_ = blocking_status::failed;
                payload_value.error_ = std::current_exception();
            }
            guard.answer(std::move(payload_value));
        });
    });
    if (posted != post_status::accepted) {
        co_return outcome_type::from_status(posted == post_status::queue_full ? blocking_status::queue_full : blocking_status::pool_stopped);
    }

    try {
        std::optional<wait_result<payload>> waited(co_await receiver.wait(execution.wait_options()));
        if (join_started && (waited->status() == wait_status::timed_out || waited->status() == wait_status::cancelled) && execution.started()) {
            waited.emplace(co_await receiver.wait());
        }
        if (waited->has_value()) {
            auto payload_value = std::move(*waited).take_value();
            if (payload_value.error_ != nullptr) {
                co_return outcome_type::from_error(std::move(payload_value.error_));
            }
            if (payload_value.status_ != blocking_status::completed) {
                co_return outcome_type::from_status(payload_value.status_);
            }
            co_return completed_blocking_result(std::move(payload_value));
        }
        if (waited->status() == wait_status::timed_out) {
            co_return outcome_type::from_status(blocking_status::timed_out);
        }
        if (waited->status() == wait_status::cancelled) {
            co_return outcome_type::from_status(blocking_status::cancelled);
        }
        // Closed cannot happen -- this coroutine owns the receiver and is the only
        // waiter -- so anything else is the worker going away under the operation.
        co_return outcome_type::from_status(blocking_status::worker_stopping);
    } catch (...) {
        // Payload delivery now moves the result on the receiving worker.
        co_return outcome_type::from_error(std::current_exception());
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
// awaiting it elsewhere throws std::logic_error before `fn` is posted. `fn`
// is moved into the pool and runs on a foreign thread: it must own everything
// it uses (see the file header).
//
// options.timeout_ bounds the caller, not a started callable: a blocking call
// cannot be interrupted, so an already-started pool thread stays occupied until
// `fn` returns. A queued callable whose wait ends before a pool thread picks it
// up is skipped without running. A relative timeout starts when the task
// starts; an absolute one shares a deadline with the caller's other phases.
// options.stop_token_ cancels the wait the same way.
//
// Never throws for a rejection; the status says what happened. A worker
// without a pool, a stop token that is already stopped, or a deadline that has
// passed rejects without posting `fn`. Use std::move(result).take_value()
// to turn a rejection into an exception and to rethrow the callable's own
// exception.
template <blocking_callable fn_type>
[[nodiscard]] auto run_blocking(worker_ref worker_value, fn_type fn, operation_options options = {}) -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return detail::run_blocking_impl(std::move(worker_value), std::move(fn), std::move(options), false);
}

// Like run_blocking(), except options.timeout_ and options.stop_token_ only
// cancel or time out `fn` before a pool thread starts executing it. Once a
// pool thread has claimed `fn`, the caller stays suspended until `fn` finishes
// (unless `worker_value` itself stops), so exclusive files, buffers or leases
// touched by `fn` are not accessed concurrently by the caller's cleanup path.
template <blocking_callable fn_type>
[[nodiscard]] auto run_blocking_joined(worker_ref worker_value, fn_type fn, operation_options options = {}) -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return detail::run_blocking_impl(std::move(worker_value), std::move(fn), std::move(options), true);
}

}  // namespace ruvia
