#pragma once

#include "ruvia/core/Task.h"

#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {

template <typename T>
class ScopedOperation;

namespace detail {

class ScopedOperationNode;
class ScopedCapabilityNode;

class ScopedOperationScope final {
public:
    ScopedOperationScope() noexcept = default;
    ~ScopedOperationScope() {
        close();
    }

    ScopedOperationScope(const ScopedOperationScope&) = delete;
    ScopedOperationScope& operator=(const ScopedOperationScope&) = delete;
    ScopedOperationScope(ScopedOperationScope&&) = delete;
    ScopedOperationScope& operator=(ScopedOperationScope&&) = delete;

    void close() noexcept;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    // A cold operation still owns a coroutine frame that may borrow its
    // capability owner.  Owner move constructors use this to reject moving
    // that owner before the frame has either started and completed or been
    // explicitly discarded by closing the scope.
    [[nodiscard]] bool hasPendingOperations() const noexcept {
        return head_ != nullptr;
    }

private:
    friend class ScopedOperationNode;
    friend class ScopedCapabilityNode;
    void link(ScopedOperationNode& operation) noexcept;
    void unlink(ScopedOperationNode& operation) noexcept;

    ScopedOperationNode* head_{nullptr};
    ScopedCapabilityNode* capabilityHead_{nullptr};
    bool active_{true};
};

// Intrusive, allocation-free capability lifetime. Moving a public capability
// relinks it into the same parent scope; closing that scope invokes typed
// cleanup while the owning request or worker memory domain is still alive.
class ScopedCapabilityNode {
public:
    ScopedCapabilityNode(const ScopedCapabilityNode& other) noexcept;
    ScopedCapabilityNode& operator=(const ScopedCapabilityNode&) = delete;
    ScopedCapabilityNode(ScopedCapabilityNode&& other) noexcept;
    ScopedCapabilityNode& operator=(ScopedCapabilityNode&&) = delete;
    ~ScopedCapabilityNode();

protected:
    ScopedCapabilityNode() noexcept
        : active_(false) {}
    ScopedCapabilityNode(ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept;
    void requireActive() const;
    [[nodiscard]] ScopedOperationScope& operationScope() const;
    void bind(ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept;

private:
    friend class ScopedOperationScope;
    void link(ScopedOperationScope& scope) noexcept;
    void unlink() noexcept;
    void expire() noexcept;

    ScopedOperationScope* scope_{nullptr};
    ScopedCapabilityNode* previous_{nullptr};
    ScopedCapabilityNode* next_{nullptr};
    void (*expire_)(ScopedCapabilityNode&) noexcept {nullptr};
    bool active_{true};
};

class ScopedOperationNode {
public:
    ScopedOperationNode(const ScopedOperationNode&) = delete;
    ScopedOperationNode& operator=(const ScopedOperationNode&) = delete;
    ScopedOperationNode(ScopedOperationNode&&) = delete;
    ScopedOperationNode& operator=(ScopedOperationNode&&) = delete;
    ~ScopedOperationNode();

protected:
    explicit ScopedOperationNode(ScopedOperationScope& scope) noexcept;
    void setExpireCold(void (*expireCold)(ScopedOperationNode&) noexcept) noexcept {
        expireCold_ = expireCold;
    }
    void begin();
    void complete() noexcept;

private:
    friend class ScopedOperationScope;
    enum class Phase { kCold, kRunning, kComplete, kExpired };
    void expire() noexcept;

    ScopedOperationScope* scope_{nullptr};
    ScopedOperationNode* previous_{nullptr};
    ScopedOperationNode* next_{nullptr};
    Phase phase_{Phase::kCold};
    void (*expireCold_)(ScopedOperationNode&) noexcept {nullptr};
};

template <typename T>
[[nodiscard]] ScopedOperation<T> makeScopedOperation(ScopedOperationScope& scope, Task<T> task);

}  // namespace detail

template <typename T = void>
class [[nodiscard]] ScopedOperation final : private detail::ScopedOperationNode {
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
            struct Complete final {
                ScopedOperation* owner;
                ~Complete() {
                    owner->complete();
                }
            } complete{owner_};
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
                  owner.begin();
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
    friend ScopedOperation<U> detail::makeScopedOperation(detail::ScopedOperationScope&, Task<U>);

    ScopedOperation(detail::ScopedOperationScope& scope, Task<T> task)
        : detail::ScopedOperationNode(scope),
          task_(std::move(task)) {
        setExpireCold([](detail::ScopedOperationNode& node) noexcept { static_cast<ScopedOperation&>(node).task_.reset(); });
    }

    std::optional<Task<T>> task_;
};

namespace detail {

template <typename T>
[[nodiscard]] ScopedOperation<T> makeScopedOperation(ScopedOperationScope& scope, Task<T> task) {
    return ScopedOperation<T>(scope, std::move(task));
}

}  // namespace detail

}  // namespace ruvia
