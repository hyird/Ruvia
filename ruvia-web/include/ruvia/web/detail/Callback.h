#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/CallbackRef.h"

namespace ruvia::detail {

// Value-semantic, PMR-owned type-erased callback. This is the single template
// behind the App callback surface (HttpErrorHandler, HttpNotFoundHandler,
// AppHook, AccessLogCallback, ConnectionFailureCallback): self-contained
// callables are copied into process-owned storage, and the public owner keeps
// no borrowing API. A self-contained callable is owned by the callback value;
// references captured by that callable must still outlive the callback.
//
// The ordinary specialization invokes a throwing callable; the noexcept
// specialization requires a nothrow-invocable callable and marks its own
// invocation noexcept. Calling an empty callback is a programming error: the
// ordinary form throws std::logic_error, the noexcept form terminates.
template <typename Signature>
class Callback;

template <typename Result, typename... Args>
class Callback<Result(Args...)> final {
public:
    constexpr Callback() noexcept = default;

    constexpr Callback(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, Callback> && std::is_invocable_r_v<Result, Stored&, Args...> && std::is_copy_constructible_v<Stored>)
    Callback(Callable&& callable)
        : target_(constructPmrObject<Stored>(processResource(), std::forward<Callable>(callable))),
          invoke_([](void* target, Args... args) -> Result { return (*static_cast<Stored*>(target))(std::forward<Args>(args)...); }),
          destroy_([](void* target, std::pmr::memory_resource* resource) noexcept { destroyPmrObject(static_cast<Stored*>(target), resource); }),
          clone_([](const void* target, std::pmr::memory_resource* resource) -> void* { return constructPmrObject<Stored>(resource, *static_cast<const Stored*>(target)); }),
          resource_(processResource()) {}

    Callback(const Callback& other) {
        copyFrom(other);
    }

    Callback& operator=(const Callback& other) {
        if (this != &other) {
            Callback copy(other);
            swap(copy);
        }
        return *this;
    }

    Callback(Callback&& other) noexcept {
        moveFrom(other);
    }

    Callback& operator=(Callback&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    ~Callback() {
        reset();
    }

    [[nodiscard]] Result operator()(Args... args) const {
        if (invoke_ == nullptr) {
            throw std::logic_error("callback is empty");
        }
        return invoke_(target_, std::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const Callback& left, const Callback& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    friend struct CallbackAccess;

    using Invoke = Result (*)(void*, Args...);
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using Clone = void* (*)(const void*, std::pmr::memory_resource*);

    [[nodiscard]] constexpr CallbackRef<Result(Args...)> callbackRef() const noexcept {
        return CallbackAccess::make<Result(Args...)>(target_, invoke_);
    }

    void copyFrom(const Callback& other) {
        target_ = other.clone_ == nullptr ? other.target_ : other.clone_(other.target_, other.resource_);
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        clone_ = other.clone_;
        resource_ = other.resource_;
    }

    void moveFrom(Callback& other) noexcept {
        target_ = std::exchange(other.target_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void swap(Callback& other) noexcept {
        std::swap(target_, other.target_);
        std::swap(invoke_, other.invoke_);
        std::swap(destroy_, other.destroy_);
        std::swap(clone_, other.clone_);
        std::swap(resource_, other.resource_);
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        clone_ = nullptr;
        resource_ = nullptr;
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
    Destroy destroy_{nullptr};
    Clone clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename Result, typename... Args>
class Callback<Result(Args...) noexcept> final {
public:
    constexpr Callback() noexcept = default;

    constexpr Callback(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, Callback> && !std::is_lvalue_reference_v<Callable> && std::is_nothrow_invocable_r_v<Result, Stored&, Args...> && std::is_copy_constructible_v<Stored>)
    Callback(Callable&& callable)
        : target_(constructPmrObject<Stored>(processResource(), std::forward<Callable>(callable))),
          invoke_([](void* target, Args... args) noexcept -> Result { return (*static_cast<Stored*>(target))(std::forward<Args>(args)...); }),
          destroy_([](void* target, std::pmr::memory_resource* resource) noexcept { destroyPmrObject(static_cast<Stored*>(target), resource); }),
          clone_([](const void* target, std::pmr::memory_resource* resource) -> void* { return constructPmrObject<Stored>(resource, *static_cast<const Stored*>(target)); }),
          resource_(processResource()) {}

    Callback(const Callback& other) {
        copyFrom(other);
    }

    Callback& operator=(const Callback& other) {
        if (this != &other) {
            Callback copy(other);
            swap(copy);
        }
        return *this;
    }

    Callback(Callback&& other) noexcept {
        moveFrom(other);
    }

    Callback& operator=(Callback&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    ~Callback() {
        reset();
    }

    [[nodiscard]] Result operator()(Args... args) const noexcept {
        if (invoke_ == nullptr) {
            std::terminate();
        }
        return invoke_(target_, std::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const Callback& left, const Callback& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    friend struct CallbackAccess;

    using Invoke = Result (*)(void*, Args...) noexcept;
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using Clone = void* (*)(const void*, std::pmr::memory_resource*);

    [[nodiscard]] constexpr CallbackRef<Result(Args...) noexcept> callbackRef() const noexcept {
        return CallbackAccess::make<Result(Args...) noexcept>(target_, invoke_);
    }

    void copyFrom(const Callback& other) {
        target_ = other.clone_ == nullptr ? other.target_ : other.clone_(other.target_, other.resource_);
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        clone_ = other.clone_;
        resource_ = other.resource_;
    }

    void moveFrom(Callback& other) noexcept {
        target_ = std::exchange(other.target_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void swap(Callback& other) noexcept {
        std::swap(target_, other.target_);
        std::swap(invoke_, other.invoke_);
        std::swap(destroy_, other.destroy_);
        std::swap(clone_, other.clone_);
        std::swap(resource_, other.resource_);
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        clone_ = nullptr;
        resource_ = nullptr;
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
    Destroy destroy_{nullptr};
    Clone clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia::detail
