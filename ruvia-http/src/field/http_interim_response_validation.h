#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"

#include "field/response_field_validation.h"

namespace ruvia::detail {

enum class http_interim_response_header_validation_status : std::uint8_t {
    ok,
    invalid_header,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    repeated_singleton,
};

// Incremental sender validation for a version-neutral interim head. Recipients
// use their version's field syntax and message-boundary rules instead: 1xx
// framing advertisements do not establish content (RFC 9112 section 6.3 and
// RFC 9113 section 8.1.1).
class http_interim_response_sender_validator final {
public:
    [[nodiscard]] http_interim_response_header_validation_status validate(
        std::string_view name, std::string_view value,
        std::uint32_t* known_bit_out = nullptr) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto known_bit = classify_response_header_name(name);
        if (known_bit_out != nullptr) {
            *known_bit_out = known_bit;
        }
        if (known_bit == response_header_content_length) {
            return http_interim_response_header_validation_status::content_length_forbidden;
        }
        if (known_bit == response_header_transfer_encoding) {
            return http_interim_response_header_validation_status::transfer_encoding_forbidden;
        }
        if (known_bit == 0 && http_ascii_equals_ignore_case(name, "Trailer")) {
            return http_interim_response_header_validation_status::trailer_forbidden;
        }
        const auto kind = known_bit == response_header_content_type
                              ? request_header_kind::content_type
                              : (known_bit == response_header_content_encoding
                                        ? request_header_kind::content_encoding
                                        : request_header_kind::other);
        switch (validate_response_field(kind, value, http_field_list_role::sender, fields_)) {
            case response_field_status::ok:
                break;
            case response_field_status::invalid:
                return http_interim_response_header_validation_status::invalid_header;
            case response_field_status::repeated_singleton:
                return http_interim_response_header_validation_status::repeated_singleton;
        }
        if (known_bit != 0 && known_bit != response_header_content_type && (known_bits_ & known_bit) != 0 &&
            response_header_append_forbidden(known_bit)) {
            return http_interim_response_header_validation_status::repeated_singleton;
        }
        known_bits_ |= known_bit;
        return http_interim_response_header_validation_status::ok;
    }

private:
    response_field_state fields_;
    std::uint32_t known_bits_{0};
};

// Version-neutral interim-message validation. RFC 9110 section 8.6 and RFC
// 9112 section 6 forbid Content-Length and Transfer-Encoding on every 1xx;
// an interim response also cannot have the trailer section advertised by
// Trailer. Version-specific connection fields are checked by the HTTP/1 and
// HTTP/2 writers after this common pass.
[[nodiscard]] inline http_interim_response_header_validation_status validate_http_interim_response_headers(
    const http_interim_response_head& response) noexcept {
    http_interim_response_sender_validator validator;
    for (const auto& header : response.headers()) {
        const auto status = validator.validate(header.name(), header.value());
        if (status != http_interim_response_header_validation_status::ok) {
            return status;
        }
    }
    return http_interim_response_header_validation_status::ok;
}

}  // namespace ruvia::detail
