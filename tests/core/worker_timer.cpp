#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerWaitAwaiter.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <barrier>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

static_assert(!std::is_move_constructible_v<
              ruvia::detail::WorkerTimerRegistration>);
static_assert(!std::is_move_assignable_v<
              ruvia::detail::WorkerTimerRegistration>);

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

    [[nodiscard]] int value() const noexcept { return value_; }

    static inline bool throwOnMove{false};

private:
    int value_{0};
};

bool discriminatedWaitStateWorks() {
    ruvia::detail::WorkerWaitAwaitState<int> early;
    if (early.complete(ruvia::detail::WorkerWaitResultAccess::value(3)) ||
        early.suspend(std::noop_coroutine())) {
        return false;
    }
    const auto earlyResult = early.takeResult();
    if (earlyResult.value() == nullptr || *earlyResult.value() != 3) {
        return false;
    }

    ruvia::detail::WorkerWaitAwaitState<int> suspended;
    const auto continuation = std::noop_coroutine();
    if (!suspended.suspend(continuation) ||
        !suspended.complete(ruvia::detail::WorkerWaitResultAccess::timedOut<int>()) ||
        suspended.continuation() != continuation) {
        return false;
    }
    const auto suspendedResult = suspended.takeResult();
    if (suspendedResult.timedOut() == nullptr) {
        return false;
    }

    ruvia::detail::WorkerWaitAwaitState<ThrowingMove> recovering;
    auto failedResult = ruvia::detail::WorkerWaitResultAccess::value(
        ThrowingMove(5));
    ThrowingMove::throwOnMove = true;
    bool moveFailed = false;
    try {
        static_cast<void>(recovering.complete(std::move(failedResult)));
    } catch (const std::runtime_error&) {
        moveFailed = true;
    }
    ThrowingMove::throwOnMove = false;
    if (!moveFailed ||
        recovering.complete(ruvia::detail::WorkerWaitResultAccess::value(
            ThrowingMove(7)))) {
        return false;
    }
    const auto recovered = recovering.takeResult();
    return recovered.value() != nullptr && recovered.value()->value() == 7;
}

bool saturatingTimerDeadlineWorks() {
    using Clock = std::chrono::steady_clock;
    using ruvia::detail::workerTimerSaturatingDeadline;

    const auto ordinaryNow = Clock::time_point(Clock::duration(100));
    if (workerTimerSaturatingDeadline(
            ordinaryNow, Clock::duration(25)) !=
        Clock::time_point(Clock::duration(125))) {
        return false;
    }

    const auto nearMaximum = Clock::time_point::max() - Clock::duration(5);
    if (workerTimerSaturatingDeadline(
            nearMaximum, Clock::duration(10)) !=
        Clock::time_point::max()) {
        return false;
    }

    // The old direct `now + duration` expression overflowed for this public
    // input and could turn an effectively infinite wait into an expired timer.
    return workerTimerSaturatingDeadline(
               ordinaryNow, Clock::duration::max()) ==
        Clock::time_point::max();
}

bool saturatingTimerDurationCastWorks() {
    using Target = std::chrono::steady_clock::duration;
    using ruvia::detail::workerTimerSaturatingDurationCast;

    const auto ordinary = workerTimerSaturatingDurationCast(
        std::chrono::microseconds(1500));
    if (ordinary != std::chrono::duration_cast<Target>(
                        std::chrono::microseconds(1500))) {
        return false;
    }

    using UnsignedSeconds = std::chrono::duration<std::uint64_t>;
    if (workerTimerSaturatingDurationCast(UnsignedSeconds::max()) !=
        Target::max()) {
        return false;
    }
    if (ruvia::detail::workerTimerDeadlineAfter(
            std::chrono::milliseconds::max()) !=
        std::chrono::steady_clock::time_point::max()) {
        return false;
    }

    using FloatingSeconds = std::chrono::duration<long double>;
    return workerTimerSaturatingDurationCast(FloatingSeconds(
               std::numeric_limits<long double>::infinity())) ==
            Target::max() &&
        workerTimerSaturatingDurationCast(FloatingSeconds(
            -std::numeric_limits<long double>::infinity())) ==
            Target::min() &&
        workerTimerSaturatingDurationCast(FloatingSeconds(
            std::numeric_limits<long double>::quiet_NaN())) ==
            Target::zero();
}

