#include "ratelimit/rate_limit_decision.h"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/web/rate_limit.h"

#include "context/context_access.h"
#include "ratelimit/rate_limit_key.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] rate_limit_decision decide_route_rate_limit(
    context& context_value, const route_rate_limit_options& options) noexcept {
    auto* limiter = context_access::rate_limiter(context_value);
    if (limiter == nullptr) {
        return rate_limit_decision::allow();
    }

    char key_buffer[rate_limit_key_buffer_bytes];
    // The client, not the hop -- same as the app-wide limiter. Behind a trusted
    // proxy, remote() is the proxy and every caller would share one key.
    return limiter->allow_route(context_access::route_rate_limit_scope(context_value),
        rate_limit_key_for(context_value.conn().client().address(), key_buffer), options.rule_);
}

void set_unsigned_header(http_response& response, std::string_view name, std::uint64_t value) {
    char buffer[24];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        response.header(name, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

[[nodiscard]] std::uint64_t retry_after_seconds(std::chrono::milliseconds retry_after) noexcept {
    const auto milliseconds = retry_after.count();
    const auto positive_milliseconds =
        milliseconds <= 0 ? std::uint64_t{1} : static_cast<std::uint64_t>(milliseconds);
    return positive_milliseconds / 1000 + (positive_milliseconds % 1000 == 0 ? 0 : 1);
}

}  // namespace

http_error_info rate_limit_rejection_error() noexcept {
    return http_error_info({.status_ = ruvia::http_status::too_many_requests,
        .code_ = "too_many_requests",
        .message_ = "rate limit exceeded"});
}

void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection) {
    set_unsigned_header(response, "Retry-After", retry_after_seconds(rejection.retry_after()));
}

void apply_route_rate_limit_rejection_headers(
    http_response& response, const rate_limit_rejection& rejection, std::size_t max_requests) {
    const auto retry_after = retry_after_seconds(rejection.retry_after());
    set_unsigned_header(response, "Retry-After", retry_after);
    set_unsigned_header(response, "X-RateLimit-Limit", max_requests);
    set_unsigned_header(response, "X-RateLimit-Remaining", 0);
    set_unsigned_header(response, "X-RateLimit-Reset", retry_after);
}

bool apply_route_rate_limit(context& context_value, const route_rate_limit_options& options) {
    const auto decision = decide_route_rate_limit(context_value, options);
    const auto* rejection = decision.rejection();
    if (rejection == nullptr) {
        return true;
    }

    const auto error = rate_limit_rejection_error();
    auto response = context_value.error({
        .status_ = error.status(),
        .code_ = error.code(),
        .message_ = error.message(),
        .status_text_ = error.status_text(),
    });
    apply_route_rate_limit_rejection_headers(response, *rejection, options.rule_.max_requests_);
    context_value.respond(std::move(response));
    return false;
}

}  // namespace ruvia::detail
