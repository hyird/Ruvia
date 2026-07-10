#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "ruvia/http/detail/http2/Http2StreamState.h"

namespace ruvia::detail {

enum class Http2BodyAccountingResult : std::uint8_t {
    kOk,
    kTooLarge,
    kContentLengthExceeded
};

[[nodiscard]] inline Http2BodyAccountingResult http2AccountDataBody(
    Http2StreamState& stream,
    std::size_t dataSize,
    std::size_t maxStreamBodyBytes,
    std::size_t maxBufferedBodyBytes) noexcept {
    if (stream.webSocketTunnel()) {
        return Http2BodyAccountingResult::kOk;
    }
    const auto maxBody = httpRequestBodyByteLimit(
        stream.bodyMode(),
        maxStreamBodyBytes,
        maxBufferedBodyBytes);
    if (maxBody != 0 &&
        (stream.receivedBodyBytes() > maxBody ||
            dataSize > maxBody - stream.receivedBodyBytes())) {
        return Http2BodyAccountingResult::kTooLarge;
    }
    // The streaming body mode has no total-size cap by default (maxStreamBodyBytes is
    // 0 = unbounded, since streaming exists precisely to accept arbitrarily large
    // uploads). But the receive flow-control window is re-credited on every DATA frame,
    // so a peer that streams faster than the handler drains it would grow the queued
    // backlog without bound -- a memory DoS. Bound the *un-drained* backlog (not the
    // total) at maxBufferedBodyBytes: a handler that keeps up still accepts an upload of
    // any size, while one that falls this far behind has its stream reset rather than
    // buffering without limit. The buffered mode is already bounded by its total cap.
    if (maxBufferedBodyBytes != 0 && stream.usesStreamRequestBody()) {
        const auto queued = stream.queuedBodyBytes();
        if (queued > maxBufferedBodyBytes ||
            dataSize > maxBufferedBodyBytes - queued) {
            return Http2BodyAccountingResult::kTooLarge;
        }
    }
    if (!stream.addReceivedBodyBytes(dataSize)) {
        return Http2BodyAccountingResult::kTooLarge;
    }
    if (stream.receivedBodyExceedsContentLength()) {
        return Http2BodyAccountingResult::kContentLengthExceeded;
    }
    return Http2BodyAccountingResult::kOk;
}

[[nodiscard]] inline bool http2BodyLengthComplete(const Http2StreamState& stream) noexcept {
    return stream.webSocketTunnel() || stream.bodyLengthComplete();
}

inline void http2MarkBodyEnded(Http2StreamState& stream) noexcept {
    stream.markPeerEndStream();
    stream.markBodyEnded();
}

}  // namespace ruvia::detail
