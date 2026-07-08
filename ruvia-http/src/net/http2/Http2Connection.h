#pragma once

// HTTP/2 sans-I/O connection core.
//
// A pure protocol state machine: it never touches a socket, a coroutine, a timer,
// or asio. You feed it inbound bytes and it advances the protocol and emits events
// (request ready, body chunk, stream closed, ...); you submit responses and it
// produces outbound bytes for you to write. The I/O loop, concurrency model and
// timeouts live entirely in the caller (see the generic asio driver in ruvia-core;
// external users can drive it from any runtime, nghttp2-style).
//
// Design mirrors nghttp2's mem_recv / mem_send: feed() ~ nghttp2_session_mem_recv,
// pendingOutput()/consumeOutput() ~ nghttp2_session_mem_send. Flow-control back
// pressure is expressed with defer/resume (submitData may return kBlocked; the
// caller resumes blocked streams via pumpWritable after WINDOW_UPDATE/SETTINGS),
// not with coroutine suspension.
//
// All protocol primitives it builds on (frame codec, HPACK, stream state, flow
// control, input buffer, settings) are already pure and reused as-is.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Http2ClosedStreams.h"
#include "Http2Frame.h"
#include "Http2HeaderContinuation.h"
#include "Http2Hpack.h"
#include "Http2LocalSettings.h"
#include "Http2PeerSettings.h"
#include "Http2ReadyQueue.h"
#include "Http2StreamState.h"
#include "Http2StreamTable.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// Connection phase, used by the (external) I/O layer to pick an inactivity timeout.
// A pure enum so the core never depends on ConnectionScanner (which is asio-bound).
enum class Http2ConnectionPhase : std::uint8_t {
    kIdle,
    kReadingHeader,
    kReadingBody,
    kWriting,
    kWebSocket,
};

// Protocol-only configuration (NOT server policy). Server policy such as timeouts,
// CORS, rate limiting and access logging lives in ruvia-web, never here.
struct Http2CoreConfig final {
    std::uint32_t maxFrameSize{kHttp2DefaultMaxFrameSize};
    std::uint32_t initialSendWindow{kHttp2DefaultInitialWindowSize};
    std::uint32_t initialReceiveWindow{kHttp2LocalInitialWindowSize};
    std::size_t maxHeaderListBytes{kMaxHttpHeaderBytes};
};

enum class Http2FeedStatus : std::uint8_t {
    kOk,        // consumed some bytes, may have emitted events
    kNeedMore,  // buffered a partial frame; feed more bytes
    kError,     // protocol error; connection should GOAWAY/close (bytes in output)
};

struct Http2FeedResult final {
    std::size_t consumed{0};
    Http2FeedStatus status{Http2FeedStatus::kNeedMore};
};

// Events pulled by the core's owner (ruvia-web / ruvia-edge) to drive handlers.
struct Http2Event final {
    enum class Kind : std::uint8_t {
        kNone,
        kRequestHeaders,   // a full request head is available on `streamId`
        kRequestBodyChunk, // inbound DATA for `streamId` (view valid until next feed)
        kRequestEnd,       // END_STREAM seen for `streamId`
        kStreamClosed,     // `streamId` fully closed / reset
        kGoaway,           // peer sent GOAWAY; drain
    };
    Kind kind{Kind::kNone};
    std::uint32_t streamId{0};
    std::string_view bytes{};  // for kRequestBodyChunk
};

// Result of submitting response DATA: kBlocked means flow-control window is closed;
// the caller keeps the source and retries after pumpWritable().
enum class Http2SubmitResult : std::uint8_t {
    kOk,
    kBlocked,
    kClosed,  // stream is gone (reset/closed); drop the response
};

class Http2Connection final {
public:
    explicit Http2Connection(std::pmr::memory_resource* resource, Http2CoreConfig config = {});

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer; advances the protocol. Returns bytes
    // consumed + status. Emitted events are pulled with nextEvent().
    [[nodiscard]] Http2FeedResult feed(std::string_view in);
    // Pull the next protocol event, or Kind::kNone when drained.
    [[nodiscard]] Http2Event nextEvent();

