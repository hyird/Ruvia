#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include "move_only_function.h"
#include "worker_ref.h"

namespace ruvia {

class blocking_pool;

// Owns the worker dispatcher endpoint and detaches escaped worker refs on teardown.
// The io_context remains owned by the runtime that supplied it, and the
// blocking pool, when given, by its owner; both must outlive this context.
class worker_runtime_context final {
public:
    // `pool` is borrowed for run_blocking() on this worker and must outlive it;
    // null leaves the worker without one.
    worker_runtime_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool = nullptr);
    ~worker_runtime_context();

    worker_runtime_context(const worker_runtime_context&) = delete;
    worker_runtime_context& operator=(const worker_runtime_context&) = delete;
    worker_runtime_context(worker_runtime_context&&) = delete;
    worker_runtime_context& operator=(worker_runtime_context&&) = delete;

    [[nodiscard]] asio::io_context& io_context() const& noexcept;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    const worker_ref& worker() const&& = delete;

    void run();
    void run(move_only_function<void()> startup_handler, move_only_function<void(std::exception_ptr)> failure_handler, move_only_function<void()> shutdown_handler);

    void close() noexcept;
    void stop_timers() noexcept;
    void detach() noexcept;

private:
    asio::io_context* io_context_;
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    worker_ref worker_;
};

}  // namespace ruvia

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

    // Once, before the chain is published or stopped; nothing else touches
    // the hooks yet, so they move in without the chain mutex.
    void configure(hooks_type hooks) noexcept;
    void request_stop() noexcept;
    // The first call wins; it also requests stop.
    void finalize() noexcept;
    // Cold-path owner control. Returns false once the work guard is released;
    // an accepted control is queued while the guard still keeps the context
    // running, but moves into the queue without the chain mutex.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // The context is shutting down and will run no further handler: cancels
    // timers and releases the work guard without finalize_.
    void abandon() noexcept;

private:
    using work_guard_type = asio::executor_work_guard<asio::io_context::executor_type>;

    // Queues a control whose post was counted in control_posts_ under the
    // chain mutex; the post itself runs unlocked.
    void post_counted_control(move_only_function<void()> control) noexcept;
    void post_stop_control() noexcept;
    // Refuses further controls, waits out accepted ones still being queued,
    // and hands the work guard to the caller to release unlocked.
    void release_work_locked(std::unique_lock<std::mutex>& lock, std::optional<work_guard_type>& work) noexcept;
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
    // Accepted post_control() calls still queuing their control unlocked.
    std::size_t control_posts_{};
    std::condition_variable control_posts_drained_;
    // Owner thread only.
    bool stop_applied_{};
};

}  // namespace ruvia::detail
