#pragma once

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

struct MiddlewareScopeOptions final {
    BorrowedText prefix;
};

template <typename MiddlewareT>
class Middleware {
protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

}  // namespace ruvia
