#pragma once

#include <utility>

#include "ruvia/core/task.h"

namespace ruvia::detail {

template <typename result_type, typename... args_type>
class callable_ref final {
public:
    using invoke_type = task<result_type> (*)(void*, args_type...);

    constexpr callable_ref() noexcept = default;
    constexpr callable_ref(void* target, invoke_type invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    [[nodiscard]] bool valid() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] void* target() const noexcept {
        return target_;
    }

    [[nodiscard]] invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] task<result_type> operator()(args_type... args) const {
        return invoke_(target_, std::forward<args_type>(args)...);
    }

private:
    void* target_{nullptr};
    invoke_type invoke_{nullptr};
};

template <typename result_type, typename... args_type, typename callable_type>
[[nodiscard]] callable_ref<result_type, args_type...> make_callable_ref(callable_type& callable) noexcept {
    return callable_ref<result_type, args_type...>(&callable, [](void* target, args_type... args) -> task<result_type> {
        return (*static_cast<callable_type*>(target))(std::forward<args_type>(args)...);
    });
}

}  // namespace ruvia::detail
