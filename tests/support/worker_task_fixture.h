#pragma once

#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"

namespace ruvia::test {

// Start roots on their handle's owner and drain every root. The roots use
// their own stop sources when exercising cancellation.
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
    co_await scope.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// The root task holds the loop's retirement open, so a test may stop the
// attachment while its roots still run; the loop retires once they joined.
template <std::size_t Count>
task<void> join_worker_tasks_then_stop(event_loop_attachment& attachment, const worker_handle& worker,
    std::array<task<void>, Count> tasks, bool& joined) {
    std::exception_ptr failure;
    try {
        co_await join_worker_tasks(worker, std::move(tasks));
    } catch (...) {
        failure = std::current_exception();
    }
    joined = true;
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Runs the roots on the attached loop, stops the attachment once they have
// joined, and drives the context until the loop retires.
template <typename... Tasks>
void run_worker_tasks(event_loop_attachment& attachment, Tasks&&... tasks) {
    bool joined = false;
    const auto loop = attachment.loop();
    auto root = loop.start(join_worker_tasks_then_stop(attachment, loop.handle(),
        std::array<task<void>, sizeof...(Tasks)>{std::forward<Tasks>(tasks)...}, joined));
    attachment.run();
    if (!joined) {
        throw std::logic_error("worker owner stopped before root tasks joined");
    }
    root.get();
}

}  // namespace ruvia::test