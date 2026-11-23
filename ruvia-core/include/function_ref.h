#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include "lifetimebound.h"
#include "move_only_function.h"

namespace ruvia {

template <typename signature_type, typename plain_signature = typename detail::function_signature<signature_type>::plain_type>
class function_ref;

// Non-owning, allocation-free callable view. Borrows an lvalue callable object
// (or wraps a compile-time function via from_function) without extending its
// lifetime; rvalue callables are rejected at compile time.
template <typename signature_type, typename result_type, typename... args_type>
class function_ref<signature_type, result_type(args_type...)> final {
    static constexpr bool is_noexcept = detail::function_signature<signature_type>::is_noexcept;

public:
    using invoke_type = typename detail::function_signature<signature_type>::invoke_type;

    constexpr function_ref() noexcept = default;
    constexpr function_ref(std::nullptr_t) noexcept {}

    constexpr function_ref(const move_only_function<signature_type>& owner RUVIA_LIFETIMEBOUND) noexcept {
        if (owner) {
            target_ = const_cast<void*>(static_cast<const void*>(std::addressof(owner)));
            invoke_ = &invoke_callable<const move_only_function<signature_type>>;
        }
    }
    function_ref(move_only_function<signature_type>&&) = delete;
    function_ref(const move_only_function<signature_type>&&) = delete;

    template <typename callable_type>
        requires(!std::same_as<std::remove_cvref_t<callable_type>, function_ref>) &&
                    (!std::same_as<std::remove_cvref_t<callable_type>, move_only_function<signature_type>>) &&
                    std::is_object_v<std::remove_reference_t<callable_type>> &&
                    detail::move_only_function_target<result_type, callable_type&, args_type...> &&
                    (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, callable_type&, args_type...>)
    constexpr function_ref(callable_type& callable RUVIA_LIFETIMEBOUND) noexcept :
        target_(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))),
        invoke_(&invoke_callable<callable_type>) {}

    template <typename callable_type>
        requires(!std::is_lvalue_reference_v<callable_type>) &&
        (!std::same_as<std::remove_cvref_t<callable_type>, function_ref>) &&
        (!std::same_as<std::remove_cvref_t<callable_type>, std::nullptr_t>)
    function_ref(callable_type&&) = delete;

    template <auto function_value>
        requires std::is_invocable_r_v<result_type, decltype(function_value), args_type...> &&
        (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, decltype(function_value), args_type...>)
    [[nodiscard]] static constexpr function_ref from_function() noexcept {
        return function_ref(nullptr, [](void*, args_type&&... args) noexcept(is_noexcept) -> result_type {
            return function_value(std::forward<args_type>(args)...);
        });
    }

    [[nodiscard]] static constexpr function_ref from_raw(void* target, invoke_type invoke) noexcept {
        return function_ref(target, invoke);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    result_type operator()(args_type... args) const noexcept(is_noexcept) {
        if (invoke_ == nullptr) {
            std::terminate();
        }
        return invoke_(target_, std::forward<args_type>(args)...);
    }

    [[nodiscard]] constexpr void* raw_target() const noexcept {
        return target_;
    }

    [[nodiscard]] constexpr invoke_type raw_invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] friend constexpr bool operator==(function_ref, function_ref) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(function_ref value, std::nullptr_t) noexcept {
        return !value;
    }

private:
    constexpr function_ref(void* target, invoke_type invoke) noexcept :
        target_(target),
        invoke_(invoke) {}

    template <typename callable_type>
    static result_type invoke_callable(void* target, args_type&&... args) noexcept(is_noexcept) {
        return (*static_cast<callable_type*>(target))(std::forward<args_type>(args)...);
    }

    void* target_{nullptr};
    invoke_type invoke_{nullptr};
};

static_assert(sizeof(function_ref<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<function_ref<void()>>);

}  // namespace ruvia
