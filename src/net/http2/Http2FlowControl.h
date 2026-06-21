#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "Http2Frame.h"
#include "Http2StreamState.h"

namespace ruvia::detail {

enum class Http2WindowUpdateResult : std::uint8_t {
    kOk,
    kZeroIncrement,
    kOverflow
};

enum class Http2ReceiveWindowResult : std::uint8_t {
    kOk,
    kConnectionExceeded,
    kStreamExceeded
};

[[nodiscard]] inline std::uint32_t http2WindowUpdateIncrement(std::string_view payload) noexcept {
    return http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
}

[[nodiscard]] inline Http2WindowUpdateResult http2ApplyWindowUpdate(
    std::int32_t& window,
    std::uint32_t increment) noexcept {
    if (increment == 0) {
        return Http2WindowUpdateResult::kZeroIncrement;
    }
    const auto amount = static_cast<std::int32_t>(increment);
    if (window > std::numeric_limits<std::int32_t>::max() - amount) {
        return Http2WindowUpdateResult::kOverflow;
    }
    window += amount;
    return Http2WindowUpdateResult::kOk;
}

[[nodiscard]] inline Http2ReceiveWindowResult http2ConsumeReceiveWindows(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::int32_t bytes) noexcept {
    if (bytes > connectionWindow) {
        return Http2ReceiveWindowResult::kConnectionExceeded;
    }
    if (bytes > stream.receiveWindow) {
        return Http2ReceiveWindowResult::kStreamExceeded;
    }
    connectionWindow -= bytes;
    stream.receiveWindow -= bytes;
    return Http2ReceiveWindowResult::kOk;
}

inline void http2RestoreReceiveWindows(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::int32_t bytes) noexcept {
    connectionWindow += bytes;
    stream.receiveWindow += bytes;
}

[[nodiscard]] inline bool http2SendWindowAvailable(
    std::int32_t connectionWindow,
    const Http2StreamState& stream) noexcept {
    return connectionWindow > 0 && stream.sendWindow > 0;
}

[[nodiscard]] inline std::size_t http2AvailableSendWindow(
    std::int32_t connectionWindow,
    const Http2StreamState& stream) noexcept {
    return static_cast<std::size_t>(std::min(connectionWindow, stream.sendWindow));
}

inline void http2ConsumeSendWindow(
    std::int32_t& connectionWindow,
    Http2StreamState& stream,
    std::size_t bytes) noexcept {
    const auto amount = static_cast<std::int32_t>(bytes);
    connectionWindow -= amount;
    stream.sendWindow -= amount;
}

}  // namespace ruvia::detail
