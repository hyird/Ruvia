#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "ruvia/core/task.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/next.h"
#include "ruvia/web/rate_limit_rule.h"

namespace ruvia {

namespace detail {

struct route_rate_limit_options final {
    rate_limit_rule rule_{};
};

[[nodiscard]] bool apply_route_rate_limit(context& context_value, const route_rate_limit_options& options);

[[nodiscard]] constexpr route_rate_limit_options get_route_rate_limit_options(
    std::size_t max_requests, std::int64_t window_ms) {
    return route_rate_limit_options{.rule_ = {
                                        .max_requests_ = max_requests,
                                        .window_ = std::chrono::milliseconds(window_ms),
                                    }};
}

}  // namespace detail

// A per-route rate limit, configured through the type:
//
//     RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
//
// Route and controller middleware lists name types, so a middleware registered
// there is default constructed and cannot take constructor arguments. Carrying
// the configuration as template parameters is how such a middleware is
// configured -- the values are constexpr, the type stays default constructible,
// and the chain is still finalized at startup with nothing allocated per
// request. A comma inside the template argument list is fine: the route macro
// pastes its arguments back together.
//
// The limit is scoped to the route it is registered on and keyed on the client
// address, so two routes carrying the same numbers count independently. It does
// not replace application::get_rate_limit() -- both apply, so the stricter is what a
// caller meets, the same "narrower scope may only tighten" rule body_limit
// follows. Worker-local, like the app-wide rule.
template <std::size_t max_requests, std::int64_t window_ms>
class rate_limit final : public middleware {
public:
    static_assert(max_requests > 0, "route rate limit max requests must be greater than 0");
    static_assert(window_ms > 0, "route rate limit window must be greater than 0ms");

    static constexpr bool ruvia_uses_route_rate_limit = true;

    task<void> handle(context& context_value, next& next_value) {
        if (!detail::apply_route_rate_limit(context_value, options())) {
            co_return;
        }

        co_await next_value();
    }

private:
    [[nodiscard]] static const detail::route_rate_limit_options& options() noexcept {
        static constexpr auto value = detail::get_route_rate_limit_options(max_requests, window_ms);
        return value;
    }
};

}  // namespace ruvia
