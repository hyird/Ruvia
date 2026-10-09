#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

// One request's handler deadline: a stop source that trips either when the
// worker begins stopping or when the deadline elapses, whichever comes first.
//
// The reason this is a stop source rather than a cancellation is that a
// suspended coroutine cannot be abandoned in C++ -- destroying its frame while
// an awaiter still points at it is a use-after-free. So the deadline stops the
// WAITS instead: everything already taking a stop_token returns at once, and the
// handler unwinds itself.

namespace ruvia::detail {

class request_deadline final {
public:
    // The worker link is established here rather than in arm(): stop_registration
    // is neither movable nor assignable, so it can only be initialized. If the
    // worker is already stopping, register_callback runs the callback at once and
    // this request starts already stopped -- which is correct.
    explicit request_deadline(const stop_token& worker_stop)
        : token_(source_.token()),
          worker_link_(worker_stop.register_callback([this]() noexcept { source_.request_stop(); })) {}

    request_deadline(const request_deadline&) = delete;
    request_deadline& operator=(const request_deadline&) = delete;
    request_deadline(request_deadline&&) = delete;
    request_deadline& operator=(request_deadline&&) = delete;

    // Starts the clock. `worker` must outlive this object; the session that owns
    // the request guarantees that.
    void arm(const worker_handle& worker_value, std::chrono::milliseconds deadline_value) {
        worker_value.schedule_timer(timer_, worker_timer_deadline_after(deadline_value), [this](worker_timer_outcome outcome) {
            if (outcome != worker_timer_outcome::expired) {
                return;
            }
            exceeded_ = true;
            source_.request_stop();
        });
    }

    // Held by value and handed out by reference: context_services stores the
    // token by address, so returning a temporary here would dangle.
    [[nodiscard]] const stop_token& token() const noexcept {
        return token_;
    }

    // Distinguishes "the deadline elapsed" from "the worker is shutting down",
    // which the token alone cannot. A handler that catches the cancellation its
    // own await raised needs this to know not to press on.
    [[nodiscard]] bool exceeded() const noexcept {
        return exceeded_;
    }

private:
    // Declaration order IS the safety argument: both the timer registration and
    // the worker-stop registration hold a callback capturing `this` and are
    // destroyed before source_, so neither can fire into a dead source. The
    // timer's destructor cancels a still-pending entry, which is what keeps a
    // request that finished early from leaving one armed.
    stop_source source_;
    stop_token token_;
    stop_registration worker_link_;
    worker_timer_registration timer_;
    bool exceeded_{false};
};

// The strictest of the deployment's handler deadline and the route's own, with
// 0/absent meaning "not declared". A route may only tighten, never extend.
[[nodiscard]] inline std::chrono::milliseconds effective_handler_deadline(
    const std::optional<std::chrono::milliseconds>& app_deadline,
    std::int64_t route_deadline_ms) noexcept {
    const auto route = route_deadline_ms > 0 ? std::optional<std::chrono::milliseconds>(
                                                   std::chrono::milliseconds(route_deadline_ms))
                                             : std::nullopt;
    if (!app_deadline.has_value()) {
        return route.value_or(std::chrono::milliseconds::zero());
    }
    if (!route.has_value()) {
        return *app_deadline;
    }
    return *route < *app_deadline ? *route : *app_deadline;
}

}  // namespace ruvia::detail
