#pragma once

#include <coroutine>
#include <optional>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"

#include "http2/http2_sans_io_termination.h"

namespace ruvia::detail {

// The one asynchronous wake primitive owned by a dispatched Web stream. Body
// readers and send-window-blocked writers share it and always re-check their own
// readiness after wakeup, so cancellation is only a level-change notification.
class http2_sans_io_stream_signal final {
public:
    http2_sans_io_stream_signal(const worker_handle& worker_value, http2_sans_io_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    http2_sans_io_stream_signal(worker_handle&&, http2_sans_io_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // Only opt-in streaming consumers allocate a stop state. Other stream
    // readers retain the existing worker-local wake/termination path.
    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped_ || terminated()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] http2_sans_io_termination& termination() noexcept {
        return termination_;
    }

    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        wait_operation(wait_operation&&) noexcept = default;
        wait_operation& operator=(wait_operation&&) = delete;

        [[nodiscard]] bool await_ready() {
            return !wait_.has_value() || termination_->terminated() || wait_->await_ready();
        }
        bool await_suspend(std::coroutine_handle<> continuation) noexcept {
            return wait_->await_suspend(continuation);
        }
        void await_resume() noexcept {
            if (wait_.has_value()) {
                wait_->await_resume();
            }
        }

    private:
        friend class http2_sans_io_stream_signal;

        explicit wait_operation(const http2_sans_io_termination& termination) noexcept
            : termination_(&termination) {}
        wait_operation(
            const http2_sans_io_termination& termination, worker_signal::wait_operation&& wait_value) noexcept
            : termination_(&termination),
              wait_(std::move(wait_value)) {}

        const http2_sans_io_termination* termination_;
        std::optional<worker_signal::wait_operation> wait_;
    };

    [[nodiscard]] wait_operation wait() {
        if (terminated()) {
            return wait_operation(termination_);
        }
        return wait_operation(termination_, signal_.wait());
    }

private:
    worker_signal signal_;
    http2_sans_io_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
};

}  // namespace ruvia::detail
