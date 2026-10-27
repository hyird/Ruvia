#pragma once

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"

namespace ruvia {

class scoped_capability_registration;

namespace detail {
class scoped_operation_registration;
}

// Worker-affine owner of borrowed capabilities and their lazy operations.
// Close discards cold frames before capability cleanup. Started operations must
// finish; close_and_join waits for their frames, leases and capability cleanup
// to retire, including when invoked reentrantly by a cleanup callback.
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
    void bind_context(void* context) noexcept {
        context_ = context;
    }
    [[nodiscard]] void* context() const noexcept {
        return context_;
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
    void* context_{nullptr};
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
    task_value.bind_scope(scope, nullptr, nullptr);
    return task_value;
}

template <typename t_type>
[[nodiscard]] task<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value, void (*check_affinity)(void*) noexcept, void* target) {
    task_value.bind_scope(scope, check_affinity, target);
    return task_value;
}
}  // namespace ruvia
