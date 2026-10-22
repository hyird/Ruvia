#pragma once
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "context/http_interim_response_output.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/inbound_transport_error.h"
namespace ruvia::detail {
// Streaming request-body reader for the sans-I/O session; the body_reader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine read_body_chunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class http2_sans_io_request_body_reader final {
public:
    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so an unread content never prompts the client to send it.
    http2_sans_io_request_body_reader(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, const std::optional<http_request_body_failure>& body_failure,
        http2_sans_io_stream_signal& signal, worker_signal& write_signal,
        http_interim_response_output* continue_output) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          body_failure_(body_failure),
          signal_(signal),
          write_signal_(write_signal),
          continue_output_(continue_output) {}

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() {
        if (body_failure_.has_value()) {
            throw http_request_body_protocol_error(*body_failure_);
        }
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            if (body_failure_.has_value()) {
                throw http_request_body_protocol_error(*body_failure_);
            }
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), "HTTP/2 connection ended during body read");
            }
            if (connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), "HTTP/2 request stream reset during body read");
            }
            const auto chunk = body_queue_.pop();
            // pop() returns the previous chunk's receive-window credit; the
            // resulting WINDOW_UPDATE must reach the socket even if this reader
            // now suspends while the peer is blocked on that window.
            if (connection_.wants_write()) {
                write_signal_.notify();
            }
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (!body_queue_.empty()) {
                continue;
            }
            if (body_queue_.ended()) {
                co_return std::nullopt;
            }
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), "HTTP/2 connection ended during body read");
            }
            co_await signal_.wait();
        }
    }

private:
    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    const std::optional<http_request_body_failure>& body_failure_;
    http2_sans_io_stream_signal& signal_;
    worker_signal& write_signal_;
    http_interim_response_output* continue_output_;
};

}  // namespace ruvia::detail
