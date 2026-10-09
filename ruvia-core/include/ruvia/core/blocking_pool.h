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
// context, the request arena, or any other worker-owned state. Nothing in C++
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

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/one_shot.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

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

    // Safe from any thread. A rejected task is destroyed by the caller's thread
    // before submit() returns, so whatever it owns is released either way.
    [[nodiscard]] blocking_submit_status submit(move_only_function<void()> task);

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
    struct thread_state_type;
    std::shared_ptr<impl_type> impl_;
    std::unique_ptr<thread_state_type> threads_;
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

struct blocking_result_access;

template <typename t_type>
struct blocking_payload final {
    blocking_status status_{blocking_status::completed};
    std::optional<t_type> value_;
    std::exception_ptr error_;
};

template <>
struct blocking_payload<void> final {
    blocking_status status_{blocking_status::completed};
    std::exception_ptr error_;
};

// Guards the one-shot half a submitted task owns. If the task is destroyed
// without running -- a stopped pool discards its queue -- the guard still
// answers the waiting coroutine instead of leaving it suspended forever.
template <typename t_type>
class blocking_completion_guard final {
public:
    explicit blocking_completion_guard(one_shot_completion<blocking_payload<t_type>> completion) noexcept
        : completion_(std::move(completion)) {}

    blocking_completion_guard(const blocking_completion_guard&) = delete;
    blocking_completion_guard& operator=(const blocking_completion_guard&) = delete;
    blocking_completion_guard(blocking_completion_guard&& other) noexcept
        : completion_(std::move(other.completion_)),
          answered_(std::exchange(other.answered_, true)) {}
    blocking_completion_guard& operator=(blocking_completion_guard&&) = delete;

    ~blocking_completion_guard() {
        if (answered_) {
            return;
        }
        blocking_payload<t_type> payload;
        payload.status_ = blocking_status::pool_stopped;
        // Nothing above this can receive a failure, and a rejected completion is
        // the normal case (the receiver may be gone), so the result is dropped.
        (void)completion_.complete(std::move(payload));
    }

    void answer(blocking_payload<t_type>&& payload_value) {
        try {
            (void)completion_.complete(std::move(payload_value));
            answered_ = true;
        } catch (...) {
            blocking_payload<t_type> failure;
            failure.error_ = std::current_exception();
            // An error-only payload has no result value to move, so it remains transportable
            // even when moving the callable's result was what failed above.
            (void)completion_.complete(std::move(failure));
            answered_ = true;
        }
    }

private:
    one_shot_completion<blocking_payload<t_type>> completion_;
    bool answered_{false};
};

}  // namespace detail

// What a blocking operation produced. Either the callable ran -- returning a
// value or throwing -- or the operation was rejected before that could happen.
template <typename t_type>
class blocking_result final {
public:
    blocking_result(const blocking_result&) = delete;
    blocking_result& operator=(const blocking_result&) = delete;
    blocking_result(blocking_result&&) = default;
    blocking_result& operator=(blocking_result&&) = default;

    [[nodiscard]] blocking_status status() const noexcept {
        return payload_.status_;
    }

    // True when the callable ran and returned normally.
    [[nodiscard]] bool completed() const noexcept {
        return payload_.status_ == blocking_status::completed && payload_.error_ == nullptr;
    }

    // The callable ran and threw. The exception is rethrown by value().
    [[nodiscard]] bool failed() const noexcept {
        return payload_.error_ != nullptr;
    }

    [[nodiscard]] const std::exception_ptr& error() const& noexcept {
        return payload_.error_;
    }
    const std::exception_ptr& error() const&& = delete;

    // Rethrows what the callable threw, or throws blocking_operation_rejected if
    // it never ran.
    t_type value() && {
        if (payload_.error_ != nullptr) {
            std::rethrow_exception(payload_.error_);
        }
        if (payload_.status_ != blocking_status::completed) {
            throw blocking_operation_rejected(payload_.status_);
        }
        if constexpr (!std::is_void_v<t_type>) {
            auto& value = payload_.value_;
            if (!value.has_value()) {
                std::terminate();
            }
            return std::move(value).value();
        }
    }

private:
    friend struct detail::blocking_result_access;

    explicit blocking_result(blocking_status status) noexcept {
        payload_.status_ = status;
    }

