#include <ruvia/core/Timer.h>

#include <coroutine>
#include <stdexcept>
#include <utility>

#include <ruvia/core/detail/worker/WorkerTimer.h>

namespace ruvia {
namespace {

class SleepAwaiter final {
public:
    SleepAwaiter(const WorkerHandle& worker, std::chrono::steady_clock::duration duration)
        : worker_(&worker),
          duration_(duration) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        detail::WorkerHandleAccess::scheduleTimer(*worker_, registration_, detail::workerTimerDeadlineAfter(duration_), [this](detail::WorkerTimerOutcome outcome) {
            outcome_ = outcome;
            continuation_.resume();
        });
        return true;
    }

    // A zero/negative duration never suspends and reports elapsed (the default),
    // so the caller sees a consistent result either way.
    TimerSleepResult await_resume() const noexcept {
        return outcome_ == detail::WorkerTimerOutcome::kExpired ? TimerSleepResult::kElapsed : TimerSleepResult::kWorkerStopping;
    }

private:
    const WorkerHandle* worker_;
    std::chrono::steady_clock::duration duration_;
    std::coroutine_handle<> continuation_{};
    detail::WorkerTimerRegistration registration_;
    detail::WorkerTimerOutcome outcome_{detail::WorkerTimerOutcome::kExpired};
};

}  // namespace

Task<TimerSleepResult> sleepFor(const WorkerHandle& worker, std::chrono::steady_clock::duration duration) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    co_return co_await SleepAwaiter(worker, duration);
}

}  // namespace ruvia
