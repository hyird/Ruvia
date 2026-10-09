#pragma once

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory_resource>
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
    ~task_scope();

    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;
    task_scope(task_scope&&) = delete;
    task_scope& operator=(task_scope&&) = delete;

    // Every operation is bound to this scope's stable address: child
    // completion nodes point back to it, and join() reserves it from a lazy
    // coroutine frame. Reject temporary scopes before either lifetime can be
    // created and keep even state/token access on the same explicit owner.
    void spawn(task<void> task) &;
    void spawn(task<void>) && = delete;
    void request_stop() & noexcept;
    void request_stop() && = delete;
    [[nodiscard]] stop_token get_stop_token() const& noexcept;
    stop_token get_stop_token() const&& = delete;
    [[nodiscard]] bool stop_requested() const& noexcept;
    bool stop_requested() const&& = delete;
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    [[nodiscard]] task<void> join() &;
    task<void> join() && = delete;

private:
    struct node_type;

    struct task_scope_empty_type final {};
    struct task_scope_open_type final {};
    struct task_scope_join_reserved_type final {};
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

    struct task_scope_success_type final {};
    class task_scope_failure_type final {
    public:
        explicit task_scope_failure_type(std::exception_ptr exception) noexcept
            : exception_(std::move(exception)) {}

        [[nodiscard]] const std::exception_ptr& exception() const& noexcept {
            return exception_;
        }
        const std::exception_ptr& exception() const&& = delete;

    private:
        std::exception_ptr exception_;
    };

    struct join_awaiter_type {
        task_scope& scope_;
        [[nodiscard]] bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> continuation);
        void await_resume();
    };

    class join_reservation_type final {
    public:
        explicit join_reservation_type(task_scope& scope) noexcept
            : scope_(&scope) {}
        ~join_reservation_type();

        join_reservation_type(const join_reservation_type&) = delete;
        join_reservation_type& operator=(const join_reservation_type&) = delete;
        join_reservation_type(join_reservation_type&& other) noexcept
            : scope_(std::exchange(other.scope_, nullptr)) {}
        join_reservation_type& operator=(join_reservation_type&&) = delete;

        [[nodiscard]] task_scope& scope() const noexcept {
            return *scope_;
        }

    private:
        task_scope* scope_;
    };

    static void child_complete(void* raw) noexcept;
    [[nodiscard]] static task<void> join_reserved(join_reservation_type reservation);
    void release_join_reservation() noexcept;
    void finish(node_type* node) noexcept;
    void rethrow_failure();

    using lifecycle_type = std::variant<task_scope_empty_type, task_scope_open_type, task_scope_join_reserved_type,
        task_scope_joining_type, task_scope_joined_type>;
    using outcome_type = std::variant<task_scope_success_type, task_scope_failure_type>;

    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    stop_source stop_source_;
    node_type* head_{nullptr};
    std::size_t active_{0};
    lifecycle_type lifecycle_;
    outcome_type outcome_;
};

}  // namespace ruvia
