#include <ruvia/core/Timer.h>

#include <coroutine>
#include <memory>
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
        return outcome_ == detail::WorkerTimerOutcome::kExpired ? TimerSleepResult::kElapsed : TimerSleepResult::kStopRequested;
    }

private:
    const WorkerHandle* worker_;
    std::chrono::steady_clock::duration duration_;
    std::coroutine_handle<> continuation_{};
    detail::WorkerTimerRegistration registration_;
    detail::WorkerTimerOutcome outcome_{detail::WorkerTimerOutcome::kExpired};
};

// Cancellation state shared with the deferred cancel: the awaiter clears the
// registration pointer on the worker as it completes, so a stop that lands
// afterwards finds nothing to cancel. Both the clear and the cancel run on the
// worker, so there is no race between them -- what the shared_ptr buys is that
// the state outlives the awaiter, not mutual exclusion.
struct SleepCancellation final {
    detail::WorkerTimerRegistration* registration{nullptr};
};

class StoppableSleepAwaiter final {
public:
    // The stop registration is established here, not in await_suspend:
    // StopRegistration is neither movable nor assignable, so it can only be
    // initialized. A stop that lands before the timer is scheduled finds a null
    // registration pointer and does nothing; one that lands after finds it set,
    // because the deferred cancel runs on this worker and therefore after
    // await_suspend has returned to the loop.
    StoppableSleepAwaiter(const WorkerHandle& worker, std::chrono::steady_clock::duration duration, const StopToken& stopToken)
        : worker_(&worker),
          duration_(duration),
          stopToken_(&stopToken),
          cancellation_(std::make_shared<SleepCancellation>()),
          stopRegistration_(stopToken.registerCallback([worker = &worker, cancellation = cancellation_]() noexcept {
              detail::WorkerHandleAccess::deferOrTerminate(*worker, [cancellation] {
                  if (cancellation->registration != nullptr) {
                      cancellation->registration->cancel();
                  }
              });
          })) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() || stopToken_->stopRequested();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        cancellation_->registration = &registration_;
        detail::WorkerHandleAccess::scheduleTimer(*worker_, registration_, detail::workerTimerDeadlineAfter(duration_), [this](detail::WorkerTimerOutcome outcome) {
            outcome_ = outcome;
            // Runs on the worker, as does the deferred cancel below, so clearing
            // here is enough to make a late cancel a no-op.
            cancellation_->registration = nullptr;
            continuation_.resume();
        });
        return true;
    }

    TimerSleepResult await_resume() const noexcept {
        if (duration_ <= std::chrono::steady_clock::duration::zero() && !stopToken_->stopRequested()) {
            return TimerSleepResult::kElapsed;
        }
        return outcome_ == detail::WorkerTimerOutcome::kExpired && !stopToken_->stopRequested() ? TimerSleepResult::kElapsed : TimerSleepResult::kStopRequested;
    }

private:
    const WorkerHandle* worker_;
    std::chrono::steady_clock::duration duration_;
    const StopToken* stopToken_;
    std::coroutine_handle<> continuation_{};
    detail::WorkerTimerOutcome outcome_{detail::WorkerTimerOutcome::kExpired};
    detail::WorkerTimerRegistration registration_;
    std::shared_ptr<SleepCancellation> cancellation_;
    // Declared last so it is destroyed first: the callback it holds captures the
    // shared state, and must stop being reachable before anything it touches.
    StopRegistration stopRegistration_;
};

}  // namespace

Task<TimerSleepResult> sleepFor(const WorkerHandle& worker, std::chrono::steady_clock::duration duration) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    co_return co_await SleepAwaiter(worker, duration);
}

Task<TimerSleepResult> sleepFor(const WorkerHandle& worker, std::chrono::steady_clock::duration duration, const StopToken& stopToken) {
    if (!worker.isCurrent()) {
        throw std::logic_error("sleepFor must run on its bound worker");
    }
    if (!stopToken.stoppable()) {
        co_return co_await SleepAwaiter(worker, duration);
    }
    co_return co_await StoppableSleepAwaiter(worker, duration, stopToken);
}

}  // namespace ruvia
