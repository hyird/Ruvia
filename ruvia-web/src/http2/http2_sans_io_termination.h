#pragma once

#include <chrono>
#include <coroutine>
#include <system_error>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

class http2_sans_io_termination;

// Intrusive, allocation-free observer used only on the connection's worker.
// An observer remains linked until its suspended operation is resumed, which
// makes session termination a level-triggered condition rather than a lossy
// notification edge.
class http2_sans_io_termination_observer final {
public:
    using notify_type = void (*)(void*) noexcept;

    http2_sans_io_termination_observer(void* target, notify_type notify) noexcept
        : target_(target),
          notify_(notify) {}

    http2_sans_io_termination_observer(const http2_sans_io_termination_observer&) = delete;
    http2_sans_io_termination_observer& operator=(const http2_sans_io_termination_observer&) = delete;

private:
    friend class http2_sans_io_termination;

    void* target_;
    notify_type notify_;
    http2_sans_io_termination_observer* previous_{nullptr};
    http2_sans_io_termination_observer* next_{nullptr};
    bool linked_{false};
};

// Connection-owned terminal state shared by every Web capability derived from
// one HTTP/2 session. The state is confined to the connection worker; no atomics,
// mutexes, shared ownership, or per-operation allocation are required.
class http2_sans_io_termination final {
public:
    ~http2_sans_io_termination() {
        if (head_ != nullptr) {
            std::terminate();
        }
    }

    http2_sans_io_termination(const http2_sans_io_termination&) = delete;
    http2_sans_io_termination& operator=(const http2_sans_io_termination&) = delete;

    http2_sans_io_termination() noexcept = default;

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
            auto* next_value = observer->next_;
            observer->notify_(observer->target_);
            observer = next_value;
        }
        return true;
    }

    [[nodiscard]] bool attach(http2_sans_io_termination_observer& observer) noexcept {
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

    void detach(http2_sans_io_termination_observer& observer) noexcept {
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
    http2_sans_io_termination_observer* head_{nullptr};
};

// A response-stream sleep races its worker timer against connection termination.
// Cancelling the timer is the wakeup path, so the timer callback remains the sole
// continuation owner and a terminal event cannot double-resume the coroutine.
class http2_sans_io_sleep_awaiter final {
public:
    http2_sans_io_sleep_awaiter(const worker_handle& worker_value, http2_sans_io_termination& termination,
        std::chrono::steady_clock::duration duration) noexcept
        : worker_(worker_value),
          termination_(termination),
          duration_(duration),
          observer_(this, &http2_sans_io_sleep_awaiter::notify_termination) {}
    http2_sans_io_sleep_awaiter(
        worker_handle&&, http2_sans_io_termination&, std::chrono::steady_clock::duration) = delete;

    http2_sans_io_sleep_awaiter(const worker_handle& worker_value, http2_sans_io_termination& termination,
        std::chrono::steady_clock::duration duration, stop_token stop_token_value)
        : worker_(worker_value),
          termination_(termination),
          duration_(duration),
          stop_token_(std::move(stop_token_value)),
          observer_(this, &http2_sans_io_sleep_awaiter::notify_termination) {}
    http2_sans_io_sleep_awaiter(worker_handle&&, http2_sans_io_termination&,
        std::chrono::steady_clock::duration, stop_token) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() ||
               termination_.terminated() || stop_token_.stop_requested();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        if (!termination_.attach(observer_)) {
            return false;
        }
        try {
            worker_.schedule_timer(timer_, worker_timer_deadline_after(duration_),
                [this](worker_timer_outcome outcome) noexcept {
                    timer_outcome_ = outcome;
                    termination_.detach(observer_);
                    continuation_.resume();
                });
            if (stop_token_.stoppable()) {
                const auto cancellation = timer_.cancellation();
                stop_token_.register_callback(
                    stop_registration_, [cancellation] { cancellation.cancel(); });
            }
        } catch (...) {
            stop_registration_.reset();
            timer_.cancel_quietly();
            termination_.detach(observer_);
            throw;
        }
        return true;
    }

    timer_sleep_result await_resume() const {
        if (termination_.terminated()) {
            throw std::system_error(termination_.error());
        }
        if (timer_outcome_ == worker_timer_outcome::cancelled) {
            return timer_sleep_result::stop_requested;
        }
        return stop_token_.stop_requested() ? timer_sleep_result::stop_requested
                                            : timer_sleep_result::elapsed;
    }

private:
    static void notify_termination(void* raw) noexcept {
        auto& self = *static_cast<http2_sans_io_sleep_awaiter*>(raw);
        // Timer cancellation delivers its completion on the same worker. It owns
        // the eventual resume and removes this observer in that callback.
        self.timer_.cancel();
    }

    const worker_handle& worker_;
    http2_sans_io_termination& termination_;
    std::chrono::steady_clock::duration duration_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_registration timer_;
    worker_timer_outcome timer_outcome_{worker_timer_outcome::expired};
    stop_registration stop_registration_;
    http2_sans_io_termination_observer observer_;
};

}  // namespace ruvia::detail
