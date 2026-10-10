#pragma once

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"

namespace ruvia {

template <typename t_type>
class scoped_operation;
class scoped_capability_registration;

namespace detail {
class scoped_operation_registration;
}

// Worker-affine owner of borrowed capabilities and their lazy operations.
// Close discards cold frames before capability cleanup. Started operations must
// finish; close_and_join waits for their frames and leases to retire first.
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
    [[nodiscard]] task<void> close_and_join() &;
    task<void> close_and_join() && = delete;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool has_pending_operations() const noexcept {
        return head_ != nullptr;
    }

private:
    friend class detail::scoped_operation_registration;
    friend class scoped_capability_registration;
    struct drain_guard;
    struct join_awaiter;
    void retire_cold_operations() noexcept;
    void expire_for_join() noexcept;
    void expire_capabilities() noexcept;
    void resume_joiner() noexcept;
    void link(detail::scoped_operation_registration& operation) noexcept;
    void unlink(detail::scoped_operation_registration& operation) noexcept;

    detail::scoped_operation_registration* head_{nullptr};
    scoped_capability_registration* capability_head_{nullptr};
    std::coroutine_handle<> join_continuation_{};
    bool active_{true};
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
[[nodiscard]] scoped_operation<t_type> make_scoped_operation(operation_scope& scope, task<t_type> task_value);
template <typename t_type>
[[nodiscard]] scoped_operation<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value, void (*check_affinity)(void*) noexcept, void* target);

namespace detail {

// Internal registration is composed by the typed frame owner and the drain
// obligation. The list never relies on inheritance or a downcast to that owner.
class scoped_operation_registration final {
public:
    explicit scoped_operation_registration(operation_scope& scope) noexcept;
    ~scoped_operation_registration();

    scoped_operation_registration(const scoped_operation_registration&) = delete;
    scoped_operation_registration& operator=(const scoped_operation_registration&) = delete;
    scoped_operation_registration(scoped_operation_registration&&) = delete;
    scoped_operation_registration& operator=(scoped_operation_registration&&) = delete;

    void bind_frame(void* target, void (*retire_cold)(void*) noexcept,
        void (*check_affinity)(void*) noexcept, void* affinity_target) noexcept;
    // Validates phase and affinity without changing state; the frame owner then
    // takes the task (rejecting an empty one) before start() commits running.
    void prepare_start() const;
    void start() noexcept;
    void prepare_completion() const noexcept;
    void complete() noexcept;
    void retire_frame() noexcept;

private:
    friend class ::ruvia::operation_scope;
    enum class phase : std::uint8_t { cold,
        running,
        retiring,
        complete,
        expired };
    void clear_frame_binding() noexcept;

    operation_scope* scope_{nullptr};
    scoped_operation_registration* previous_{nullptr};
    scoped_operation_registration* next_{nullptr};
    phase phase_{phase::cold};
    void* frame_target_{nullptr};
    void (*retire_cold_)(void*) noexcept {nullptr};
    void (*check_affinity_)(void*) noexcept {nullptr};
    void* affinity_target_{nullptr};
};

}  // namespace detail

template <typename t_type = void>
class [[nodiscard]] scoped_operation final {
    class awaiter_type final {
    public:
        awaiter_type(const awaiter_type&) = delete;
        awaiter_type& operator=(const awaiter_type&) = delete;
        awaiter_type(awaiter_type&&) = delete;
        awaiter_type& operator=(awaiter_type&&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return awaiter_.await_ready();
        }
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) {
            return awaiter_.await_suspend(continuation);
        }
        t_type await_resume() {
            owner_->registration_.prepare_completion();
            struct complete final {
                awaiter_type& awaiter_;
                ~complete() {
                    awaiter_.awaiter_.retire_completed_frame();
                    // Completion may resume join and destroy the owner inline.
                    awaiter_.owner_->registration_.complete();
                }
            } complete_value{*this};
            if constexpr (std::is_void_v<t_type>) {
                awaiter_.await_resume();
            } else {
                return awaiter_.await_resume();
            }
        }

        const awaiter_type* operator&() const = delete;
        awaiter_type* operator&() = delete;

    private:
        friend class scoped_operation;
        explicit awaiter_type(scoped_operation& owner_value)
            : owner_(std::addressof(owner_value)),
              awaiter_([&owner_value]() {
                  owner_value.registration_.prepare_start();
                  // An empty task throws here while the registration is still
                  // cold, so the owner remains retirable.
                  auto awaiter = std::move(*owner_value.task_).operator co_await();
                  owner_value.task_.reset();
                  owner_value.registration_.start();
                  return awaiter;
              }()) {}
        scoped_operation* owner_;
        detail::task_awaiter<t_type> awaiter_;
    };

public:
    scoped_operation(const scoped_operation&) = delete;
    scoped_operation& operator=(const scoped_operation&) = delete;
    scoped_operation(scoped_operation&&) = delete;
    scoped_operation& operator=(scoped_operation&&) = delete;
    ~scoped_operation() {
        registration_.retire_frame();
    }

    [[nodiscard]] awaiter_type operator co_await() && {
        return awaiter_type(*this);
    }
    [[nodiscard]] auto operator co_await() & = delete;
    [[nodiscard]] auto operator co_await() const& = delete;
    [[nodiscard]] auto operator co_await() const&& = delete;
    const scoped_operation* operator&() const = delete;
    scoped_operation* operator&() = delete;

private:
    template <typename u_type>
    friend scoped_operation<u_type> make_scoped_operation(operation_scope&, task<u_type>);
    template <typename u_type>
    friend scoped_operation<u_type> make_scoped_operation(
        operation_scope&, task<u_type>, void (*)(void*) noexcept, void*);

    scoped_operation(operation_scope& scope, task<t_type> task_value,
        void (*check_affinity)(void*) noexcept = nullptr, void* affinity_target = nullptr)
        : task_(std::move(task_value)),
          registration_(scope) {
        registration_.bind_frame(std::addressof(task_), [](void* target) noexcept { static_cast<std::optional<task<t_type>>*>(target)->reset(); }, check_affinity, affinity_target);
    }

    std::optional<task<t_type>> task_;
    detail::scoped_operation_registration registration_;
};

template <typename t_type>
[[nodiscard]] scoped_operation<t_type> make_scoped_operation(operation_scope& scope, task<t_type> task_value) {
    return scoped_operation<t_type>(scope, std::move(task_value));
}

template <typename t_type>
[[nodiscard]] scoped_operation<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value, void (*check_affinity)(void*) noexcept, void* target) {
    return scoped_operation<t_type>(scope, std::move(task_value), check_affinity, target);
}

}  // namespace ruvia
