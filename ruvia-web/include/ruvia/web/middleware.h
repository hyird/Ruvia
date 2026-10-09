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
// context so application.h does not pull it in.
// A middleware may declare `static constexpr bool ruvia_replay_safe = true;`.
// Putting that middleware in a route macro's middleware list explicitly opts
// the route's handler and declared middleware chain into HTTP/3 0-RTT replay.

class middleware {
protected:
    constexpr middleware() noexcept = default;
    ~middleware() = default;
};

}  // namespace ruvia
