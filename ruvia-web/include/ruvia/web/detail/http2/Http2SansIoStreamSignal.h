#pragma once

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/detail/http2/Http2SansIoTermination.h"

namespace ruvia::detail {

// The one asynchronous wake primitive owned by a dispatched Web stream. Body
// readers and send-window-blocked writers share it and always re-check their own
// readiness after wakeup, so cancellation is only a level-change notification.
class Http2SansIoStreamSignal final {
public:
    Http2SansIoStreamSignal(
        const WorkerHandle& worker,
        Http2SansIoTermination& termination)
        : signal_(worker), termination_(termination) {}
    Http2SansIoStreamSignal(WorkerHandle&&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminalError() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] Http2SansIoTermination& termination() noexcept {
        return termination_;
    }

    [[nodiscard]] Task<void> wait() {
        if (terminated()) {
            co_return;
        }
        co_await signal_.wait();
    }

private:
    WorkerSignal signal_;
    Http2SansIoTermination& termination_;
};

}  // namespace ruvia::detail
