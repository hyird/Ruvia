#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http_header.h"
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
    subprotocol,
    extensions,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

// RFC 6455/7692 client negotiation owner. Configuration is copied into resource
// once. It supplies the Sec-WebSocket-Version/Protocol/Extensions request
// fields to HTTP/1 Upgrade and validates the server's exact selection; the
// handshake owner decides which application headers accompany them. The
// resource must outlive this object.
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
        return fields_;
    }
    std::span<const http_header> request_fields() const&& = delete;
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_negotiation_error>
    validate_fields(std::span<const http_header> fields) const;

private:
    std::pmr::memory_resource* resource_;
    websocket_client_deflate_offer deflate_;
    std::pmr::vector<std::pmr::string> protocols_;
    std::pmr::vector<http_header> fields_;
};
}  // namespace ruvia
