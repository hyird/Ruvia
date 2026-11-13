#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"

namespace ruvia::detail {

// Trivial request/runtime view. Never allocates or extends the callable's
// lifetime. Signature policy changes invocation, not storage or binding logic.
template <typename result_type, bool is_noexcept, typename... args_types>
class callback_ref_view final {
public:
    constexpr callback_ref_view() noexcept = default;
    constexpr callback_ref_view(std::nullptr_t) noexcept {}
    // Select concrete pointer types so MSVC resolves the exception specification.
    using invoke_type = std::conditional_t<is_noexcept,
        result_type (*)(void*, args_types...) noexcept,
        result_type (*)(void*, args_types...)>;
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] result_type operator()(args_types... args) const noexcept(is_noexcept) {
        return invoke_target(std::forward<args_types>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(callback_ref_view, callback_ref_view) noexcept = default;

private:
    friend struct callback_access;

    constexpr callback_ref_view(void* target, invoke_type invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    template <typename callable_type>
        requires(std::is_invocable_r_v<result_type, callable_type&, args_types...> &&
                 (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, callable_type&, args_types...>))
    [[nodiscard]] static constexpr callback_ref_view bind(callable_type& callable) noexcept {
        return callback_ref_view(const_cast<void*>(static_cast<const void*>(std::addressof(callable))),
            [](void* target, args_types... args) noexcept(is_noexcept) -> result_type {
                return (*static_cast<callable_type*>(target))(std::forward<args_types>(args)...);
            });
    }

    template <typename... invocation_args>
    [[nodiscard]] result_type invoke_target(invocation_args&&... args) const noexcept(is_noexcept) {
        return invoke_(target_, std::forward<invocation_args>(args)...);
    }

    void* target_{nullptr};
    invoke_type invoke_{nullptr};
};

// Signature decomposition selects the borrowed view's noexcept policy.
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
using callback_ref = typename callback_signature<signature_type>::template apply<callback_ref_view>;

struct callback_access final {
    template <typename signature_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> make(
        void* target, typename callback_ref<signature_type>::invoke_type invoke) noexcept {
        return callback_ref<signature_type>(target, invoke);
    }

    template <typename signature_type>
    [[nodiscard]] static callback_ref<signature_type> ref(const move_only_function<signature_type>& owner_value) noexcept {
        return owner_value ? callback_ref<signature_type>::bind(owner_value) : callback_ref<signature_type>{};
    }

    template <typename callback_type, typename deleter_type>
    [[nodiscard]] static auto ref(const std::unique_ptr<callback_type, deleter_type>& owner_value) noexcept {
        using ref_type = decltype(ref(*owner_value));
        return owner_value ? ref(*owner_value) : ref_type{};
    }

    template <typename signature_type, typename callable_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> bind(callable_type& callable) noexcept {
        return callback_ref<signature_type>::bind(callable);
    }
    template <typename signature_type, typename callable_type>
        requires(!std::is_lvalue_reference_v<callable_type>)
    static void bind(callable_type&&) = delete;

    template <typename ref_type>
    [[nodiscard]] static constexpr void* target(ref_type value) noexcept {
        return value.target_;
    }

    template <typename ref_type>
    [[nodiscard]] static constexpr typename ref_type::invoke_type invoke(ref_type value) noexcept {
        return value.invoke_;
    }
};

static_assert(sizeof(callback_ref<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<callback_ref<void()>>);
}  // namespace ruvia::detail

// The three non-owning callable shapes a route runs: a buffered handler, a
// streaming (response-stream / websocket / tunnel) handler, and one middleware
// frame. Each binds a worker-local instance to a process-invariant invoke thunk.

namespace ruvia {

class context;
class http_response;
class next;

namespace detail {

enum class request_body_mode : std::uint8_t { buffered,
    stream };

using route_handler = callback_ref<task<http_response>(context&)>;
using route_stream_handler = callback_ref<task<void>(context&)>;
using route_middleware = callback_ref<task<void>(context&, next&)>;

}  // namespace detail

}  // namespace ruvia
