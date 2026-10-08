#pragma once

#include <cstddef>
#include <exception>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSubmissionView.h"

namespace ruvia {

// Owns the worker dispatcher endpoint and detaches escaped handles on teardown.
// The io_context remains owned by the runtime that supplied it.
class WorkerRuntimeContext final {
public:
    WorkerRuntimeContext(asio::io_context& ioContext, std::size_t queue_capacity);
    ~WorkerRuntimeContext();

    WorkerRuntimeContext(const WorkerRuntimeContext&) = delete;
    WorkerRuntimeContext& operator=(const WorkerRuntimeContext&) = delete;
    WorkerRuntimeContext(WorkerRuntimeContext&&) = delete;
    WorkerRuntimeContext& operator=(WorkerRuntimeContext&&) = delete;

    [[nodiscard]] asio::io_context& ioContext() const noexcept;
    [[nodiscard]] const WorkerHandle& handle() const noexcept;
    [[nodiscard]] WorkerSubmissionView submission() const& noexcept;
    WorkerSubmissionView submission() const&& = delete;

    void run();
    void run(MoveOnlyFunction<void(std::exception_ptr)> failureHandler);
    void run(MoveOnlyFunction<void()> startupHandler,
        MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
        MoveOnlyFunction<void()> shutdownHandler);

    void close() noexcept;
    void deferOrTerminate(MoveOnlyFunction<void()> task) noexcept;
    void stopTimers() noexcept;
    void detach() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
