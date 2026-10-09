#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"

#include "coding/http_content_coding.h"
#include "field/http_media_type.h"

namespace ruvia::detail {

enum class http_interim_response_header_validation_status : std::uint8_t {
    ok,
    invalid_header,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    repeated_singleton,
};

// Incremental form of the version-neutral interim-field contract. Senders use
// it while validating a complete http_interim_response_head; receivers use the
// same state while HPACK progressively yields fields. Keeping the seen-known-
// field set here prevents protocol drivers from reimplementing the forbidden
// field and singleton rules with subtly different wire acceptance.
class http_interim_response_header_validator final {
public:
    explicit http_interim_response_header_validator(http_field_list_role role) noexcept
        : role_(role) {}

    [[nodiscard]] http_interim_response_header_validation_status validate(
        std::string_view name, std::string_view value) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto known_bit = classify_response_header_name(name);
        if (known_bit == response_header_content_encoding &&
            !is_valid_http_content_encoding_field_value(value, role_)) {
            return http_interim_response_header_validation_status::invalid_header;
        }
        if (known_bit == response_header_content_type && !is_valid_http_content_type_field_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }
        if (known_bit == response_header_content_length) {
            return http_interim_response_header_validation_status::content_length_forbidden;
        }
        if (known_bit == response_header_transfer_encoding) {
            return http_interim_response_header_validation_status::transfer_encoding_forbidden;
        }
        if (http_ascii_equals_ignore_case(name, "Trailer")) {
            return http_interim_response_header_validation_status::trailer_forbidden;
        }
        if (known_bit != 0 && (known_bits_ & known_bit) != 0 &&
            response_header_append_forbidden(known_bit)) {
            return http_interim_response_header_validation_status::repeated_singleton;
        }
        known_bits_ |= known_bit;
        return http_interim_response_header_validation_status::ok;
    }

private:
    http_field_list_role role_;
    std::uint32_t known_bits_{0};
};

// Version-neutral interim-message validation. RFC 9110 section 8.6 and RFC
// 9112 section 6 forbid Content-Length and Transfer-Encoding on every 1xx;
// an interim response also cannot have the trailer section advertised by
// Trailer. Version-specific connection fields are checked by the HTTP/1 and
// HTTP/2 writers after this common pass.
[[nodiscard]] inline http_interim_response_header_validation_status validate_http_interim_response_headers(
    const http_interim_response_head& response) noexcept {
    http_interim_response_header_validator validator(http_field_list_role::sender);
    for (const auto& header : response.headers()) {
        const auto status = validator.validate(header.name(), header.value());
        if (status != http_interim_response_header_validation_status::ok) {
            return status;
        }
    }
    return http_interim_response_header_validation_status::ok;
}

}  // namespace ruvia::detail
