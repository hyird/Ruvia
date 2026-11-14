#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "http_header.h"

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class http_field_kind : std::uint8_t { other,
    accept,
    accept_encoding,
    access_control_request_headers,
    access_control_request_method,
    authorization,
    connection,
    content_encoding,
    content_length,
    content_type,
    cookie,
    expect,
    host,
    if_match,
    if_modified_since,
    if_none_match,
    if_range,
    if_unmodified_since,
    origin,
    range,
    sec_websocket_key,
    sec_websocket_protocol,
    sec_websocket_version,
    transfer_encoding,
    upgrade,
    user_agent,
    forwarded,
    x_forwarded_for,
    x_forwarded_proto,
    sec_websocket_extensions,
    te,
    trailer,
    proxy_authorization };

inline constexpr std::size_t http_field_kind_count = static_cast<std::uint8_t>(http_field_kind::proxy_authorization) + 1;

[[nodiscard]] inline constexpr std::size_t http_field_kind_known_slot(http_field_kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? http_field_kind_count : index - 1;
}

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::access_control_request_method:
        case http_field_kind::authorization:
        case http_field_kind::content_type:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::origin:
        case http_field_kind::range:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::user_agent:
            return 1U << static_cast<std::uint32_t>(kind);
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_length:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::host:
        case http_field_kind::if_match:
        case http_field_kind::if_none_match:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::transfer_encoding:
        case http_field_kind::upgrade:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return 0;
    }
    return 0;
}

struct http_field_classification final {
    http_field_kind kind_{http_field_kind::other};
    std::uint32_t response_bit_{0};
};

[[nodiscard]] http_field_classification classify_http_field_name(std::string_view name) noexcept;
[[nodiscard]] http_field_kind classify_http_field(std::string_view name) noexcept;

inline constexpr std::uint32_t response_header_content_length = 1U << 0;
inline constexpr std::uint32_t response_header_content_encoding = 1U << 1;
inline constexpr std::uint32_t response_header_content_type = 1U << 2;
inline constexpr std::uint32_t response_header_connection = 1U << 3;
inline constexpr std::uint32_t response_header_vary = 1U << 4;
inline constexpr std::uint32_t response_header_date = 1U << 5;
inline constexpr std::uint32_t response_header_server = 1U << 6;
inline constexpr std::uint32_t response_header_cache_control = 1U << 7;
inline constexpr std::uint32_t response_header_transfer_encoding = 1U << 8;
inline constexpr std::uint32_t response_header_allow = 1U << 9;
inline constexpr std::uint32_t response_header_access_control_allow_origin = 1U << 10;
inline constexpr std::uint32_t response_header_access_control_allow_credentials = 1U << 11;
inline constexpr std::uint32_t response_header_access_control_allow_methods = 1U << 12;
inline constexpr std::uint32_t response_header_access_control_allow_headers = 1U << 13;
inline constexpr std::uint32_t response_header_access_control_max_age = 1U << 14;
inline constexpr std::uint32_t response_header_access_control_expose_headers = 1U << 15;
inline constexpr std::uint32_t response_header_accept_ranges = 1U << 16;
inline constexpr std::uint32_t response_header_content_range = 1U << 17;
inline constexpr std::uint32_t response_header_etag = 1U << 18;
inline constexpr std::uint32_t response_header_last_modified = 1U << 19;
inline constexpr std::uint32_t response_header_location = 1U << 20;
inline constexpr std::uint32_t response_header_set_cookie = 1U << 21;
inline constexpr std::size_t response_known_header_count = 22;
inline constexpr std::uint32_t response_header_upgrade = 1U << 22;
inline constexpr std::uint32_t response_header_trailer = 1U << 23;
inline constexpr std::uint32_t response_header_keep_alive = 1U << 24;
inline constexpr std::uint32_t response_header_proxy_connection = 1U << 25;

