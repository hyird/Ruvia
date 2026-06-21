#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "Http2StreamState.h"

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
    if (stream.webSocketTunnel) {
        return Http2BodyAccountingResult::kOk;
    }
    const auto maxBody = stream.bodyMode == RequestBodyMode::kStream
        ? maxStreamBodyBytes
        : maxBufferedBodyBytes;
    if (maxBody != 0 &&
        (stream.receivedBodyBytes > maxBody || dataSize > maxBody - stream.receivedBodyBytes)) {
        return Http2BodyAccountingResult::kTooLarge;
    }
    if (dataSize > std::numeric_limits<std::size_t>::max() - stream.receivedBodyBytes) {
        return Http2BodyAccountingResult::kTooLarge;
    }
    stream.receivedBodyBytes += dataSize;
    if (stream.hasContentLength && stream.receivedBodyBytes > stream.contentLength) {
        return Http2BodyAccountingResult::kContentLengthExceeded;
    }
    return Http2BodyAccountingResult::kOk;
}

[[nodiscard]] inline bool http2BodyLengthComplete(const Http2StreamState& stream) noexcept {
    return stream.webSocketTunnel ||
        !stream.hasContentLength ||
        stream.receivedBodyBytes == stream.contentLength;
}

inline void http2MarkBodyEnded(Http2StreamState& stream) noexcept {
    stream.endStream = true;
    stream.bodyEnded = true;
}

}  // namespace ruvia::detail
