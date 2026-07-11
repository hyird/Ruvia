#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "ruvia/http/detail/http2/Http2Role.h"
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
    if (stream.tunnel().open() != nullptr) {
        return Http2BodyAccountingResult::kOk;
    }
    const auto maxBody = httpRequestBodyByteLimit(
        stream.bodyMode(),
        maxStreamBodyBytes,
        maxBufferedBodyBytes);
    const auto receivedBytes = stream.remoteContent().receivedBytes();
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
    switch (stream.checkRemoteContentAccept(dataSize)) {
        case Http2RemoteContentCheck::kAccepted:
            break;
        case Http2RemoteContentCheck::kCounterOverflow:
            return Http2BodyAccountingResult::kTooLarge;
        case Http2RemoteContentCheck::kDeclaredLengthExceeded:
            return Http2BodyAccountingResult::kContentLengthExceeded;
    }
    stream.acceptRemoteContent(dataSize);
    return Http2BodyAccountingResult::kOk;
}

[[nodiscard]] inline bool http2RemoteContentTerminalValid(
    const Http2StreamState& stream,
    Http2Role role) noexcept {
    // RFC 9113 §8.1.1 delegates no-content response semantics to HTTP. A HEAD
    // response and 204/304 may carry representation Content-Length metadata even
    // though their HTTP/2 message contains no DATA. Successful CONNECT is already
    // a tunnel and its DATA is not HTTP content.
    const bool lengthExempt = role == Http2Role::kClient &&
        (stream.requestKnownMethod() == HttpKnownMethod::kHead ||
         stream.responseStatus() == 204 || stream.responseStatus() == 304);
    return stream.tunnel().open() != nullptr || lengthExempt ||
        stream.remoteContent().terminalLengthValid();
}

}  // namespace ruvia::detail
