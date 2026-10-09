#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_response.h"
#include "ruvia/web/error.h"

#include "ratelimit/rate_limit_key.h"
#include "ratelimit/rate_limiter.h"

namespace ruvia::detail {

[[nodiscard]] inline rate_limit_decision decide_request_rate_limit(
    rate_limiter_type* limiter, std::string_view remote_address) noexcept {
    if (limiter == nullptr || !limiter->has_default_rule()) {
        return rate_limit_decision::allow();
    }
    char key_buffer[rate_limit_key_buffer_bytes];
    return limiter->allow_default(rate_limit_key_for(remote_address, key_buffer));
}

[[nodiscard]] http_error_info rate_limit_rejection_error() noexcept;

void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection);

void apply_route_rate_limit_rejection_headers(
    http_response& response, const rate_limit_rejection& rejection, std::size_t max_requests);

}  // namespace ruvia::detail
