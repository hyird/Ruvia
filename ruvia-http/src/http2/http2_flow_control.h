#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "http2/http2_stream_state.h"
#include "http2/http2_window_update.h"

namespace ruvia::detail {

[[nodiscard]] inline std::size_t http2_data_frame_count(std::size_t data_bytes, std::size_t max_frame_size) {
    return data_bytes == 0 ? 0 : data_bytes / max_frame_size + (data_bytes % max_frame_size == 0 ? 0 : 1);
}

[[nodiscard]] inline std::size_t http2_data_frame_encoded_bytes(
    std::size_t data_bytes, std::size_t max_frame_size) {
    const auto frame_count = http2_data_frame_count(data_bytes, max_frame_size);
    if (frame_count > (std::numeric_limits<std::size_t>::max() - data_bytes) / http2_frame_header_bytes) {
        throw std::length_error("HTTP/2 DATA output size overflow");
    }
    return data_bytes + frame_count * http2_frame_header_bytes;
}

[[nodiscard]] inline http2_window_update_result http2_apply_stream_window_update(
    http2_stream_state& stream, std::uint32_t increment) noexcept {
    if (increment == 0) {
        return http2_window_update_result::zero_increment;
    }
    return stream.add_send_window(static_cast<std::int64_t>(increment))
               ? http2_window_update_result::ok
               : http2_window_update_result::overflow;
}

inline void http2_credit_connection_receive_window(
    std::int32_t& connection_window, std::int32_t bytes_value) noexcept {
    connection_window += bytes_value;
}

inline void http2_credit_stream_receive_window(http2_stream_state& stream, std::int32_t bytes_value) noexcept {
    stream.restore_receive_window(bytes_value);
}

[[nodiscard]] inline std::size_t http2_available_send_window(
    std::int32_t connection_window, const http2_stream_state& stream) noexcept {
    // Either window can be non-positive (a stream send window goes negative when
    // the peer lowers SETTINGS_INITIAL_WINDOW_SIZE, RFC 7540 6.9.2). A negative
    // value must not wrap to a huge size_t and let a full frame be sent past an
    // exhausted window, so clamp: nothing is available until the window recovers.
    const auto available = std::min(connection_window, stream.send_window());
    return available > 0 ? static_cast<std::size_t>(available) : 0;
}

inline void http2_consume_send_window(
    std::int32_t& connection_window, http2_stream_state& stream, std::size_t bytes_value) noexcept {
    const auto amount = static_cast<std::int32_t>(bytes_value);
    connection_window -= amount;
    stream.consume_send_window(bytes_value);
}

}  // namespace ruvia::detail
