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
class http2_sans_io_request_body_reader final
    : public multiplexed_request_body_reader_base<http2_sans_io_request_body_reader> {
public:
    static constexpr const char* terminated_error_message = "HTTP/2 connection ended during body read";
    static constexpr const char* reset_error_message = "HTTP/2 request stream reset during body read";

    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so an unread content never prompts the client to send it.
    http2_sans_io_request_body_reader(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, const std::optional<http_request_body_failure>& body_failure,
        multiplexed_stream_signal& signal, worker_signal& write_signal,
        http_interim_response_output* continue_output) noexcept
        : multiplexed_request_body_reader_base(signal, continue_output),
          connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          body_failure_(body_failure),
          write_signal_(write_signal) {}

private:
    friend class multiplexed_request_body_reader_base<http2_sans_io_request_body_reader>;

    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const noexcept {
        return body_failure_;
    }

    [[nodiscard]] bool stream_aborted() const noexcept {
        return connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed;
    }

    [[nodiscard]] std::string_view pop_chunk() noexcept {
        const auto chunk = body_queue_.pop();
        // pop() returns the previous chunk's receive-window credit; the
        // resulting WINDOW_UPDATE must reach the socket even if this reader
        // now suspends while the peer is blocked on that window.
        if (connection_.wants_write()) {
            write_signal_.notify();
        }
        return chunk;
    }

    [[nodiscard]] bool body_ended() const noexcept {
        return body_queue_.ended();
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    const std::optional<http_request_body_failure>& body_failure_;
    worker_signal& write_signal_;
};

}  // namespace ruvia::detail
