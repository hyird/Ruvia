#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/websocket_handshake.h"

#include "field/http_field_kind.h"

namespace ruvia {

class http_request;

namespace detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-websocket-Accept response field (always 28 bytes).
using websocket_accept_key_type = std::array<char, 28>;

// Throws std::length_error before inspecting the view when key + the RFC
// GUID cannot be represented by the SHA-1 64-bit bit-length field or by the
// size_t arithmetic used by the incremental implementation.
void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key);

[[nodiscard]] bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

// Request-side Sec-WebSocket-* field checks. A recipient (server) skips a
// repeated offered subprotocol; a sender (client) must offer unique ones
// (RFC 6455 section 4.1).
class websocket_request_header_validator final {
public:
    explicit websocket_request_header_validator(http_field_list_role role) noexcept
        : role_(role) {}

    [[nodiscard]] bool observe_header(http_field_kind kind, std::string_view value) noexcept;
    [[nodiscard]] bool offers_valid() const noexcept;
    [[nodiscard]] bool client_offers_valid() const noexcept;
    [[nodiscard]] websocket_handshake_validation_result finish(
        websocket_handshake_validation_options options = {}) const noexcept;

private:
    websocket_subprotocol_set protocols_{};
    http_field_list_role role_;
    std::string_view version_{};
    std::size_t version_count_{0};
    bool valid_{true};
    bool protocols_present_{false};
    bool extensions_present_{false};
    bool has_extension_{false};
};

[[nodiscard]] websocket_handshake_validation_result validate_extended_connect_websocket_headers(
    const http_request& request, websocket_handshake_validation_options options = {}) noexcept;
}  // namespace detail
}  // namespace ruvia
