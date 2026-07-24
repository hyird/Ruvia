#pragma once

#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

struct WebSocketAccess final {
    static void noopAbort(void*) noexcept {}

    [[nodiscard]] static WebSocket make(void* target, WebSocket::Read read, WebSocket::Write write, WebSocket::Close close) noexcept {
        return WebSocket(target, read, write, close, &noopAbort);
    }

    [[nodiscard]] static WebSocket make(void* target, WebSocket::Read read, WebSocket::Write write, WebSocket::Close close, WebSocket::Abort abort) noexcept {
        return WebSocket(target, read, write, close, abort);
    }
};

}  // namespace ruvia::detail
