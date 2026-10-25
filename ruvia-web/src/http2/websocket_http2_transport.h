#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_connection.h"

#include "client/client_tcp_transport.h"
#include "http2/http2_client_session.h"

namespace ruvia::detail {
// Product limit: the longest retirement waits to write the final GOAWAY before
// it closes the transport anyway (an unread peer, or write_timeout_ nullopt).
inline constexpr std::chrono::milliseconds websocket_http2_goaway_flush_time{1000};

class websocket_client_state;

// RFC 8441 WebSocket over one HTTP/2 Extended CONNECT stream. The shared
// http2_client_session drives the owner's client_tcp_transport; this adapter
// only maps the tunnel stream onto WebSocket bytes. Received DATA retains its
// linear credit until the application consumes the buffer.
class websocket_http2_transport final {
public:
    websocket_http2_transport(websocket_client_state& owner_value, const worker_handle& worker_value,
        std::pmr::memory_resource* resource);
    websocket_http2_transport(websocket_client_state&, worker_handle&&, std::pmr::memory_resource*) = delete;

    websocket_http2_transport(const websocket_http2_transport&) = delete;
    websocket_http2_transport& operator=(const websocket_http2_transport&) = delete;

    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<std::size_t> read(std::span<char> output);
    [[nodiscard]] task<void> write(std::string_view bytes);
    [[nodiscard]] task<void> finish();
    void end_peer_read() noexcept {
        eof_ = true;
        session_.state_signal().notify();
    }
    // Retires the connection for the owner's abort reason. A running session
    // first queues GOAWAY(NO_ERROR) (RFC 9113 section 6.8) and returns true:
    // the transport then stays open until join() has flushed it, for at most
    // websocket_http2_goaway_flush_time, and join() closes it. Otherwise the
    // session fails at once and the owner closes the transport. Idempotent.
    [[nodiscard]] bool stop() noexcept;
    [[nodiscard]] task<void> join();

    // http2_client_session handler.
    void on_http2_event(http2_event& event);
    void on_http2_data_drained(std::uint32_t stream_id) noexcept;
    void on_http2_failure(const client_transport_result& transport, std::exception_ptr failure) noexcept;

private:
    [[nodiscard]] task<void> flush();
    [[nodiscard]] task<void> flush_goaway();
    void check_failure() const;
    void require_live() const;

    websocket_client_state& owner_;
    task_scope drivers_;
    http2_client_session<websocket_http2_transport> session_;
    std::pmr::string received_;
    std::size_t read_offset_{0};
    std::optional<http2_received_data_credit> received_credit_{};
    std::optional<http_client_response_head> response_{};
    std::exception_ptr failure_{};
    std::uint32_t stream_id_{0};
    bool eof_{false};
    // The final response HEADERS carried END_STREAM (no WebSocket stream).
    bool response_stream_ended_{false};
    bool clean_reset_{false};
    // The peer ended the connection before pending output was written.
    bool output_lost_{false};
    bool stopped_{false};
    // stop() queued a GOAWAY that join() still has to flush.
    bool goaway_pending_{false};
    bool goaway_expired_{false};
    // Last: destroyed first, before the session its completion notifies.
    worker_timer_registration goaway_timer_;
};
}  // namespace ruvia::detail
