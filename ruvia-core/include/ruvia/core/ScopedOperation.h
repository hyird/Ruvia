#pragma once

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/Task.h"

namespace ruvia {

template <typename T>
class ScopedOperation;
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
    [[nodiscard]] Task<void> close_and_join() &;
    Task<void> close_and_join() && = delete;
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

template <typename T>
[[nodiscard]] ScopedOperation<T> make_scoped_operation(operation_scope& scope, Task<T> task);
template <typename T>
[[nodiscard]] ScopedOperation<T> make_scoped_operation(
    operation_scope& scope, Task<T> task, void (*check_affinity)(void*) noexcept, void* target);

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
    void begin();
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

template <typename T = void>
class [[nodiscard]] ScopedOperation final {
    class Awaiter final {
    public:
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&&) = delete;
        Awaiter& operator=(Awaiter&&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return awaiter_.await_ready();
        }
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) {
            return awaiter_.await_suspend(continuation);
        }
        T await_resume() {
            owner_->registration_.prepare_completion();
            struct Complete final {
                Awaiter& awaiter;
                ~Complete() {
                    awaiter.awaiter_.retireCompletedFrame();
                    // Completion may resume join and destroy the owner inline.
                    awaiter.owner_->registration_.complete();
                }
            } complete{*this};
            if constexpr (std::is_void_v<T>) {
                awaiter_.await_resume();
            } else {
                return awaiter_.await_resume();
            }
        }

        const Awaiter* operator&() const = delete;
        Awaiter* operator&() = delete;

    private:
        friend class ScopedOperation;
        explicit Awaiter(ScopedOperation& owner)
            : owner_(std::addressof(owner)),
              awaiter_([&owner]() {
                  owner.registration_.begin();
                  auto awaiter = std::move(*owner.task_).operator co_await();
                  owner.task_.reset();
                  return awaiter;
              }()) {}
        ScopedOperation* owner_;
        detail::TaskAwaiter<T> awaiter_;
    };

public:
    ScopedOperation(const ScopedOperation&) = delete;
    ScopedOperation& operator=(const ScopedOperation&) = delete;
    ScopedOperation(ScopedOperation&&) = delete;
    ScopedOperation& operator=(ScopedOperation&&) = delete;
    ~ScopedOperation() {
        registration_.retire_frame();
    }

    [[nodiscard]] Awaiter operator co_await() && {
        return Awaiter(*this);
    }
    [[nodiscard]] auto operator co_await() & = delete;
    [[nodiscard]] auto operator co_await() const& = delete;
    [[nodiscard]] auto operator co_await() const&& = delete;
    const ScopedOperation* operator&() const = delete;
    ScopedOperation* operator&() = delete;

private:
    template <typename U>
    friend ScopedOperation<U> make_scoped_operation(operation_scope&, Task<U>);
    template <typename U>
    friend ScopedOperation<U> make_scoped_operation(
        operation_scope&, Task<U>, void (*)(void*) noexcept, void*);

    ScopedOperation(operation_scope& scope, Task<T> task,
        void (*check_affinity)(void*) noexcept = nullptr, void* affinity_target = nullptr)
        : task_(std::move(task)),
          registration_(scope) {
        registration_.bind_frame(std::addressof(task_), [](void* target) noexcept { static_cast<std::optional<Task<T>>*>(target)->reset(); }, check_affinity, affinity_target);
    }

    std::optional<Task<T>> task_;
    detail::scoped_operation_registration registration_;
};

template <typename T>
[[nodiscard]] ScopedOperation<T> make_scoped_operation(operation_scope& scope, Task<T> task) {
    return ScopedOperation<T>(scope, std::move(task));
}

template <typename T>
[[nodiscard]] ScopedOperation<T> make_scoped_operation(
    operation_scope& scope, Task<T> task, void (*check_affinity)(void*) noexcept, void* target) {
    return ScopedOperation<T>(scope, std::move(task), check_affinity, target);
}

}  // namespace ruvia
