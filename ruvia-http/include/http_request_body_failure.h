#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "http_content_coding.h"
#include "http_protocol.h"
#include "http_status.h"

namespace ruvia {

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
    buffer_limit,
};

[[nodiscard]] inline http_protocol_error make_http_protocol_error(http_request_body_failure failure) noexcept {
    switch (failure) {
        case http_request_body_failure::too_large:
            return http_protocol_error(http_status::content_too_large, "request_body_too_large", "request body is too large");
        case http_request_body_failure::incomplete:
            return http_protocol_error(http_status::bad_request, "incomplete_request_body", "incomplete request body");
        case http_request_body_failure::buffer_limit:
            return http_protocol_error(http_status::service_unavailable, "inbound_buffer_limit", "inbound buffer capacity exhausted");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request_body", "invalid request body");
}

// The request field that named the coding a decoder failed to remove. The same
// decode error answers with a different status for each.
enum class http_coding_field : std::uint8_t {
    content_encoding,
    transfer_encoding,
};

// Status for a request body whose Content-Encoding or Transfer-Encoding could
// not be decoded. A transfer-coding unsupported_coding cannot occur: framing
// rejects such stacks with 501 first.
[[nodiscard]] inline http_protocol_error make_http_protocol_error(http_content_decode_error error, http_coding_field field) noexcept {
    if (error == http_content_decode_error::decoded_size_exceeded) {
        return make_http_protocol_error(http_request_body_failure::too_large);
    }
    if (field == http_coding_field::transfer_encoding) {
        if (error == http_content_decode_error::unsupported_coding) {
            return make_http_protocol_error(http_parse_error::unsupported_transfer_encoding);
        }
        return http_protocol_error(http_status::bad_request, "invalid_transfer_coding_body", "invalid transfer-coding body");
    }
    if (error == http_content_decode_error::unsupported_coding) {
        return http_protocol_error(http_status::unsupported_media_type, "unsupported_content_coding", "request Content-Encoding is not supported");
    }
    return http_protocol_error(http_status::bad_request, "invalid_content_coding", "failed to decode request body");
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(std::size_t size, protocol_byte_limit limit) noexcept {
    return limit.exceeds(size) ? std::optional<http_request_body_failure>(http_request_body_failure::too_large) : std::nullopt;
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(std::size_t current_size, std::size_t additional_size, protocol_byte_limit limit) noexcept {
    return limit.addition_exceeds(current_size, additional_size) ? std::optional<http_request_body_failure>(http_request_body_failure::too_large) : std::nullopt;
}

}  // namespace ruvia
