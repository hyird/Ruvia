#include <ruvia/core/TaskScope.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerWaitAwaiter.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

static_assert(std::same_as<decltype(ruvia::sleepFor(std::declval<const ruvia::WorkerHandle&>(), std::chrono::steady_clock::duration{})), ruvia::Task<ruvia::TimerSleepResult>>);
static_assert(std::same_as<decltype(ruvia::sleepFor(std::declval<const ruvia::WorkerHandle&>(), std::chrono::steady_clock::duration{}, std::declval<ruvia::StopToken>())), ruvia::Task<ruvia::TimerSleepResult>>);

template <typename Worker>
concept AcceptsTemporaryBorrowedWorker = requires(Worker&& worker) {
    ruvia::sleepFor(std::forward<Worker>(worker), std::chrono::steady_clock::duration{});
};

template <typename Worker>
concept AcceptsTemporaryBorrowedWorkerWithStopToken = requires(Worker&& worker, ruvia::StopToken token) {
    ruvia::sleepFor(std::forward<Worker>(worker), std::chrono::steady_clock::duration{}, token);
};

static_assert(!AcceptsTemporaryBorrowedWorker<ruvia::WorkerHandle>);
static_assert(!AcceptsTemporaryBorrowedWorkerWithStopToken<ruvia::WorkerHandle>);
static_assert(!std::is_convertible_v<ruvia::TimerSleepResult, bool>);
static_assert(!std::is_move_constructible_v<ruvia::detail::WorkerTimerRegistration>);
static_assert(!std::is_move_assignable_v<ruvia::detail::WorkerTimerRegistration>);

class ThrowingMove final {
public:
    explicit ThrowingMove(int value) noexcept
        : value_(value) {}

    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&& other) {
        if (throwOnMove) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline bool throwOnMove{false};

private:
    int value_{0};
};

bool discriminatedWaitStateWorks() {
    ruvia::detail::WorkerWaitAwaitState<int> early;
    if (early.complete(ruvia::detail::WorkerWaitResultAccess::value(3)) || early.suspend(std::noop_coroutine())) {
        return false;
    }
    const auto earlyResult = early.takeValue();
    if (earlyResult.value() == nullptr || *earlyResult.value() != 3) {
        return false;
    }

    ruvia::detail::WorkerWaitAwaitState<int> suspended;
    const auto continuation = std::noop_coroutine();
    if (!suspended.suspend(continuation) || !suspended.complete(ruvia::detail::WorkerWaitResultAccess::timedOut<int>()) || suspended.continuation() != continuation) {
        return false;
    }
    const auto suspendedResult = suspended.takeValue();
    if (suspendedResult.timedOut() == nullptr) {
        return false;
    }

    ruvia::detail::WorkerWaitAwaitState<ThrowingMove> recovering;
    auto failedResult = ruvia::detail::WorkerWaitResultAccess::value(ThrowingMove(5));
    ThrowingMove::throwOnMove = true;
    bool moveFailed = false;
    try {
        static_cast<void>(recovering.complete(std::move(failedResult)));
    } catch (const std::runtime_error&) {
        moveFailed = true;
    }
    ThrowingMove::throwOnMove = false;
    if (!moveFailed || recovering.complete(ruvia::detail::WorkerWaitResultAccess::value(ThrowingMove(7)))) {
        return false;
    }
    const auto recovered = recovering.takeValue();
    return recovered.value() != nullptr && recovered.value()->value() == 7;
}

bool saturatingTimerDeadlineWorks() {
    using Clock = std::chrono::steady_clock;
    using ruvia::detail::workerTimerSaturatingDeadline;

    const auto ordinaryNow = Clock::time_point(Clock::duration(100));
    if (workerTimerSaturatingDeadline(ordinaryNow, Clock::duration(25)) != Clock::time_point(Clock::duration(125))) {
        return false;
    }

    const auto nearMaximum = Clock::time_point::max() - Clock::duration(5);
    if (workerTimerSaturatingDeadline(nearMaximum, Clock::duration(10)) != Clock::time_point::max()) {
        return false;
    }

    // The old direct `now + duration` expression overflowed for this public
    // input and could turn an effectively infinite wait into an expired timer.
    return workerTimerSaturatingDeadline(ordinaryNow, Clock::duration::max()) == Clock::time_point::max();
}

