#pragma once

#include <stdexcept>

#include "ruvia/web/websocket.h"

namespace ruvia::detail {

// The one lifecycle check of server routes and clients.
inline void validate_websocket_lifecycle_options(const websocket_lifecycle_options& options) {
    if (options.close_handshake_timeout_.has_value() && options.close_handshake_timeout_->count() <= 0) {
        throw std::invalid_argument("websocket close-handshake timeout must be greater than zero");
    }
    const auto& heartbeat = options.heartbeat_;
    if (!heartbeat.ping_interval_.has_value()) {
        if (heartbeat.pong_timeout_.has_value()) {
            throw std::invalid_argument("websocket pong timeout requires a ping interval");
        }
        return;
    }
    if (heartbeat.ping_interval_->count() <= 0 ||
        (heartbeat.pong_timeout_.has_value() && heartbeat.pong_timeout_->count() <= 0)) {
        throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
    }
}

// Validated options with an absent pong timeout defaulted to the ping interval.
[[nodiscard]] inline websocket_lifecycle_options normalize_websocket_lifecycle_options(
    websocket_lifecycle_options options) {
    validate_websocket_lifecycle_options(options);
    auto& heartbeat = options.heartbeat_;
    if (heartbeat.ping_interval_.has_value() && !heartbeat.pong_timeout_.has_value()) {
        heartbeat.pong_timeout_ = heartbeat.ping_interval_;
    }
    return options;
}

}  // namespace ruvia::detail
