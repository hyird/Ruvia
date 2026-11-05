#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <string_view>

#include "ruvia/http/http_response.h"
#include "ruvia/web/error.h"

#include "ratelimit/rate_limit_key.h"
#include "ratelimit/rate_limiter.h"

namespace ruvia::detail {

// The instant a request's rate-limit decisions are made for: the arrival its
// owner recorded (test_app supplies one), else a steady-clock reading taken
// only once a rule is actually consulted.
[[nodiscard]] inline std::chrono::steady_clock::time_point rate_limit_instant(
    const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
    return arrival.has_value() ? *arrival : std::chrono::steady_clock::now();
}

[[nodiscard]] inline std::optional<rate_limit_rejection> decide_request_rate_limit(rate_limiter* limiter,
    std::string_view remote_address, const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
    if (limiter == nullptr || !limiter->has_default_rule()) {
        return std::nullopt;
    }
    char key_buffer[rate_limit_key_buffer_bytes];
    return limiter->allow_default(rate_limit_key_for(remote_address, key_buffer), rate_limit_instant(arrival));
}

// Thrown by a route rate-limit middleware. Error dispatch answers it the way a
// driver answers the app-wide rule: rate_limit_rejection_error() through the
// application error handler, followed by the rejection headers.
class rate_limit_rejected final : public std::exception {
public:
    explicit rate_limit_rejected(rate_limit_rejection rejection) noexcept
        : rejection_(rejection) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "rate limit exceeded";
    }

    [[nodiscard]] const rate_limit_rejection& rejection() const noexcept {
        return rejection_;
    }

private:
    rate_limit_rejection rejection_;
};

[[nodiscard]] http_error_info rate_limit_rejection_error() noexcept;

// Every 429 a rate-limit rule produces carries the same fields: Retry-After
// (RFC 6585 section 4, RFC 9110 section 10.2.3) and the X-RateLimit-* fields
// naming the refusing rule.
void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection);

}  // namespace ruvia::detail
