#pragma once

#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_field_syntax.h"

namespace ruvia::detail {

// The configured User-Agent (RFC 9110 section 10.1.5) of the HTTP client and
// both WebSocket client protocols: sent when a request or handshake carries no
// User-Agent field of its own; empty sends none.
inline void validate_client_user_agent(std::string_view user_agent) {
    if (!user_agent.empty() && !is_valid_http_header_value(user_agent)) {
        throw std::invalid_argument("client user agent is invalid");
    }
}

}  // namespace ruvia::detail
