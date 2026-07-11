#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "ruvia/http/detail/http2/Http2StreamState.h"

namespace ruvia::detail {

enum class Http2BodyAccountingResult : std::uint8_t {
    kOk,
    kTooLarge,
    kContentLengthExceeded,
    kContentForbidden
};

[[nodiscard]] inline Http2BodyAccountingResult http2AccountDataBody(
    Http2StreamState& stream,
    std::size_t dataSize,
    std::size_t maxStreamBodyBytes,
    std::size_t maxBufferedBodyBytes) noexcept {
    const auto& remoteContent = stream.remoteContent();
    if (remoteContent.metadataOnlyWithoutLength() != nullptr ||
        remoteContent.metadataOnlyKnownLength() != nullptr) {
        return stream.accountRemoteContent(dataSize) ==
                Http2RemoteContentAccountingResult::kAccepted
            ? Http2BodyAccountingResult::kOk
            : Http2BodyAccountingResult::kContentForbidden;
    }
    const auto maxBody = httpRequestBodyByteLimit(
        stream.bodyMode(),
        maxStreamBodyBytes,
        maxBufferedBodyBytes);
    const auto receivedBytes =
        remoteContent.allowedWithoutLength() != nullptr
        ? remoteContent.allowedWithoutLength()->receivedBytes()
        : remoteContent.allowedKnownLength()->receivedBytes();
    if (maxBody != 0 &&
        (receivedBytes > maxBody || dataSize > maxBody - receivedBytes)) {
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
    switch (stream.accountRemoteContent(dataSize)) {
        case Http2RemoteContentAccountingResult::kAccepted:
            return Http2BodyAccountingResult::kOk;
        case Http2RemoteContentAccountingResult::kCounterOverflow:
            return Http2BodyAccountingResult::kTooLarge;
        case Http2RemoteContentAccountingResult::kDeclaredLengthExceeded:
            return Http2BodyAccountingResult::kContentLengthExceeded;
        case Http2RemoteContentAccountingResult::kContentForbidden:
            return Http2BodyAccountingResult::kContentForbidden;
    }
    return Http2BodyAccountingResult::kTooLarge;
}

}  // namespace ruvia::detail
