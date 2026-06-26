#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "Http2StreamState.h"
#include "../RequestBodyLimit.h"

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
    const auto maxBody = requestBodyByteLimit(
        stream.bodyMode(),
        maxStreamBodyBytes,
        maxBufferedBodyBytes);
    if (maxBody != 0 &&
        (stream.receivedBodyBytes() > maxBody ||
            dataSize > maxBody - stream.receivedBodyBytes())) {
        return Http2BodyAccountingResult::kTooLarge;
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
