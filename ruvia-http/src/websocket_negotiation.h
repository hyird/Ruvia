#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
#include "websocket_handshake.h"

// RFC 6455 handshake negotiation shared by the HTTP/1 Upgrade path and the
// RFC 8441 / RFC 9220 Extended CONNECT path.
namespace ruvia::detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-websocket-Accept response field (always 28 bytes).
using websocket_accept_key_type = std::array<char, 28>;

// Throws std::length_error before inspecting the view when key + the RFC
// GUID cannot be represented by the SHA-1 64-bit bit-length field or by the
// size_t arithmetic used by the incremental implementation.
void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key);

[[nodiscard]] bool append_websocket_extension_list(std::string_view value, bool& has_extension) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(const websocket_subprotocol_set& offered, std::span<const std::string_view> supported) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(const http_request& request, std::span<const std::string_view> supported) noexcept;

// Request-side Sec-WebSocket-* field checks. A recipient (server) skips a
// repeated offered subprotocol; a sender (client) must offer unique ones
// (RFC 6455 section 4.1).
class websocket_request_header_validator final {
public:
    explicit websocket_request_header_validator(http_field_list_role role) noexcept :
        role_(role) {}

    [[nodiscard]] bool observe_header(http_field_kind kind, std::string_view value) noexcept;
    [[nodiscard]] bool offers_valid() const noexcept;
    [[nodiscard]] bool client_offers_valid() const noexcept;
    [[nodiscard]] websocket_handshake_validation_result finish(websocket_handshake_validation_options options = {}) const noexcept;

private:
    websocket_subprotocol_set protocols_{};
    http_field_list_role role_;
    std::string_view version_{};
    bool version_present_{false};
    bool valid_{true};
    bool protocols_present_{false};
    bool extensions_present_{false};
    bool has_extension_{false};
};

[[nodiscard]] websocket_handshake_validation_result validate_extended_connect_websocket_headers(const http_request& request, websocket_handshake_validation_options options = {}) noexcept;

// HTTP-version-independent result of one server-side websocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent ws_connection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into owned storage so this
// committed value never borrows mutable route configuration.
class websocket_server_negotiation final {
public:
    websocket_server_negotiation(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation& operator=(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation(websocket_server_negotiation&&) noexcept = default;
    websocket_server_negotiation& operator=(websocket_server_negotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view extensions() const& noexcept {
        return extensions_.view();
    }
    std::string_view extensions() const&& = delete;

    [[nodiscard]] std::span<const http_header> response_headers() const& noexcept {
        return response_headers_;
    }
    std::span<const http_header> response_headers() const&& = delete;

    [[nodiscard]] std::string take_subprotocol() && noexcept {
        return std::move(subprotocol_);
    }

    [[nodiscard]] std::vector<http_header> take_response_headers() && noexcept {
        return std::move(response_headers_);
    }

private:
    friend websocket_server_negotiation make_websocket_server_negotiation(const http_request&, const websocket_server_handshake_options&);

    websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression, std::span<const http_header_view> response_headers_value);

    std::string subprotocol_;
    websocket_compression compression_;
    websocket_compression_extension extensions_;
    std::vector<http_header> response_headers_;
};

// Copies the selected subprotocol and response fields.
[[nodiscard]] websocket_server_negotiation make_websocket_server_negotiation(const http_request& request, const websocket_server_handshake_options& options);

}  // namespace ruvia::detail
