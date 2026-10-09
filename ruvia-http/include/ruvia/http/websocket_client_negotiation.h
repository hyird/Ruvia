#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_message_head.h"
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
    std::span<const http_header_view> headers_{};
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
// RFC 8441/9220 Extended CONNECT, and validates the server's exact selection.
// The resource must outlive this object and prepared field sections. All header
// views produced below expire with this owner; response views expire with the head.
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
        return fields_;
    }
    std::span<const http_header> request_fields() const&& = delete;
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_fields(std::span<const http_header> fields) const;
    [[nodiscard]] http2_request_head_submit_result submit_http2_request(http2_connection& connection,
        std::string_view scheme, std::string_view authority, std::string_view target) const;
    [[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_http3_request(
        std::string_view scheme, std::string_view authority, std::string_view target,
        bool peer_enable_connect_protocol, http3_field_section_limits limits = {}) const;
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_response(const http_client_response_head& head, bool stream_open) const;
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_response(const http3_message_head& head, bool stream_open) const;

private:
    std::pmr::memory_resource* resource_;
    websocket_client_deflate_offer deflate_;
    std::pmr::vector<std::pmr::string> protocols_;
    std::pmr::vector<http_header> fields_;
};
}  // namespace ruvia
