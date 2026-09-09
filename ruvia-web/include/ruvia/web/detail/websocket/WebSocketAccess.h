#pragma once

#include <memory_resource>

#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

struct WebSocketAccess final {
    static void noopAbort(void*) noexcept {}

    [[nodiscard]] static WebSocket make(std::pmr::memory_resource& resource, void* target, WebSocket::Read read, WebSocket::Write write,
        WebSocket::Close close) noexcept {
        return WebSocket(resource, target, read, write, close, &noopAbort);
    }

    [[nodiscard]] static WebSocket make(std::pmr::memory_resource& resource, void* target, WebSocket::Read read, WebSocket::Write write,
        WebSocket::Close close, WebSocket::Abort abort) noexcept {
        return WebSocket(resource, target, read, write, close, abort);
    }
};

}  // namespace ruvia::detail
