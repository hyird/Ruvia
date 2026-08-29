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

struct TaskScopeOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

class TaskScope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable handle, matching the borrow-only hot-path rule.
    explicit TaskScope(const WorkerHandle& worker, TaskScopeOptions options = {});
    TaskScope(WorkerHandle&&, TaskScopeOptions = {}) = delete;
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    // Every operation is bound to this scope's stable address: child
    // completion nodes point back to it, and join() reserves it from a lazy
    // coroutine frame. Reject temporary scopes before either lifetime can be
    // created and keep even state/token access on the same explicit owner.
    void spawn(Task<void> task) &;
    void spawn(Task<void>) && = delete;
    void requestStop() & noexcept;
    void requestStop() && = delete;
    [[nodiscard]] StopToken stopToken() const& noexcept;
    StopToken stopToken() const&& = delete;
    [[nodiscard]] bool stopRequested() const& noexcept;
    bool stopRequested() const&& = delete;
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    [[nodiscard]] Task<void> join() &;
    Task<void> join() && = delete;

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

    using Lifecycle = std::variant<TaskScopeEmpty, TaskScopeOpen, TaskScopeJoinReserved,
        TaskScopeJoining, TaskScopeJoined>;
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
