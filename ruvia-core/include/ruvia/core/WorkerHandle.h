#pragma once

#include <concepts>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include <ruvia/core/MoveOnlyFunction.h>

namespace ruvia {

using WorkerId = std::uint64_t;

enum class PostResult : std::uint8_t {
    kAccepted,
    kQueueFull,
    kWorkerStopping,
};

namespace detail {
class WorkerDispatcher;
class WorkerShutdownListener;
class WorkerTimerRegistration;
enum class WorkerTimerOutcome : std::uint8_t;
struct WorkerHandleAccess;
}

class WorkerHandle {
public:
    WorkerHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        return postTask(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
    }

private:
    explicit WorkerHandle(std::shared_ptr<detail::WorkerDispatcher> dispatcher) noexcept;
    [[nodiscard]] PostResult postTask(MoveOnlyFunction<void()> task) const;

    // A handle owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::WorkerDispatcher> dispatcher_;
    friend struct detail::WorkerHandleAccess;
};

namespace detail {

struct WorkerHandleAccess {
    [[nodiscard]] static WorkerHandle
    make(const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept;
    static void defer(const WorkerHandle& worker, MoveOnlyFunction<void()> task);
    static void deferOrTerminate(
        const WorkerHandle& worker,
        MoveOnlyFunction<void()> task) noexcept;
    static void registerShutdownListener(
        const WorkerHandle& worker,
        const std::shared_ptr<WorkerShutdownListener>& listener);
    static void scheduleTimer(
        const WorkerHandle& worker,
        WorkerTimerRegistration& registration,
        std::chrono::steady_clock::time_point deadline,
        MoveOnlyFunction<void(WorkerTimerOutcome)> completion);
};

}

}
