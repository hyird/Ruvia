#pragma once

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <utility>
#include <variant>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

class TaskScope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable handle, matching the borrow-only hot-path rule.
    explicit TaskScope(const WorkerHandle& worker, std::pmr::memory_resource* resource = nullptr);
    TaskScope(WorkerHandle&&, std::pmr::memory_resource* = nullptr) = delete;
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    void spawn(Task<void> task);
    void requestStop() noexcept;
    [[nodiscard]] StopToken stopToken() const noexcept;
    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Task<void> join();

private:
    struct Node;

    struct TaskScopeEmpty final {};
    struct TaskScopeOpen final {};
    struct TaskScopeJoinReserved final {};
    class TaskScopeJoining final {
    public:
        explicit TaskScopeJoining(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };
    struct TaskScopeJoined final {};

    struct TaskScopeSuccess final {};
    class TaskScopeFailure final {
    public:
        explicit TaskScopeFailure(std::exception_ptr exception) noexcept
            : exception_(std::move(exception)) {}

        [[nodiscard]] const std::exception_ptr& exception() const& noexcept {
            return exception_;
        }
        const std::exception_ptr& exception() const&& = delete;

    private:
        std::exception_ptr exception_;
    };

    struct JoinAwaiter {
        TaskScope& scope;
        [[nodiscard]] bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> continuation);
        void await_resume();
    };

    class JoinReservation final {
    public:
        explicit JoinReservation(TaskScope& scope) noexcept
            : scope_(&scope) {}
        ~JoinReservation();

        JoinReservation(const JoinReservation&) = delete;
        JoinReservation& operator=(const JoinReservation&) = delete;
        JoinReservation(JoinReservation&& other) noexcept
            : scope_(std::exchange(other.scope_, nullptr)) {}
        JoinReservation& operator=(JoinReservation&&) = delete;

        [[nodiscard]] TaskScope& scope() const noexcept {
            return *scope_;
        }

    private:
        TaskScope* scope_;
    };

    static void childComplete(void* raw) noexcept;
    [[nodiscard]] static Task<void> joinReserved(JoinReservation reservation);
    void releaseJoinReservation() noexcept;
    void finish(Node* node) noexcept;
    void rethrowFailure();

    using Lifecycle = std::variant<TaskScopeEmpty, TaskScopeOpen, TaskScopeJoinReserved, TaskScopeJoining, TaskScopeJoined>;
    using Outcome = std::variant<TaskScopeSuccess, TaskScopeFailure>;

    const WorkerHandle& worker_;
    std::pmr::memory_resource* resource_;
    StopSource stopSource_;
    Node* head_{nullptr};
    std::size_t active_{0};
    Lifecycle lifecycle_;
    Outcome outcome_;
};

}  // namespace ruvia
