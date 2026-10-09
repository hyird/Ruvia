#pragma once

#include <cstddef>
#include <utility>

#include "ruvia/core/connection_scanner.h"

#include "context/context_services.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"

// What an HTTP/2 session needs from the server that accepted the connection.
// context_services is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.

namespace ruvia::detail {

class http2_sans_io_session_context final {
public:
    http2_sans_io_session_context(context_services services, const http_server_options& options,
        ruvia::connection_scanner::entry_type& scanner_entry, const http_server_worker_state& worker_state)
        : services_(std::move(services)),
          options_(options),
          scanner_entry_(scanner_entry),
          worker_state_(worker_state) {}

    [[nodiscard]] const http_server_options& options() const noexcept {
        return options_;
    }

    [[nodiscard]] ruvia::connection_scanner::entry_type& scanner_entry() const noexcept {
        return scanner_entry_;
    }

    [[nodiscard]] bool worker_running() const noexcept {
        return http_server_worker_running(worker_state_);
    }

    [[nodiscard]] const context_services& services() const noexcept {
        return services_;
    }

private:
    context_services services_;
    const http_server_options& options_;
    ruvia::connection_scanner::entry_type& scanner_entry_;
    const http_server_worker_state& worker_state_;
};

[[nodiscard]] inline ruvia::connection_scanner::phase_type http2_sans_io_inactivity_phase(
    bool header_block_in_progress, std::size_t active_runtime_count,
    bool websocket_tunnel_active) noexcept {
    if (header_block_in_progress) {
        return ruvia::connection_scanner::phase_type::reading_initial;
    }
    if (websocket_tunnel_active) {
        // A successful RFC 8441 tunnel is long-lived, not a stalled request body.
        // Keep the header timeout above so another stream's incomplete header block
        // cannot hold the connection open indefinitely.
        return ruvia::connection_scanner::phase_type::long_lived;
    }
    return active_runtime_count == 0 ? ruvia::connection_scanner::phase_type::idle
                                     : ruvia::connection_scanner::phase_type::reading_payload;
}

}  // namespace ruvia::detail
