#pragma once

#include <cstddef>
#include <chrono>
#include <functional>
#include <memory>

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
    void registerShutdownListener(const std::shared_ptr<WorkerShutdownListener>& listener);
    [[nodiscard]] WorkerTimerRegistration scheduleTimer(
        std::chrono::steady_clock::time_point deadline,
        std::move_only_function<void(bool)> completion);
    void cancelTimer(const std::shared_ptr<WorkerTimerState>& state) noexcept;
    void stopTimers() noexcept;
    void close() noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

private:
    void drain();
    void armTimer();
    void fireTimers();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
