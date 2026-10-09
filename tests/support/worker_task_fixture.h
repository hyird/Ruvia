#pragma once

#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/co_spawn.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

namespace ruvia::test {

// Start roots on their handle's owner, drain every root, then close the owner.
// The roots use their own stop sources when exercising cancellation; the
// enclosing scope's stop request marks structured shutdown, not test input.
template <std::size_t Count>
task<void> join_worker_tasks(const worker_handle& worker,
    std::array<task<void>, Count> tasks) {
    task_scope scope(worker);
    std::exception_ptr failure;
    try {
        for (auto& root : tasks) {
            scope.spawn(std::move(root));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    scope.request_stop();
    co_await scope.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

template <typename... Tasks>
void run_worker_tasks(worker_runtime_context& runtime, Tasks&&... tasks) {
    std::exception_ptr failure;
    bool joined = false;
    asio::co_spawn(runtime.io_context(),
        as_awaitable(join_worker_tasks(runtime.handle(),
            std::array<task<void>, sizeof...(Tasks)>{std::forward<Tasks>(tasks)...})),
        [&](std::exception_ptr error) {
            failure = std::move(error);
            joined = true;
            runtime.close();
            runtime.stop_timers();
        });
    runtime.run();
    if (!joined) {
        throw std::logic_error("worker owner stopped before root tasks joined");
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace ruvia::test
