#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class http_field_kind : std::uint8_t {
    other,
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
    proxy_authorization
};

inline constexpr std::size_t http_field_kind_count =
    static_cast<std::uint8_t>(http_field_kind::proxy_authorization) + 1;

[[nodiscard]] inline constexpr std::size_t http_field_kind_known_slot(
    http_field_kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? http_field_kind_count : index - 1;
}

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(
    http_field_kind kind) noexcept {
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

}  // namespace ruvia::detail
