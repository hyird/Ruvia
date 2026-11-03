#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {
class http2_connection;
class http_client_response_head;

struct websocket_client_negotiation_config_view final {
    borrowed_span<std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};
// The one failure shape of a client opening handshake over HTTP/1.1 Upgrade
// (RFC 6455 section 4.1) or RFC 8441 Extended CONNECT.
enum class websocket_client_handshake_error : std::uint8_t {
    // Not 101 over HTTP/1.1, not 2xx over HTTP/2.
    response_status,
    // HTTP/2: the 2xx HEADERS ended the stream.
    stream_closed,
    // HTTP/1.1: Sec-WebSocket-Accept is missing, repeated or wrong.
    accept,
    // HTTP/1.1: no Upgrade: websocket.
    upgrade,
    // HTTP/1.1: no Connection: Upgrade.
    connection,
    subprotocol,
    extensions,
    // HTTP/2: a connection-specific response field (RFC 9113 section 8.2.2).
    forbidden_field,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

class websocket_client_negotiation;
namespace detail {
// The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
// against the offer; `extended` (RFC 8441) also rejects connection-specific
// fields. The result view borrows `fields`.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended);
}  // namespace detail

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied into resource once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441 Extended CONNECT, and validates the server's exact selection; the
// version-specific response validators check the same selection after their
// own status checks.
// The resource must outlive this object.
// All offer views expire with this owner; response views expire with the head.
class websocket_client_negotiation final {
public:
    // Validates the subprotocols and the deflate offer, including its deflater.
    static void validate_configuration(websocket_client_negotiation_config_view config);
    websocket_client_negotiation(websocket_client_negotiation_config_view config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    websocket_client_negotiation(websocket_client_negotiation&&) noexcept = default;
    websocket_client_negotiation& operator=(websocket_client_negotiation&&) = delete;
    websocket_client_negotiation(const websocket_client_negotiation&) = delete;
    websocket_client_negotiation& operator=(const websocket_client_negotiation&) = delete;
    [[nodiscard]] std::span<const http_header> request_fields() const& noexcept {
        return std::span<const http_header>(fields_.data(), field_count_);
    }
    std::span<const http_header> request_fields() const&& = delete;
    // The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
    // in `fields` alone, without status or connection-specific field checks.
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(std::span<const http_header> fields) const;
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(fields_type&&) const = delete;
    // submit_http2_websocket_client_request() with this negotiation.
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_http2_request(http2_connection& connection,
        std::string_view scheme, std::string_view authority, std::string_view target,
        std::span<const http_header_view> headers = {}) const;
    // validate_http2_websocket_client_response() with this negotiation.
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_response(const http_client_response_head& head, bool stream_open) const;
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_response(const http_client_response_head&&, bool) const = delete;
    // The resource the configuration was copied into.
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    friend std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    detail::validate_websocket_client_selection(
        const websocket_client_negotiation&, std::span<const http_header>, bool);

    std::pmr::memory_resource* resource_;
    websocket_client_deflate_offer deflate_;
    std::array<http_header, 3> fields_{};
    std::size_t field_count_{0};
    bool has_subprotocols_{false};
};

class http1_parsed_client_response_head;

// RFC 6455 section 4.1 opening handshake over HTTP/1.1 Upgrade. The caller
// owns the base64-encoded 16-byte nonce (`encoded_key`), the negotiation and the
// application headers for the whole exchange; subprotocols and the deflate offer
// are validated by websocket_client_negotiation.

// Rejects application headers the handshake or request writer generates itself
// (Host, framing, Expect, Upgrade/Connection and every Sec-WebSocket-* field)
// and any header the request writer would refuse.
void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers);

[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer, std::pmr::memory_resource* resource);

// Validates the 101 response against the key, then the server's selection
// against the negotiation. The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24>,
    const websocket_client_negotiation&, const http1_parsed_client_response_head&&) = delete;
}  // namespace ruvia