bool saturatingTimerDurationCastWorks() {
    using Target = std::chrono::steady_clock::duration;
    using ruvia::detail::workerTimerSaturatingDurationCast;

    const auto ordinary = workerTimerSaturatingDurationCast(std::chrono::microseconds(1500));
    if (ordinary != std::chrono::duration_cast<Target>(std::chrono::microseconds(1500))) {
        return false;
    }

    using UnsignedSeconds = std::chrono::duration<std::uint64_t>;
    if (workerTimerSaturatingDurationCast(UnsignedSeconds::max()) != Target::max()) {
        return false;
    }
    if (ruvia::detail::workerTimerDeadlineAfter(std::chrono::milliseconds::max()) != std::chrono::steady_clock::time_point::max()) {
        return false;
    }

    using FloatingSeconds = std::chrono::duration<long double>;
    return workerTimerSaturatingDurationCast(FloatingSeconds(std::numeric_limits<long double>::infinity())) == Target::max() && workerTimerSaturatingDurationCast(FloatingSeconds(-std::numeric_limits<long double>::infinity())) == Target::min() && workerTimerSaturatingDurationCast(FloatingSeconds(std::numeric_limits<long double>::quiet_NaN())) == Target::zero();
}

bool timerImmediateShutdownWorks() {
    asio::io_context ioContext;
    for (int attempt = 0; attempt < 32; ++attempt) {
        ioContext.restart();
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        ruvia::detail::WorkerTimerRegistration registration;
        std::promise<void> stopped;
        auto stoppedReady = stopped.get_future();
        asio::post(ioContext, [&] {
            ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::detail::WorkerTimerOutcome) {});
            dispatcher->stopTimers();
            asio::post(ioContext, [&] {
                stopped.set_value();
                ioContext.stop();
            });
        });
        std::thread workerThread([&] { ioContext.run(); });
        stoppedReady.get();
        workerThread.join();
        dispatcher->detachContext();
    }
    return true;
}

bool stoppedDispatcherCanOutliveContext() {
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher;
    ruvia::WorkerHandle worker;
    {
        asio::io_context ioContext;
        dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
        worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        dispatcher->stopTimers();
    }
    dispatcher.reset();
    worker = ruvia::WorkerHandle{};
    return true;
}

bool timerRegistrationResetAfterStopDoesNotQueueCancellation() {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    auto registration = std::make_unique<ruvia::detail::WorkerTimerRegistration>();
    std::size_t cancelled = 0;
    std::size_t expired = 0;

    asio::post(ioContext, [&] {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(
            worker,
            *registration,
            std::chrono::steady_clock::now() + std::chrono::hours(1),
            [&](ruvia::detail::WorkerTimerOutcome outcome) {
                if (outcome == ruvia::detail::WorkerTimerOutcome::kCancelled) {
                    ++cancelled;
                } else if (outcome == ruvia::detail::WorkerTimerOutcome::kExpired) {
                    ++expired;
                }
            });
        dispatcher->stopTimers();
        ioContext.stop();
    });
    ioContext.run();
    if (cancelled != 1 || expired != 0) {
        dispatcher->detachContext();
        return false;
    }

    ioContext.restart();
    while (ioContext.poll() != 0) {
    }

    registration.reset();

    ioContext.restart();
    const auto queuedHandlers = ioContext.poll();
    dispatcher->detachContext();
    return queuedHandlers == 0 && cancelled == 1 && expired == 0;
}

