#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "http2/http2_stream_state.h"
#include "http2/http2_window_update.h"

namespace ruvia::detail {

enum class http2_receive_window_debit_status : std::uint8_t { accepted,
    exceeded };

[[nodiscard]] inline http2_window_update_result http2_apply_stream_window_update(
    http2_stream_state& stream, std::uint32_t increment) noexcept {
    if (increment == 0) {
        return http2_window_update_result::zero_increment;
    }
    return stream.add_send_window(static_cast<std::int64_t>(increment))
               ? http2_window_update_result::ok
               : http2_window_update_result::overflow;
}

// Unless the frame itself is rejected as a connection error, DATA is debited from the
// connection window first. Keeping connection and stream operations separate lets
// callers account for frames on closed/reset streams, which have no live stream window
// but still consume connection credit (RFC 9113 §6.9/§6.9.1). A rejected debit is
// transactional.
[[nodiscard]] inline http2_receive_window_debit_status http2_debit_connection_receive_window(
    std::int32_t& connection_window, std::int32_t bytes_value) noexcept {
    if (bytes_value > connection_window) {
        return http2_receive_window_debit_status::exceeded;
    }
    connection_window -= bytes_value;
    return http2_receive_window_debit_status::accepted;
}

[[nodiscard]] inline http2_receive_window_debit_status http2_debit_stream_receive_window(
    http2_stream_state& stream, std::int32_t bytes_value) noexcept {
    return stream.consume_receive_window(bytes_value) ? http2_receive_window_debit_status::accepted
                                                      : http2_receive_window_debit_status::exceeded;
}

inline void http2_credit_connection_receive_window(
    std::int32_t& connection_window, std::int32_t bytes_value) noexcept {
    connection_window += bytes_value;
}

inline void http2_credit_stream_receive_window(http2_stream_state& stream, std::int32_t bytes_value) noexcept {
    stream.restore_receive_window(bytes_value);
}

[[nodiscard]] inline bool http2_send_window_available(
    std::int32_t connection_window, const http2_stream_state& stream) noexcept {
    return connection_window > 0 && stream.send_window() > 0;
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
