#pragma once

#include <memory_resource>

#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

struct WebSocketAccess final {
    static void noopAbort(void*) noexcept {}

    // Unbound facade construction is retained for isolated capability tests;
    // runtime-created facades always borrow their connection's worker handle.
    [[nodiscard]] static WebSocket make(std::pmr::memory_resource& resource, void* target,
        WebSocket::Read read, WebSocket::Write write, WebSocket::Close close) noexcept {
        return WebSocket(resource, nullptr, target, read, write, close, &noopAbort);
    }

    [[nodiscard]] static WebSocket make(std::pmr::memory_resource& resource, const WorkerHandle& worker,
        void* target, WebSocket::Read read, WebSocket::Write write, WebSocket::Close close) noexcept {
        return WebSocket(resource, &worker, target, read, write, close, &noopAbort);
    }

    [[nodiscard]] static WebSocket make(std::pmr::memory_resource& resource, const WorkerHandle& worker,
        void* target, WebSocket::Read read, WebSocket::Write write, WebSocket::Close close,
        WebSocket::Abort abort) noexcept {
        return WebSocket(resource, &worker, target, read, write, close, abort);
    }
};

}  // namespace ruvia::detail
