#include "worker_stop_chain.h"

#include <exception>
#include <utility>

#include "ruvia/core/detail/worker/worker_dispatcher.h"

namespace ruvia::detail {

worker_stop_chain::worker_stop_chain(worker_runtime_context& context)
    : context_(context),
      work_(asio::make_work_guard(context.io_context())) {}

void worker_stop_chain::configure(hooks_type hooks) noexcept {
    const std::lock_guard lock(mutex_);
    hooks_ = std::move(hooks);
}

void worker_stop_chain::request_stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
    }
    context_.close();
    try {
        // close() can lose the race to a dispatcher failure that is still
        // notifying listeners. Keep the work guard until all of them have
        // published their cleanup.
        worker_handle_access::when_shutdown_notifications_complete(context_.handle(), [this] {
            const std::lock_guard lock(mutex_);
            defer_stop_control_locked();
        });
    } catch (...) {
        std::terminate();
    }
}

void worker_stop_chain::finalize() noexcept {
    request_stop();
    const std::lock_guard lock(mutex_);
    if (finalization_requested_) {
        return;
    }
    finalization_requested_ = true;
    // A stop control that has not yet run consumes this request itself.
    if (stop_control_ran_ && !work_released_) {
        defer_stop_control_locked();
    }
}

bool worker_stop_chain::post_control(move_only_function<void()> control) noexcept {
    const std::lock_guard lock(mutex_);
    if (work_released_) {
        return false;
    }
    context_.defer_or_terminate(std::move(control));
    return true;
}

void worker_stop_chain::abandon() noexcept {
    std::optional<work_guard_type> work;
    {
        const std::lock_guard lock(mutex_);
        stop_requested_ = true;
        work_released_ = true;
        work.emplace(std::move(work_));
    }
    context_.stop_timers();
}

void worker_stop_chain::defer_stop_control_locked() noexcept {
    context_.defer_or_terminate([this] { apply_stop(); });
}

void worker_stop_chain::apply_stop() noexcept {
    if (!stop_applied_) {
        stop_applied_ = true;
        try {
            if (hooks_.stop_admission_) {
                hooks_.stop_admission_();
            } else {
                finalize();
            }
        } catch (...) {
            std::terminate();
        }
        context_.stop_timers();
    }
    // Declared before the hook so the work guard is released after it.
    std::optional<work_guard_type> work;
    move_only_function<void()> finalize_hook;
    {
        const std::lock_guard lock(mutex_);
        stop_control_ran_ = true;
        if (!finalization_requested_ || work_released_) {
            return;
        }
        work_released_ = true;
        work.emplace(std::move(work_));
        finalize_hook = std::move(hooks_.finalize_);
    }
    try {
        if (finalize_hook) {
            finalize_hook();
        }
    } catch (...) {
        std::terminate();
    }
}

}  // namespace ruvia::detail
