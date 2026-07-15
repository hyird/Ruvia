#include <ruvia/core/WorkerHandle.h>

#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerTimer.h>

#include <stdexcept>

namespace ruvia {

WorkerHandle::WorkerHandle(std::shared_ptr<detail::WorkerDispatcher> dispatcher) noexcept
    : dispatcher_(std::move(dispatcher)) {}

bool WorkerHandle::valid() const noexcept {
    return dispatcher_ && dispatcher_->attached();
}

bool WorkerHandle::accepting() const noexcept {
    return dispatcher_ && dispatcher_->accepting();
}

bool WorkerHandle::isCurrent() const noexcept {
    return dispatcher_ && dispatcher_->isCurrent();
}

WorkerId WorkerHandle::id() const noexcept {
    return dispatcher_ ? dispatcher_->id() : 0;
}

PostResult WorkerHandle::postTask(std::move_only_function<void()> task) const {
    return dispatcher_
        ? dispatcher_->post(std::move(task))
        : PostResult::kWorkerStopping;
}

WorkerHandle detail::WorkerHandleAccess::make(
    const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept {
    return WorkerHandle(dispatcher);
}

void detail::WorkerHandleAccess::defer(
    const WorkerHandle& worker,
    std::move_only_function<void()> task) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("worker stopped before internal continuation was scheduled");
    }
    dispatcher->defer(std::move(task));
}

void detail::WorkerHandleAccess::registerShutdownListener(
    const WorkerHandle& worker,
    const std::shared_ptr<WorkerShutdownListener>& listener) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot register state on a stopped worker");
    }
    dispatcher->registerShutdownListener(listener);
}

detail::WorkerTimerRegistration detail::WorkerHandleAccess::scheduleTimer(
    const WorkerHandle& worker,
    std::chrono::steady_clock::time_point deadline,
    std::move_only_function<void(WorkerTimerOutcome)> completion) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    return dispatcher->scheduleTimer(deadline, std::move(completion));
}

}
