#pragma once

#include <cstdint>

#include "Http2Frame.h"
#include "../server/ConnectionScanner.h"

namespace ruvia::detail {

// Pick the connection-scanner phase for the next HTTP/2 frame read. Only a header
// block still being assembled (a HEADERS without END_HEADERS, so we are awaiting
// CONTINUATION frames) uses the tight header timeout: that is the CONTINUATION-
// flood / slow-loris bound and is non-evadable, since the peer stays in this state
// until it sends END_HEADERS and the phaseStartedMs is not reset by more frames.
// Every other read wait -- request-body DATA, or idling between requests -- uses
// the body timeout, so a slow but legitimate upload is not cut off by the much
// shorter header timeout (the whole connection previously read as kReadingHeader).
[[nodiscard]] inline ConnectionScanner::Phase http2ReadFramePhase(bool headerBlockInProgress) noexcept {
    return headerBlockInProgress
        ? ConnectionScanner::Phase::kReadingHeader
        : ConnectionScanner::Phase::kReadingBody;
}

class Http2HeaderContinuation final {
public:
    [[nodiscard]] bool active() const noexcept {
        return streamId_ != 0;
    }

    [[nodiscard]] bool expectsFrameType(std::uint8_t frameType) const noexcept {
        return !active() || frameType == static_cast<std::uint8_t>(Http2FrameType::kContinuation);
    }

    [[nodiscard]] bool matches(std::uint32_t streamId) const noexcept {
        return streamId != 0 && streamId == streamId_;
    }

    void start(std::uint32_t streamId, bool trailers) noexcept {
        streamId_ = streamId;
        trailers_ = trailers;
    }

    void reset() noexcept {
        streamId_ = 0;
        trailers_ = false;
    }

    [[nodiscard]] bool finishWasTrailers() noexcept {
        const bool trailers = trailers_;
        reset();
        return trailers;
    }

private:
    std::uint32_t streamId_{0};
    bool trailers_{false};
};

}  // namespace ruvia::detail
