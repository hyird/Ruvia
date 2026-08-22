#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "ruvia/web/RateLimitRule.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

namespace detail {

struct RouteRateLimitOptions final {
    RateLimitRule rule;
};

[[nodiscard]] bool applyRouteRateLimit(Context& context, const RouteRateLimitOptions& options);

[[nodiscard]] constexpr RouteRateLimitOptions routeRateLimitOptions(std::size_t maxRequests, std::int64_t windowMs) {
    return RouteRateLimitOptions{.rule = RateLimitRule::fixedWindow({
        .maxRequests = maxRequests,
        .window = std::chrono::milliseconds(windowMs),
    })};
}

}  // namespace detail

// A per-route rate limit, configured through the type:
//
//     RUVIA_GET("/ready", ready, ruvia::RateLimit<10, 1000>);
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
// not replace App::setRateLimit() -- both apply, so the stricter is what a
// caller meets, the same "narrower scope may only tighten" rule BodyLimit
// follows. Worker-local, like the app-wide rule.
template <std::size_t MaxRequests, std::int64_t WindowMs>
class RateLimit final : public Middleware<RateLimit<MaxRequests, WindowMs>> {
public:
    static_assert(MaxRequests > 0, "route rate limit max requests must be greater than 0");
    static_assert(WindowMs > 0, "route rate limit window must be greater than 0ms");

    static constexpr bool ruviaUsesRouteRateLimit = true;

    Task<void> handle(Context& context, Next& next) {
        if (!detail::applyRouteRateLimit(context, options())) {
            co_return;
        }

        co_await next();
    }

private:
    [[nodiscard]] static const detail::RouteRateLimitOptions& options() noexcept {
        static constexpr auto value = detail::routeRateLimitOptions(MaxRequests, WindowMs);
        return value;
    }
};

}  // namespace ruvia