    // Access an assembled request head / stream for the owner to build an HttpRequest.
    [[nodiscard]] Http2StreamState* stream(std::uint32_t streamId) noexcept;

    // --- outbound --------------------------------------------------------------
    // Bytes the core wants written to the peer (frame headers + payloads, batched).
    [[nodiscard]] std::string_view pendingOutput() const noexcept;
    void consumeOutput(std::size_t n) noexcept;
    [[nodiscard]] bool wantsWrite() const noexcept { return outOffset_ < outBuffer_.size(); }

    // Submit a response for `streamId`. Head first, then data chunks, then end.
    void submitResponseHead(std::uint32_t streamId, const HttpResponse& response, bool bodyForbidden);
    [[nodiscard]] Http2SubmitResult submitData(std::uint32_t streamId, std::string_view chunk, bool endStream);
    void submitReset(std::uint32_t streamId, std::uint32_t errorCode);

    // After WINDOW_UPDATE/SETTINGS opened windows, the owner calls this so blocked
    // streams get another chance; the owner then re-submits their deferred sources.
    // Returns the streams that just unblocked (owner resumes their handlers).
    [[nodiscard]] std::span<const std::uint32_t> takeUnblockedStreams() noexcept;

    // --- lifecycle / timeout ---------------------------------------------------
    [[nodiscard]] Http2ConnectionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool closing() const noexcept { return closing_; }
    void beginGoaway(std::uint32_t errorCode);

    // Emit connection preface SETTINGS (call once after construction). Bytes land in
    // the outbound buffer.
    void queueLocalSettings();

private:
    // Outbound frame emission: encode a 9-byte header + payload into outBuffer_.
    // Replaces the coroutine writeFramePayload; the encoders are pure.
    void appendFrame(
        Http2FrameType type, std::uint8_t flags, std::uint32_t streamId,
        std::string_view first, std::string_view second = {});
    void appendGoaway(Http2ErrorCode error, std::string_view debug = {});

    // Synchronous per-frame dispatch (ported 1:1 from processFrame/*; returns false
    // on a fatal protocol error, having appended GOAWAY and set closing_).
    [[nodiscard]] bool processFrame(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processSettings(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool applySettingsPayload(std::string_view payload);

    std::pmr::memory_resource* resource_;
    Http2CoreConfig config_;

    // inbound byte buffer (reused across feeds; inputOffset_ = consumed cursor)
    std::pmr::string input_;
    std::size_t inputOffset_{0};

    // outbound byte buffer (batched writes; outOffset_ = flushed cursor)
    std::pmr::string outBuffer_;
    std::size_t outOffset_{0};

    // pure protocol state (all reused as-is)
    Http2StreamTable streams_;
    Http2ClosedStreamHistory closedStreams_;
    Http2ReadyQueue readyQueue_;
    HpackDecoder decoder_;
    Http2HeaderContinuation headerContinuation_;
    Http2PeerSettings peerSettings_;
    std::optional<Http2StreamState> refusedHeaderStream_;

    // event queue drained by nextEvent()
    std::pmr::vector<Http2Event> events_;
    std::size_t eventOffset_{0};

    // flow-control-blocked streams awaiting more window (defer/resume)
    std::pmr::vector<std::uint32_t> blockedStreams_;
    std::pmr::vector<std::uint32_t> unblockedStreams_;

    std::uint32_t localMaxFrameSize_{kHttp2DefaultMaxFrameSize};
    std::uint32_t lastStreamId_{0};
    std::int32_t connectionSendWindow_{kHttp2DefaultInitialWindowSize};
    std::int32_t connectionReceiveWindow_{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
    Http2ConnectionPhase phase_{Http2ConnectionPhase::kIdle};
    bool receivedFirstSettings_{false};
    bool closing_{false};
};

}  // namespace ruvia::detail
