#pragma once

#include <utility>

#include "context/context_services.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"

// What an HTTP/2 session needs from the server that accepted the connection.
// context_services is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.

namespace ruvia::detail {

class http2_sans_io_session_context final {
public:
    http2_sans_io_session_context(context_services services, const http_server_options& options,
        connection_scanner::entry& scanner_entry)
        : services_(std::move(services)),
          options_(options),
          scanner_entry_(scanner_entry) {}

    [[nodiscard]] const http_server_options& options() const noexcept {
        return options_;
    }

    [[nodiscard]] connection_scanner::entry& scanner_entry() const noexcept {
        return scanner_entry_;
    }

    [[nodiscard]] const context_services& services() const noexcept {
        return services_;
    }

private:
    context_services services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
};

// The connection-wide scanner phase. Request bodies and send-window waits are
// timed per stream by the session's periodic check, not by this phase.
[[nodiscard]] inline connection_scanner::phase http2_sans_io_inactivity_phase(bool peer_settings_received,
    bool header_block_in_progress, bool stream_active, bool websocket_tunnel_active) noexcept {
    // Readiness (the client preface and its SETTINGS) and an incomplete header
    // block are bounded like an HTTP/1 request head.
    if (!peer_settings_received || header_block_in_progress) {
        return connection_scanner::phase::reading_initial;
    }
    if (websocket_tunnel_active) {
        // A successful RFC 8441 tunnel is long-lived, not a stalled request body.
        // Keep the header timeout above so another stream's incomplete header block
        // cannot hold the connection open indefinitely.
        return connection_scanner::phase::long_lived;
    }
    // As in Pingora's h2 idle timeout, idleness starts when the last stream
    // ends; an active stream keeps the inactivity deadman of an HTTP/1 handler.
    return stream_active ? connection_scanner::phase::idle : connection_scanner::phase::awaiting_stream;
}

}  // namespace ruvia::detail
