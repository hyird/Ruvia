#pragma once

#include <cstddef>
#include <chrono>
#include <functional>
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

    [[nodiscard]] PostResult post(std::move_only_function<void()> task);
    void defer(std::move_only_function<void()> task);
    void deferOrTerminate(std::move_only_function<void()> task) noexcept;
    void registerShutdownListener(const std::shared_ptr<WorkerShutdownListener>& listener);
    [[nodiscard]] WorkerTimerRegistration scheduleTimer(
        std::chrono::steady_clock::time_point deadline,
        std::move_only_function<void(WorkerTimerOutcome)> completion);
    void requestTimerCancellation(
        const std::shared_ptr<WorkerTimerState>& state) noexcept;
    void cancelTimer(const std::shared_ptr<WorkerTimerState>& state) noexcept;
    void stopTimers() noexcept;
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

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
