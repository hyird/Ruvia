#include "ruvia/core/WorkerRuntimeContext.h"

#include <memory>
#include <utility>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"

namespace ruvia {

class WorkerRuntimeContext::Impl final {
public:
    Impl(asio::io_context& ioContext, std::size_t mailboxCapacity)
        : ioContext_(&ioContext),
          dispatcher_(std::make_shared<detail::WorkerDispatcher>(ioContext, mailboxCapacity)),
          handle_(detail::WorkerHandleAccess::make(dispatcher_)) {}

    ~Impl() {
        detach();
    }

    void detach() noexcept {
        dispatcher_->detachContext();
    }

    asio::io_context* ioContext_;
    std::shared_ptr<detail::WorkerDispatcher> dispatcher_;
    WorkerHandle handle_;
};

WorkerRuntimeContext::WorkerRuntimeContext(
    asio::io_context& ioContext, std::size_t mailboxCapacity)
    : impl_(std::make_unique<Impl>(ioContext, mailboxCapacity)) {}

WorkerRuntimeContext::~WorkerRuntimeContext() = default;

asio::io_context& WorkerRuntimeContext::ioContext() const noexcept {
    return *impl_->ioContext_;
}

const WorkerHandle& WorkerRuntimeContext::handle() const noexcept {
    return impl_->handle_;
}

void WorkerRuntimeContext::run() {
    impl_->dispatcher_->runContext();
}

void WorkerRuntimeContext::run(MoveOnlyFunction<void(std::exception_ptr)> failureHandler) {
    impl_->dispatcher_->runContext(std::move(failureHandler));
}

void WorkerRuntimeContext::run(MoveOnlyFunction<void()> startupHandler,
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
    MoveOnlyFunction<void()> shutdownHandler) {
    impl_->dispatcher_->runContext(
        std::move(startupHandler), std::move(failureHandler), std::move(shutdownHandler));
}

void WorkerRuntimeContext::close() noexcept {
    impl_->dispatcher_->close();
}

void WorkerRuntimeContext::deferOrTerminate(MoveOnlyFunction<void()> task) noexcept {
    impl_->dispatcher_->deferOrTerminate(std::move(task));
}

void WorkerRuntimeContext::stopTimers() noexcept {
    impl_->dispatcher_->stopTimers();
}

void WorkerRuntimeContext::detach() noexcept {
    impl_->dispatcher_->detachContext();
}

}  // namespace ruvia
