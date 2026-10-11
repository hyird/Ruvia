#pragma once

#include <string>

namespace ruvia {

struct middleware_scope_options final {
    std::string prefix_{};
};

// application::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs controller.h, testing.h, or
// detail/middleware/middleware_registration.h. This header stays free of
// context so app.h does not pull it in.

class middleware {
protected:
    constexpr middleware() noexcept = default;
    ~middleware() = default;
};

}  // namespace ruvia
