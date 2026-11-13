#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <variant>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_runtime.h"

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
    void run(move_only_function<void()> startup_handler,
        move_only_function<void(std::exception_ptr)> failure_handler,
        move_only_function<void()> shutdown_handler);

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

namespace ruvia::detail {

class external_context_attachment_service;
struct event_loop_state;

// Retirement gate of one loop: root tasks, stop callbacks, and cleanup each
// hold a pending count; finalize runs once the gate is sealed and drained.
class event_loop_retirement final {
public:
    event_loop_retirement() = default;

    [[nodiscard]] bool begin_stop(move_only_function<void()> finalize) noexcept {
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            if (sealed_ || finalized_) {
                return false;
            }
            std::swap(finalize_, finalize);
        }
        return true;
    }

    [[nodiscard]] bool try_begin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    [[nodiscard]] bool try_begin_cleanup() noexcept {
        const std::lock_guard lock(mutex_);
        if (finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    void begin() noexcept {
        if (!try_begin()) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<void> try_acquire(const std::shared_ptr<event_loop_state>& owner) {
        {
            const std::lock_guard lock(mutex_);
            if (finalized_ || root_admission_closed_ || sealed_) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::make_shared<lease_token_type>(owner);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            take_finalize_locked(finalize);
        }
        if (finalize) {
            finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void close_admission() noexcept {
        move_only_function<void()> abandoned;
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            sealed_ = true;
            finalized_ = true;
            std::swap(abandoned, finalize_);
        }
    }

    void seal() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            take_finalize_locked(finalize);
        }
        if (finalize) {
            finalize();
        }
    }

private:
    // Both drain and seal claim finalization under the gate mutex; the caller
    // invokes the claimed callback only after releasing that mutex.
    void take_finalize_locked(move_only_function<void()>& finalize) noexcept {
        if (sealed_ && pending_ == 0 && !finalized_) {
            finalized_ = true;
            finalize = std::move(finalize_);
        }
    }

    struct lease_token_type final {
        explicit lease_token_type(std::shared_ptr<event_loop_state> owner_value) noexcept
            : owner_(std::move(owner_value)) {}
        ~lease_token_type();
        std::shared_ptr<event_loop_state> owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool root_admission_closed_{false};
    bool sealed_{false};
    bool finalized_{false};
    move_only_function<void()> finalize_;
};

// The single Ruvia attachment claim on a caller-owned io_context.
class external_context_claim final {
public:
    explicit external_context_claim(asio::io_context& io_context);
    ~external_context_claim();

    external_context_claim(const external_context_claim&) = delete;
    external_context_claim& operator=(const external_context_claim&) = delete;

    // Hands the state to the context service, which retires it at the latest
    // when the context shuts down.
    void retain(std::shared_ptr<event_loop_state> state_value) noexcept;
    // Releases the retained state and the claim once the state has retired.
    void release(event_loop_state* state_value) noexcept;

private:
    external_context_attachment_service* service_;
    bool retained_{false};
};

// A caller-owned io_context bound to one loop: its claim, the worker endpoint,
// and the same worker stop chain that a pool loop's worker_runtime owns.
struct attached_context final {
    attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool,
        move_only_function<void(std::exception_ptr)> failure);

    external_context_claim claim_;
    worker_runtime_context runtime_;
    worker_stop_chain stop_;
    move_only_function<void(std::exception_ptr)> failure_;
};

// A standalone worker_runtime that exposes its loop through
// worker_runtime::loop(); the runtime outlives every handle to the loop.
struct borrowed_runtime final {
    worker_runtime* runtime_;
};

// One loop's runtime and retirement. A pool loop owns a worker_runtime; an
// attached loop owns the same worker stop chain over a caller-owned context;
// a borrowed loop gates the stop admission of a standalone worker_runtime.
// Both stop through stop(): it seals the retirement gate on the stopping
// thread and requests the worker stop chain, whose owner-thread stop control
// re-enters stop(); the drained gate finalizes the chain on the owner.
struct event_loop_state final : std::enable_shared_from_this<event_loop_state> {
    using context_ownership_type = std::variant<worker_runtime, attached_context, borrowed_runtime>;

    event_loop_state(std::size_t queue_capacity, blocking_pool* pool);
    explicit event_loop_state(worker_runtime& runtime);
    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool,
        move_only_function<void(std::exception_ptr)> failure);

    [[nodiscard]] worker_runtime& owned_runtime() noexcept {
        return *std::get_if<worker_runtime>(&context_ownership_);
    }
    // Pool loops only: the runtime's first failure, without observing it.
    [[nodiscard]] std::exception_ptr recorded_failure() noexcept {
        return owned_runtime().recorded_failure();
    }

    // Attached loops only: hooks the stop chain and hands the state to the
    // context service, which retires it at the latest when the context shuts
    // down.
    void attach(const std::shared_ptr<event_loop_state>& self);
    void install_shutdown_registration();

    void worker_stopping_complete() noexcept;

    // Delivers to the bound failure sink; only a state whose sink is not yet
    // bound (pool construction rollback) reports under `unbound_context`.
    void route_failure(std::exception_ptr failure, const char* unbound_context) noexcept;
    void report_attached_failure(std::exception_ptr failure) noexcept;
    [[noreturn]] void fail_teardown_start(std::exception_ptr failure) noexcept;

    void stop() noexcept;
    void complete_stop_admission() noexcept;
    void finalize_stop() noexcept;
    // Asio service shutdown of an attached context: retires the worker before
    // the context destroys its scheduler.
    void shutdown_external_context() noexcept;

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    context_ownership_type context_ownership_;
    asio::io_context* const io_context_;
    asio::io_context::executor_type executor_;
    worker_runtime_context& runtime_;
    std::atomic_bool stopping_{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Root tasks and stop callbacks borrow it through the loop state
    // instead of copying it.
    move_only_function<void(std::exception_ptr)> failure_sink_;
    event_loop_retirement retirement_;

private:
    void schedule_finalize_when_idle() noexcept;
    void defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept;
};

inline event_loop_retirement::lease_token_type::~lease_token_type() {
    owner_->retirement_.finish();
}

}  // namespace ruvia::detail
