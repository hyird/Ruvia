#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

// Trivial, allocation-free request/runtime view over an App-owned callback.
// This type never extends the callable lifetime. Public callback owners expose
// no borrowing API; only framework internals create these views.
template <typename Signature>
class CallbackRef;

template <typename Result, typename... Args>
class CallbackRef<Result(Args...)> final {
public:
    constexpr CallbackRef() noexcept = default;
    constexpr CallbackRef(std::nullptr_t) noexcept {}
    using Invoke = Result (*)(void*, Args...);

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] Result operator()(Args... args) const {
        return invoke_(target_, std::forward<Args>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(CallbackRef, CallbackRef) noexcept = default;

private:
    friend struct CallbackAccess;

    constexpr CallbackRef(void* target, Invoke invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    template <typename Callable>
        requires std::is_invocable_r_v<Result, Callable&, Args...>
    [[nodiscard]] static constexpr CallbackRef bind(Callable& callable) noexcept {
        return CallbackRef(std::addressof(callable), [](void* target, Args... args) -> Result { return (*static_cast<Callable*>(target))(std::forward<Args>(args)...); });
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

template <typename Result, typename... Args>
class CallbackRef<Result(Args...) noexcept> final {
public:
    constexpr CallbackRef() noexcept = default;
    constexpr CallbackRef(std::nullptr_t) noexcept {}
    using Invoke = Result (*)(void*, Args...) noexcept;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    Result operator()(Args... args) const noexcept {
        return invoke_(target_, std::forward<Args>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(CallbackRef, CallbackRef) noexcept = default;

private:
    friend struct CallbackAccess;

    constexpr CallbackRef(void* target, Invoke invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    template <typename Callable>
        requires std::is_nothrow_invocable_r_v<Result, Callable&, Args...>
    [[nodiscard]] static constexpr CallbackRef bind(Callable& callable) noexcept {
        return CallbackRef(std::addressof(callable), [](void* target, Args... args) noexcept -> Result { return (*static_cast<Callable*>(target))(std::forward<Args>(args)...); });
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

struct CallbackAccess final {
    template <typename Signature>
    [[nodiscard]] static constexpr CallbackRef<Signature> make(void* target, typename CallbackRef<Signature>::Invoke invoke) noexcept {
        return CallbackRef<Signature>(target, invoke);
    }

    template <typename Owner>
    [[nodiscard]] static constexpr auto ref(const Owner& owner) noexcept {
        return owner.callbackRef();
    }

    template <typename Signature, typename Callable>
    [[nodiscard]] static constexpr CallbackRef<Signature> bind(Callable& callable) noexcept {
        return CallbackRef<Signature>::bind(callable);
    }
};

static_assert(sizeof(CallbackRef<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<CallbackRef<void()>>);

}  // namespace ruvia::detail
