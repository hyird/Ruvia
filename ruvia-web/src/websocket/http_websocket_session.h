#pragma once

#include <exception>
#include <memory_resource>

#include "ruvia/core/task.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/websocket.h"

#include "context/context_access.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "websocket/http_websocket_connection.h"

namespace ruvia::detail {

template <typename connection_type>
[[nodiscard]] task<std::optional<websocket_message>> websocket_read_thunk(void* target) {
    return static_cast<connection_type*>(target)->read();
}

template <typename connection_type>
task<void> websocket_write_thunk(void* target, websocket_opcode opcode, std::string_view payload_value, bool compress) {
    return static_cast<connection_type*>(target)->write(opcode, payload_value, compress);
}

template <typename connection_type>
task<void> websocket_close_thunk(void* target, ::ruvia::websocket_close_options options) {
    return static_cast<connection_type*>(target)->close(options);
}

template <typename connection_type>
void websocket_abort_thunk(void* target) noexcept {
    static_cast<connection_type*>(target)->abort();
}

template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(
    connection_type& connection, std::pmr::memory_resource& resource) noexcept {
    return websocket(resource, connection.worker(), &connection, &websocket_read_thunk<connection_type>,
        &websocket_write_thunk<connection_type>, &websocket_close_thunk<connection_type>,
        &websocket_abort_thunk<connection_type>);
}

// The terminal handler borrows an established connection, but does not close it:
// onion middleware post-processing still belongs to the same websocket request
// and must be able to turn its own failure into the session's 1011 outcome.
template <typename stream_type>
task<void> invoke_websocket_handler(server_websocket_connection<stream_type>& connection,
    connection_scanner::entry_type& scanner_entry, const route_stream_handler_type& handler,
    context& context_value) {
    auto websocket_value = make_websocket_facade(connection, *context_value.pool());
    context_websocket_binding websocket_binding(context_value, websocket_value);

    // A draining worker does not admit the session: it has already started
    // the 1001 Going Away close, which finish_websocket_session completes.
    if (!scanner_entry.enter_long_lived()) {
        co_return;
    }
    co_await handler(context_value);
}

// The session retains the connection until the complete route middleware
// chain finishes, then converge here. close() is idempotent through the protocol
// core's typed close phase, so a handler that already closed itself is safe.
//
// A handler that failed is already past the upgrade, so its exception can only
// become a 1011 close code -- which tells the peer that something went wrong
// but not what, and is the last thing that references the failure. It, a
// failure to close, and a failed heartbeat or Going Away write are reported
// here, since nothing after this frame holds any of them. The sink classifies
// each: a peer that resets instead of answering the Close, or a close-handshake
// timeout that aborted the transport, ends the connection normally.
template <typename stream_type>
task<void> finish_websocket_session(server_websocket_connection<stream_type>& connection,
    std::exception_ptr exception, const connection_failure_sink& connection_failure,
    std::string_view remote_address) {
    connection_failure.invoke(remote_address, exception);
    try {
        if (exception != nullptr) {
            co_await connection.close({.code_ = 1011, .reason_ = "internal server error"});
        } else {
            co_await connection.close();
        }
    } catch (...) {
        connection_failure.invoke(remote_address, std::current_exception());
    }
    co_await connection.detach_and_drain_writes();
    connection_failure.invoke(remote_address, connection.take_background_failure());
}

}  // namespace ruvia::detail
