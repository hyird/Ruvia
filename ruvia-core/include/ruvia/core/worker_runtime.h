#pragma once

#include <cstddef>
#include <exception>
#include <memory>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/runtime_lifecycle.h"
#include "ruvia/core/worker_runtime_context.h"

namespace ruvia {

class worker_group;

enum class worker_io_policy { synchronized_value,
    single_owner };

struct worker_runtime_options final {
    std::size_t queue_capacity_{4096};
    worker_io_policy io_policy_{worker_io_policy::synchronized_value};
};

struct worker_runtime_hooks final {
    // Owner thread, once, at launch. Begins startup; the owner publishes
    // readiness with mark_ready() once its (possibly asynchronous) startup work
    // has completed.
    move_only_function<void()> startup_{};
    // Owner thread, once, after request_serve() follows readiness. Serving is
    // published when it returns; a throw fails the runtime.
    move_only_function<void()> serve_{};
    // Runs on the owner after dispatcher admission and shutdown notifications
    // close. A custom policy must eventually call finalize(), after its external
    // producers are quiescent. Without a policy, stopping finalizes immediately.
    move_only_function<void()> stop_admission_{};
    // Owner thread, once, when finalization releases the work guard. Retires
    // domain resources; pending I/O and continuations still drain afterwards.
    move_only_function<void()> finalize_{};
    // Runs once, on the stopping thread, when the runtime stops before its owner
    // thread launched. Releases state other runtimes staged into it; it must be
    // thread-safe. Owner-affine cleanup still drains in shutdown_ during join().
    move_only_function<void()> abandon_{};
    // Runs once, for the first failure only, on the thread that reported it:
    // the owner for escaped failures, the caller for fail().
    move_only_function<void(std::exception_ptr)> failure_{};
    // Terminal owner-thread cleanup, after I/O and completion handlers drain.
    move_only_function<void()> shutdown_{};
};

// The unique owner of a worker's thread, io_context, dispatcher, and generic
// startup/serve/stop/drain/join lifecycle, including its first failure.
// Protocol/application state is composed via hooks; it must remain alive
// through join(). No hook may throw except startup_ and serve_.
class worker_runtime final {
public:
    explicit worker_runtime(worker_runtime_options options = {});
    ~worker_runtime();

    worker_runtime(const worker_runtime&) = delete;
    worker_runtime& operator=(const worker_runtime&) = delete;
    worker_runtime(worker_runtime&&) = delete;
    worker_runtime& operator=(worker_runtime&&) = delete;

    // Configure once, before publishing the runtime or starting/stopping it.
    void configure(worker_runtime_hooks hooks);
    void start();
    // Two-phase startup barrier. Owner-published readiness; a stop first aborts
    // it. wait_until_ready() returns false once stopped before readiness and
    // rethrows the first failure. request_serve() requires readiness and runs
    // serve_ on the owner; wait_until_serving() mirrors wait_until_ready().
    void mark_ready() noexcept;
    [[nodiscard]] bool wait_until_ready();
    void request_serve();
    [[nodiscard]] bool wait_until_serving();
    // Every call closes dispatcher admission before returning. The winning
    // caller dispatches cold abandon once; repeated/reentrant calls do not wait
    // for those hooks to finish.
    void request_stop() noexcept;
    // Records the first failure, wakes phase waiters, and requests stop. Only
    // the winning failure reaches the failure_ hook. Returns true for it.
    bool fail(std::exception_ptr failure) noexcept;
    // Cold-path lifecycle control, serialized with finalization. Returns false
    // once stopping starts; it cannot strand a control after the thread barrier.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // A reliable owner-thread phase-two control; the first call wins. The
    // finalize_ hook retires domain resources before the work guard is
    // released. Pending I/O/continuations still drain; the io_context is never
    // stopped.
    void finalize() noexcept;
    // Establishes the thread barrier, including owner-affine draining when
    // stopped before start. Concurrent joins wait for the same barrier. This
    // does not rethrow run failures: inspect/rethrow them after all owners join.
    // Waits for any external stop dispatch before draining a cold owner. Joining
    // from its owner or from a thread currently dispatching its stop hooks
    // throws logic_error instead of waiting on itself.
    void join();
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrow_failure() const;
    [[nodiscard]] runtime_lifecycle::state_type state() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] worker_runtime_context& context() & noexcept;
    worker_runtime_context& context() && = delete;

private:
    friend class worker_group;
    void bind_group(worker_group& group);
    void unbind_group(worker_group& group) noexcept;

    class impl;
    struct impl_deleter final {
        void operator()(impl* value) const noexcept;
    };
    std::unique_ptr<impl, impl_deleter> impl_;
};

}  // namespace ruvia