bool detachedTimerCancellationRaceWorks() {
    for (int attempt = 0; attempt < 32; ++attempt) {
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        ruvia::detail::WorkerTimerRegistration registration;
        std::promise<void> scheduled;
        auto ready = scheduled.get_future();
        asio::post(ioContext, [&] {
            ruvia::detail::WorkerHandleAccess::scheduleTimer(
                worker, registration,
                std::chrono::steady_clock::now() + std::chrono::hours(1),
                [](ruvia::detail::WorkerTimerOutcome) {});
            scheduled.set_value();
        });
        std::thread workerThread([&] { ioContext.run(); });
        ready.get();
        ioContext.stop();
        workerThread.join();

        std::barrier gate(3);
        std::jthread cancelling([&] {
            gate.arrive_and_wait();
            registration.cancel();
        });
        std::jthread detaching([&] {
            gate.arrive_and_wait();
            dispatcher->detachContext();
        });
        gate.arrive_and_wait();
    }
    return true;
}

ruvia::Task<void> markAfterSleep(ruvia::WorkerHandle worker, bool& completed) {
    co_await ruvia::sleepFor(worker, std::chrono::hours(1));
    completed = true;
}

ruvia::Task<void> exercise(
    const std::shared_ptr<ruvia::detail::WorkerDispatcher>& dispatcher,
    ruvia::WorkerHandle worker,
    bool& success) {
    co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));

    bool expired = false;
    bool cancelled = false;
    ruvia::detail::WorkerTimerRegistration expiredTimer;
    ruvia::detail::WorkerHandleAccess::scheduleTimer(
        worker, expiredTimer,
        std::chrono::steady_clock::now(),
        [&expired](ruvia::detail::WorkerTimerOutcome outcome) {
            expired = outcome == ruvia::detail::WorkerTimerOutcome::kExpired;
        });
    ruvia::detail::WorkerTimerRegistration cancelledTimer;
    ruvia::detail::WorkerHandleAccess::scheduleTimer(
        worker, cancelledTimer,
        std::chrono::steady_clock::now() + std::chrono::hours(1),
        [&cancelled](ruvia::detail::WorkerTimerOutcome outcome) {
            cancelled = outcome ==
                ruvia::detail::WorkerTimerOutcome::kCancelled;
        });
    if (!expiredTimer.registered() || !cancelledTimer.registered()) {
        co_return;
    }
    cancelledTimer.cancel();
    co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));

    bool cancelledSleepResumed = false;
    ruvia::TaskScope scope(worker);
    scope.spawn(markAfterSleep(worker, cancelledSleepResumed));
    dispatcher->stopTimers();
    co_await scope.join();
    success = expired && cancelled && cancelledSleepResumed;
}

ruvia::Task<void> exerciseSlotReuse(
    ruvia::WorkerHandle worker, bool& success) {
    constexpr std::size_t kTimerCount = 256;
    auto registrations = std::make_unique<
        ruvia::detail::WorkerTimerRegistration[]>(kTimerCount);
    std::size_t cancelled = 0;
    std::size_t expired = 0;

    for (std::size_t index = 0; index < kTimerCount; ++index) {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(
            worker, registrations[index],
            std::chrono::steady_clock::now() + std::chrono::hours(1),
            [&cancelled](ruvia::detail::WorkerTimerOutcome outcome) {
                if (outcome == ruvia::detail::WorkerTimerOutcome::kCancelled) {
                    ++cancelled;
                }
            });
    }

    bool rejectedActiveReuse = false;
    try {
        ruvia::detail::WorkerHandleAccess::scheduleTimer(
            worker, registrations[0],
            std::chrono::steady_clock::now(),
            [](ruvia::detail::WorkerTimerOutcome) {});
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
        ruvia::detail::WorkerHandleAccess::scheduleTimer(
            worker, registrations[index],
            std::chrono::steady_clock::now(),
            [&expired](ruvia::detail::WorkerTimerOutcome outcome) {
                if (outcome == ruvia::detail::WorkerTimerOutcome::kExpired) {
                    ++expired;
                }
            });
    }
    co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));
    success = rejectedActiveReuse && cancelled == kTimerCount &&
        expired == kTimerCount;
}

}

int main() {
    if (!discriminatedWaitStateWorks() ||
        !saturatingTimerDeadlineWorks() ||
        !saturatingTimerDurationCastWorks() ||
        !detachedTimerCancellationRaceWorks()) {
        return 1;
    }
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    bool slotReuseSuccess = false;
    asio::co_spawn(ioContext,
                   ruvia::detail::taskAsAwaitable(exercise(dispatcher, worker, success)),
                   asio::detached);
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseSlotReuse(worker, slotReuseSuccess)),
        asio::detached);
    ioContext.run();
    dispatcher->close();
    return success && slotReuseSuccess ? 0 : 1;
}
