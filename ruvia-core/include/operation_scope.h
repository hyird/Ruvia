#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

#include "lifetimebound.h"
#include "task.h"

namespace ruvia {

class worker_ref;
class capability_link;

namespace detail {

template <typename member_type>
struct capability_owner;

template <typename owner_type>
struct capability_owner<void (owner_type::*)() noexcept> {
    using type = owner_type;
};

}  // namespace detail

// Worker-affine lifetime arena for the cold task frames and borrowed
// capabilities of one owner (a request, a client, a session). Not a
// structured-concurrency scope: running children belong to task_scope.
//
// Lifecycle (each step seals the scope; a sealed scope admits nothing new):
// - close(): discards cold frames; terminates if any attached operation has
//   started and not finished. The destructor and synchronous teardown path.
// - seal(): discards cold frames and leaves started operations running;
//   capabilities stay live until those operations finish.
// - close_and_join(): seal(), then waits for started operations, their leases
//   and capability cleanup to retire, including when invoked reentrantly by a
//   cleanup callback. Single-use.
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

    class join_operation final {
    public:
        join_operation(const join_operation&) = delete;
        join_operation& operator=(const join_operation&) = delete;
        join_operation(join_operation&& other) noexcept;
        join_operation& operator=(join_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        bool await_suspend(std::coroutine_handle<> continuation) noexcept;
        void await_resume() const noexcept {}

    private:
        friend class operation_scope;

        explicit join_operation(operation_scope& scope) noexcept :
            scope_(&scope) {}

        operation_scope* scope_;
        bool started_{false};
    };

    void close() noexcept;
    void seal() noexcept;
    [[nodiscard]] join_operation close_and_join() & noexcept;
    join_operation close_and_join() && = delete;
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

    // Ties a cold task to this scope: close()/seal() discard it while cold.
    template <typename t_type>
    [[nodiscard]] task<t_type> attach(task<t_type> task_value) & {
        task_value.bind_scope(*this);
        return task_value;
    }
    template <typename t_type>
    task<t_type> attach(task<t_type>) && = delete;

private:
    friend class detail::scoped_operation_registration;
    friend class capability_link;
    struct drain_guard;
    void check_worker() const noexcept;
    void retire_cold_operations() noexcept;
    void expire_for_join() noexcept;
    void expire_capabilities() noexcept;
    void resume_joiner() noexcept;
    void link(detail::scoped_operation_registration& operation) noexcept;
    void unlink(detail::scoped_operation_registration& operation) noexcept;

    detail::scoped_operation_registration* head_{nullptr};
    capability_link* capability_head_{nullptr};
    const worker_ref* worker_{nullptr};
    std::coroutine_handle<> join_continuation_{};
    bool active_{true};
    bool expiring_capabilities_{false};
    bool join_started_{false};
    bool join_complete_{false};
};

// Untyped intrusive link behind capability_registration.
class capability_link final {
public:
    capability_link() noexcept = default;
    capability_link(operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept;
    capability_link(const capability_link& other, void* target) noexcept;
    capability_link(capability_link&& other, void* target) noexcept;
    ~capability_link();

    capability_link(const capability_link&) = delete;
    capability_link& operator=(const capability_link&) = delete;
    capability_link(capability_link&&) = delete;
    capability_link& operator=(capability_link&&) = delete;

    void require_active() const;
    [[nodiscard]] operation_scope& scope() const& RUVIA_LIFETIMEBOUND;
    operation_scope& scope() const&& = delete;
    void bind(operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept;

private:
    friend class operation_scope;
    void link(operation_scope& scope) noexcept;
    void unlink() noexcept;
    void expire() noexcept;

    operation_scope* scope_{nullptr};
    capability_link* previous_{nullptr};
    capability_link* next_{nullptr};
    void* target_{nullptr};
    void (*cleanup_)(void*) noexcept {nullptr};
    bool active_{false};
};

// Allocation-free link of one owner into an operation_scope: the scope's
// expiry calls `(owner.*expire_member)()` once, on the scope's worker. An
// owner must explicitly rebind itself when copying or moving; ordinary
// copy/move is forbidden. Declare it after the payload so destruction unlinks
// before payload teardown.
template <auto expire_member>
class capability_registration final {
public:
    using owner_type = typename detail::capability_owner<decltype(expire_member)>::type;

    capability_registration() noexcept = default;
    capability_registration(operation_scope& scope, owner_type& owner) noexcept :
        link_(scope, std::addressof(owner), &expire_owner) {}
    capability_registration(const capability_registration& other, owner_type& rebound) noexcept :
        link_(other.link_, std::addressof(rebound)) {}
    capability_registration(capability_registration&& other, owner_type& rebound) noexcept :
        link_(std::move(other.link_), std::addressof(rebound)) {}

    void bind(operation_scope& scope, owner_type& owner) noexcept {
        link_.bind(scope, std::addressof(owner), &expire_owner);
    }
    // Throws std::logic_error once the scope has expired this capability.
    void require_active() const {
        link_.require_active();
    }
    [[nodiscard]] operation_scope& scope() const& RUVIA_LIFETIMEBOUND {
        return link_.scope();
    }
    operation_scope& scope() const&& = delete;
    // require_active(), then attaches the cold task to the scope.
    template <typename t_type>
    [[nodiscard]] task<t_type> attach(task<t_type> task_value) const& {
        return link_.scope().attach(std::move(task_value));
    }
    template <typename t_type>
    task<t_type> attach(task<t_type>) const&& = delete;

private:
    static void expire_owner(void* owner) noexcept {
        (static_cast<owner_type*>(owner)->*expire_member)();
    }

    capability_link link_;
};

}  // namespace ruvia
