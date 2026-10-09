#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

// RFC 9110 method requirements for a request that explicitly carries content.
// Senders and recipients use the same protocol rule regardless of wire version.
enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics http_request_content_semantics(
    std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return http_request_content_semantics::forbidden;
    }
    if (method == "OPTIONS") {
        return http_request_content_semantics::content_type_required;
    }
    return http_request_content_semantics::no_additional_requirements;
}

}  // namespace ruvia
