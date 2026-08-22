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
// A BlockingPool is the escape hatch: a fixed set of ordinary threads with a
// bounded queue. runBlocking() hands the callable to that pool, suspends the
// calling coroutine, and resumes it on its own worker once the result comes
// back -- so the worker keeps serving other connections meanwhile.
//
// The callable runs on a foreign thread and outlives nothing: it must own
// everything it touches (capture by value or move), and it must not touch the
// Context, the request arena, or any other worker-owned state. Nothing in C++
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

#include <ruvia/core/MoveOnlyFunction.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>
#include <ruvia/core/memory/ProcessResource.h>

namespace ruvia {

struct BlockingPoolOptions final {
    // 0 selects half of std::thread::hardware_concurrency(), clamped to 2..8.
    // Blocking work benefits from some oversubscription on small machines, but
    // the default must not create one process thread per logical CPU on large
    // hosts. Set an explicit value when the workload has different needs.
    std::size_t threadCount{0};
    // Queued tasks waiting for a free thread. 0 selects threadCount * 64.
    // The queue is bounded on purpose: an unbounded one converts an overloaded
    // pool into unbounded memory growth and unbounded latency, and hides the
    // overload from the caller that could still shed load.
    std::size_t queueCapacity{0};
};

enum class BlockingSubmitStatus : std::uint8_t {
    kAccepted,
    kQueueFull,
    kPoolStopped,
};

struct BlockingPoolStats final {
    // Tasks waiting for a free thread, and tasks a thread is running right now.
    std::size_t queued{0};
    std::size_t running{0};
    // Tasks that ran to completion, whether or not the callable threw.
    std::uint64_t completed{0};
    // Tasks refused because no thread and no queue slot were free. This is the
    // sizing signal: a growing count means threadCount or queueCapacity is too
    // small for the offered load.
    std::uint64_t rejected{0};
    // Tasks that never ran because the pool was stopping -- dropped from the
    // queue by stop(), or submitted after it. Shutdown accounting, kept apart
    // from `rejected` so it cannot be mistaken for overload.
    std::uint64_t discarded{0};
};

// Fixed threads, bounded queue. Construction starts the threads. Destruction
// stops accepting work, discards queued tasks, and detaches threads that are
// already running a callable; those callables may finish after the pool object
// is gone. Call join() explicitly when the owner must wait for every thread.
// Copy and move are deleted: submitters hold a reference.
class BlockingPool final {
public:
    explicit BlockingPool(BlockingPoolOptions options = {});
    ~BlockingPool();

    BlockingPool(const BlockingPool&) = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;
    BlockingPool(BlockingPool&&) = delete;
    BlockingPool& operator=(BlockingPool&&) = delete;

    [[nodiscard]] std::size_t threadCount() const noexcept;
    [[nodiscard]] std::size_t queueCapacity() const noexcept;
    [[nodiscard]] BlockingPoolStats stats() const noexcept;

    // Safe from any thread. A rejected task is destroyed by the caller's thread
    // before submit() returns, so whatever it owns is released either way.
    [[nodiscard]] BlockingSubmitStatus submit(MoveOnlyFunction<void()> task);

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
    struct Impl;
    struct ThreadState;
    std::shared_ptr<Impl> impl_;
    std::unique_ptr<ThreadState> threads_;
};

enum class BlockingStatus : std::uint8_t {
    // The callable ran to completion. It may still have thrown: that is a
    // result, not a rejection.
    kCompleted,
    // The pool's queue was full. The callable never ran.
    kQueueFull,
    // The pool was stopped before a thread picked the task up. Never ran.
    kPoolStopped,
    // The worker stopped while the task was outstanding, so there is nobody
    // left to deliver a result to. The callable may or may not have run; its
    // result, if any, was discarded on the pool thread.
    kWorkerStopping,
    // The caller's stop token fired first. The callable may still be running;
    // its eventual result is discarded just like a timed-out result.
    kCancelled,
    // The caller stopped waiting first. The callable keeps running -- a
    // blocking call cannot be interrupted -- and its result is discarded when
    // it finishes. This releases the caller's request, not the pool thread.
    kTimedOut,
};

[[nodiscard]] std::string_view describeBlockingStatus(BlockingStatus status) noexcept;

// Thrown when a blocking operation produced no result of its own: the pool
// refused it or the worker went away. A callable's own exception is rethrown
// unchanged instead -- it is the operation's result.
class BlockingOperationRejected final : public std::runtime_error {
public:
    [[nodiscard]] BlockingStatus status() const noexcept {
        return status_;
    }

private:
    template <typename>
    friend class BlockingResult;

