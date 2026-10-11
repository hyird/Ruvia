#pragma once

#include <mutex>
#include <optional>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_runtime_context.h"

namespace ruvia::detail {

// The one stop/finalize sequence of a worker, shared by worker_runtime and by
// event loops attached to a caller-owned io_context.
//
// request_stop() closes dispatcher admission. Once the shutdown notification
// batch has completed, the owner thread runs the stop control: stop_admission_
// (or an immediate finalize() without one), then timer cancellation.
// finalize() makes the owner run finalize_ and then release the work guard
// that keeps the context running. The io_context is never stopped.
class worker_stop_chain final {
public:
    struct hooks_type final {
        move_only_function<void()> stop_admission_{};
        // The chain's last access to itself happens before this hook, which
        // may destroy the chain; the work guard is released after it returns.
        move_only_function<void()> finalize_{};
    };

    explicit worker_stop_chain(worker_runtime_context& context);

    worker_stop_chain(const worker_stop_chain&) = delete;
    worker_stop_chain& operator=(const worker_stop_chain&) = delete;
    worker_stop_chain(worker_stop_chain&&) = delete;
    worker_stop_chain& operator=(worker_stop_chain&&) = delete;

    // Once, before the chain is published or stopped.
    void configure(hooks_type hooks) noexcept;
    void request_stop() noexcept;
    // The first call wins; it also requests stop.
    void finalize() noexcept;
    // Cold-path owner control. Returns false once the work guard is released.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // The context is shutting down and will run no further handler: cancels
    // timers and releases the work guard without finalize_.
    void abandon() noexcept;

private:
    using work_guard_type = asio::executor_work_guard<asio::io_context::executor_type>;

    void defer_stop_control_locked() noexcept;
    void apply_stop() noexcept;

    worker_runtime_context& context_;
    std::mutex mutex_;
    work_guard_type work_;
    hooks_type hooks_;
    bool stop_requested_{};
    // At most one stop control is pending at a time, so finalize_ may destroy
    // the chain: finalize() defers another only after the first one has run.
    bool stop_control_ran_{};
    bool finalization_requested_{};
    bool work_released_{};
    // Owner thread only.
    bool stop_applied_{};
};

}  // namespace ruvia::detail
