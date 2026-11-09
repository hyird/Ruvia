#pragma once

// Per-stream async plumbing for the sans-I/O HTTP/2 session.
//
// Inbound bytes consumed asynchronously (a websocket tunnel or streaming request
// body) live in a Web-owned queue. The HTTP core retains receive-window debt for
// each delivered DATA event; these consumers acknowledge it only after the queue
// drains, so a suspended handler naturally backpressures the peer.
//
// The same-executor discipline of the session (reader, writer and handlers all run
// on the connection's executor) means these never race: a signal wake
// while nothing is waiting is a no-op, and every consumer re-checks its condition
// before suspending, so no wakeup is lost.

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/websocket_protocol.h"

#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_session_engine.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"

namespace ruvia::detail {

// Duplex CONNECT transport driven by one HTTP/2 session reader and writer.
// Queued input retains its receive-window credit until the application consumes it.
class http2_sans_io_tunnel_transport final
    : public multiplexed_tunnel_transport_base<http2_sans_io_tunnel_transport> {
public:
    using multiplexed_tunnel_transport_base<http2_sans_io_tunnel_transport>::write_bytes;

    http2_sans_io_tunnel_transport(http2_sans_io_session_engine& engine, ruvia::http2_connection& connection,
        std::uint32_t stream_id, http2_sans_io_body_queue& body_queue, multiplexed_stream_signal& signal,
        worker_signal& write_signal, http2_data_output_budget& output_budget) noexcept
        : multiplexed_tunnel_transport_base(signal),
          engine_(&engine),
          connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          write_signal_(write_signal),
          output_budget_(output_budget) {}

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end disposition) {
        const auto terminal = disposition == http_stream_end::end
                                  ? http2_end_stream::end_stream
                                  : http2_end_stream::keep_open;
        const auto status = co_await write_http2_stream_data(connection_, stream_id_, &signal_,
            write_signal_, output_budget_, bytes_value, terminal, &aborted_);
        switch (status) {
            case http2_stream_data_write_status::completed:
                co_return std::error_code{};
            case http2_stream_data_write_status::local_aborted:
                co_return std::make_error_code(std::errc::operation_canceled);
            case http2_stream_data_write_status::session_terminated:
                co_return signal_.terminal_error();
            case http2_stream_data_write_status::peer_closed:
                co_return std::make_error_code(std::errc::connection_reset);
            case http2_stream_data_write_status::invalid_state:
            case http2_stream_data_write_status::content_length_exceeded:
                co_return std::make_error_code(std::errc::protocol_error);
        }
        co_return std::make_error_code(std::errc::protocol_error);
    }

    // Malformed received data is a stream error of type PROTOCOL_ERROR
    // (RFC 9113 section 8.1.1); any other abort cancels the stream.
    void abort(bool malformed = false) noexcept {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        engine_->reset_stream_no_throw(
            stream_id_, malformed ? http2_error_code::protocol_error : http2_error_code::cancel);
        signal_.wake();
    }

private:
    friend class multiplexed_tunnel_transport_base<http2_sans_io_tunnel_transport>;

    void require_live() const {
        if (aborted_ || signal_.terminated()) {
            throw inbound_transport_error(
                aborted_ ? std::make_error_code(std::errc::operation_canceled)
                         : signal_.terminal_error(),
                "HTTP/2 tunnel read");
        }
        if (connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), "HTTP/2 tunnel read");
        }
    }

    template <typename buffer_type>
    [[nodiscard]] bool consume_tunnel_chunk(buffer_type& buffer) {
        const auto chunk = body_queue_.pop();
        // pop() returns the previous chunk's receive-window credit; flush
        // the WINDOW_UPDATE even if this reader suspends below.
        if (connection_.wants_write()) {
            wake_writer();
        }
        if (chunk.empty()) {
            return false;
        }
        buffer.append(std::string_view(chunk.data(), chunk.size()));
        body_queue_.release_active_credits();
        if (connection_.wants_write()) {
            wake_writer();
        }
        return true;
    }

    [[nodiscard]] bool receive_ended() const noexcept {
        return connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::ended;
    }

    void wake_writer() noexcept {
        write_signal_.notify();
    }

    http2_sans_io_session_engine* engine_;
    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    bool aborted_{false};
};

}  // namespace ruvia::detail
