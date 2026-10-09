#pragma once

#include <stdexcept>

#include "ruvia/web/websocket.h"

namespace ruvia::detail {

inline void validate_websocket_heartbeat_config(const websocket_heartbeat_config& config) {
    if (!config.ping_interval_.has_value()) {
        if (config.pong_timeout_.has_value()) {
            throw std::invalid_argument("websocket pong timeout requires a ping interval");
        }
        return;
    }
    if (config.ping_interval_->count() <= 0 ||
        (config.pong_timeout_.has_value() && config.pong_timeout_->count() <= 0)) {
        throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
    }
}

[[nodiscard]] inline websocket_heartbeat_config normalize_websocket_heartbeat_config(
    websocket_heartbeat_config config) {
    validate_websocket_heartbeat_config(config);
    if (config.ping_interval_.has_value() && !config.pong_timeout_.has_value()) {
        config.pong_timeout_ = config.ping_interval_;
    }
    return config;
}

}  // namespace ruvia::detail
