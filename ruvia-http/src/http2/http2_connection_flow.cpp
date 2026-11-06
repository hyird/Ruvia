#include <algorithm>

#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"

// Receive credit and send-window observation. nghttp2 owns both windows; the
// connection only decides when received bytes count as consumed.

namespace ruvia::detail {

void http2_connection::consume_received(std::uint32_t stream_id, std::size_t bytes) {
    if (bytes == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    // nghttp2 returns connection credit even when the stream is already gone,
    // and emits WINDOW_UPDATE at its half-window threshold.
    check_nghttp2(nghttp2_session_consume(session_.get(), static_cast<std::int32_t>(stream_id), bytes));
}

void http2_connection::flush_window_debt(http2_stream_state& stream) {
    // Unacknowledged event credit must survive stream removal at connection
    // scope, or an owner that stops consuming after a reset would permanently
    // shrink the shared window.
    const auto debt = stream.take_window_debt();
    if (debt == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    check_nghttp2(nghttp2_session_consume_connection(session_.get(), debt));
}

bool http2_connection::release_received_data(std::uint32_t stream_id, std::uint32_t bytes_value) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || !stream->take_window_debt(bytes_value)) {
        return false;  // debt (if any) died with the stream; nothing left to credit
    }
    consume_received(stream_id, bytes_value);
    return true;
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return output_.pending_data_bytes(stream_id);
}

std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr || http2_stream_is_closed(*stream)) {
        return std::nullopt;
    }
    const auto stream_window =
        nghttp2_session_get_stream_remote_window_size(session_.get(), static_cast<std::int32_t>(stream_id));
    if (stream_window < 0 && nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        return std::nullopt;
    }
    const auto connection_window = connection_send_window();
    return http2_send_window_state{
        .connection_window_ = connection_window,
        .stream_window_ = stream_window,
        .available_ = static_cast<std::uint32_t>((std::max)(0, (std::min)(connection_window, stream_window))),
    };
}

bool http2_connection::can_send_empty_end_stream(std::uint32_t stream_id) const noexcept {
    const auto window = send_window_state(stream_id);
    return window.has_value() && window->connection_window_ > 0 && window->stream_window_ > 0;
}

bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    return stream == nullptr || stream->is_aborted();
}

}  // namespace ruvia::detail
