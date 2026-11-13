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

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/one_shot.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait_result.h"

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
