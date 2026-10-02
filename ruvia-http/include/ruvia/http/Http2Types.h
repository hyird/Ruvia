#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia {

enum class Http2FinishRequestStatus : std::uint8_t {
    kAccepted,
    kQueued,
    kClosed,
    kInvalidState,
    kContentLengthIncomplete,
    kInvalidTrailer,
};

enum class Http2Role : std::uint8_t {
    kServer,
    kClient,
};

// Read-only receive-side state for one stream; never exposes stream storage.
enum class Http2StreamReceiveStatus : std::uint8_t {
    kOpen,
    kEnded,
    kClosed,
};

// Borrowed snapshot of the peer's HTTP/2 request metadata. Views remain valid
// only until the connection consumes more input; no stream storage is exposed.
struct http2_server_request_view final {
    std::string_view method{};
    std::string_view path{};
    std::string_view authority{};
    std::string_view protocol{};
};

// Read-only send-flow-control observation for a live stream. The available
// amount is the minimum of the connection and stream windows, clamped at zero.
struct Http2SendWindowState final {
    std::int32_t connectionWindow{0};
    std::int32_t streamWindow{0};
    std::uint32_t available{0};
    bool queuedData{false};
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

enum class Http2OutputBatchStatus : std::uint8_t { kTaken,
    kEmpty,
    kUnaligned };

struct Http2OutputBatchResult final {
    Http2OutputBatchStatus status{Http2OutputBatchStatus::kEmpty};
    std::size_t bytes{0};
};

using Http2DataOutputObserver = void (*)(void*, std::uint32_t, std::size_t) noexcept;

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

enum class Http2DataQueueState : std::uint8_t { kDrained,
    kQueued,
    kAborted };

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

enum class Http2ResponseHeadSubmitError : std::uint8_t {
    kPeerStreamLimitReached,
    kClosed,
    kInvalidState,
    kResponsePlanMismatch,
    kInvalidMessage,
};

}  // namespace ruvia
