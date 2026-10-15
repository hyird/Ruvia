#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http_client.h"

namespace ruvia {

// All views are borrowed only for the duration of encode_http3_client_request_head.
struct http3_client_request_head_view final {
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    // Application fields. Content-Length and Expect are generated from
    // body_length_ and expectation_ only; supplying either raw field is
    // http3_client_request_head_error::forbidden_field, as on HTTP/1 and HTTP/2.
    std::span<const http3_field_section_field_view> fields_{};
    // Declared content length; nullopt means content of unknown length follows
    // (or a CONNECT tunnel), zero means no content.
    // TRACE requires zero and emits no Content-Length. OPTIONS requires
    // Content-Type when content follows or Content-Length is emitted.
    std::optional<std::uint64_t> body_length_{};
    // Suppress automatic Content-Length generation from body_length_.
    bool emit_content_length_{true};
    // Generates `expect: 100-continue`. Valid only when content follows
    // (RFC 9110 section 10.1.1); otherwise, and for unknown values, the head
    // is rejected with http3_client_request_head_error::invalid_field.
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    // RFC 9220 Extended CONNECT; only valid with method CONNECT.
    std::string_view protocol_{};
    // Must come from received or restored peer SETTINGS (the connection's
    // effective_peer_settings()), not local configuration.
    bool peer_enable_connect_protocol_{false};
};

enum class http3_client_request_head_error : std::uint8_t {
    invalid_method,
    invalid_target,
    invalid_authority,
    invalid_field,
    forbidden_field,
    invalid_content_length,
    invalid_protocol,
    connect_protocol_disabled,
    field_section_error,
};

struct http3_client_request_body_plan final {
    std::optional<std::uint64_t> expected_length_{};
    [[nodiscard]] bool matches(std::uint64_t bytes_value) const noexcept {
        return !expected_length_ || *expected_length_ == bytes_value;
    }
};

struct http3_client_request_head final {
    // The complete HEADERS frame, owned by the resource supplied to the encoder.
    http3_headers_frame headers_;
    http3_client_request_body_plan body_plan_{};
};

struct http3_client_request_head_failure final {
    http3_client_request_head_error kind_;
    http3_field_section_error field_section_error_{http3_field_section_error::invalid_prefix};
};

// Produces the complete request HEADERS frame. QPACK's dynamic table capacity
// is zero; body bytes are never copied or concatenated.
[[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure>
encode_http3_client_request_head(http3_client_request_head_view view,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class http3_qpack_encoder;
[[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_http3_client_request_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, http3_client_request_head_view view,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
