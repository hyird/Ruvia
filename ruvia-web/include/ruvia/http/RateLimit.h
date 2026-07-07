#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "ruvia/app/RateLimitRule.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

namespace ruvia {

namespace detail {

struct RouteRateLimitOptions final {
    RateLimitRule rule;
};

[[nodiscard]] bool applyRouteRateLimit(
    Context& context,
    const RouteRateLimitOptions& options,
    std::size_t maxRequests);

[[nodiscard]] constexpr RouteRateLimitOptions routeRateLimitOptions(
    std::size_t maxRequests,
    std::int64_t windowMs) noexcept {
    RateLimitRule rule;
    rule.maxRequests = maxRequests;
    rule.window = std::chrono::milliseconds(windowMs);
    return RouteRateLimitOptions{.rule = normalizeRateLimitRule(rule)};
}

}  // namespace detail

template <typename Derived>
class RouteRateLimitMiddleware : public Middleware<Derived> {
public:
    Task<void> handle(Context& context, Next& next) {
        static_assert(
            Derived::ruviaRateLimitMaxRequests > 0,
            "route rate limit max requests must be greater than 0");
        static_assert(
            Derived::ruviaRateLimitWindowMs > 0,
            "route rate limit window must be greater than 0ms");

        if (!detail::applyRouteRateLimit(context, routeRateLimitOptions(), Derived::ruviaRateLimitMaxRequests)) {
            co_return;
        }

        co_await next();
    }

private:
    [[nodiscard]] static const detail::RouteRateLimitOptions& routeRateLimitOptions() noexcept {
        static constexpr auto options = detail::routeRateLimitOptions(
            Derived::ruviaRateLimitMaxRequests,
            Derived::ruviaRateLimitWindowMs);
        return options;
    }
};

template <std::size_t MaxRequests, std::int64_t WindowMs>
class RouteRateLimit final : public RouteRateLimitMiddleware<RouteRateLimit<MaxRequests, WindowMs>> {
public:
    static constexpr std::size_t ruviaRateLimitMaxRequests = MaxRequests;
    static constexpr std::int64_t ruviaRateLimitWindowMs = WindowMs;
};

}  // namespace ruvia

#define RUVIA_ROUTE_RATE_LIMIT(name, max_requests, window_ms) \
    class name final : public ::ruvia::RouteRateLimitMiddleware<name> { \
    public: \
        static constexpr ::std::size_t ruviaRateLimitMaxRequests = max_requests; \
        static constexpr ::std::int64_t ruviaRateLimitWindowMs = window_ms; \
    }
