#include "ruvia/core/timer.h"

#include <coroutine>
#include <stdexcept>
#include <utility>

#include "ruvia/core/worker_timer.h"
namespace ruvia {
namespace {

class sleep_awaiter final {
public:
    sleep_awaiter(const worker_handle& worker_value, std::chrono::steady_clock::duration duration)
        : worker_(worker_value),
          duration_(duration) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        (worker_).schedule_timer(registration_, ::ruvia::worker_timer_deadline_after(duration_), [this](::ruvia::worker_timer_outcome outcome) {
            outcome_ = outcome;
            continuation_.resume();
        });
        return true;
    }

    // A zero/negative duration never suspends and reports elapsed (the default),
    // so the caller sees a consistent result either way.
    timer_sleep_result await_resume() const noexcept {
        return outcome_ == ::ruvia::worker_timer_outcome::expired ? timer_sleep_result::elapsed
                                                                  : timer_sleep_result::stop_requested;
    }

private:
    const worker_handle& worker_;
    std::chrono::steady_clock::duration duration_;
    std::coroutine_handle<> continuation_{};
    ::ruvia::worker_timer_registration registration_;
    ::ruvia::worker_timer_outcome outcome_{::ruvia::worker_timer_outcome::expired};
};

class stoppable_sleep_awaiter final {
public:
    stoppable_sleep_awaiter(const worker_handle& worker_value, std::chrono::steady_clock::duration duration,
        stop_token stop_token_value)
        : worker_(worker_value),
          duration_(duration),
          stop_token_(std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() ||
               stop_token_.stop_requested();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        (worker_).schedule_timer(registration_, ::ruvia::worker_timer_deadline_after(duration_), [this](::ruvia::worker_timer_outcome outcome) {
            outcome_ = outcome;
            continuation_.resume();
        });
        stop_token_.register_callback(stop_registration_,
            [cancellation = registration_.cancellation()] { cancellation.cancel(); });
        return true;
    }

    timer_sleep_result await_resume() const noexcept {
        if (duration_ <= std::chrono::steady_clock::duration::zero() &&
            !stop_token_.stop_requested()) {
            return timer_sleep_result::elapsed;
        }
        return outcome_ == ::ruvia::worker_timer_outcome::expired && !stop_token_.stop_requested()
                   ? timer_sleep_result::elapsed
                   : timer_sleep_result::stop_requested;
    }

private:
    const worker_handle& worker_;
    std::chrono::steady_clock::duration duration_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    ::ruvia::worker_timer_outcome outcome_{::ruvia::worker_timer_outcome::expired};
    ::ruvia::worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

}  // namespace

task<timer_sleep_result> sleep_for(
    const worker_handle& worker_value, std::chrono::steady_clock::duration duration) {
    if (!worker_value.is_current()) {
        throw std::logic_error("sleep_for must run on its bound worker");
    }
    co_return co_await sleep_awaiter(worker_value, duration);
}

task<timer_sleep_result> sleep_for(
    const worker_handle& worker_value, std::chrono::steady_clock::duration duration, stop_token stop_token_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("sleep_for must run on its bound worker");
    }
    if (!stop_token_value.stoppable()) {
        co_return co_await sleep_awaiter(worker_value, duration);
    }
    co_return co_await stoppable_sleep_awaiter(worker_value, duration, std::move(stop_token_value));
}

}  // namespace ruvia
