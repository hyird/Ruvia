#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "coding/http_content_length.h"
#include "coding/http_transfer_encoding.h"

namespace ruvia::detail {

enum class http1_version_parse_result : std::uint8_t { invalid,
    unsupported,
    http10,
    http11 };

// HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3). A higher
// HTTP/1 minor version is processed as HTTP/1.1 (RFC 9110 section 6.2).
[[nodiscard]] inline http1_version_parse_result parse_http1_version(std::string_view version) noexcept {
    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9') {
        return http1_version_parse_result::invalid;
    }
    if (version[5] != '1') {
        return http1_version_parse_result::unsupported;
    }
    return version[7] == '0' ? http1_version_parse_result::http10 : http1_version_parse_result::http11;
}

// Callers decide whether a framing field applies before accumulating it and
// retain their own error enum. Unsupported transfer codings are recorded by
// the accumulator for the caller's whole-section policy, not rejected here.
template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_content_length_field(
    std::string_view value, http_content_length_state& state) noexcept {
    switch (state.parse_field(value)) {
        case http_content_length_parse_status::ok:
            return std::nullopt;
        case http_content_length_parse_status::invalid:
            return error_type::invalid_content_length;
        case http_content_length_parse_status::conflicting:
            return error_type::conflicting_content_length;
    }
    return std::nullopt;
}

template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_transfer_encoding_field(
    std::string_view value, http_transfer_encoding_state& state) {
    if (state.parse_field(value) == http_transfer_encoding_parse_status::malformed) {
        return error_type::invalid_transfer_encoding;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
