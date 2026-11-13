#pragma once

#include <exception>
#include <memory_resource>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/websocket.h"

#include "context/context_access.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/websocket_facade.h"

namespace ruvia::detail {

// The terminal handler borrows an established connection, but does not close it:
// onion middleware post-processing still belongs to the same websocket request
// and must be able to turn its own failure into the session's 1011 outcome.
template <typename stream_type>
task<void> invoke_websocket_handler(websocket_session_connection<stream_type>& connection,
    connection_scanner::entry& scanner_entry, const route_stream_handler& handler,
    context& context_value) {
    auto websocket_value = make_websocket_facade(connection, *process_resource());
    context_websocket_binding websocket_binding(context_value, websocket_value);

    scanner_entry.enter_long_lived();
    co_await handler(context_value);
}

// The session retains the connection until the complete route middleware
// chain finishes, then converge here. close() is idempotent through the protocol
// core's typed close phase, so a handler that already closed itself is safe.
//
// A handler that failed is already past the upgrade, so its exception can only
// become a 1011 close code -- which tells the peer that something went wrong
// but not what, and is the last thing that references the failure. It, a
// failure to close, and a failed heartbeat write are reported
// here, since nothing after this frame holds any of them. The sink classifies
// each: a peer that resets instead of answering the Close, or a close-handshake
// timeout that aborted the transport, ends the connection normally.
template <typename stream_type>
task<void> finish_websocket_session(websocket_session_connection<stream_type>& connection,
    std::exception_ptr exception, const connection_failure_sink& connection_failure,
    std::string_view remote_address) {
    connection_failure.invoke(remote_address, exception);
    try {
        if (exception != nullptr) {
            co_await connection.close({.code_ = websocket_close_status::internal_error, .reason_ = "internal server error"});
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
