#include "ruvia/core/timer.h"

#include <coroutine>
#include <stdexcept>
#include <utility>

#include "ruvia/core/worker_timer.h"
namespace ruvia {
namespace {

// One sleep on the caller's worker. An empty stop token makes it a plain
// timer wait; a stoppable one also cancels the timer when stop is requested.
class sleep_awaiter final {
public:
    sleep_awaiter(const worker_handle& worker_value, std::chrono::steady_clock::duration duration,
        stop_token stop_token_value)
        : worker_(worker_value),
          duration_(duration),
          stop_token_(std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return duration_ <= std::chrono::steady_clock::duration::zero() || stop_token_.stop_requested();
    }

    // A stopped timer queue resolves immediately with the outcome
    // stop_timers() would have delivered to a pending sleep.
    bool await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        const auto status = worker_.schedule_timer(registration_, worker_timer_deadline_after(duration_),
            [this](worker_timer_outcome value) {
                outcome_ = value;
                continuation_.resume();
            });
        if (status == worker_timer_schedule_status::worker_stopping) {
            outcome_ = worker_timer_outcome::cancelled;
            return false;
        }
        if (stop_token_.stoppable()) {
            stop_token_.register_callback(stop_registration_,
                [cancellation = registration_.cancellation()] { cancellation.cancel(); });
        }
        return true;
    }

    // A zero/negative duration never suspends and keeps the default expired
    // outcome, so it reports elapsed unless stop was already requested.
    timer_sleep_result await_resume() const noexcept {
        return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested()
                   ? timer_sleep_result::elapsed
                   : timer_sleep_result::stop_requested;
    }

private:
    const worker_handle& worker_;
    std::chrono::steady_clock::duration duration_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_outcome outcome_{worker_timer_outcome::expired};
    worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

}  // namespace

namespace detail {

task<timer_sleep_result> sleep_on_worker(
    const worker_handle& worker_value, std::chrono::steady_clock::duration duration, stop_token stop_token_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("sleep_for must run on its bound worker");
    }
    co_return co_await sleep_awaiter(worker_value, duration, std::move(stop_token_value));
}

}  // namespace detail

}  // namespace ruvia
