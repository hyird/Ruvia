#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class request_header_kind : std::uint8_t {
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
    sec_websocket_extensions
};

inline constexpr std::size_t request_header_kind_count =
    static_cast<std::uint8_t>(request_header_kind::sec_websocket_extensions) + 1;

[[nodiscard]] inline constexpr std::size_t request_header_kind_known_slot(
    request_header_kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? request_header_kind_count : index - 1;
}

}  // namespace ruvia::detail
