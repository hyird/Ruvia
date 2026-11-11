#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"

#include "field/http_connection_fields.h"
#include "field/response_field_validation.h"
#include "response/http_response_header_bits.h"

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
    // kind_out receives the field identity of an accepted line.
    [[nodiscard]] http_interim_response_header_validation_status validate(
        std::string_view name, std::string_view value,
        http_field_kind* kind_out = nullptr) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto field = classify_http_field_name(name);
        const auto known_bit = field.response_bit_;
        const auto kind = field.kind_;
        switch (kind) {
            case http_field_kind::content_length:
                return http_interim_response_header_validation_status::content_length_forbidden;
            case http_field_kind::transfer_encoding:
                return http_interim_response_header_validation_status::transfer_encoding_forbidden;
            case http_field_kind::trailer:
                return http_interim_response_header_validation_status::trailer_forbidden;
            default:
                break;
        }
        switch (validate_response_field(kind, value, fields_)) {
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
        if (kind_out != nullptr) {
            *kind_out = kind;
        }
        return http_interim_response_header_validation_status::ok;
    }

private:
    response_field_state fields_;
    std::uint32_t known_bits_{0};
};

}  // namespace ruvia::detail
