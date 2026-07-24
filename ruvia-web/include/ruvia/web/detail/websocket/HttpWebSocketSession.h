#pragma once

#include <exception>

#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"
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
    return WebSocketAccess::make(&connection, &webSocketReadThunk<Connection>, &webSocketWriteThunk<Connection>, &webSocketCloseThunk<Connection>, &webSocketAbortThunk<Connection>);
}

// The terminal handler borrows an established connection, but does not close it:
// onion middleware post-processing still belongs to the same WebSocket request
// and must be able to turn its own failure into the session's 1011 outcome.
template <typename Transport>
Task<void> invokeWebSocketHandler(WebSocketConnection<Transport>& connection, ConnectionScanner::Entry& scannerEntry, const CallableRef<void, Context&>& handler, Context& context) {
    auto webSocket = makeWebSocketFacade(connection);
    ContextWebSocketBinding webSocketBinding(context, webSocket);

    scannerEntry.setPhase(ConnectionScanner::Phase::kLongLived);
    co_await handler(context);
}

// HTTP/1 and HTTP/2 retain the connection until the complete route middleware
// chain finishes, then converge here. close() is idempotent through the protocol
// core's typed close phase, so a handler that already closed itself is safe.
//
// A handler that failed is already past the upgrade, so its exception can only
// become a 1011 close code -- which tells the peer that something went wrong
// but not what, and is the last thing that references the failure. Both it and
// a failure to close are reported here, since nothing after this frame holds
// either one. close() itself signals a dead peer through error codes, so what
// it throws is a real fault (an invalid code, or exhaustion), not a routine
// disconnect.
template <typename Transport>
Task<void> finishWebSocketSession(WebSocketConnection<Transport>& connection, std::exception_ptr exception, const ConnectionFailureSink& connectionFailure, std::string_view remoteAddress) {
    connectionFailure.invoke(remoteAddress, exception);
    try {
        if (exception != nullptr) {
            co_await connection.close(1011, "internal server error");
        } else {
            co_await connection.close(1000, {});
        }
    } catch (...) {
        connectionFailure.invoke(remoteAddress, std::current_exception());
    }
    co_await connection.detachAndDrainWrites();
}

}  // namespace ruvia::detail
