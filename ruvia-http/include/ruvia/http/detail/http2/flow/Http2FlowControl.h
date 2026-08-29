#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/http2/flow/Http2WindowUpdate.h"

namespace ruvia::detail {

enum class Http2ReceiveWindowDebitStatus : std::uint8_t { kAccepted, kExceeded };

[[nodiscard]] inline Http2WindowUpdateResult http2ApplyStreamWindowUpdate(
    Http2StreamState& stream, std::uint32_t increment) noexcept {
    if (increment == 0) {
        return Http2WindowUpdateResult::kZeroIncrement;
    }
    return stream.addSendWindow(static_cast<std::int64_t>(increment))
               ? Http2WindowUpdateResult::kOk
               : Http2WindowUpdateResult::kOverflow;
}

// Unless the frame itself is rejected as a connection error, DATA is debited from the
// connection window first. Keeping connection and stream operations separate lets
// callers account for frames on closed/reset streams, which have no live stream window
// but still consume connection credit (RFC 9113 §6.9/§6.9.1). A rejected debit is
// transactional.
[[nodiscard]] inline Http2ReceiveWindowDebitStatus http2DebitConnectionReceiveWindow(
    std::int32_t& connectionWindow, std::int32_t bytes) noexcept {
    if (bytes > connectionWindow) {
        return Http2ReceiveWindowDebitStatus::kExceeded;
    }
    connectionWindow -= bytes;
    return Http2ReceiveWindowDebitStatus::kAccepted;
}

[[nodiscard]] inline Http2ReceiveWindowDebitStatus http2DebitStreamReceiveWindow(
    Http2StreamState& stream, std::int32_t bytes) noexcept {
    return stream.consumeReceiveWindow(bytes) ? Http2ReceiveWindowDebitStatus::kAccepted
                                              : Http2ReceiveWindowDebitStatus::kExceeded;
}

inline void http2CreditConnectionReceiveWindow(
    std::int32_t& connectionWindow, std::int32_t bytes) noexcept {
    connectionWindow += bytes;
}

inline void http2CreditStreamReceiveWindow(Http2StreamState& stream, std::int32_t bytes) noexcept {
    stream.restoreReceiveWindow(bytes);
}

[[nodiscard]] inline bool http2SendWindowAvailable(
    std::int32_t connectionWindow, const Http2StreamState& stream) noexcept {
    return connectionWindow > 0 && stream.sendWindow() > 0;
}

[[nodiscard]] inline std::size_t http2AvailableSendWindow(
    std::int32_t connectionWindow, const Http2StreamState& stream) noexcept {
    // Either window can be non-positive (a stream send window goes negative when
    // the peer lowers SETTINGS_INITIAL_WINDOW_SIZE, RFC 7540 6.9.2). A negative
    // value must not wrap to a huge size_t and let a full frame be sent past an
    // exhausted window, so clamp: nothing is available until the window recovers.
    const auto available = std::min(connectionWindow, stream.sendWindow());
    return available > 0 ? static_cast<std::size_t>(available) : 0;
}

inline void http2ConsumeSendWindow(
    std::int32_t& connectionWindow, Http2StreamState& stream, std::size_t bytes) noexcept {
    const auto amount = static_cast<std::int32_t>(bytes);
    connectionWindow -= amount;
    stream.consumeSendWindow(bytes);
}

}  // namespace ruvia::detail
