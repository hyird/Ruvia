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
#include <ruvia/core/memory/ProcessResource.h>

namespace ruvia {

struct BlockingPoolOptions final {
    // 0 selects std::thread::hardware_concurrency() (at least one).
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
    kStopped,
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

// Fixed threads, bounded queue. Construction starts the threads; destruction
// stops and joins them. Copy and move are deleted: submitters hold a reference.
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
    // Waits for the threads to leave their loops. Call stop() first (or let the
    // destructor do both); joining from a pool thread is a deadlock and is
    // rejected instead.
    void join() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
    explicit BlockingOperationRejected(BlockingStatus status);

    [[nodiscard]] BlockingStatus status() const noexcept {
        return status_;
    }

private:
    BlockingStatus status_;
};

namespace detail {

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
        answered_ = true;
        (void)completion_.complete(std::move(payload));
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
    explicit BlockingResult(BlockingStatus status) noexcept {
        payload_.status = status;
    }

    explicit BlockingResult(detail::BlockingPayload<T>&& payload) noexcept
        : payload_(std::move(payload)) {}

    BlockingResult(const BlockingResult&) = delete;
    BlockingResult& operator=(const BlockingResult&) = delete;
    BlockingResult(BlockingResult&&) noexcept = default;
    BlockingResult& operator=(BlockingResult&&) noexcept = default;

    [[nodiscard]] BlockingStatus status() const noexcept {
        return payload_.status;
    }

    // True when the callable ran and returned normally.
    [[nodiscard]] bool completed() const noexcept {
        return payload_.status == BlockingStatus::kCompleted &&
            payload_.error == nullptr;
    }

    // The callable ran and threw. The exception is rethrown by value().
    [[nodiscard]] bool failed() const noexcept {
        return payload_.error != nullptr;
    }

    [[nodiscard]] const std::exception_ptr& error() const noexcept {
        return payload_.error;
    }

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
    detail::BlockingPayload<T> payload_;
};

namespace detail {

template <typename Fn>
[[nodiscard]] auto runBlockingUntil(
    BlockingPool& pool,
    WorkerHandle worker,
    std::optional<std::chrono::steady_clock::duration> timeout,
    Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    using Result = std::invoke_result_t<Fn&>;
    using Payload = BlockingPayload<Result>;
    static_assert(
        std::is_void_v<Result> || std::is_move_constructible_v<Result>,
        "a blocking callable's result travels back to the worker by move");

    // The one-shot outlives the request that started it: a worker that stops
    // resumes the waiter immediately while the pool thread is still running, so
    // this state cannot live in the request arena.
    //
    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::optional<std::pair<OneShotCompletion<Payload>, OneShotReceiver<Payload>>>
        channel;
    try {
        channel.emplace(makeOneShot<Payload>(std::move(worker), processResource()));
    } catch (const std::runtime_error&) {
        co_return BlockingResult<Result>(BlockingStatus::kWorkerStopping);
    }
    auto& [completion, receiver] = *channel;
    const auto submitted = pool.submit(
        [guard = BlockingCompletionGuard<Result>(std::move(completion)),
         call = std::move(fn)]() mutable {
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
        co_return BlockingResult<Result>(
            submitted == BlockingSubmitStatus::kQueueFull
                ? BlockingStatus::kQueueFull
                : BlockingStatus::kPoolStopped);
    }

    auto waited = timeout.has_value()
        ? co_await receiver.waitFor(*timeout)
        : co_await receiver.wait();
    if (auto* payload = waited.value(); payload != nullptr) {
        co_return BlockingResult<Result>(std::move(*payload));
    }
    if (waited.timedOut() != nullptr) {
        co_return BlockingResult<Result>(BlockingStatus::kTimedOut);
    }
    // Closed cannot happen -- this coroutine owns the receiver and is the only
    // waiter -- so anything else is the worker going away under the operation.
    co_return BlockingResult<Result>(BlockingStatus::kWorkerStopping);
}

}  // namespace detail

// Runs `fn` on `pool` and resumes the caller on `worker` with its result.
//
// The returned task must be awaited on `worker` -- that is the thread the
// coroutine resumes on and the only thread the result is touched from. `fn` is
// moved into the pool and runs on a foreign thread: it must own everything it
// uses (see the file header).
//
// Never throws for a rejection; the status says what happened. Use .value() to
// turn a rejection into an exception and to rethrow the callable's own.
template <typename Fn>
[[nodiscard]] auto runBlocking(BlockingPool& pool, WorkerHandle worker, Fn fn)
    -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    return detail::runBlockingUntil(
        pool, std::move(worker), std::nullopt, std::move(fn));
}

// The same, but the caller stops waiting after `timeout` and gets kTimedOut.
// This bounds the caller, not the work: a blocking call cannot be interrupted,
// so the pool thread stays occupied until `fn` returns. Use it to keep one
// wedged dependency from pinning a request -- its connection, its arena, its
// leases -- indefinitely.
template <typename Rep, typename Period, typename Fn>
[[nodiscard]] auto runBlocking(
    BlockingPool& pool,
    WorkerHandle worker,
    std::chrono::duration<Rep, Period> timeout,
    Fn fn) -> Task<BlockingResult<std::invoke_result_t<Fn&>>> {
    return detail::runBlockingUntil(
        pool,
        std::move(worker),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout),
        std::move(fn));
}

}  // namespace ruvia
