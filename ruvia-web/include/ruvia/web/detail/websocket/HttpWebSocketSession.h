#pragma once

#include <exception>

#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/CallableRef.h"
#include "ruvia/web/detail/websocket/WebSocketInternal.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/WebSocket.h"

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
void webSocketAbortThunk(void* target) noexcept {
    static_cast<Connection*>(target)->abort();
}

template <typename Connection>
[[nodiscard]] WebSocket makeWebSocketFacade(Connection& connection) noexcept {
    return WebSocketAccess::make(
        &connection,
        &webSocketReadThunk<Connection>,
        &webSocketWriteThunk<Connection>,
        &webSocketCloseThunk<Connection>,
        &webSocketAbortThunk<Connection>);
}

// The terminal handler borrows an established connection, but does not close it:
// onion middleware post-processing still belongs to the same WebSocket request
// and must be able to turn its own failure into the session's 1011 outcome.
template <typename Transport>
Task<void> invokeWebSocketHandler(
    WebSocketConnection<Transport>& connection,
    ConnectionScanner::Entry& scannerEntry,
    const CallableRef<void, Context&>& handler,
    Context& context) {
    auto webSocket = makeWebSocketFacade(connection);
    ContextWebSocketBinding webSocketBinding(context, webSocket);

    scannerEntry.setPhase(ConnectionScanner::Phase::kLongLived);
    co_await handler(context);
}

// HTTP/1 and HTTP/2 retain the connection until the complete route middleware
// chain finishes, then converge here. close() is idempotent through the protocol
// core's typed close phase, so a handler that already closed itself is safe.
template <typename Transport>
Task<void> finishWebSocketSession(
    WebSocketConnection<Transport>& connection,
    std::exception_ptr exception) {
    try {
        if (exception != nullptr) {
            co_await connection.close(1011, "internal server error");
        } else {
            co_await connection.close(1000, {});
        }
    } catch (...) {
    }
    co_await connection.detachAndDrainWrites();
}

}  // namespace ruvia::detail
