#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Http2Frame.h"
#include "Http2StreamState.h"
#include "Http2WindowUpdate.h"

namespace ruvia::detail {

enum class Http2ReceiveWindowResult : std::uint8_t {
    kOk,
    kConnectionExceeded,
    kStreamExceeded
};

[[nodiscard]] inline Http2WindowUpdateResult http2ApplyStreamWindowUpdate(
    Http2StreamState& stream,
    std::uint32_t increment) noexcept {
    if (increment == 0) {
        return Http2WindowUpdateResult::kZeroIncrement;
    }
    return stream.addSendWindow(static_cast<std::int64_t>(increment))
        ? Http2WindowUpdateResult::kOk
        : Http2WindowUpdateResult::kOverflow;
}

[[nodiscard]] inline Http2ReceiveWindowResult http2ConsumeReceiveWindows(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::int32_t bytes) noexcept {
    if (bytes > connectionWindow) {
        return Http2ReceiveWindowResult::kConnectionExceeded;
    }
    if (!stream.consumeReceiveWindow(bytes)) {
        return Http2ReceiveWindowResult::kStreamExceeded;
    }
    connectionWindow -= bytes;
    return Http2ReceiveWindowResult::kOk;
}

inline void http2RestoreReceiveWindows(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::int32_t bytes) noexcept {
    connectionWindow += bytes;
    stream.restoreReceiveWindow(bytes);
}

[[nodiscard]] inline bool http2SendWindowAvailable(
    std::int32_t connectionWindow,
    const Http2StreamState& stream) noexcept {
    return connectionWindow > 0 && stream.sendWindow() > 0;
}

[[nodiscard]] inline std::size_t http2AvailableSendWindow(
    std::int32_t connectionWindow,
    const Http2StreamState& stream) noexcept {
    // Either window can be non-positive (a stream send window goes negative when
    // the peer lowers SETTINGS_INITIAL_WINDOW_SIZE, RFC 7540 6.9.2). A negative
    // value must not wrap to a huge size_t and let a full frame be sent past an
    // exhausted window, so clamp: nothing is available until the window recovers.
    const auto available = std::min(connectionWindow, stream.sendWindow());
    return available > 0 ? static_cast<std::size_t>(available) : 0;
}

inline void http2ConsumeSendWindow(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::size_t bytes) noexcept {
    const auto amount = static_cast<std::int32_t>(bytes);
    connectionWindow -= amount;
    stream.consumeSendWindow(bytes);
}

}  // namespace ruvia::detail
