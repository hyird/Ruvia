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

namespace ruvia {

// All views are borrowed only for the duration of encode_http3_client_request_head.
struct http3_client_request_head_view final {
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    std::span<const http3_field_section_field_view> fields_{};
    std::optional<std::uint64_t> body_length_{};
    // Suppress automatic Content-Length generation while still validating an
    // explicitly supplied field against body_length.
    bool emit_content_length_{true};
    // RFC 9220 Extended CONNECT; only valid with method CONNECT.
    std::string_view protocol_{};
    // Must come from the received peer SETTINGS, not local configuration.
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
    // Both values are owned by the resource supplied to the encoder.
    std::pmr::vector<char> field_section_;
    http3_client_request_body_plan body_plan_{};

    explicit http3_client_request_head(std::pmr::memory_resource* resource)
        : field_section_(resource) {}
};

struct http3_client_request_head_failure final {
    http3_client_request_head_error kind_;
    http3_field_section_error field_section_error_{http3_field_section_error::invalid_prefix};
};

// Produces the QPACK field-section payload (not an HTTP/3 frame). QPACK's
// dynamic table capacity is zero; body bytes are never copied or concatenated.
[[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure>
encode_http3_client_request_head(http3_client_request_head_view view,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class http3_qpack_encoder;
[[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_http3_client_request_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, http3_client_request_head_view view,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
