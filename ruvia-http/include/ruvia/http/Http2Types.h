#pragma once

#include <cstdint>

namespace ruvia {

enum class Http2Role : std::uint8_t {
    kServer,
    kClient,
};

// feed() has all-or-nothing ownership for each supplied span; it never partially
// consumes caller input. The public facade always begins the connection, so it
// never returns kConnectionNotStarted; the sans-I/O core can, and the caller
// retries the same span after beginConnection().
enum class Http2FeedResult : std::uint8_t {
    kConnectionNotStarted,
    kEventsPending,
    kAccepted,
    kNeedInput,
    kProtocolFailure,
};

enum class Http2EndStream : std::uint8_t {
    kKeepOpen,
    kEndStream,
};

[[nodiscard]] constexpr bool http2EndsStream(Http2EndStream value) noexcept {
    return value == Http2EndStream::kEndStream;
}

enum class Http2OutputConsumeStatus : std::uint8_t {
    kPending,
    kDrained,
    kOutOfRange,
};

// Initial-head/control submission status. kClosed is an expected race with a
// reset peer; kInvalidState is a caller contract violation and emits no bytes.
// kQueued/kBackpressured belong on Http2DataSubmitStatus, not here.
enum class Http2SubmitStatus : std::uint8_t {
    kAccepted,
    kClosed,
    kInvalidState,
    kInvalidMessage,
    kPeerCapabilityUnavailable,
};

enum class Http2DataSubmitStatus : std::uint8_t {
    kAccepted,
    kQueued,
    kBackpressured,
    kExpectationPending,
    kClosed,
    kInvalidState,
    kContentLengthExceeded,
    kContentLengthIncomplete,
};

enum class Http2RequestContentReleaseStatus : std::uint8_t {
    kReleased,
    kNotPending,
    kClosed,
};

enum class Http2StreamCloseSource : std::uint8_t {
    kLocal,
    kPeer,
    kPeerGoaway,
};

[[nodiscard]] constexpr bool http2IsValidStreamCloseSource(Http2StreamCloseSource source) noexcept {
    return source == Http2StreamCloseSource::kLocal || source == Http2StreamCloseSource::kPeer ||
           source == Http2StreamCloseSource::kPeerGoaway;
}

enum class Http2RequestHeadSubmitError : std::uint8_t {
    kInvalidState,
    kConnectionNotStarted,
    kConnectionUnavailable,
    kPeerStreamLimitReached,
    kLocalStreamCapacityReached,
    kPeerCapabilityUnavailable,
    kInvalidMessage,
};

}  // namespace ruvia
