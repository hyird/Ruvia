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
#include "Http2HeaderDecode.h"
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
    // DoS-protection body caps (protocol-level, nghttp2-style; NOT server policy).
    std::size_t maxStreamBodyBytes{kDefaultMaxStreamBodyBytes};      // 0 = unlimited
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
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

// A response body the send window could not fully drain: the core keeps the unsent
// remainder and flushes it as WINDOW_UPDATE/SETTINGS reopen the window (nghttp2-style
// deferred data). One per blocked stream.
struct Http2PendingSend final {
    std::uint32_t streamId{0};
    std::pmr::string bytes;
    std::size_t offset{0};
    bool endStream{false};
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
    void appendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    // sans-I/O replacement for resumeSendWindowWaiters: a WINDOW_UPDATE/SETTINGS that
    // opened the send window drains buffered response bodies (pendingSends_) into the
    // outbound buffer; a stream whose body fully drains is reported via unblockedStreams_
    // so the owner can pull the next chunk of a streaming source. No coroutine resume.
    void markSendWindowOpened();

    // Emit a response header block as HEADERS + CONTINUATION frames (atomic sequence,
    // RFC 9113 §6.10) into the outbound buffer, ending the stream when endStream is set.
    void appendResponseHeaderFrames(
        Http2StreamState& stream, std::string_view headerBlock, bool endStream);
    // Emit DATA frames for data.substr(offset) while the send window allows, returning
    // the new offset (== data.size() when fully sent). Consumes send-window credit.
    [[nodiscard]] std::size_t sendDataUpToWindow(
        Http2StreamState& stream, std::string_view data, std::size_t offset, bool endStream);

    // Synchronous per-frame dispatch (ported 1:1 from processFrame/*; returns false
    // on a fatal protocol error, having appended GOAWAY and set closing_).
    [[nodiscard]] bool processFrame(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processSettings(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processPing(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processWindowUpdate(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processRstStream(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processPriority(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processHeaders(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processTrailerHeaders(
        Http2StreamState& stream, const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processContinuation(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processData(const Http2FrameHeader& header, std::string_view payload);
    // A DATA frame we must discard while keeping the connection: hand the peer its
    // connection flow-control credit back (WINDOW_UPDATE) so its send window recovers.
    void dropDataFrame(std::size_t flowBytes, bool windowConsumed);
    [[nodiscard]] bool applySettingsPayload(std::string_view payload);

    // HPACK header-block decode (all pure; ported 1:1 from the coroutine session but
    // WITHOUT resolveStreamRoute -- route resolution is web/edge policy the owner runs
    // after pulling kRequestHeaders). Return the classification; the caller reacts.
    [[nodiscard]] HeaderDecodeStatus decodeHeaderBlock(Http2StreamState& stream);
    [[nodiscard]] HeaderDecodeStatus decodeRefusedHeaderBlock(Http2StreamState& stream);
    [[nodiscard]] HeaderDecodeStatus finishTrailerBlock(Http2StreamState& stream);
    // On a decode failure: compression error is fatal (GOAWAY, returns false); anything
    // else RST_STREAMs the stream and survives (returns true).
    [[nodiscard]] bool handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status);
    // sans-I/O replacement for admitDecodedInitialStream/queueReady: emit kRequestHeaders
    // (and kRequestEnd when the peer already ended the stream) for the owner to dispatch.
    void emitRequestHeaders(Http2StreamState& stream);

    [[nodiscard]] Http2StreamState* findStream(std::uint32_t streamId) noexcept;
    [[nodiscard]] Http2StreamState* createStream(std::uint32_t streamId);

    // Close a stream: drop it from the ready queue, mark closed, emit kStreamClosed
    // (so the owner cancels any handler), remove it, and remember it as closed.
    void closeStream(std::uint32_t streamId, Http2StreamCloseSource source);

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

    // flow-control-deferred response bodies + streams that just fully drained
    std::pmr::vector<Http2PendingSend> pendingSends_;
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
