#pragma once

#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

struct WebSocketAccess final {
    [[nodiscard]] static constexpr WebSocket make(
        void* target,
        WebSocket::Read read,
        WebSocket::Write write,
        WebSocket::Close close) noexcept {
        return WebSocket(target, read, write, close);
    }
};

}  // namespace ruvia::detail
