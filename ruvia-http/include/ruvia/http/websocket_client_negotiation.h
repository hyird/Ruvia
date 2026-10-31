#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {
struct websocket_client_deflate_offer final {
    bool enabled_{false};
    bool server_no_context_takeover_{true};
    bool client_no_context_takeover_{true};
    std::optional<int> server_max_window_bits_{};
    bool offer_client_max_window_bits_{true};
    std::optional<int> client_max_window_bits_{};
};
struct websocket_client_negotiation_config_view final {
    std::span<const std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};
enum class websocket_client_negotiation_error : std::uint8_t {
    response_status,
    stream_closed,
    subprotocol,
    extensions,
    forbidden_field,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied into resource once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441 Extended CONNECT, and validates the server's exact selection.
// The resource must outlive this object.
// All offer views expire with this owner; response views expire with the head.
class websocket_client_negotiation final {
public:
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
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_fields(std::span<const http_header> fields) const;
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_http2_request(http2_connection& connection,
        std::string_view scheme, std::string_view authority, std::string_view target,
        std::span<const http_header_view> headers = {}) const;
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_response(const http_client_response_head& head, bool stream_open) const;

private:
    std::pmr::memory_resource* resource_;
    websocket_client_deflate_offer deflate_;
    std::array<http_header, 3> fields_{};
    std::size_t field_count_{0};
    bool has_subprotocols_{false};
};

class http1_parsed_client_response_head;

enum class http1_websocket_client_handshake_error : std::uint8_t {
    response_status,
    accept,
    upgrade,
    connection,
    subprotocol,
    extensions,
};

// RFC 6455 section 4.1 opening handshake over HTTP/1.1 Upgrade. The caller
// owns the base64-encoded 16-byte nonce (`encoded_key`), the negotiation and the
// application headers for the whole exchange; subprotocols and the deflate offer
// are validated by websocket_client_negotiation.

// Rejects application headers the handshake or request writer generates itself
// (Host, framing, Expect, Upgrade/Connection and every Sec-WebSocket-* field)
// and any header or User-Agent the request writer would refuse.
void validate_http1_websocket_client_handshake_configuration(
    std::span<const http_header_view> headers, std::string_view user_agent = {});

[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::string_view user_agent, std::span<char> head_buffer,
    std::pmr::memory_resource* resource);

// Validates the 101 response against the key, then the server's selection
// against the negotiation. The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, http1_websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response);
}  // namespace ruvia
