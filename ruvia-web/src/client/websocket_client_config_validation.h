#pragma once
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ruvia/http/http1_websocket_client_handshake.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_transport.h"
#include "http3/http3_qpack_config_validation.h"
#include "websocket/websocket_heartbeat_config_validation.h"

namespace ruvia::detail {

inline void validate_websocket_client_config(const websocket_client_config& config) {
    if (config.scheme_ != websocket_scheme::ws && config.scheme_ != websocket_scheme::wss) {
        throw std::invalid_argument("WebSocket client scheme is invalid");
    }
    if (config.protocol_ != websocket_client_protocol::http1 && config.protocol_ != websocket_client_protocol::http2 && config.protocol_ != websocket_client_protocol::http3) {
        throw std::invalid_argument("WebSocket client HTTP protocol is invalid");
    }
    if (config.protocol_ == websocket_client_protocol::http3 && config.scheme_ != websocket_scheme::wss) {
        throw std::invalid_argument("HTTP/3 WebSocket requires wss");
    }
    validate_http3_qpack_config(config.qpack_);
    validate_client_origin_host(
        config.host_, "WebSocket client host must not be empty", "WebSocket client host is invalid");
    if (config.port_.has_value() && config.port_.value() == 0) {
        throw std::invalid_argument("WebSocket client port must be greater than zero");
    }
    if (!is_valid_http_origin_form_target(config.target_)) {
        throw std::invalid_argument("WebSocket client target must use origin-form");
    }
    if (config.max_message_bytes_ == 0) {
        throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    }
    if (config.connect_timeout_.count() <= 0) {
        throw std::invalid_argument("WebSocket client connect timeout must be greater than zero");
    }
    for (const std::optional<std::chrono::milliseconds> timeout :
        {config.read_timeout_, config.write_timeout_, config.close_handshake_timeout_}) {
        if (timeout.has_value() && timeout->count() <= 0) {
            throw std::invalid_argument("WebSocket client timeout must be greater than zero");
        }
    }
    validate_websocket_heartbeat_config(config.heartbeat_);
    validate_client_transport_config(make_client_transport_config_view(config));

    if (config.compression_level_ < 0 || config.compression_level_ > 9) {
        throw std::invalid_argument("WebSocket client compression level must be between 0 and 9");
    }
}

inline void validate_websocket_client_protocols_and_headers(
    std::span<const http_header_view> headers, std::span<const std::string_view> subprotocols,
    std::string_view user_agent, const websocket_client_deflate_offer& deflate) {
    http1_websocket_client_handshake::validate_configuration(headers, subprotocols, user_agent);
    websocket_client_negotiation::validate_configuration(
        {.headers_ = headers, .subprotocols_ = subprotocols, .deflate_ = deflate});
}

}  // namespace ruvia::detail
