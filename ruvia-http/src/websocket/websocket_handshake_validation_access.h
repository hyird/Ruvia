#pragma once

#include "ruvia/http/websocket_handshake.h"

namespace ruvia::detail {

struct websocket_handshake_validation_result_access final {
    [[nodiscard]] static constexpr websocket_handshake_validation_result accepted() noexcept {
        return websocket_handshake_validation_result::make_accepted();
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result invalid_request() noexcept {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    unsupported_version() noexcept {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
};

}  // namespace ruvia::detail
