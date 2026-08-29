#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"

namespace ruvia::detail {

// The one assembly point for a worker's execution context, dispatcher endpoint,
// and stable handle. Runtime owners remain responsible for threads and domain
// resources; this object guarantees that escaped handles are detached before
// the io_context they identify can disappear.
class WorkerRuntimeContext final {
public:
    WorkerRuntimeContext(asio::io_context& ioContext, std::size_t mailboxCapacity)
        : ioContext_(&ioContext),
          dispatcher_(std::make_shared<WorkerDispatcher>(ioContext, mailboxCapacity)),
          handle_(WorkerHandleAccess::make(dispatcher_)) {}

    ~WorkerRuntimeContext() {
        detach();
    }

    WorkerRuntimeContext(const WorkerRuntimeContext&) = delete;
    WorkerRuntimeContext& operator=(const WorkerRuntimeContext&) = delete;

    [[nodiscard]] asio::io_context& ioContext() const noexcept {
        return *ioContext_;
    }

    [[nodiscard]] const WorkerHandle& handle() const noexcept {
        return handle_;
    }

    [[nodiscard]] WorkerDispatcher& dispatcher() const noexcept {
        return *dispatcher_;
    }

    void run() {
        dispatcher_->runContext();
    }

    void run(MoveOnlyFunction<void(std::exception_ptr)> failureHandler) {
        dispatcher_->runContext(std::move(failureHandler));
    }

    void run(MoveOnlyFunction<void()> startupHandler,
        MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
        MoveOnlyFunction<void()> shutdownHandler) {
        dispatcher_->runContext(
            std::move(startupHandler), std::move(failureHandler), std::move(shutdownHandler));
    }

    void close() noexcept {
        dispatcher_->close();
    }

    void stopTimers() noexcept {
        dispatcher_->stopTimers();
    }

    void detach() noexcept {
        dispatcher_->detachContext();
    }

private:
    asio::io_context* ioContext_;
    std::shared_ptr<WorkerDispatcher> dispatcher_;
    WorkerHandle handle_;
};

}  // namespace ruvia::detail
