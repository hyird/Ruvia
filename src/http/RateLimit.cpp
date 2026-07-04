#include "ruvia/http/RateLimit.h"

#include "ContextInternal.h"
#include "../net/server/RateLimiter.h"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia::detail {

namespace {

class RouteRateLimitCheck final {
public:
    [[nodiscard]] bool allowed() const noexcept {
        return allowed_;
    }

    [[nodiscard]] std::int64_t resetAfterMs() const noexcept {
        return resetAfterMs_;
    }

    constexpr RouteRateLimitCheck() noexcept = default;

    constexpr RouteRateLimitCheck(bool allowed, std::int64_t resetAfterMs) noexcept
        : allowed_(allowed),
          resetAfterMs_(resetAfterMs) {}

private:
    bool allowed_{true};
    std::int64_t resetAfterMs_{1};
};

[[nodiscard]] RouteRateLimitCheck checkRouteRateLimit(
    Context& context,
    const RouteRateLimitOptions& options) noexcept {
    auto* limiter = ContextAccess::rateLimiter(context);
    if (limiter == nullptr || options.rule.maxRequests == 0) {
        return RouteRateLimitCheck{};
    }

    const auto check = limiter->allowRoute(
        ContextAccess::routeRateLimitScope(context),
        context.req().raw().remoteAddress(),
        options.rule);
    return RouteRateLimitCheck(check.allowed, check.resetAfterMs);
}

void setUnsignedHeader(HttpResponse& response, std::string_view name, std::uint64_t value) {
    char buffer[24];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        response.header(name, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

[[nodiscard]] std::uint64_t retryAfterSeconds(std::int64_t resetAfterMs) noexcept {
    return static_cast<std::uint64_t>((resetAfterMs <= 0 ? 1 : resetAfterMs + 999) / 1000);
}

}  // namespace

bool applyRouteRateLimit(
    Context& context,
    const RouteRateLimitOptions& options,
    std::size_t maxRequests) {
    const auto check = checkRouteRateLimit(context, options);
    if (check.allowed()) {
        return true;
    }

    auto response = context.error(429, "too_many_requests", "rate limit exceeded");
    const auto retryAfter = retryAfterSeconds(check.resetAfterMs());
    setUnsignedHeader(response, "Retry-After", retryAfter);
    setUnsignedHeader(response, "X-RateLimit-Limit", maxRequests);
    setUnsignedHeader(response, "X-RateLimit-Remaining", 0);
    setUnsignedHeader(response, "X-RateLimit-Reset", retryAfter);
    context.res(std::move(response));
    return false;
}

}  // namespace ruvia::detail
