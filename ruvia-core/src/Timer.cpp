#include "ruvia/core/Timer.h"

#include <coroutine>
#include <stdexcept>
#include <utility>

#include "ruvia/core/WorkerTimer.h"
namespace ruvia {
namespace {

class SleepAwaiter final {
public:
    SleepAwaiter(const WorkerHandle& worker, std::chrono::steady_clock::duration duration)
        : worker_(worker),
          duration_(duration) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        (worker_).schedule_timer(registration_, ::ruvia::workerTimerDeadlineAfter(duration_), [this](::ruvia::WorkerTimerOutcome outcome) {
            outcome_ = outcome;
            continuation_.resume();
        });
        return true;
    }

    // A zero/negative duration never suspends and reports elapsed (the default),
    // so the caller sees a consistent result either way.
    TimerSleepResult await_resume() const noexcept {
        return outcome_ == ::ruvia::WorkerTimerOutcome::kExpired ? TimerSleepResult::kElapsed
                                                                 : TimerSleepResult::kStopRequested;
    }

private:
    const WorkerHandle& worker_;
    std::chrono::steady_clock::duration duration_;
    std::coroutine_handle<> continuation_{};
    ::ruvia::WorkerTimerRegistration registration_;
    ::ruvia::WorkerTimerOutcome outcome_{::ruvia::WorkerTimerOutcome::kExpired};
};

class StoppableSleepAwaiter final {
public:
    StoppableSleepAwaiter(const WorkerHandle& worker, std::chrono::steady_clock::duration duration,
        StopToken stopToken)
        : worker_(worker),
          duration_(duration),
          stopToken_(std::move(stopToken)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() ||
               stopToken_.stopRequested();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        (worker_).schedule_timer(registration_, ::ruvia::workerTimerDeadlineAfter(duration_), [this](::ruvia::WorkerTimerOutcome outcome) {
            outcome_ = outcome;
            continuation_.resume();
        });
        stopToken_.registerCallback(stopRegistration_,
            [cancellation = registration_.cancellation()] { cancellation.cancel(); });
        return true;
    }

    TimerSleepResult await_resume() const noexcept {
        if (duration_ <= std::chrono::steady_clock::duration::zero() &&
            !stopToken_.stopRequested()) {
            return TimerSleepResult::kElapsed;
        }
        return outcome_ == ::ruvia::WorkerTimerOutcome::kExpired && !stopToken_.stopRequested()
                   ? TimerSleepResult::kElapsed
                   : TimerSleepResult::kStopRequested;
    }

private:
    const WorkerHandle& worker_;
    std::chrono::steady_clock::duration duration_;
    StopToken stopToken_;
    std::coroutine_handle<> continuation_{};
    ::ruvia::WorkerTimerOutcome outcome_{::ruvia::WorkerTimerOutcome::kExpired};
    ::ruvia::WorkerTimerRegistration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    StopRegistration stopRegistration_;
};

}  // namespace

Task<TimerSleepResult> sleepFor(
    const WorkerHandle& worker, std::chrono::steady_clock::duration duration) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    co_return co_await SleepAwaiter(worker, duration);
}

Task<TimerSleepResult> sleepFor(
    const WorkerHandle& worker, std::chrono::steady_clock::duration duration, StopToken stopToken) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    if (!stopToken.stoppable()) {
        co_return co_await SleepAwaiter(worker, duration);
    }
    co_return co_await StoppableSleepAwaiter(worker, duration, std::move(stopToken));
}

}  // namespace ruvia
