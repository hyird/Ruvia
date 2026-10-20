#pragma once

#include <optional>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
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

    [[nodiscard]] task<void> wait() {
        if (terminated()) {
            co_return;
        }
        co_await signal_.wait();
    }

private:
    worker_signal signal_;
    http2_sans_io_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
};

}  // namespace ruvia::detail
