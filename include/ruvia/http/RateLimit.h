#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/app/RateLimitRule.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

namespace ruvia {

namespace detail {

struct RouteRateLimitOptions final {
    RateLimitRule rule;
};

struct RouteRateLimitResult final {
    bool allowed{true};
    std::int64_t resetAfterMs{1};
};

[[nodiscard]] RouteRateLimitResult checkRouteRateLimit(
    Context& context,
    const RouteRateLimitOptions& options) noexcept;

[[nodiscard]] constexpr RouteRateLimitOptions routeRateLimitOptions(
    std::size_t maxRequests,
    std::int64_t windowMs) noexcept {
    RateLimitRule rule;
    rule.maxRequests = maxRequests;
    rule.window = std::chrono::milliseconds(windowMs);
    return RouteRateLimitOptions{.rule = normalizeRateLimitRule(rule)};
}

inline void setUnsignedHeader(HttpResponse& response, std::string_view name, std::uint64_t value) {
    char buffer[24];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        response.setHeader(name, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

[[nodiscard]] inline std::uint64_t retryAfterSeconds(std::int64_t resetAfterMs) noexcept {
    return static_cast<std::uint64_t>((resetAfterMs <= 0 ? 1 : resetAfterMs + 999) / 1000);
}

}  // namespace detail

template <typename Derived>
class RouteRateLimitMiddleware : public Middleware<Derived> {
public:
    Task<void> handle(Context& context, const Next& next) {
        static_assert(
            Derived::ruviaRateLimitMaxRequests > 0,
            "route rate limit max requests must be greater than 0");
        static_assert(
            Derived::ruviaRateLimitWindowMs > 0,
            "route rate limit window must be greater than 0ms");

        const auto check = detail::checkRouteRateLimit(context, routeRateLimitOptions());
        if (!check.allowed) {
            auto response = context.error(429, "too_many_requests", "rate limit exceeded");
            detail::setUnsignedHeader(response, "Retry-After", detail::retryAfterSeconds(check.resetAfterMs));
            detail::setUnsignedHeader(response, "X-RateLimit-Limit", Derived::ruviaRateLimitMaxRequests);
            detail::setUnsignedHeader(response, "X-RateLimit-Remaining", 0);
            detail::setUnsignedHeader(response, "X-RateLimit-Reset", detail::retryAfterSeconds(check.resetAfterMs));
            context.res(std::move(response));
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
