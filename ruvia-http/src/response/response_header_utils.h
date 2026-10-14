#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"

namespace ruvia::detail {

inline bool set_known_static_vary_token(http_response& response, std::string_view token) {
    if (token == "Accept-Encoding" || token == "Origin" ||
        token == "Access-Control-Request-Headers" || token == "Access-Control-Request-Method") {
        response.header_stable_view("Vary", token);
        return true;
    }
    return false;
}

[[nodiscard]] inline bool response_vary_has_token(
    const http_response& response, std::string_view token) noexcept {
    if (!response_has_known_header(response, response_header_vary)) {
        return false;
    }
    for (const auto& header : response.headers()) {
        if (http_response_header_access::known_bit(header) == response_header_vary &&
            http_has_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

inline void add_vary_token(http_response& response, std::string_view token) {
    // RFC 9110 sections 5.2-5.3 define repeated Vary field lines as one
    // comma-joined value in wire order. Inspect every line: the O(1) known-header
    // lookup intentionally returns only the first occurrence and cannot decide
    // wildcard or token membership for this list-based field.
    token = http_trim_ows(token);
    if (token.empty() || response_vary_has_token(response, "*")) {
        return;
    }

    if (token == "*") {
        response.header_stable_view("Vary", "*");
        return;
    }
    if (response_vary_has_token(response, token)) {
        return;
    }
    if (!is_valid_http_header_name(token)) {
        throw std::invalid_argument("invalid HTTP Vary header token");
    }

    std::size_t existing_value_count = 0;
    std::size_t existing_bytes = 0;
    if (response_has_known_header(response, response_header_vary)) {
        for (const auto& header : response.headers()) {
            if (http_response_header_access::known_bit(header) != response_header_vary ||
                http_trim_ows(header.value()).empty()) {
                continue;
            }
            ++existing_value_count;
            existing_bytes += header.value().size();
        }
    }
    if (existing_value_count == 0 && set_known_static_vary_token(response, token)) {
        return;
    }

    std::pmr::string updated(response.memory_resource());
    updated.reserve(existing_bytes + token.size() + existing_value_count * 2);
    if (existing_value_count != 0) {
        for (const auto& header : response.headers()) {
            if (http_response_header_access::known_bit(header) != response_header_vary ||
                http_trim_ows(header.value()).empty()) {
                continue;
            }
            updated.append(header.value());
            updated.append(", ");
        }
    }
    updated.append(token);
    response.header("Vary", updated);
}

}  // namespace ruvia::detail
