#pragma once
#include <optional>
#include <span>
#include <system_error>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_sans_io_stream_runtime.h"
namespace ruvia::detail {
// Streaming request-body reader for the sans-I/O session; the body_reader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine read_body_chunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class http2_sans_io_request_body_reader final {
public:
    http2_sans_io_request_body_reader(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, http2_sans_io_stream_signal& signal,
        worker_signal& write_signal) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          signal_(signal),
          write_signal_(write_signal) {}

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() {
        for (;;) {
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminal_error());
            }
            const auto receive_status = connection_.stream_receive_status(stream_id_);
            if (receive_status == http2_stream_receive_status::closed) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
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
            if (receive_status == http2_stream_receive_status::ended) {
                co_return std::nullopt;
            }
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminal_error());
            }
            co_await signal_.wait();
        }
    }

private:
    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    http2_sans_io_stream_signal& signal_;
    worker_signal& write_signal_;
};

}  // namespace ruvia::detail
