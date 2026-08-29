#pragma once

#include "ruvia/core/Task.h"

#include <utility>

namespace ruvia::detail {

template <typename Result, typename... Args>
class CallableRef final {
public:
    using Invoke = Task<Result> (*)(void*, Args...);

    constexpr CallableRef() noexcept = default;
    constexpr CallableRef(void* target, Invoke invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    [[nodiscard]] bool valid() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] void* target() const noexcept {
        return target_;
    }

    [[nodiscard]] Invoke invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] Task<Result> operator()(Args... args) const {
        return invoke_(target_, std::forward<Args>(args)...);
    }

private:
    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

template <typename Result, typename... Args, typename Callable>
[[nodiscard]] CallableRef<Result, Args...> makeCallableRef(Callable& callable) noexcept {
    return CallableRef<Result, Args...>(&callable, [](void* target, Args... args) -> Task<Result> {
        return (*static_cast<Callable*>(target))(std::forward<Args>(args)...);
    });
}

}  // namespace ruvia::detail
