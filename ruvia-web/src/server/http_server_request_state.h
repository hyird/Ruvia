#pragma once

#include <optional>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_request_body_failure.h"

namespace ruvia::detail {

inline std::optional<http_request_body_failure> content_length_limit_failure(
    const http1_request_body_plan& body_plan, protocol_byte_limit limit) noexcept {
    const auto* known_length = body_plan.known_length();
    return known_length == nullptr ? std::nullopt
                                   : http_request_body_size_failure(known_length->content_length(), limit);
}

}  // namespace ruvia::detail
