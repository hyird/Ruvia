#include <ruvia/core/WorkerHandle.h>

#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerTimer.h>

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

PostResult WorkerHandle::postTask(MoveOnlyFunction<void()> task) const {
    if (!task) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ ? dispatcher_->post(std::move(task))
                       : PostResult::reject(PostStatus::kWorkerStopping, std::move(task));
}

WorkerHandle detail::WorkerHandleAccess::make(
    const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept {
    return WorkerHandle(dispatcher);
}

void detail::WorkerHandleAccess::defer(const WorkerHandle& worker, MoveOnlyFunction<void()> task) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("worker stopped before internal continuation was scheduled");
    }
    dispatcher->defer(std::move(task));
}

bool detail::WorkerHandleAccess::deferIfAttached(
    const WorkerHandle& worker, MoveOnlyFunction<void()> task) {
    const auto& dispatcher = worker.dispatcher_;
    return dispatcher && dispatcher->deferIfAttached(std::move(task));
}

void detail::WorkerHandleAccess::deferOrTerminate(
    const WorkerHandle& worker, MoveOnlyFunction<void()> task) noexcept {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        std::terminate();
    }
    dispatcher->deferOrTerminate(std::move(task));
}

void detail::WorkerHandleAccess::registerShutdownListener(
    const WorkerHandle& worker, const std::shared_ptr<WorkerShutdownListener>& listener) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot register state on a stopped worker");
    }
    dispatcher->registerShutdownListener(listener);
}

void detail::WorkerHandleAccess::scheduleTimer(const WorkerHandle& worker,
    WorkerTimerRegistration& registration, std::chrono::steady_clock::time_point deadline,
    MoveOnlyFunction<void(WorkerTimerOutcome)> completion) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    dispatcher->scheduleTimer(registration, deadline, std::move(completion));
}

PostStatus detail::WorkerHandleAccess::postFactory(
    const WorkerHandle& worker, MoveOnlyFunction<MoveOnlyFunction<void()>()> factory) {
    const auto& dispatcher = worker.dispatcher_;
    return dispatcher ? dispatcher->postFactory(std::move(factory)) : PostStatus::kWorkerStopping;
}

}  // namespace ruvia
