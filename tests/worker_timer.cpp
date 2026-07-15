#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerWaitAwaiter.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

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
    auto expiredTimer = ruvia::detail::WorkerHandleAccess::scheduleTimer(
        worker,
        std::chrono::steady_clock::now(),
        [&expired](ruvia::detail::WorkerTimerOutcome outcome) {
            expired = outcome == ruvia::detail::WorkerTimerOutcome::kExpired;
        });
    auto cancelledTimer = ruvia::detail::WorkerHandleAccess::scheduleTimer(
        worker,
        std::chrono::steady_clock::now() + std::chrono::hours(1),
        [&cancelled](ruvia::detail::WorkerTimerOutcome outcome) {
            cancelled = outcome ==
                ruvia::detail::WorkerTimerOutcome::kCancelled;
        });
    if (!expiredTimer.valid() || !cancelledTimer.valid()) {
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

}

int main() {
    if (!discriminatedWaitStateWorks()) {
        return 1;
    }
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    asio::co_spawn(ioContext,
                   ruvia::detail::taskAsAwaitable(exercise(dispatcher, worker, success)),
                   asio::detached);
    ioContext.run();
    dispatcher->close();
    return success ? 0 : 1;
}
