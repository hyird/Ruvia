#pragma once

#include <cstddef>
#include <chrono>
#include <memory>
#include <vector>

#include <asio/io_context.hpp>

#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/WorkerTimer.h>

namespace ruvia::detail {

class WorkerShutdownListener {
public:
    virtual ~WorkerShutdownListener() = default;
    virtual void workerStopping() noexcept = 0;
};

class WorkerDispatcher final : public std::enable_shared_from_this<WorkerDispatcher> {
public:
    WorkerDispatcher(asio::io_context& ioContext, std::size_t capacity);
    ~WorkerDispatcher();

    WorkerDispatcher(const WorkerDispatcher&) = delete;
    WorkerDispatcher& operator=(const WorkerDispatcher&) = delete;

    [[nodiscard]] PostResult post(MoveOnlyFunction<void()> task);
    void defer(MoveOnlyFunction<void()> task);
    void deferOrTerminate(MoveOnlyFunction<void()> task) noexcept;
    void registerShutdownListener(const std::shared_ptr<WorkerShutdownListener>& listener);
    void scheduleTimer(
        WorkerTimerRegistration& registration,
        std::chrono::steady_clock::time_point deadline,
        MoveOnlyFunction<void(WorkerTimerOutcome)> completion);
    void requestTimerCancellation(
        std::size_t slot, std::uint64_t generation) noexcept;
    void cancelTimer(std::size_t slot, std::uint64_t generation) noexcept;
    void stopTimers() noexcept;
    // Runs the owned/attached io_context with a thread-local worker identity so
    // worker-affine hot paths do not lock merely to prove their current worker.
    // External owners that run an attached context directly retain the safe
    // executor-based fallback in isCurrent().
    void runContext();
    void close() noexcept;
    // Called by the execution-context owner after all worker work has joined and
    // before the io_context is destroyed. Handles remain safe terminal endpoints.
    void detachContext() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

private:
    using ShutdownListeners =
        std::vector<std::weak_ptr<WorkerShutdownListener>>;

    [[nodiscard]] ShutdownListeners beginStopping(bool abandonDrain) noexcept;
    static void notifyStopping(const ShutdownListeners& listeners) noexcept;
    void drain();
    void armTimer();
    void fireTimers();
    [[nodiscard]] bool hasTimer(
        std::size_t slot, std::uint64_t generation) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
