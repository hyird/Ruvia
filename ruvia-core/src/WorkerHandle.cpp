#include <ruvia/core/WorkerHandle.h>

#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerTimer.h>

#include <stdexcept>

namespace ruvia {

WorkerHandle::WorkerHandle(std::weak_ptr<detail::WorkerDispatcher> dispatcher) noexcept
    : dispatcher_(std::move(dispatcher)) {}

bool WorkerHandle::valid() const noexcept {
    return !dispatcher_.expired();
}

bool WorkerHandle::accepting() const noexcept {
    if (const auto dispatcher = dispatcher_.lock()) {
        return dispatcher->accepting();
    }
    return false;
}

bool WorkerHandle::isCurrent() const noexcept {
    if (const auto dispatcher = dispatcher_.lock()) {
        return dispatcher->isCurrent();
    }
    return false;
}

WorkerId WorkerHandle::id() const noexcept {
    if (const auto dispatcher = dispatcher_.lock()) {
        return dispatcher->id();
    }
    return 0;
}

PostResult WorkerHandle::postTask(std::move_only_function<void()> task) const {
    if (const auto dispatcher = dispatcher_.lock()) {
        return dispatcher->post(std::move(task));
    }
    return PostResult::kWorkerStopping;
}

WorkerHandle detail::WorkerHandleAccess::make(
    const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept {
    return WorkerHandle(dispatcher);
}

void detail::WorkerHandleAccess::defer(
    const WorkerHandle& worker,
    std::move_only_function<void()> task) {
    const auto dispatcher = worker.dispatcher_.lock();
    if (!dispatcher) {
        throw std::runtime_error("worker stopped before internal continuation was scheduled");
    }
    dispatcher->defer(std::move(task));
}

void detail::WorkerHandleAccess::registerShutdownListener(
    const WorkerHandle& worker,
    const std::shared_ptr<WorkerShutdownListener>& listener) {
    const auto dispatcher = worker.dispatcher_.lock();
    if (!dispatcher) {
        throw std::runtime_error("cannot register state on a stopped worker");
    }
    dispatcher->registerShutdownListener(listener);
}

detail::WorkerTimerRegistration detail::WorkerHandleAccess::scheduleTimer(
    const WorkerHandle& worker,
    std::chrono::steady_clock::time_point deadline,
    std::move_only_function<void(WorkerTimerOutcome)> completion) {
    const auto dispatcher = worker.dispatcher_.lock();
    if (!dispatcher) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    return dispatcher->scheduleTimer(deadline, std::move(completion));
}

}