    explicit BlockingOperationRejected(BlockingStatus status);

    BlockingStatus status_;
};

namespace detail {

struct BlockingResultAccess;

template <typename T>
struct BlockingPayload final {
    BlockingStatus status{BlockingStatus::kCompleted};
    std::optional<T> value;
    std::exception_ptr error;
};

template <>
struct BlockingPayload<void> final {
    BlockingStatus status{BlockingStatus::kCompleted};
    std::exception_ptr error;
};

// Guards the one-shot half a submitted task owns. If the task is destroyed
// without running -- a stopped pool discards its queue -- the guard still
// answers the waiting coroutine instead of leaving it suspended forever.
template <typename T>
class BlockingCompletionGuard final {
public:
    explicit BlockingCompletionGuard(OneShotCompletion<BlockingPayload<T>> completion) noexcept
        : completion_(std::move(completion)) {}

    BlockingCompletionGuard(const BlockingCompletionGuard&) = delete;
    BlockingCompletionGuard& operator=(const BlockingCompletionGuard&) = delete;
    BlockingCompletionGuard(BlockingCompletionGuard&& other) noexcept
        : completion_(std::move(other.completion_)),
          answered_(std::exchange(other.answered_, true)) {}
    BlockingCompletionGuard& operator=(BlockingCompletionGuard&&) = delete;

    ~BlockingCompletionGuard() {
        if (answered_) {
            return;
        }
        BlockingPayload<T> payload;
        payload.status = BlockingStatus::kPoolStopped;
        // Nothing above this can receive a failure, and a rejected completion is
        // the normal case (the receiver may be gone), so the result is dropped.
        (void)completion_.complete(std::move(payload));
    }

    void answer(BlockingPayload<T>&& payload) {
        try {
            (void)completion_.complete(std::move(payload));
            answered_ = true;
        } catch (...) {
            BlockingPayload<T> failure;
            failure.error = std::current_exception();
            // An error-only payload has no T to move, so it remains transportable
            // even when moving the callable's result was what failed above.
            (void)completion_.complete(std::move(failure));
            answered_ = true;
        }
    }

private:
    OneShotCompletion<BlockingPayload<T>> completion_;
    bool answered_{false};
};

}  // namespace detail

// What a blocking operation produced. Either the callable ran -- returning a
// value or throwing -- or the operation was rejected before that could happen.
template <typename T>
class BlockingResult final {
public:
    BlockingResult(const BlockingResult&) = delete;
    BlockingResult& operator=(const BlockingResult&) = delete;
    BlockingResult(BlockingResult&&) = default;
    BlockingResult& operator=(BlockingResult&&) = default;

    [[nodiscard]] BlockingStatus status() const noexcept {
        return payload_.status;
    }

    // True when the callable ran and returned normally.
    [[nodiscard]] bool completed() const noexcept {
        return payload_.status == BlockingStatus::kCompleted && payload_.error == nullptr;
    }

    // The callable ran and threw. The exception is rethrown by value().
    [[nodiscard]] bool failed() const noexcept {
        return payload_.error != nullptr;
    }

    [[nodiscard]] const std::exception_ptr& error() const& noexcept {
        return payload_.error;
    }
    const std::exception_ptr& error() const&& = delete;

    // Rethrows what the callable threw, or throws BlockingOperationRejected if
    // it never ran.
    T value() && {
        if (payload_.error != nullptr) {
            std::rethrow_exception(payload_.error);
        }
        if (payload_.status != BlockingStatus::kCompleted) {
            throw BlockingOperationRejected(payload_.status);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*payload_.value);
        }
    }

private:
    friend struct detail::BlockingResultAccess;

