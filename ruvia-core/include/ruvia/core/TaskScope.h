#pragma once

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <stop_token>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

class TaskScope final {
public:
    explicit TaskScope(WorkerHandle worker, std::pmr::memory_resource* resource = nullptr);
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    void spawn(Task<void> task);
    void requestStop() noexcept;
    [[nodiscard]] std::stop_token stopToken() const noexcept;
    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Task<void> join();

private:
    struct Node;

    struct JoinAwaiter {
        TaskScope& scope;
        [[nodiscard]] bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> continuation);
        void await_resume();
    };

    static void childComplete(void* raw) noexcept;
    void finish(Node* node) noexcept;
    void rethrowFailure();

    WorkerHandle worker_;
    std::pmr::memory_resource* resource_;
    std::stop_source stopSource_;
    Node* head_{nullptr};
    std::size_t active_{0};
    std::exception_ptr firstFailure_;
    std::coroutine_handle<> joinContinuation_{};
    bool joinStarted_{false};
};

}