[[nodiscard]] inline constexpr std::size_t response_known_header_slot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t known_mask = (1U << response_known_header_count) - 1U;
    if (bit == 0 || (bit & ~known_mask) != 0 || (bit & (bit - 1U)) != 0) {
        return response_known_header_count;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

[[nodiscard]] inline bool response_header_append_forbidden(std::uint32_t bit) noexcept {
    return bit == response_header_content_length || bit == response_header_content_type || bit == response_header_date || bit == response_header_server || bit == response_header_transfer_encoding || bit == response_header_allow || bit == response_header_access_control_allow_origin || bit == response_header_access_control_allow_credentials || bit == response_header_access_control_max_age || bit == response_header_accept_ranges || bit == response_header_content_range || bit == response_header_etag || bit == response_header_last_modified || bit == response_header_location;
}

[[nodiscard]] std::uint32_t classify_response_header_name(std::string_view name) noexcept;

// RFC 7541 §7.1.3 / RFC 9204 §4.2.1: credential-bearing fields use the
// never-indexed literal so an intermediary never places them in a shared dynamic table.
[[nodiscard]] inline bool http_field_never_indexed(std::string_view name) noexcept {
    const auto field = classify_http_field_name(name);
    return field.kind_ == http_field_kind::authorization || field.kind_ == http_field_kind::proxy_authorization || field.kind_ == http_field_kind::cookie || field.response_bit_ == response_header_set_cookie;
}

[[nodiscard]] std::string_view response_known_header_lowercase_name(std::uint32_t known_bit) noexcept;

// Case-insensitive field-name set membership (RFC 9110 section 5.1).
[[nodiscard]] inline bool http_field_name_in(std::string_view name, std::span<const std::string_view> fields) noexcept {
    return std::ranges::any_of(fields, [name](std::string_view field) noexcept {
        return http_ascii_equals_ignore_case(name, field);
    });
}

// A successful CONNECT or extended-CONNECT tunnel carries no representation
// (RFC 9110 section 9.3.6, RFC 9298 section 3): its head must not declare
// framing or representation metadata.
inline constexpr std::array<std::string_view, 5> http_connect_forbidden_fields{"content-length", "transfer-encoding", "content-type", "content-encoding", "trailer"};

// HTTP/2 applies the connection-specific field ban. Decoded-name
// lowercase validation and the request-only TE value exception belong to the
// protocol boundary; application-owned names are matched case-insensitively.
[[nodiscard]] inline bool is_forbidden_http_binary_connection_field(std::string_view name) noexcept {
    switch (name.size()) {
        case 7:
            return http_ascii_equals_ignore_case(name, "upgrade");
        case 10:
            return http_ascii_equals_ignore_case(name, "connection") || http_ascii_equals_ignore_case(name, "keep-alive");
        case 16:
            return http_ascii_equals_ignore_case(name, "proxy-connection");
        case 17:
            return http_ascii_equals_ignore_case(name, "transfer-encoding");
        default:
            return false;
    }
}

[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

inline constexpr auto binary_field_name_char_table = [] {
    auto table_value = http_token_char_table;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table_value[byte] = false;
    }
    return table_value;
}();

// HTTP/2 (RFC 9113 §8.2.1) and HTTP/3 (RFC 9114 §4.2) regular field names are
// nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (name.size() - index >= 4) {
        const auto* bytes_value = reinterpret_cast<const unsigned char*>(name.data() + index);
        const bool first = binary_field_name_char_table[bytes_value[0]];
        const bool second = binary_field_name_char_table[bytes_value[1]];
        const bool third = binary_field_name_char_table[bytes_value[2]];
        const bool fourth = binary_field_name_char_table[bytes_value[3]];
        if (!(first & second & third & fourth)) {
            return false;
        }
        index += 4;
    }
    while (index < name.size()) {
        if (!binary_field_name_char_table[static_cast<unsigned char>(name[index])]) {
            return false;
        }
        ++index;
    }
    return true;
}

}  // namespace ruvia::detail
