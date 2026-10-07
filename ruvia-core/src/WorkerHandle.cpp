#include "ruvia/core/WorkerHandle.h"

#include <stdexcept>

#include "ruvia/core/WorkerTimer.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
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

void detail::WorkerHandleAccess::whenShutdownNotificationsComplete(
    const WorkerHandle& worker, MoveOnlyFunction<void()> callback) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe shutdown notifications on a stopped worker");
    }
    dispatcher->whenShutdownNotificationsComplete(std::move(callback));
}

void detail::WorkerHandleAccess::whenIdle(
    const WorkerHandle& worker, MoveOnlyFunction<void()> callback) {
    const auto& dispatcher = worker.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe idleness on a stopped worker");
    }
    dispatcher->whenIdle(std::move(callback));
}

void detail::WorkerHandleAccess::waitForReservations(const WorkerHandle& worker) noexcept {
    if (worker.dispatcher_) {
        worker.dispatcher_->waitForReservations();
    }
}

void WorkerHandle::schedule_timer(WorkerTimerRegistration& registration,
    std::chrono::steady_clock::time_point deadline,
    MoveOnlyFunction<void(WorkerTimerOutcome)> completion) const& {
    if (!dispatcher_) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    dispatcher_->scheduleTimer(registration, deadline, std::move(completion));
}

PostStatus WorkerHandle::post_factory(MoveOnlyFunction<MoveOnlyFunction<void()>()> factory) const {
    return dispatcher_ ? dispatcher_->postFactory(std::move(factory)) : PostStatus::kWorkerStopping;
}

}  // namespace ruvia
