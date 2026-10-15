#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/task_scope.h"
#include "ruvia/http/http2_connection.h"

#include "client/client_tcp_transport.h"
#include "http2/http2_client_session.h"

namespace ruvia::detail {
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
    // Fails the session for the owner's abort reason; join() then retires it.
    void stop() noexcept;
    [[nodiscard]] task<void> join();

    // http2_client_session handler.
    void on_http2_event(http2_event& event);
    void on_http2_data_drained(std::uint32_t stream_id) noexcept;
    void on_http2_failure(const client_transport_result& transport, std::exception_ptr failure) noexcept;

private:
    [[nodiscard]] task<void> flush();
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
    bool clean_reset_{false};
    // The peer ended the connection before pending output was written.
    bool output_lost_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
