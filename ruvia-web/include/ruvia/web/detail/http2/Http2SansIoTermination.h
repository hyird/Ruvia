#pragma once

#include <chrono>
#include <coroutine>
#include <memory>
#include <system_error>
#include <utility>

#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"

namespace ruvia::detail {

class Http2SansIoTermination;

// Intrusive, allocation-free observer used only on the connection's worker.
// An observer remains linked until its suspended operation is resumed, which
// makes session termination a level-triggered condition rather than a lossy
// notification edge.
class Http2SansIoTerminationObserver final {
public:
    using Notify = void (*)(void*) noexcept;

    Http2SansIoTerminationObserver(void* target, Notify notify) noexcept
        : target_(target),
          notify_(notify) {}

    Http2SansIoTerminationObserver(const Http2SansIoTerminationObserver&) = delete;
    Http2SansIoTerminationObserver& operator=(const Http2SansIoTerminationObserver&) = delete;

private:
    friend class Http2SansIoTermination;

    void* target_;
    Notify notify_;
    Http2SansIoTerminationObserver* previous_{nullptr};
    Http2SansIoTerminationObserver* next_{nullptr};
    bool linked_{false};
};

// Connection-owned terminal state shared by every Web capability derived from
// one HTTP/2 session. The state is confined to the connection worker; no atomics,
// mutexes, shared ownership, or per-operation allocation are required.
class Http2SansIoTermination final {
public:
    ~Http2SansIoTermination() {
        if (head_ != nullptr) {
            std::terminate();
        }
    }

    Http2SansIoTermination(const Http2SansIoTermination&) = delete;
    Http2SansIoTermination& operator=(const Http2SansIoTermination&) = delete;

    Http2SansIoTermination() noexcept = default;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        for (auto* observer = head_; observer != nullptr;) {
            auto* next = observer->next_;
            observer->notify_(observer->target_);
            observer = next;
        }
        return true;
    }

    [[nodiscard]] bool attach(Http2SansIoTerminationObserver& observer) noexcept {
        if (terminated()) {
            return false;
        }
        if (observer.linked_) {
            std::terminate();
        }
        observer.next_ = head_;
        if (head_ != nullptr) {
            head_->previous_ = &observer;
        }
        head_ = &observer;
        observer.linked_ = true;
        return true;
    }

    void detach(Http2SansIoTerminationObserver& observer) noexcept {
        if (!observer.linked_) {
            return;
        }
        if (observer.previous_ != nullptr) {
            observer.previous_->next_ = observer.next_;
        } else {
            head_ = observer.next_;
        }
        if (observer.next_ != nullptr) {
            observer.next_->previous_ = observer.previous_;
        }
        observer.previous_ = nullptr;
        observer.next_ = nullptr;
        observer.linked_ = false;
    }

private:
    std::error_code error_;
    Http2SansIoTerminationObserver* head_{nullptr};
};

struct Http2SansIoSleepCancellation final {
    WorkerTimerRegistration* timer{nullptr};
};

// A response-stream sleep races its worker timer against connection termination.
// Cancelling the timer is the wakeup path, so the timer callback remains the sole
// continuation owner and a terminal event cannot double-resume the coroutine.
class Http2SansIoSleepAwaiter final {
public:
    Http2SansIoSleepAwaiter(const WorkerHandle& worker, Http2SansIoTermination& termination, std::chrono::steady_clock::duration duration) noexcept
        : worker_(worker),
          termination_(termination),
          duration_(duration),
          observer_(this, &Http2SansIoSleepAwaiter::notifyTermination) {}
    Http2SansIoSleepAwaiter(WorkerHandle&&, Http2SansIoTermination&, std::chrono::steady_clock::duration) = delete;

    Http2SansIoSleepAwaiter(const WorkerHandle& worker, Http2SansIoTermination& termination, std::chrono::steady_clock::duration duration, StopToken stopToken)
        : worker_(worker),
          termination_(termination),
          duration_(duration),
          stopToken_(std::move(stopToken)),
          cancellation_(std::make_shared<Http2SansIoSleepCancellation>()),
          stopRegistration_(stopToken_.registerCallback([worker = &worker, cancellation = cancellation_]() noexcept {
              WorkerHandleAccess::deferOrTerminate(*worker, [cancellation] {
                  if (cancellation->timer != nullptr) {
                      cancellation->timer->cancel();
                  }
              });
          })),
          observer_(this, &Http2SansIoSleepAwaiter::notifyTermination) {}
    Http2SansIoSleepAwaiter(WorkerHandle&&, Http2SansIoTermination&, std::chrono::steady_clock::duration, StopToken) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() || termination_.terminated() || stopToken_.stopRequested();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        if (!termination_.attach(observer_)) {
            return false;
        }
        if (cancellation_ != nullptr) {
            cancellation_->timer = &timer_;
        }
        try {
            WorkerHandleAccess::scheduleTimer(worker_, timer_, workerTimerDeadlineAfter(duration_), [this](WorkerTimerOutcome outcome) noexcept {
                timerOutcome_ = outcome;
                if (cancellation_ != nullptr) {
                    cancellation_->timer = nullptr;
                }
                termination_.detach(observer_);
                continuation_.resume();
            });
        } catch (...) {
            if (cancellation_ != nullptr) {
                cancellation_->timer = nullptr;
            }
            termination_.detach(observer_);
            throw;
        }
        return true;
    }

    TimerSleepResult await_resume() const {
        if (termination_.terminated()) {
            throw std::system_error(termination_.error());
        }
        if (timerOutcome_ == WorkerTimerOutcome::kCancelled) {
            return TimerSleepResult::kStopRequested;
        }
        return stopToken_.stopRequested() ? TimerSleepResult::kStopRequested : TimerSleepResult::kElapsed;
    }

private:
    static void notifyTermination(void* raw) noexcept {
        auto& self = *static_cast<Http2SansIoSleepAwaiter*>(raw);
        // Timer cancellation delivers its completion on the same worker. It owns
        // the eventual resume and removes this observer in that callback.
        self.timer_.cancel();
    }

    const WorkerHandle& worker_;
    Http2SansIoTermination& termination_;
    std::chrono::steady_clock::duration duration_;
    StopToken stopToken_;
    std::coroutine_handle<> continuation_{};
    WorkerTimerRegistration timer_;
    WorkerTimerOutcome timerOutcome_{WorkerTimerOutcome::kExpired};
    std::shared_ptr<Http2SansIoSleepCancellation> cancellation_;
    StopRegistration stopRegistration_;
    Http2SansIoTerminationObserver observer_;
};

}  // namespace ruvia::detail