bool offWorkerCancellationCanRaceWithTimerShutdown() {
    for (int attempt = 0; attempt < 64; ++attempt) {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        ruvia::detail::WorkerTimerRegistration registration;
        ruvia::detail::WorkerTimerCancellation cancellation;
        std::barrier start(2);
        std::atomic_int completions{0};

        asio::post(ioContext, [&] {
            ruvia::detail::WorkerHandleAccess::scheduleTimer(
                worker,
                registration,
                std::chrono::steady_clock::now() + std::chrono::hours(1),
                [&](ruvia::detail::WorkerTimerOutcome outcome) {
                    if (outcome == ruvia::detail::WorkerTimerOutcome::kCancelled) {
                        completions.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            cancellation = registration.cancellation();
            start.arrive_and_wait();
            dispatcher->stopTimers();
            ioContext.stop();
        });

        std::thread workerThread([&] { ioContext.run(); });
        std::thread cancellingThread([&] {
            start.arrive_and_wait();
            for (int call = 0; call < 64; ++call) {
                cancellation.cancel();
            }
        });
        cancellingThread.join();
        workerThread.join();
        dispatcher->detachContext();
        if (completions.load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool offWorkerCancellationAfterContextStopDoesNotExpireLater() {
    asio::io_context ioContext;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    auto registration = std::make_unique<ruvia::detail::WorkerTimerRegistration>();
    bool expired = false;
    bool cancelled = false;

    asio::post(ioContext, [&] {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(
            worker,
            *registration,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1),
            [&](ruvia::detail::WorkerTimerOutcome outcome) {
                expired = outcome == ruvia::detail::WorkerTimerOutcome::kExpired;
                cancelled = outcome == ruvia::detail::WorkerTimerOutcome::kCancelled;
            });
        ioContext.stop();
    });
    ioContext.run();

    // The cancellation request is issued outside the worker while the context is
    // stopped. It still has to remove the active timer slot; otherwise a later
    // restart can fire a callback owned by an already-destroyed registration.
    registration.reset();

    ioContext.restart();
    ioContext.run_for(std::chrono::milliseconds(20));
    dispatcher->detachContext();
    return !expired && cancelled;
}

ruvia::Task<void> markAfterSleep(ruvia::WorkerHandle worker, bool& completed, bool& reportedElapsed) {
    reportedElapsed = co_await ruvia::sleepFor(worker, std::chrono::hours(1)) == ruvia::TimerSleepResult::kElapsed;
    completed = true;
}

ruvia::Task<void> exercise(const std::shared_ptr<ruvia::detail::WorkerDispatcher>& dispatcher, ruvia::WorkerHandle worker, bool& success) {
    const bool firstSleepElapsed = co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)) == ruvia::TimerSleepResult::kElapsed;

    bool expired = false;
    bool cancelled = false;
    ruvia::detail::WorkerTimerRegistration expiredTimer;
    ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, expiredTimer, std::chrono::steady_clock::now(), [&expired](ruvia::detail::WorkerTimerOutcome outcome) { expired = outcome == ruvia::detail::WorkerTimerOutcome::kExpired; });
    ruvia::detail::WorkerTimerRegistration cancelledTimer;
    ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, cancelledTimer, std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::detail::WorkerTimerOutcome outcome) { cancelled = outcome == ruvia::detail::WorkerTimerOutcome::kCancelled; });
    if (!expiredTimer.registered() || !cancelledTimer.registered()) {
        co_return;
    }
    cancelledTimer.cancel();
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)));

    bool cancelledSleepResumed = false;
    bool cancelledSleepReportedElapsed = true;
    ruvia::TaskScope scope(worker);
    scope.spawn(markAfterSleep(worker, cancelledSleepResumed, cancelledSleepReportedElapsed));
    dispatcher->stopTimers();
    co_await scope.join();
    // A normal sleep reports elapsed; a shutdown-cancelled sleep resumes but
    // reports not-elapsed so a periodic loop can stop instead of re-sleeping.
    success = expired && cancelled && firstSleepElapsed && cancelledSleepResumed && !cancelledSleepReportedElapsed;
}

ruvia::Task<void> exerciseSlotReuse(ruvia::WorkerHandle worker, bool& success) {
    constexpr std::size_t kTimerCount = 256;
    auto registrations = std::make_unique<ruvia::detail::WorkerTimerRegistration[]>(kTimerCount);
    std::size_t cancelled = 0;
    std::size_t expired = 0;

    for (std::size_t index = 0; index < kTimerCount; ++index) {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, registrations[index], std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::detail::WorkerTimerOutcome outcome) {
            if (outcome == ruvia::detail::WorkerTimerOutcome::kCancelled) {
                ++cancelled;
            }
        });
    }

    bool rejectedActiveReuse = false;
    try {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, registrations[0], std::chrono::steady_clock::now(), [](ruvia::detail::WorkerTimerOutcome) {});
    } catch (const std::logic_error&) {
        rejectedActiveReuse = true;
    }

    for (std::size_t index = 0; index < kTimerCount; ++index) {
        registrations[index].cancel();
    }
    // Reuse every slot before the stale heap entries are popped. Generation
    // validation must prevent the old entries from cancelling or expiring the
    // replacements (ABA), even when cancellation compaction also runs.
    for (std::size_t index = 0; index < kTimerCount; ++index) {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(worker, registrations[index], std::chrono::steady_clock::now(), [&expired](ruvia::detail::WorkerTimerOutcome outcome) {
            if (outcome == ruvia::detail::WorkerTimerOutcome::kExpired) {
                ++expired;
            }
        });
    }
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)));
    success = rejectedActiveReuse && cancelled == kTimerCount && expired == kTimerCount;
}

}  // namespace

