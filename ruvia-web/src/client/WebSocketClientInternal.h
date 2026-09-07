#pragma once

#include <cstddef>
#include <memory>

#include "ruvia/web/WebSocketClient.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {

constexpr std::size_t kWebSocketClientHandshakeNonceBytes = 16;
constexpr std::size_t kWebSocketClientHandshakeHeaderReserve = 6;
constexpr std::size_t kWebSocketClientHandshakeRequestBufferExtraBytes = 1024;
constexpr std::size_t kWebSocketClientTransportBufferBytes = std::size_t{16} * 1024;
constexpr std::size_t kWebSocketClientCloseHandshakeBufferBytes = std::size_t{4} * 1024;

[[nodiscard]] inline WebSocketClientError::Code webSocketClientTransportErrorCode(
    bool secure) noexcept {
    return secure ? WebSocketClientError::Code::kTlsFailed : WebSocketClientError::Code::kIoError;
}

struct WebSocketClientStopAbort final {
    std::weak_ptr<WebSocketClientState> state_;

    void operator()() noexcept {
        if (const auto owner = state_.lock()) {
            owner->requestCancel();
        }
    }
};

}  // namespace ruvia::detail
