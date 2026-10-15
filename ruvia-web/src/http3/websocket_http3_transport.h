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
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http3_message_head.h"

#include "http3/http3_client_session_driver.h"
#include "http3/http3_quic_client_tls_context.h"

namespace ruvia::detail {
class websocket_client_state;

// RFC 9220 WebSocket framing adapter over one Extended CONNECT stream that it
// opens through the shared HTTP/3 client session driver, which owns DNS, QUIC,
// critical and peer streams. One TLS context serves the owning client. write()
// borrows the caller's bytes as the DATA payload until QUIC accepts them; all
// members are worker-affine.
class websocket_http3_transport final {
public:
    websocket_http3_transport(websocket_client_state& owner_value, const worker_handle& worker_value,
        std::pmr::memory_resource* resource);
    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<std::size_t> read(std::span<char> output);
    [[nodiscard]] task<void> write(std::string_view bytes);
    [[nodiscard]] task<void> finish();
    void stop() noexcept;
    [[nodiscard]] task<void> join();

private:
    using time_point_type = http3_client_session_driver::time_point_type;
    friend class http3_client_session_driver;

    // Session-driver hooks: the CONNECT stream keeps the session open until stop().
    [[nodiscard]] std::optional<time_point_type> next_deadline(std::optional<time_point_type> bound) const noexcept {
        return bound;
    }
    void expire(time_point_type) noexcept {}
    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] bool draining() const noexcept {
        return false;
    }
    void attempt_started(http3_client_sans_io_session_engine&) noexcept {}
    void attempt_closing() noexcept {}
    void attempt_abandoned() noexcept {}
    [[nodiscard]] bool early_stream_rejected(std::uint64_t) noexcept {
        return false;
    }
    [[nodiscard]] bool sweep(bool requests_may_start, bool early_data_only);
    [[nodiscard]] std::size_t peer_stream_read_budget(std::uint64_t) const noexcept {
        return http3_client_receive_driver::read_block_bytes;
    }
    [[nodiscard]] bool peer_stream_input(std::uint64_t, const http3_client_receive_driver::result_type&) const noexcept {
        return false;
    }

    [[nodiscard]] task<void> drive();
    [[nodiscard]] task<void> wait_for_output();
    [[nodiscard]] bool receive();
    [[nodiscard]] bool drive_output();
    [[nodiscard]] bool write_segment(std::span<const char> bytes);
    void record_failure(http3_client_session_driver::outcome_type outcome) noexcept;
    void abandon_output() noexcept;
    void check_failure() const;
    static void on_event(void* context, const http3_connection_event& event);

    websocket_client_state& owner_;
    std::pmr::memory_resource* resource_;
    http3_quic_client_tls_context tls_;
    http3_client_session_driver driver_;
    task_scope drivers_;
    worker_signal progress_;
    std::pmr::string received_;
    std::size_t read_offset_{0};
    // The Extended CONNECT HEADERS frame until QUIC accepts it.
    std::optional<http3_headers_frame> head_{};
    std::optional<http3_data_write_plan> data_plan_{};
    // One DATA frame: its header, then the borrowed write() payload.
    http3_data_write_plan::chunk_type chunk_{};
    std::size_t write_offset_{0};
    std::optional<http3_message_head> response_{};
    std::exception_ptr failure_{};
    std::optional<std::uint64_t> stream_id_{};
    bool open_requested_{false};
    bool chunk_pending_{false};
    bool finish_requested_{false};
    bool finished_{false};
    bool eof_{false};
    // Both stream halves ended and the QUIC stream retired.
    bool completed_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