// A framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token. These pin that sleepFor's stoppable
// overload closes it, and that stopping it does not depend on the timer having
// been long enough to notice.
ruvia::Task<void> exerciseStoppableSleep(const ruvia::WorkerHandle& worker, bool& success) {
    success = false;


    // Not stopped: the sleep runs to completion and reports elapsed.
    ruvia::detail::StopSource idle;
    if (co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1), idle.token()) != ruvia::TimerSleepResult::kElapsed) {
        co_return;
    }

    // Already stopped before the call: must not suspend for the full duration.
    ruvia::detail::StopSource stopped;
    stopped.requestStop();
    const auto before = std::chrono::steady_clock::now();
    if (co_await ruvia::sleepFor(worker, std::chrono::seconds(30), stopped.token()) != ruvia::TimerSleepResult::kStopRequested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - before > std::chrono::seconds(5)) {
        co_return;
    }

    // Stopped while suspended: the deferred cancel has to cut a sleep that is
    // already parked on the timer queue, which is the case the whole overload
    // exists for.
    ruvia::detail::StopSource inflight;
    ruvia::detail::WorkerHandleAccess::defer(worker, [&inflight] { inflight.requestStop(); });
    const auto parked = std::chrono::steady_clock::now();
    if (co_await ruvia::sleepFor(worker, std::chrono::seconds(30), inflight.token()) != ruvia::TimerSleepResult::kStopRequested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - parked > std::chrono::seconds(5)) {
        co_return;
    }

    // The stop callback can run on an arbitrary thread. It may only post the
    // timer generation back to this worker; the dispatcher borrow stays valid
    // through callback teardown without copying shared ownership into the
    // awaiter.
    ruvia::detail::StopSource crossThread;
    std::thread stopper([&crossThread] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        crossThread.requestStop();
    });
    const auto crossThreadResult =
        co_await ruvia::sleepFor(worker, std::chrono::seconds(30), crossThread.token());
    stopper.join();
    if (crossThreadResult != ruvia::TimerSleepResult::kStopRequested) {
        co_return;
    }

    // A stop that arrives long after the sleep already finished must be a
    // no-op, not a use-after-free of the awaiter's timer registration.
    ruvia::detail::StopSource late;
    if (co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1), late.token()) != ruvia::TimerSleepResult::kElapsed) {
        co_return;
    }
    late.requestStop();
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(5)));

    success = true;
}

bool stoppableSleepWorks() {
    // Its own io_context and dispatcher: other cases in this file exercise
    // worker shutdown, which stops timers, and a parked sleep sharing that
    // dispatcher would be cancelled by them rather than by its own token.
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseStoppableSleep(worker, success)), asio::detached);
    ioContext.run();
    dispatcher->close();
    return success;
}

int main() {
    if (!discriminatedWaitStateWorks() || !saturatingTimerDeadlineWorks() || !saturatingTimerDurationCastWorks() || !timerImmediateShutdownWorks() || !stoppedDispatcherCanOutliveContext() || !timerRegistrationResetAfterStopDoesNotQueueCancellation() || !offWorkerCancellationCanRaceWithTimerShutdown() ||
        !offWorkerCancellationAfterContextStopDoesNotExpireLater() || !stoppableSleepWorks()) {
        return 1;
    }
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    bool slotReuseSuccess = false;
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise(dispatcher, worker, success)), asio::detached);
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseSlotReuse(worker, slotReuseSuccess)), asio::detached);
    ioContext.run();
    dispatcher->close();
    return success && slotReuseSuccess ? 0 : 1;
}
