#include "ratelimit/rate_limit_decision.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/rate_limit.h"

#include "context/context_access.h"
#include "ratelimit/rate_limit_key.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] std::optional<rate_limit_rejection> decide_route_rate_limit(
    context& context_value, const void* declaration, const rate_limit_rule& rule) noexcept {
    auto* limiter = context_access::rate_limiter(context_value);
    if (limiter == nullptr) {
        return std::nullopt;
    }

    char key_buffer[rate_limit_key_buffer_bytes];
    // The client, not the hop -- same as the app-wide limiter. Behind a trusted
    // proxy, remote() is the proxy and every caller would share one key.
    return limiter->allow_route(context_access::route_rate_limit_scope(context_value), declaration,
        rate_limit_key_for(context_value.conn().client().address(), key_buffer), rule);
}

void set_unsigned_header(http_response& response, std::string_view name, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> buffer;
    response.header(name, http_format_unsigned(buffer, value));
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
    response.reserve_headers(response.headers().size() + 4U);
    std::array<char, http_max_unsigned_decimal_size> retry_buffer{};
    const auto retry_after_text =
        http_format_unsigned(retry_buffer, retry_after_seconds(rejection.retry_after()));
    response.header("Retry-After", retry_after_text);
    set_unsigned_header(response, "X-RateLimit-Limit", max_requests);
    response.header("X-RateLimit-Remaining", "0");
    response.header("X-RateLimit-Reset", retry_after_text);
}

bool apply_route_rate_limit(
    context& context_value, const void* declaration, const rate_limit_rule& rule) {
    const auto rejection = decide_route_rate_limit(context_value, declaration, rule);
    if (!rejection) {
        return true;
    }

    const auto error = rate_limit_rejection_error();
    auto response = context_value.error({
        .status_ = error.status(),
        .code_ = error.code(),
        .message_ = error.message(),
        .status_text_ = error.status_text(),
    });
    apply_route_rate_limit_rejection_headers(response, *rejection, rule.max_requests_);
    context_value.respond(std::move(response));
    return false;
}

}  // namespace ruvia::detail
