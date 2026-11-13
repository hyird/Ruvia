#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"

namespace ruvia {

class task_scope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable worker_ref, matching the borrow-only hot-path rule.
    explicit task_scope(const worker_ref& worker_value);
    task_scope(worker_ref&&) = delete;
    task_scope(const worker_ref&&) = delete;
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
    // Stop control is worker-affine. Construction creates the stop state so
    // token() only observes it and never allocates or requests cancellation.
    void request_stop() & noexcept;
    void request_stop() && = delete;
    [[nodiscard]] stop_token token() const&;
    stop_token token() const&& = delete;
    [[nodiscard]] bool stop_requested() const& noexcept;
    bool stop_requested() const&& = delete;
    // Includes a spawn currently acquiring its child node.
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    [[nodiscard]] bool empty() const& noexcept {
        return size() == 0;
    }
    bool empty() const&& = delete;
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
        void operator&() const = delete;

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

    // Creation and starting the join both require the bound worker. The join
    // rethrows the first child failure; each later child failure not
    // equivalent to it (see equivalent_failures) is routed to
    // report_unhandled_failure when that child completes.
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

    const worker_ref& worker_;
    stop_source stop_source_;
    std::size_t active_{0};
    retiring_guard* retiring_{nullptr};
    lifecycle_type lifecycle_;
    std::exception_ptr failure_{};
};

}  // namespace ruvia

namespace ruvia {

class scoped_capability_registration;
class worker_ref;

namespace detail {
class scoped_operation_registration;
}

// Worker-affine owner of borrowed capabilities and their lazy operations.
// Close discards cold frames before capability cleanup. Started operations must
// finish; close_and_join waits for their frames, leases and capability cleanup
// to retire, including when invoked reentrantly by a cleanup callback.
// bind_worker pins the scope to one worker: every operation in it then
// terminates when started, completed or retired cold on another thread.
class operation_scope final {
public:
    operation_scope() noexcept = default;
    ~operation_scope() {
        close();
    }

    operation_scope(const operation_scope&) = delete;
    operation_scope& operator=(const operation_scope&) = delete;
    operation_scope(operation_scope&&) = delete;
    operation_scope& operator=(operation_scope&&) = delete;

    void close() noexcept;
    void expire() noexcept;
    [[nodiscard]] task<void> close_and_join() &;
    task<void> close_and_join() && = delete;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool has_pending_operations() const noexcept {
        return head_ != nullptr;
    }
    // Borrows the worker_ref, which must outlive this scope (the same rule
    // as task_scope). Bind once, before any operation is registered.
    void bind_worker(const worker_ref& worker) & noexcept {
        if (worker_ != nullptr || head_ != nullptr || capability_head_ != nullptr || !active_) {
            std::terminate();
        }
        worker_ = &worker;
    }
    void bind_worker(const worker_ref&) && = delete;
    void bind_worker(worker_ref&&) = delete;
    void bind_worker(const worker_ref&&) = delete;

private:
    friend class detail::scoped_operation_registration;
    friend class scoped_capability_registration;
    struct drain_guard;
    struct join_awaiter;
    void check_worker() const noexcept;
    void retire_cold_operations() noexcept;
    void expire_for_join() noexcept;
    void expire_capabilities() noexcept;
    void resume_joiner() noexcept;
    void link(detail::scoped_operation_registration& operation) noexcept;
    void unlink(detail::scoped_operation_registration& operation) noexcept;

    detail::scoped_operation_registration* head_{nullptr};
    scoped_capability_registration* capability_head_{nullptr};
    const worker_ref* worker_{nullptr};
    std::coroutine_handle<> join_continuation_{};
    bool active_{true};
    bool expiring_capabilities_{false};
    bool join_started_{false};
    bool join_complete_{false};
};

// Allocation-free value registration. An owner must explicitly rebind the
// cleanup target when copying or moving; ordinary copy/move is forbidden.
// Declare it after the payload so destruction unlinks before payload teardown.
class scoped_capability_registration final {
public:
    scoped_capability_registration() noexcept = default;
    scoped_capability_registration(operation_scope& scope, void* target,
        void (*cleanup)(void*) noexcept) noexcept;
    scoped_capability_registration(const scoped_capability_registration& other, void* target) noexcept;
    scoped_capability_registration(scoped_capability_registration&& other, void* target) noexcept;
    ~scoped_capability_registration();

    scoped_capability_registration(const scoped_capability_registration&) = delete;
    scoped_capability_registration& operator=(const scoped_capability_registration&) = delete;
    scoped_capability_registration(scoped_capability_registration&&) = delete;
    scoped_capability_registration& operator=(scoped_capability_registration&&) = delete;

    void require_active() const;
    [[nodiscard]] operation_scope& scope() const;
    void bind(operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept;

private:
    friend class operation_scope;
    void link(operation_scope& scope) noexcept;
    void unlink() noexcept;
    void expire() noexcept;

    operation_scope* scope_{nullptr};
    scoped_capability_registration* previous_{nullptr};
    scoped_capability_registration* next_{nullptr};
    void* target_{nullptr};
    void (*cleanup_)(void*) noexcept {nullptr};
    bool active_{false};
};

template <typename t_type>
[[nodiscard]] task<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value) {
    task_value.bind_scope(scope);
    return task_value;
}
}  // namespace ruvia
