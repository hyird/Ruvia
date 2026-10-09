#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
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

inline bool vary_token_repeated_in_batch(const std::string_view* tokens, std::size_t current) noexcept {
    return std::ranges::any_of(
        std::span(tokens, current), [candidate = tokens[current]](std::string_view token) noexcept {
            return http_ascii_equals_ignore_case(token, candidate);
        });
}

[[nodiscard]] inline bool response_vary_has_token(
    const http_response& response, std::string_view token) noexcept {
    if (!response_has_known_header(response, response_header_vary)) {
        return false;
    }
    for (const auto& header : response.headers()) {
        if (response_header_known_bit(header) == response_header_vary &&
            http_has_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

inline void set_response_header_if_missing(
    http_response& response, std::uint32_t bit, std::string_view name, std::string_view value) {
    if (!response_has_known_header(response, bit)) {
        response.header(name, value);
    }
}

inline void set_stable_response_header_if_missing(
    http_response& response, std::uint32_t bit, std::string_view name, std::string_view value) {
    if (!response_has_known_header(response, bit)) {
        response.header_stable_view(name, value);
    }
}

inline void add_vary_tokens(
    http_response& response, const std::string_view* tokens, std::size_t token_count) {
    if (tokens == nullptr || token_count == 0) {
        return;
    }

    // RFC 9110 sections 5.2-5.3 define repeated Vary field lines as one
    // comma-joined value in wire order. Inspect every line: the O(1) known-header
    // lookup intentionally returns only the first occurrence and cannot decide
    // wildcard or token membership for this list-based field.
    if (response_vary_has_token(response, "*")) {
        return;
    }
    for (std::size_t i = 0; i < token_count; ++i) {
        if (http_trim_ows(tokens[i]) == "*") {
            response.header_stable_view("Vary", "*");
            return;
        }
    }
    const bool use_add_mask = token_count <= 64;
    std::uint64_t add_mask = 0;
    std::size_t added_count = 0;
    std::size_t added_bytes = 0;
    std::string_view first_added;
    for (std::size_t i = 0; i < token_count; ++i) {
        const auto token = tokens[i];
        if (token.empty() || response_vary_has_token(response, token) ||
            vary_token_repeated_in_batch(tokens, i)) {
            continue;
        }
        if (added_count == 0) {
            first_added = token;
        }
        if (use_add_mask) {
            add_mask |= std::uint64_t{1} << i;
        }
        ++added_count;
        added_bytes += token.size();
    }
    if (added_count == 0) {
        return;
    }

    std::size_t existing_value_count = 0;
    std::size_t existing_bytes = 0;
    if (response_has_known_header(response, response_header_vary)) {
        for (const auto& header : response.headers()) {
            if (response_header_known_bit(header) != response_header_vary ||
                http_trim_ows(header.value()).empty()) {
                continue;
            }
            ++existing_value_count;
            existing_bytes += header.value().size();
        }
    }

    if (existing_value_count == 0) {
        if (added_count == 1 && set_known_static_vary_token(response, first_added)) {
            return;
        }
    }

    std::pmr::string updated(response_resource(response));
    const auto part_count = existing_value_count + added_count;
    updated.reserve(existing_bytes + added_bytes + (part_count == 0 ? 0 : (part_count - 1) * 2));
    if (existing_value_count != 0) {
        for (const auto& header : response.headers()) {
            if (response_header_known_bit(header) != response_header_vary ||
                http_trim_ows(header.value()).empty()) {
                continue;
            }
            if (!updated.empty()) {
                updated.append(", ");
            }
            updated.append(header.value());
        }
    }
    for (std::size_t i = 0; i < token_count; ++i) {
        const auto token = tokens[i];
        if (use_add_mask) {
            if ((add_mask & (std::uint64_t{1} << i)) == 0) {
                continue;
            }
        } else {
            if (token.empty() || response_vary_has_token(response, token) ||
                vary_token_repeated_in_batch(tokens, i)) {
                continue;
            }
        }
        if (!updated.empty()) {
            updated.append(", ");
        }
        updated.append(token.data(), token.size());
    }
    response.header("Vary", updated);
}

inline void add_vary_token(http_response& response, std::string_view token) {
    // A single token is exactly a one-element batch; delegate so the dedup,
    // static-token fast path, and precise-reserve logic live in one place.
    add_vary_tokens(response, &token, 1);
}

}  // namespace ruvia::detail
