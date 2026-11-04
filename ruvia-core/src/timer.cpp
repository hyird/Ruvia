#include "ruvia/core/timer.h"

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia {

// Only a cold operation may move; a started one is referenced by its timer
// callback and must stay address-stable.
sleep_operation::sleep_operation(sleep_operation&& other) noexcept
    : worker_(std::exchange(other.worker_, nullptr)),
      target_(other.target_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.started_) {
        std::terminate();
    }
}

bool sleep_operation::await_ready() {
    if (worker_ == nullptr || started_) {
        throw std::logic_error("sleep operation cannot be awaited");
    }
    if (!worker_->is_current()) {
        throw std::logic_error("sleep_for must run on its bound worker");
    }
    started_ = true;
    if (stop_token_.stop_requested()) {
        return true;
    }
    if (const auto* duration = std::get_if<std::chrono::steady_clock::duration>(&target_)) {
        return *duration <= std::chrono::steady_clock::duration::zero();
    }
    return std::get<std::chrono::steady_clock::time_point>(target_) <= std::chrono::steady_clock::now();
}

// A stopped timer queue resolves immediately with the outcome stop_timers()
// would have delivered to a pending sleep.
bool sleep_operation::await_suspend(std::coroutine_handle<> continuation) {
    continuation_ = continuation;
    const auto deadline = [&] {
        if (const auto* duration = std::get_if<std::chrono::steady_clock::duration>(&target_)) {
            return worker_timer_deadline_after(*duration);
        }
        return std::get<std::chrono::steady_clock::time_point>(target_);
    }();
    const auto status = worker_->schedule_timer(registration_, deadline,
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
timer_sleep_result sleep_operation::await_resume() const noexcept {
    return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested()
               ? timer_sleep_result::elapsed
               : timer_sleep_result::stop_requested;
}

sleep_operation sleep_until(const worker_handle& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) {
    return sleep_operation(worker_value, deadline, std::move(stop_token_value));
}

}  // namespace ruvia
