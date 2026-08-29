#include "ruvia/web/RateLimit.h"

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/ratelimit/RateLimitKey.h"
#include "ruvia/web/detail/ratelimit/RateLimitDecision.h"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia::detail {

namespace {

[[nodiscard]] RateLimitDecision decideRouteRateLimit(
    Context& context, const RouteRateLimitOptions& options) noexcept {
    auto* limiter = ContextAccess::rateLimiter(context);
    if (limiter == nullptr) {
        return RateLimitDecision::allow();
    }

    char keyBuffer[kRateLimitKeyBufferBytes];
    // The client, not the hop -- same as the app-wide limiter. Behind a trusted
    // proxy, remote() is the proxy and every caller would share one key.
    return limiter->allowRoute(ContextAccess::routeRateLimitScope(context),
        rateLimitKeyFor(getConnInfo(context).client().address(), keyBuffer), options.rule);
}

void setUnsignedHeader(HttpResponse& response, std::string_view name, std::uint64_t value) {
    char buffer[24];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        response.header(name, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

[[nodiscard]] std::uint64_t retryAfterSeconds(std::chrono::milliseconds retryAfter) noexcept {
    const auto milliseconds = retryAfter.count();
    const auto positiveMilliseconds =
        milliseconds <= 0 ? std::uint64_t{1} : static_cast<std::uint64_t>(milliseconds);
    return positiveMilliseconds / 1000 + (positiveMilliseconds % 1000 == 0 ? 0 : 1);
}

}  // namespace

HttpErrorInfo rateLimitRejectionError() noexcept {
    return HttpErrorInfo({.status = ruvia::http_status::kTooManyRequests,
        .code = "too_many_requests",
        .message = "rate limit exceeded"});
}

void applyRateLimitRejectionHeaders(HttpResponse& response, const RateLimitRejection& rejection) {
    setUnsignedHeader(response, "Retry-After", retryAfterSeconds(rejection.retryAfter()));
}

void applyRouteRateLimitRejectionHeaders(
    HttpResponse& response, const RateLimitRejection& rejection, std::size_t maxRequests) {
    const auto retryAfter = retryAfterSeconds(rejection.retryAfter());
    setUnsignedHeader(response, "Retry-After", retryAfter);
    setUnsignedHeader(response, "X-RateLimit-Limit", maxRequests);
    setUnsignedHeader(response, "X-RateLimit-Remaining", 0);
    setUnsignedHeader(response, "X-RateLimit-Reset", retryAfter);
}

bool applyRouteRateLimit(Context& context, const RouteRateLimitOptions& options) {
    const auto decision = decideRouteRateLimit(context, options);
    const auto* rejection = decision.rejection();
    if (rejection == nullptr) {
        return true;
    }

    const auto error = rateLimitRejectionError();
    auto response = context.error({
        .status = error.status(),
        .code = error.code(),
        .message = error.message(),
        .statusText = error.statusText(),
    });
    applyRouteRateLimitRejectionHeaders(response, *rejection, options.rule.maxRequests);
    context.respond(std::move(response));
    return false;
}

}  // namespace ruvia::detail
