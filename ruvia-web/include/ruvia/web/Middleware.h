#pragma once

#include <string>

namespace ruvia {

struct MiddlewareScopeOptions final {
    std::string prefix;
};

template <typename MiddlewareT>
class Middleware {
protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

}  // namespace ruvia
