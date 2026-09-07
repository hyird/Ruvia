#pragma once

#include <string>

namespace ruvia {

struct MiddlewareScopeOptions final {
    std::string prefix{};
};

// App::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs Controller.h, Testing.h, or
// detail/middleware/MiddlewareRegistration.h. This header stays free of
// Context so App.h does not pull it in.

template <typename MiddlewareT>
class Middleware {
protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

}  // namespace ruvia
