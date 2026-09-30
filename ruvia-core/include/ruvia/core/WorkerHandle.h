#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "ruvia/core/WorkerPostTypes.h"
#include "ruvia/core/WorkerTimer.h"

namespace ruvia {

using WorkerId = std::uint64_t;

namespace detail {
class WorkerDispatcher;
class WorkerShutdownListener;
struct WorkerHandleAccess;
}  // namespace detail

class WorkerHandle {
public:
    WorkerHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        if constexpr (detail::MoveOnlyFunctionBorrowSafeInput<void(), Fn>) {
            return postTask(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const WorkerHandle snapshot = *this;
            return snapshot.postTask(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
        }
    }

private:
    explicit WorkerHandle(std::shared_ptr<detail::WorkerDispatcher> dispatcher) noexcept;
    [[nodiscard]] PostResult postTask(MoveOnlyFunction<void()> task) const;

    // Owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::WorkerDispatcher> dispatcher_;
    friend struct detail::WorkerHandleAccess;
};

namespace detail {

struct WorkerHandleAccess {
    [[nodiscard]] static WorkerHandle make(
        const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept;
    static void defer(const WorkerHandle& worker, MoveOnlyFunction<void()> task);
    [[nodiscard]] static bool deferIfAttached(
        const WorkerHandle& worker, MoveOnlyFunction<void()> task);
    static void deferOrTerminate(
        const WorkerHandle& worker, MoveOnlyFunction<void()> task) noexcept;
    static void registerShutdownListener(
        const WorkerHandle& worker, const std::shared_ptr<WorkerShutdownListener>& listener);
    static void scheduleTimer(const WorkerHandle& worker, ::ruvia::WorkerTimerRegistration& registration,
        std::chrono::steady_clock::time_point deadline,
        MoveOnlyFunction<void(::ruvia::WorkerTimerOutcome)> completion);
    [[nodiscard]] static PostStatus postFactory(
        const WorkerHandle& worker, MoveOnlyFunction<MoveOnlyFunction<void()>()> factory);
};

}  // namespace detail

}  // namespace ruvia