    explicit BlockingResult(BlockingStatus status) noexcept {
        payload_.status = status;
    }

    explicit BlockingResult(detail::BlockingPayload<T>&& payload) noexcept(std::is_nothrow_move_constructible_v<detail::BlockingPayload<T>>)
        : payload_(std::move(payload)) {}

    detail::BlockingPayload<T> payload_;
};

namespace detail {

struct BlockingResultAccess final {
    template <typename T>
    [[nodiscard]] static BlockingResult<T> rejected(BlockingStatus status) {
        if (status == BlockingStatus::kCompleted) {
            throw std::logic_error("completed blocking result requires a payload");
        }
        return BlockingResult<T>(status);
    }

    template <typename T>
    [[nodiscard]] static BlockingResult<T> completed(BlockingPayload<T>&& payload) {
        if (payload.status != BlockingStatus::kCompleted) {
            throw std::logic_error("blocking payload requires completed status");
        }
        if constexpr (!std::is_void_v<T>) {
            if (payload.error == nullptr && !payload.value.has_value()) {
                throw std::logic_error("completed blocking payload requires a value or error");
            }
        }
        return BlockingResult<T>(std::move(payload));
    }
};

template <typename Fn>
[[nodiscard]] auto tryRunBlockingUntil(BlockingPool& pool, WorkerHandle worker, std::optional<std::chrono::steady_clock::duration> timeout, StopToken stopToken, Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    using Result = std::invoke_result_t<Fn&>;
    using Payload = BlockingPayload<Result>;
    static_assert(std::is_void_v<Result> || std::is_move_constructible_v<Result>, "a blocking callable's result travels back to the worker by move");

    // The one-shot outlives the request that started it: a worker that stops
    // resumes the waiter immediately while the pool thread is still running, so
    // this state cannot live in the request arena.
    //
    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::optional<std::pair<OneShotCompletion<Payload>, OneShotReceiver<Payload>>> channel;
    try {
        channel.emplace(makeOneShot<Payload>(std::move(worker), {.resource = processResource()}));
    } catch (const std::runtime_error&) {
        co_return BlockingResultAccess::rejected<Result>(BlockingStatus::kWorkerStopping);
    }
    auto& [completion, receiver] = *channel;
    const auto submitted = pool.submit([guard = BlockingCompletionGuard<Result>(std::move(completion)), call = std::move(fn)]() mutable {
        Payload payload;
        try {
            if constexpr (std::is_void_v<Result>) {
                call();
            } else {
                payload.value.emplace(call());
            }
        } catch (...) {
            payload.error = std::current_exception();
        }
        guard.answer(std::move(payload));
    });
    if (submitted != BlockingSubmitStatus::kAccepted) {
        co_return BlockingResultAccess::rejected<Result>(submitted == BlockingSubmitStatus::kQueueFull ? BlockingStatus::kQueueFull : BlockingStatus::kPoolStopped);
    }

    auto waited = timeout.has_value() ? co_await receiver.waitFor(*timeout, std::move(stopToken)) : co_await receiver.wait(std::move(stopToken));
    if (waited.hasValue()) {
        auto& payload = waited.value();
        if (payload.status != BlockingStatus::kCompleted) {
            co_return BlockingResultAccess::rejected<Result>(payload.status);
        }
        try {
            co_return BlockingResultAccess::completed<Result>(std::move(payload));
        } catch (...) {
            Payload failure;
            failure.error = std::current_exception();
            co_return BlockingResultAccess::completed<Result>(std::move(failure));
        }
    }
    if (waited.status() == WorkerWaitStatus::kTimedOut) {
        co_return BlockingResultAccess::rejected<Result>(BlockingStatus::kTimedOut);
    }
    if (waited.status() == WorkerWaitStatus::kCancelled) {
        co_return BlockingResultAccess::rejected<Result>(BlockingStatus::kCancelled);
    }
    // Closed cannot happen -- this coroutine owns the receiver and is the only
    // waiter -- so anything else is the worker going away under the operation.
    co_return BlockingResultAccess::rejected<Result>(BlockingStatus::kWorkerStopping);
}

template <typename T>
[[nodiscard]] Task<T> unwrapBlockingResult(Task<BlockingResult<T>> operation) {
    auto result = co_await std::move(operation);
    if constexpr (std::is_void_v<T>) {
        std::move(result).value();
        co_return;
    } else {
        co_return std::move(result).value();
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
template <typename Fn>
[[nodiscard]] auto tryRunBlocking(BlockingPool& pool, WorkerHandle worker, Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    return detail::tryRunBlockingUntil(pool, std::move(worker), std::nullopt, {}, std::move(fn));
}

template <typename Fn>
[[nodiscard]] auto tryRunBlocking(BlockingPool& pool, WorkerHandle worker, StopToken stopToken, Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    return detail::tryRunBlockingUntil(pool, std::move(worker), std::nullopt, std::move(stopToken), std::move(fn));
}

// The same, but the caller stops waiting after `timeout` and gets kTimedOut.
// This bounds the caller, not the work: a blocking call cannot be interrupted,
// so the pool thread stays occupied until `fn` returns. Use it to keep one
// wedged dependency from pinning a request -- its connection, its arena, its
// leases -- indefinitely.
template <typename Rep, typename Period, typename Fn>
[[nodiscard]] auto tryRunBlocking(BlockingPool& pool, WorkerHandle worker, std::chrono::duration<Rep, Period> timeout, Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    // duration_cast can overflow for a valid user duration such as hours::max(),
    // turning a long deadline into an immediate timeout. Use the same saturating
    // conversion as worker timers so all bounded waits share one interpretation.
    return detail::tryRunBlockingUntil(pool, std::move(worker), detail::workerTimerSaturatingDurationCast(timeout), {}, std::move(fn));
}

template <typename Rep, typename Period, typename Fn>
[[nodiscard]] auto tryRunBlocking(BlockingPool& pool, WorkerHandle worker, std::chrono::duration<Rep, Period> timeout, StopToken stopToken, Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    return detail::tryRunBlockingUntil(pool, std::move(worker), detail::workerTimerSaturatingDurationCast(timeout), std::move(stopToken), std::move(fn));
}

// The throwing form has the same name and semantics in core and web: callable
// exceptions are rethrown and a rejected operation throws
// BlockingOperationRejected.
template <typename Fn>
[[nodiscard]] auto runBlocking(BlockingPool& pool, WorkerHandle worker, Fn fn) -> Task<std::invoke_result_t<Fn&>> {
    using Result = std::invoke_result_t<Fn&>;
    return detail::unwrapBlockingResult<Result>(tryRunBlocking(pool, std::move(worker), std::move(fn)));
}

template <typename Fn>
[[nodiscard]] auto runBlocking(BlockingPool& pool, WorkerHandle worker, StopToken stopToken, Fn fn) -> Task<std::invoke_result_t<Fn&>> {
    using Result = std::invoke_result_t<Fn&>;
    return detail::unwrapBlockingResult<Result>(tryRunBlocking(pool, std::move(worker), std::move(stopToken), std::move(fn)));
}

template <typename Rep, typename Period, typename Fn>
[[nodiscard]] auto runBlocking(BlockingPool& pool, WorkerHandle worker, std::chrono::duration<Rep, Period> timeout, Fn fn) -> Task<std::invoke_result_t<Fn&>> {
    using Result = std::invoke_result_t<Fn&>;
    return detail::unwrapBlockingResult<Result>(tryRunBlocking(pool, std::move(worker), timeout, std::move(fn)));
}

template <typename Rep, typename Period, typename Fn>
[[nodiscard]] auto runBlocking(BlockingPool& pool, WorkerHandle worker, std::chrono::duration<Rep, Period> timeout, StopToken stopToken, Fn fn) -> Task<std::invoke_result_t<Fn&>> {
    using Result = std::invoke_result_t<Fn&>;
    return detail::unwrapBlockingResult<Result>(tryRunBlocking(pool, std::move(worker), timeout, std::move(stopToken), std::move(fn)));
}

}  // namespace ruvia
