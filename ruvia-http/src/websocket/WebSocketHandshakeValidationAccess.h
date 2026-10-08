#pragma once

#include "ruvia/http/WebSocketHandshake.h"

namespace ruvia::detail {

struct WebSocketHandshakeValidationResultAccess final {
    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult accepted() noexcept {
        return WebSocketHandshakeValidationResult::makeAccepted();
    }

    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult invalidRequest() noexcept {
        return WebSocketHandshakeValidationResult::makeInvalidRequest();
    }

    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult
    unsupportedVersion() noexcept {
        return WebSocketHandshakeValidationResult::makeUnsupportedVersion();
    }
};

}  // namespace ruvia::detail
