#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {

class WorkerShutdownListener {
public:
    virtual ~WorkerShutdownListener() = default;
    virtual void workerStopping() noexcept = 0;
    virtual void workerStoppingComplete() noexcept {}
};

class WorkerDispatcher final : public std::enable_shared_from_this<WorkerDispatcher> {
public:
    WorkerDispatcher(asio::io_context& ioContext, std::size_t capacity);
    ~WorkerDispatcher();

    WorkerDispatcher(const WorkerDispatcher&) = delete;
    WorkerDispatcher& operator=(const WorkerDispatcher&) = delete;

    [[nodiscard]] PostResult post(MoveOnlyFunction<void()> task);
    [[nodiscard]] PostStatus postFactory(MoveOnlyFunction<MoveOnlyFunction<void()>()> factory);
    void defer(MoveOnlyFunction<void()> task);
    [[nodiscard]] bool deferIfAttached(MoveOnlyFunction<void()> task);
    void deferOrTerminate(MoveOnlyFunction<void()> task) noexcept;
    void registerShutdownListener(const std::shared_ptr<WorkerShutdownListener>& listener);
    // Runs after the first stopping batch has notified every listener, or now if
    // no batch is active. The active bit is published with accepting=false.
    void whenShutdownNotificationsComplete(MoveOnlyFunction<void()> callback);
    void whenIdle(MoveOnlyFunction<void()> callback);
    void scheduleTimer(WorkerTimerRegistration& registration,
        std::chrono::steady_clock::time_point deadline,
        MoveOnlyFunction<void(WorkerTimerOutcome)> completion);
    void requestTimerCancellation(std::size_t slot, std::uint64_t generation, bool notify) noexcept;
    void cancelTimer(std::size_t slot, std::uint64_t generation, bool notify) noexcept;
    void stopTimers() noexcept;
    // Runs the owned/attached io_context with a thread-local worker identity so
    // worker-affine hot paths do not lock merely to prove their current worker.
    // External owners that run an attached context directly retain the safe
    // executor-based fallback in isCurrent().
    void runContext();
    // Invokes failureHandler exactly once for the first escaping handler
    // exception while worker identity is still active, then re-enters run() to
    // drain shutdown continuations. A successfully returning handler consumes
    // the failure; a throwing handler is rethrown only after the drain ends.
    void runContext(MoveOnlyFunction<void(std::exception_ptr)> failureHandler);
    // Runs startup after worker identity is established and shutdown after the
    // context has drained but before that identity is cleared. Startup failures
    // enter the same first-failure path as handler failures. Shutdown is a
    // terminal cleanup hook and must not throw.
    void runContext(MoveOnlyFunction<void()> startupHandler,
        MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
        MoveOnlyFunction<void()> shutdownHandler);
    void close() noexcept;
    // Called after worker activity is serialized with teardown (by a joined
    // pool thread, an attached context's terminal handler, or its context
    // service). Handles remain safe terminal endpoints.
    void detachContext() noexcept;
    void waitForReservations() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

private:
    using ShutdownListeners = std::pmr::vector<std::weak_ptr<WorkerShutdownListener>>;
    using IdleCallbacks = std::pmr::vector<MoveOnlyFunction<void()>>;
    struct ShutdownBatch final {
        ShutdownListeners listeners{processResource()};
        bool active{false};
    };

    [[nodiscard]] IdleCallbacks takeIdleCallbacksLocked();
    static void notifyIdle(IdleCallbacks callbacks) noexcept;
    [[nodiscard]] ShutdownBatch beginStopping(bool abandonDrain) noexcept;
    void notifyStopping(ShutdownBatch batch) noexcept;
    void abandonQueued() noexcept;
    void publish(std::size_t index);
    void rollbackReserved(std::size_t index) noexcept;
    void drain();
    void armTimer();
    void fireTimers();
    [[nodiscard]] bool hasTimer(std::size_t slot, std::uint64_t generation) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia::detail
