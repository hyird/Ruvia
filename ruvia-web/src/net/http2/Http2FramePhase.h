#pragma once

#include "net/server/ConnectionScanner.h"

namespace ruvia::detail {

// Pick the connection-scanner phase for the next HTTP/2 frame read. Only a header
// block still being assembled (a HEADERS without END_HEADERS, so we are awaiting
// CONTINUATION frames) uses the tight header timeout: that is the CONTINUATION-
// flood / slow-loris bound and is non-evadable, since the peer stays in this state
// until it sends END_HEADERS and the phaseStartedMs is not reset by more frames.
// Every other read wait -- request-body DATA, or idling between requests -- uses
// the body timeout, so a slow but legitimate upload is not cut off by the much
// shorter header timeout (the whole connection previously read as kReadingHeader).
//
// Lives in ruvia-web (not with Http2HeaderContinuation in the protocol core): the
// mapping targets ConnectionScanner, which is asio-bound I/O policy, and the sans-I/O
// core must stay asio-free (it exposes the pure Http2ConnectionPhase instead).
[[nodiscard]] inline ConnectionScanner::Phase http2ReadFramePhase(bool headerBlockInProgress) noexcept {
    return headerBlockInProgress
        ? ConnectionScanner::Phase::kReadingHeader
        : ConnectionScanner::Phase::kReadingBody;
}

}  // namespace ruvia::detail
