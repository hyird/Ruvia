#pragma once

#include <cstdint>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_header_state_access final {
    [[nodiscard]] static std::uint32_t known_bits(const http_response_headers& headers) noexcept {
        return headers.known_header_bits_;
    }
};

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response.headers());
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(response.headers()) & bit) != 0;
}

}  // namespace ruvia::detail
