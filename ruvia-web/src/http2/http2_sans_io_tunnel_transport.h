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
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"

namespace ruvia::detail {

// Duplex CONNECT transport driven by one HTTP/2 session reader and writer.
// Queued input retains its receive-window credit until the application consumes it.
template <typename executor_type>
class http2_sans_io_tunnel_transport final {
public:
    http2_sans_io_tunnel_transport(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, http2_sans_io_stream_signal& signal, worker_signal& write_signal,
        http2_data_output_budget& output_budget, executor_type executor) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          signal_(signal),
          write_signal_(write_signal),
          output_budget_(output_budget),
          executor_(executor) {}
    [[nodiscard]] executor_type executor() const noexcept {
        return executor_;
    }

    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        for (;;) {
            if (aborted_ || signal_.terminated()) {
                throw inbound_transport_error(
                    aborted_ ? std::make_error_code(std::errc::operation_canceled)
                             : signal_.terminal_error(),
                    "HTTP/2 tunnel read");
            }
            const auto receive_status = connection_.stream_receive_status(stream_id_);
            if (receive_status == http2_stream_receive_status::closed) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), "HTTP/2 tunnel read");
            }
            const auto chunk = body_queue_.pop();
            // pop() returns the previous chunk's receive-window credit; flush
            // the WINDOW_UPDATE even if this reader suspends below.
            if (connection_.wants_write()) {
                wake_writer();
            }
            if (!chunk.empty()) {
                buffer.append(std::string_view(chunk.data(), chunk.size()));
                co_return true;
            }
            if (!body_queue_.empty()) {
                continue;
            }
            if (receive_status == http2_stream_receive_status::ended) {
                co_return false;
            }
            if (aborted_ || signal_.terminated()) {
                throw inbound_transport_error(
                    aborted_ ? std::make_error_code(std::errc::operation_canceled)
                             : signal_.terminal_error(),
                    "HTTP/2 tunnel read");
            }
            co_await signal_.wait();
        }
    }

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
            case http2_stream_data_write_status::content_length_incomplete:
                co_return std::make_error_code(std::errc::protocol_error);
        }
        co_return std::make_error_code(std::errc::protocol_error);
    }

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, websocket_transport_disposition disposition) {
        return write_bytes(bytes_value, disposition == websocket_transport_disposition::end_transport
                                            ? http_stream_end::end
                                            : http_stream_end::keep_open);
    }

    void abort() noexcept {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        try {
            (void)connection_.submit_reset(stream_id_, http2_error_code::cancel);
        } catch (...) {
            // abort() is best-effort teardown; callers cannot observe reset
            // serialization failure on this noexcept cleanup path.
        }
        output_budget_.release_and_reconcile(stream_id_, connection_);
        output_budget_.wake();
        signal_.wake();
        wake_writer();
    }

private:
    void wake_writer() noexcept {
        write_signal_.notify();
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    http2_sans_io_stream_signal& signal_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    executor_type executor_;
    bool aborted_{false};
};

}  // namespace ruvia::detail
