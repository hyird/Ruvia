#pragma once

#include <exception>

#include "HttpWebSocketConnection.h"
#include "../server/ConnectionScanner.h"
#include "../../router/RouteTable.h"
#include "../../http/WebSocketInternal.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/WebSocket.h"

namespace ruvia::detail {

template <typename Connection>
[[nodiscard]] Task<std::optional<WebSocketMessage>> webSocketReadThunk(void* target) {
    return static_cast<Connection*>(target)->read();
}

template <typename Connection>
Task<void> webSocketWriteThunk(void* target, WebSocketOpcode opcode, std::string_view payload) {
    return static_cast<Connection*>(target)->write(opcode, payload);
}

template <typename Connection>
Task<void> webSocketCloseThunk(void* target, std::uint16_t code, std::string_view reason) {
    return static_cast<Connection*>(target)->close(code, reason);
}

template <typename Connection>
[[nodiscard]] WebSocket makeWebSocketFacade(Connection& connection) noexcept {
    return WebSocketAccess::make(
        &connection,
        &webSocketReadThunk<Connection>,
        &webSocketWriteThunk<Connection>,
        &webSocketCloseThunk<Connection>);
}

// Shared run loop for an established WebSocket session, transport-agnostic.
// Both the HTTP/1.1 and HTTP/2 routes build a WebSocketConnection<Transport>,
// then hand it here: this wires the WebSocket facade, dispatches the user
// handler, and closes cleanly (1000) on success or abnormally (1011) on an
// unhandled exception, then drains any background heartbeat writes. Keeping the
// post-handshake chain in one place keeps the two transports identical and the
// graceful close (RFC 6455 Section 7.1.1) consistent across both. close() is
// idempotent (guarded by closeSent_), so a handler that closes itself is fine.
template <typename Transport>
Task<void> runWebSocketSession(
    WebSocketConnection<Transport>& connection,
    ConnectionScanner::Entry& scannerEntry,
    const RouteTable& routes,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    ContextServices services) {
    auto webSocket = makeWebSocketFacade(connection);

    scannerEntry.setPhase(ConnectionScanner::Phase::kWebSocket);
    std::exception_ptr exception;
    try {
        (void)co_await routes.dispatchWebSocket(request, resolution, requestMemory, webSocket, services);
    } catch (...) {
        exception = std::current_exception();
    }
    try {
        if (exception != nullptr) {
            co_await connection.close(1011, "internal server error");
        } else {
            co_await connection.close(1000, {});
        }
    } catch (...) {
    }
    co_await connection.detachAndDrainBackgroundWrites();
}

}  // namespace ruvia::detail
