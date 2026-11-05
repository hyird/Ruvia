#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/http_field_syntax.h"

namespace ruvia::detail {

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(
    request_header_kind kind) noexcept {
    switch (kind) {
        case request_header_kind::access_control_request_method:
        case request_header_kind::authorization:
        case request_header_kind::content_type:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::origin:
        case request_header_kind::range:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::user_agent:
            return 1U << static_cast<std::uint32_t>(kind);
        case request_header_kind::other:
        case request_header_kind::accept:
        case request_header_kind::accept_encoding:
        case request_header_kind::access_control_request_headers:
        case request_header_kind::connection:
        case request_header_kind::content_encoding:
        case request_header_kind::content_length:
        case request_header_kind::cookie:
        case request_header_kind::expect:
        case request_header_kind::host:
        case request_header_kind::if_match:
        case request_header_kind::if_none_match:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::transfer_encoding:
        case request_header_kind::upgrade:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
        case request_header_kind::te:
        case request_header_kind::trailer:
        case request_header_kind::proxy_authorization:
            return 0;
    }
    return 0;
}

[[nodiscard]] request_header_kind classify_request_header(std::string_view name) noexcept;

struct http_chunk_size_line final {
    std::size_t size_{0};
    // Line bytes beyond the shortest hex spelling of size_: leading zeros and
    // chunk-ext. A decoder charges them to a per-message framing budget.
    std::size_t overhead_bytes_{0};
};

// Parses `chunk-size [ chunk-ext ]` (RFC 9112 7.1) without its CRLF; nullopt on
// any malformed or overflowing line.
[[nodiscard]] std::optional<http_chunk_size_line> parse_http_chunk_size_line(std::string_view value) noexcept;

}  // namespace ruvia::detail
