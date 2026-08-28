#pragma once

#include <chrono>
#include <optional>
#include <cstdint>

#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"

// One request's handler deadline: a stop source that trips either when the
// worker begins stopping or when the deadline elapses, whichever comes first.
//
// The reason this is a stop source rather than a cancellation is that a
// suspended coroutine cannot be abandoned in C++ -- destroying its frame while
// an awaiter still points at it is a use-after-free. So the deadline stops the
// WAITS instead: everything already taking a StopToken returns at once, and the
// handler unwinds itself.

namespace ruvia::detail {

class RequestDeadline final {
public:
    // The worker link is established here rather than in arm(): StopRegistration
    // is neither movable nor assignable, so it can only be initialized. If the
    // worker is already stopping, registerCallback runs the callback at once and
    // this request starts already stopped -- which is correct.
    explicit RequestDeadline(const StopToken& workerStop)
        : token_(source_.token()),
          workerLink_(workerStop.registerCallback([this]() noexcept { source_.requestStop(); })) {}

    RequestDeadline(const RequestDeadline&) = delete;
    RequestDeadline& operator=(const RequestDeadline&) = delete;
    RequestDeadline(RequestDeadline&&) = delete;
    RequestDeadline& operator=(RequestDeadline&&) = delete;

    // Starts the clock. `worker` must outlive this object; the session that owns
    // the request guarantees that.
    void arm(const WorkerHandle& worker, std::chrono::milliseconds deadline) {
        WorkerHandleAccess::scheduleTimer(worker, timer_, workerTimerDeadlineAfter(deadline), [this](WorkerTimerOutcome outcome) {
            if (outcome != WorkerTimerOutcome::kExpired) {
                return;
            }
            exceeded_ = true;
            source_.requestStop();
        });
    }

    // Held by value and handed out by reference: ContextServices stores the
    // token by address, so returning a temporary here would dangle.
    [[nodiscard]] const StopToken& token() const noexcept {
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
    StopSource source_;
    StopToken token_;
    StopRegistration workerLink_;
    WorkerTimerRegistration timer_;
    bool exceeded_{false};
};

// The strictest of the deployment's handler deadline and the route's own, with
// 0/absent meaning "not declared". A route may only tighten, never extend.
[[nodiscard]] inline std::chrono::milliseconds effectiveHandlerDeadline(const std::optional<std::chrono::milliseconds>& appDeadline, std::int64_t routeDeadlineMs) noexcept {
    const auto route = routeDeadlineMs > 0 ? std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(routeDeadlineMs)) : std::nullopt;
    if (!appDeadline.has_value()) {
        return route.value_or(std::chrono::milliseconds::zero());
    }
    if (!route.has_value()) {
        return *appDeadline;
    }
    return *route < *appDeadline ? *route : *appDeadline;
}

}  // namespace ruvia::detail
