#pragma once

#include <cstddef>
#include <exception>
#include <memory>

#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/RuntimeLifecycle.h"
#include "ruvia/core/WorkerRuntimeContext.h"

namespace ruvia {

enum class worker_io_policy { synchronized,
    single_owner };

struct worker_runtime_options final {
    std::size_t mailbox_capacity{4096};
    worker_io_policy io_policy{worker_io_policy::synchronized};
};

struct worker_runtime_hooks final {
    MoveOnlyFunction<void()> startup{};
    // Runs on the owner after dispatcher admission and shutdown notifications
    // close. A custom policy must eventually call finalize(), after its external
    // producers are quiescent. Without a policy, stopping finalizes immediately.
    MoveOnlyFunction<void()> stop_admission{};
    MoveOnlyFunction<void(std::exception_ptr)> failure{};
    // Terminal owner-thread cleanup, after I/O and completion handlers drain.
    MoveOnlyFunction<void()> shutdown{};
};

// The unique owner of a worker's thread, io_context, dispatcher, and generic
// startup/stop/drain/join lifecycle. Protocol/application state is composed via
// hooks; it must remain alive through join(). No hook may throw except startup.
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
    void request_stop() noexcept;
    // Cold-path lifecycle control, serialized with finalization. Returns false
    // once stopping starts; it cannot strand a control after the thread barrier.
    [[nodiscard]] bool post_control(MoveOnlyFunction<void()> control) noexcept;
    // A reliable owner-thread phase-two control. The first call wins. Its
    // callback retires domain resources before the work guard is released.
    // Pending I/O/continuations still drain; the io_context is never stopped.
    void finalize(MoveOnlyFunction<void()> cleanup = {}) noexcept;
    // Establishes the thread barrier, including owner-affine draining when
    // stopped before start. Concurrent joins wait for the same barrier. This
    // does not rethrow run failures: inspect/rethrow them after all owners join.
    void join();
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrow_failure() const;
    [[nodiscard]] RuntimeLifecycle::State state() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] WorkerRuntimeContext& context() & noexcept;
    WorkerRuntimeContext& context() && = delete;

private:
    class impl;
    struct impl_deleter final {
        void operator()(impl* value) const noexcept;
    };
    std::unique_ptr<impl, impl_deleter> impl_;
};

}  // namespace ruvia