    explicit blocking_result(detail::blocking_payload<t_type>&& payload_value) noexcept(
        std::is_nothrow_move_constructible_v<detail::blocking_payload<t_type>>)
        : payload_(std::move(payload_value)) {}

    detail::blocking_payload<t_type> payload_;
};

namespace detail {

struct blocking_result_access final {
    template <typename t_type>
    [[nodiscard]] static blocking_result<t_type> rejected(blocking_status status) {
        if (status == blocking_status::completed) {
            throw std::logic_error("completed blocking result requires a payload");
        }
        return blocking_result<t_type>(status);
    }

    template <typename t_type>
    [[nodiscard]] static blocking_result<t_type> completed(blocking_payload<t_type>&& payload_value) {
        if (payload_value.status_ != blocking_status::completed) {
            throw std::logic_error("blocking payload requires completed status");
        }
        if constexpr (!std::is_void_v<t_type>) {
            if (payload_value.error_ == nullptr && !payload_value.value_.has_value()) {
                throw std::logic_error("completed blocking payload requires a value or error");
            }
        }
        return blocking_result<t_type>(std::move(payload_value));
    }
};

template <typename fn_type>
[[nodiscard]] auto try_run_blocking_until(blocking_pool& pool, worker_handle worker_value,
    std::optional<std::chrono::steady_clock::duration> timeout, stop_token stop_token_value, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    using result_type = std::invoke_result_t<fn_type&>;
    using payload = blocking_payload<result_type>;
    static_assert(std::is_void_v<result_type> || std::is_move_constructible_v<result_type>,
        "a blocking callable's result travels back to the worker by move");

    // The one-shot outlives the request that started it: a worker that stops
    // resumes the waiter immediately while the pool thread is still running, so
    // this state cannot live in the request arena.
    //
    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::optional<std::pair<one_shot_completion<payload>, one_shot_receiver<payload>>> channel;
    try {
        channel.emplace(make_one_shot<payload>(std::move(worker_value), {.resource_ = process_resource()}));
    } catch (const std::runtime_error&) {
        co_return blocking_result_access::rejected<result_type>(blocking_status::worker_stopping);
    }
    auto& [completion, receiver] = *channel;
    const auto submitted =
        pool.submit([guard = blocking_completion_guard<result_type>(std::move(completion)),
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
    if (submitted != blocking_submit_status::accepted) {
        co_return blocking_result_access::rejected<result_type>(
            submitted == blocking_submit_status::queue_full ? blocking_status::queue_full
                                                            : blocking_status::pool_stopped);
    }

    auto waited = timeout.has_value() ? co_await receiver.wait_for(*timeout, std::move(stop_token_value))
                                      : co_await receiver.wait(std::move(stop_token_value));
    if (waited.has_value()) {
        auto& payload_value = waited.value();
        if (payload_value.status_ != blocking_status::completed) {
            co_return blocking_result_access::rejected<result_type>(payload_value.status_);
        }
        try {
            co_return blocking_result_access::completed<result_type>(std::move(payload_value));
        } catch (...) {
            payload failure;
            failure.error_ = std::current_exception();
            co_return blocking_result_access::completed<result_type>(std::move(failure));
        }
    }
    if (waited.status() == worker_wait_status::timed_out) {
        co_return blocking_result_access::rejected<result_type>(blocking_status::timed_out);
    }
    if (waited.status() == worker_wait_status::cancelled) {
        co_return blocking_result_access::rejected<result_type>(blocking_status::cancelled);
    }
    // Closed cannot happen -- this coroutine owns the receiver and is the only
    // waiter -- so anything else is the worker going away under the operation.
    co_return blocking_result_access::rejected<result_type>(blocking_status::worker_stopping);
}

template <typename t_type>
[[nodiscard]] task<t_type> unwrap_blocking_result(task<blocking_result<t_type>> operation) {
    auto result_value = co_await std::move(operation);
    if constexpr (std::is_void_v<t_type>) {
        std::move(result_value).value();
        co_return;
    } else {
        co_return std::move(result_value).value();
    }
}

}  // namespace detail

// Tries to run `fn` on `pool` and resumes the caller on `worker` with a typed
// status. The callable's own exception is retained in the result.
//
// The returned task must be awaited on `worker` -- that is the thread the
// coroutine resumes on and the only thread the result is touched from. `fn` is
// moved into the pool and runs on a foreign thread: it must own everything it
// uses (see the file header).
//
// Never throws for a rejection; the status says what happened. Use
// std::move(result).value() to turn a rejection into an exception and to
// rethrow the callable's own exception; value() is rvalue-only because it
// consumes the result.
template <typename fn_type>
[[nodiscard]] auto try_run_blocking(blocking_pool& pool, worker_handle worker_value, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return detail::try_run_blocking_until(pool, std::move(worker_value), std::nullopt, {}, std::move(fn));
}

template <typename fn_type>
[[nodiscard]] auto try_run_blocking(blocking_pool& pool, worker_handle worker_value, stop_token stop_token_value,
    fn_type fn) -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return detail::try_run_blocking_until(
        pool, std::move(worker_value), std::nullopt, std::move(stop_token_value), std::move(fn));
}

// The same, but the caller stops waiting after `timeout` and gets timed_out.
// This bounds the caller, not the work: a blocking call cannot be interrupted,
// so the pool thread stays occupied until `fn` returns. Use it to keep one
// wedged dependency from pinning a request -- its connection, its arena, its
// leases -- indefinitely.
template <typename rep_type, typename period_type, typename fn_type>
[[nodiscard]] auto try_run_blocking(
    blocking_pool& pool, worker_handle worker_value, std::chrono::duration<rep_type, period_type> timeout, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    // duration_cast can overflow for a valid user duration such as hours::max(),
    // turning a long deadline into an immediate timeout. Use the same saturating
    // conversion as worker timers so all bounded waits share one interpretation.
    return detail::try_run_blocking_until(pool, std::move(worker_value),
        ::ruvia::worker_timer_saturating_duration_cast(timeout), {}, std::move(fn));
}

template <typename rep_type, typename period_type, typename fn_type>
[[nodiscard]] auto try_run_blocking(blocking_pool& pool, worker_handle worker_value,
    std::chrono::duration<rep_type, period_type> timeout, stop_token stop_token_value, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return detail::try_run_blocking_until(pool, std::move(worker_value),
        ::ruvia::worker_timer_saturating_duration_cast(timeout), std::move(stop_token_value), std::move(fn));
}

// The throwing form has the same name and semantics in core and web: callable
// exceptions are rethrown and a rejected operation throws
// blocking_operation_rejected.
template <typename fn_type>
[[nodiscard]] auto run_blocking(blocking_pool& pool, worker_handle worker_value, fn_type fn)
    -> task<std::invoke_result_t<fn_type&>> {
    using result_type = std::invoke_result_t<fn_type&>;
    return detail::unwrap_blocking_result<result_type>(
        try_run_blocking(pool, std::move(worker_value), std::move(fn)));
}

template <typename fn_type>
[[nodiscard]] auto run_blocking(blocking_pool& pool, worker_handle worker_value, stop_token stop_token_value, fn_type fn)
    -> task<std::invoke_result_t<fn_type&>> {
    using result_type = std::invoke_result_t<fn_type&>;
    return detail::unwrap_blocking_result<result_type>(
        try_run_blocking(pool, std::move(worker_value), std::move(stop_token_value), std::move(fn)));
}

template <typename rep_type, typename period_type, typename fn_type>
[[nodiscard]] auto run_blocking(blocking_pool& pool, worker_handle worker_value,
    std::chrono::duration<rep_type, period_type> timeout, fn_type fn) -> task<std::invoke_result_t<fn_type&>> {
    using result_type = std::invoke_result_t<fn_type&>;
    return detail::unwrap_blocking_result<result_type>(
        try_run_blocking(pool, std::move(worker_value), timeout, std::move(fn)));
}

template <typename rep_type, typename period_type, typename fn_type>
[[nodiscard]] auto run_blocking(blocking_pool& pool, worker_handle worker_value,
    std::chrono::duration<rep_type, period_type> timeout, stop_token stop_token_value, fn_type fn)
    -> task<std::invoke_result_t<fn_type&>> {
    using result_type = std::invoke_result_t<fn_type&>;
    return detail::unwrap_blocking_result<result_type>(
        try_run_blocking(pool, std::move(worker_value), timeout, std::move(stop_token_value), std::move(fn)));
}

}  // namespace ruvia
