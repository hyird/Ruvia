#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

template <typename result_type, bool is_noexcept, typename... args_types>
class callback_owner;

// Trivial request/runtime view. Never allocates or extends the callable's
// lifetime. Signature policy changes invocation, not storage or binding logic.
template <typename result_type, bool is_noexcept, typename... args_types>
class callback_ref final {
public:
    constexpr callback_ref() noexcept = default;
    constexpr callback_ref(std::nullptr_t) noexcept {}
    // Select concrete pointer types so MSVC resolves the exception specification.
    using Invoke = std::conditional_t<is_noexcept,
        result_type (*)(void*, args_types...) noexcept,
        result_type (*)(void*, args_types...)>;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] result_type operator()(args_types... args) const noexcept(is_noexcept) {
        return invoke_target(std::forward<args_types>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(callback_ref, callback_ref) noexcept = default;

private:
    friend struct CallbackAccess;
    template <typename, bool, typename...>
    friend class callback_owner;

    constexpr callback_ref(void* target, Invoke invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    template <typename callable_type>
        requires(std::is_invocable_r_v<result_type, callable_type&, args_types...> &&
                 (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, callable_type&, args_types...>))
    [[nodiscard]] static constexpr callback_ref bind(callable_type& callable) noexcept {
        return callback_ref(std::addressof(callable), [](void* target, args_types... args) noexcept(is_noexcept) -> result_type {
            return (*static_cast<callable_type*>(target))(std::forward<args_types>(args)...);
        });
    }

    template <typename... invocation_args>
    [[nodiscard]] result_type invoke_target(invocation_args&&... args) const noexcept(is_noexcept) {
        return invoke_(target_, std::forward<invocation_args>(args)...);
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

// Signature decomposition is the only specialization boundary. Both callback
// families use the same implementation with the appropriate noexcept policy.
template <typename signature_type>
struct callback_signature;

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...)> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, false, args_types...>;
};

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...) noexcept> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, true, args_types...>;
};

template <typename signature_type>
using CallbackRef = typename callback_signature<signature_type>::template apply<callback_ref>;

struct CallbackAccess final {
    template <typename signature_type>
    [[nodiscard]] static constexpr CallbackRef<signature_type> make(
        void* target, typename CallbackRef<signature_type>::Invoke invoke) noexcept {
        return CallbackRef<signature_type>(target, invoke);
    }

    template <typename owner_type>
    [[nodiscard]] static constexpr auto ref(const owner_type& owner) noexcept {
        return owner.callback_ref_view();
    }

    template <typename signature_type, typename callable_type>
    [[nodiscard]] static constexpr CallbackRef<signature_type> bind(callable_type& callable) noexcept {
        return CallbackRef<signature_type>::bind(callable);
    }
};

static_assert(sizeof(CallbackRef<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<CallbackRef<void()>>);

}  // namespace ruvia::detail
