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
    return RouteRateLimitOptions{.rule = RateLimitRule::fixedWindow(maxRequests, std::chrono::milliseconds(windowMs))};
}

}  // namespace detail

template <typename Derived>
class RouteRateLimitMiddleware : public Middleware<Derived> {
public:
    static constexpr bool ruviaUsesRouteRateLimit = true;

    Task<void> handle(Context& context, Next& next) {
        static_assert(Derived::ruviaRateLimitMaxRequests > 0, "route rate limit max requests must be greater than 0");
        static_assert(Derived::ruviaRateLimitWindowMs > 0, "route rate limit window must be greater than 0ms");

        if (!detail::applyRouteRateLimit(context, routeRateLimitOptions())) {
            co_return;
        }

        co_await next();
    }

private:
    [[nodiscard]] static const detail::RouteRateLimitOptions& routeRateLimitOptions() noexcept {
        static constexpr auto options = detail::routeRateLimitOptions(Derived::ruviaRateLimitMaxRequests, Derived::ruviaRateLimitWindowMs);
        return options;
    }
};

}  // namespace ruvia

// The only named entry point for a per-route rate limit. Derived middleware
// declares its own name (RUVIA_ROUTE_RATE_LIMIT(ReadyRateLimit, 10, 1000)) so
// it can be registered like any other middleware type.
#define RUVIA_ROUTE_RATE_LIMIT(name, max_requests, window_ms)                    \
    class name final : public ::ruvia::RouteRateLimitMiddleware<name> {          \
    public:                                                                      \
        static constexpr ::std::size_t ruviaRateLimitMaxRequests = max_requests; \
        static constexpr ::std::int64_t ruviaRateLimitWindowMs = window_ms;      \
    }
