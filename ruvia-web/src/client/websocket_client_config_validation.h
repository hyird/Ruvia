#pragma once
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_request_target.h"
#include "ruvia/http/websocket_client_negotiation.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_tls.h"
#include "client/client_tls_config_storage.h"
#include "client/client_user_agent.h"
#include "util/config_validation.h"
#include "util/tcp_socket_policies.h"
#include "websocket/websocket_lifecycle_validation.h"

namespace ruvia::detail {

inline void validate_websocket_client_config(const websocket_client_config& config) {
    if (config.scheme_ != websocket_scheme::ws && config.scheme_ != websocket_scheme::wss) {
        throw std::invalid_argument("WebSocket client scheme is invalid");
    }
    if (config.protocol_ != websocket_client_protocol::http1 && config.protocol_ != websocket_client_protocol::http2) {
        throw std::invalid_argument("WebSocket client HTTP protocol is invalid");
    }
    validate_client_origin_host(
        config.host_.view(), "WebSocket client host must not be empty", "WebSocket client host is invalid");
    if (config.port_.has_value() && config.port_.value() == 0) {
        throw std::invalid_argument("WebSocket client port must be greater than zero");
    }
    if (!is_valid_http_origin_form_target(config.target_.view())) {
        throw std::invalid_argument("WebSocket client target must use origin-form");
    }
    if (config.max_message_bytes_ == 0) {
        throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    }
    ensure_positive_optional_durations("WebSocket client timeouts must be greater than zero",
        std::optional{config.connect_timeout_}, config.read_timeout_, config.write_timeout_);
    validate_websocket_lifecycle_options(config.lifecycle_);
    validate_client_tls_config(config.tls_);
    if (config.scheme_ == websocket_scheme::wss && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("wss WebSocket client requires TLS; use the ws scheme for plaintext");
    }
    validate_client_tcp_config(config.tcp_);
    validate_client_user_agent(config.user_agent_.view());
}

// The handshake owns application-header policy; the negotiation owns the
// subprotocol list and the deflate offer.
inline void validate_websocket_client_protocols_and_headers(std::span<const http_header_view> headers,
    std::span<const std::string_view> subprotocols, const websocket_client_deflate_offer& deflate) {
    validate_http1_websocket_client_handshake_configuration(headers);
    websocket_client_negotiation::validate_configuration({.subprotocols_ = subprotocols, .deflate_ = deflate});
}

}  // namespace ruvia::detail
