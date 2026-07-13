#pragma once

namespace ruvia {

template <typename MiddlewareT>
class Middleware {
protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

}  // namespace ruvia
