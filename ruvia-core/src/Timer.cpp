#include <ruvia/core/Timer.h>

#include <coroutine>
#include <stdexcept>
#include <utility>

#include <ruvia/core/detail/WorkerTimer.h>

namespace ruvia {
namespace {

class SleepAwaiter final {
public:
    SleepAwaiter(WorkerHandle worker, std::chrono::steady_clock::duration duration)
        : worker_(std::move(worker)), duration_(duration) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        detail::WorkerHandleAccess::scheduleTimer(
            worker_, registration_,
            detail::workerTimerDeadlineAfter(duration_),
            [this](detail::WorkerTimerOutcome) { continuation_.resume(); });
        return true;
    }

    void await_resume() const noexcept {}

private:
    WorkerHandle worker_;
    std::chrono::steady_clock::duration duration_;
    std::coroutine_handle<> continuation_{};
    detail::WorkerTimerRegistration registration_;
};

}

Task<void> sleepFor(WorkerHandle worker, std::chrono::steady_clock::duration duration) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    co_await SleepAwaiter(std::move(worker), duration);
}

}
