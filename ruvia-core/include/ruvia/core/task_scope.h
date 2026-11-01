#pragma once

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

struct task_scope_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

class task_scope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable handle, matching the borrow-only hot-path rule.
    explicit task_scope(const worker_handle& worker_value, task_scope_options options = {});
    task_scope(worker_handle&&, task_scope_options = {}) = delete;
    // Destruction requires that no child is active and no join is pending;
    // violating that is a lifecycle contract violation and terminates. A
    // scope whose children all completed may be destroyed without a join; a
    // child failure nobody awaited is then routed to
    // report_unhandled_failure.
    ~task_scope();

    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;
    task_scope(task_scope&&) = delete;
    task_scope& operator=(task_scope&&) = delete;

    // Every operation is bound to this scope's stable address: child
    // completion nodes point back to it, and lazy join tasks borrow it.
    // Reject temporary scopes before either lifetime can be
    // created and keep even state/token access on the same explicit owner.
    void spawn(task<void> task) &;
    void spawn(task<void>) && = delete;
    // Stop control is worker-affine like the rest of the scope. The stop state
    // is created by the first token request, so a scope whose token is never
    // observed allocates none; get_stop_token() may throw std::bad_alloc.
    void request_stop() & noexcept;
    void request_stop() && = delete;
    [[nodiscard]] stop_token get_stop_token() const&;
    stop_token get_stop_token() const&& = delete;
    [[nodiscard]] bool stop_requested() const& noexcept;
    bool stop_requested() const&& = delete;
    // Includes a spawn currently acquiring its child node.
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    // One join: single-use, movable only before it is awaited. A cold join
    // does not close spawn admission; starting it claims the scope exactly
    // once and prevents further spawning. It waits for in-progress spawn
    // admission and child node retirement. The scope and worker must remain
    // alive until the started operation and all children have completed.
    class join_operation final {
    public:
        join_operation(const join_operation&) = delete;
        join_operation& operator=(const join_operation&) = delete;
        join_operation(join_operation&& other) noexcept;
        join_operation& operator=(join_operation&&) = delete;

        [[nodiscard]] bool await_ready();
        void await_suspend(std::coroutine_handle<> continuation) noexcept;
        void await_resume();

    private:
        friend class task_scope;

        explicit join_operation(task_scope& scope) noexcept
            : scope_(&scope) {}

        task_scope* scope_;
        bool started_{false};
    };

    // Creation and starting the join both require the bound worker.
    [[nodiscard]] join_operation join() &;
    join_operation join() && = delete;

private:
    struct node_type;
    struct retiring_guard;

    struct task_scope_open_type final {};
    class task_scope_joining_type final {
    public:
        explicit task_scope_joining_type(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };
    struct task_scope_joined_type final {};

    static void child_complete(void* raw) noexcept;
    void finish(node_type* node) noexcept;
    void resume_join_if_idle() noexcept;
    void rethrow_failure();

    using lifecycle_type = std::variant<task_scope_open_type, task_scope_joining_type, task_scope_joined_type>;

    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    mutable std::optional<stop_source> stop_source_;
    bool stop_requested_{false};
    std::size_t active_{0};
    retiring_guard* retiring_{nullptr};
    lifecycle_type lifecycle_;
    std::exception_ptr failure_{};
};

}  // namespace ruvia
