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

struct HttpServerParseResult;  // h1 parse result seeding an h2c-upgraded stream

// Which side of the connection this endpoint is. A server accepts peer-initiated odd
// streams and answers them; a client opens odd streams itself and decodes RESPONSE
// heads (:status) off them. One state machine serves both roles (the frame codec,
// HPACK, flow control and lifecycle are identical); only stream-id admission rules
// and header-block semantics switch on the role.
enum class Http2Role : std::uint8_t {
    kServer,
    kClient,
};

// Protocol-only configuration (NOT server policy). Server policy such as timeouts,
// CORS, rate limiting and access logging lives in ruvia-web, never here.
struct Http2CoreConfig final {
    // These three seed the flow-control / framing ACCOUNTING and MUST match the values
    // advertised in the local connection preface SETTINGS (queueLocalSettings). They
    // default to exactly the advertised constants; only override them together with a
    // matching custom SETTINGS frame, or accounting will diverge from the wire.
    std::uint32_t maxFrameSize{kHttp2DefaultMaxFrameSize};
    std::uint32_t initialSendWindow{kHttp2DefaultInitialWindowSize};
    std::uint32_t initialReceiveWindow{kHttp2LocalInitialWindowSize};
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

// Events pulled by the core's owner to drive handlers. Direction-neutral: the peer
// "message" is the request when this endpoint is the server, the response when it is
// the client.
struct Http2Event final {
    enum class Kind : std::uint8_t {
        kNone,
        kMessageHead,       // the peer's full message head decoded on `streamId`
        kMessageBodyChunk,  // inbound DATA for `streamId` (view valid until next feed)
        kMessageEnd,        // END_STREAM seen for `streamId`
        kStreamClosed,      // `streamId` aborted (peer RST or local protocol reject)
        kGoaway,            // peer sent GOAWAY; `streamId` = its last-processed id
    };
    Kind kind{Kind::kNone};
    std::uint32_t streamId{0};
    std::string_view bytes{};  // for kMessageBodyChunk
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
    explicit Http2Connection(
        std::pmr::memory_resource* resource,
        Http2CoreConfig config = {},
        Http2Role role = Http2Role::kServer);

    [[nodiscard]] Http2Role role() const noexcept { return role_; }

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
    // Move ALL pending outbound bytes into `into` (allocator-matching swap when nothing
    // was partially consumed, so the common path is copy-free) and reset the buffer.
    // REQUIRED for any writer that awaits mid-write: a pendingOutput() view dangles if
    // a concurrent submit reallocates the buffer during the write.
    void takeOutput(std::pmr::string& into);
    [[nodiscard]] bool wantsWrite() const noexcept { return outOffset_ < outBuffer_.size(); }

    // Submit a response for `streamId`. Head first, then data chunks, then end.
    void submitResponseHead(std::uint32_t streamId, const HttpResponse& response, bool bodyForbidden);
    // Submit a STREAMING response head: emit the HEADERS block with NO auto
    // Content-Length (the body length is unknown), leaving the stream open for
    // subsequent submitData chunks unless bodyForbidden (then END_STREAM on the head).
    // The owner then streams the body with submitData(..., endStream) at the end.
    void submitStreamingResponseHead(
        std::uint32_t streamId, const HttpResponse& head, bool bodyForbidden);
    [[nodiscard]] Http2SubmitResult submitData(std::uint32_t streamId, std::string_view chunk, bool endStream);
    // RFC 8441 Extended CONNECT: emit the WebSocket handshake response HEADERS (200 +
    // optional sec-websocket-protocol, NO END_STREAM) so the stream stays open as the
    // tunnel; the owner then exchanges WebSocket frames via submitData. Mirrors the
    // coroutine session's writeHttp2WebSocketHandshake byte-for-byte.
    void submitWebSocketHandshake(
        std::uint32_t streamId, std::string_view subprotocol, std::string_view extensions = {});
    // Emit an HPACK-encoded trailer block as the stream's final HEADERS (END_STREAM),
    // in place of the empty END_STREAM DATA frame (RFC 9113 §8.1). PRECONDITION: the
    // stream has NO window-blocked DATA remainder (hasBlockedSend(streamId) == false);
    // callers must pace the body on the send window before ending (the streaming sink
    // does via awaitSendWindow), or the END_STREAM trailer would jump ahead of queued
    // body bytes. Emitting when blocked is a caller bug.
    void submitTrailers(std::uint32_t streamId, std::string_view headerBlock);
    void submitReset(std::uint32_t streamId, std::uint32_t errorCode);

    // After WINDOW_UPDATE/SETTINGS opened windows, the owner calls this so blocked
    // streams get another chance; the owner then re-submits their deferred sources.
    // Returns the streams that just unblocked (owner resumes their handlers).
    [[nodiscard]] std::span<const std::uint32_t> takeUnblockedStreams() noexcept;

    // --- lifecycle / timeout ---------------------------------------------------
    // (Inactivity-timeout phase selection is the I/O layer's job; it keys off
    // headerBlockInProgress() -- see the web session's scanner-phase mapping.)
    [[nodiscard]] bool closing() const noexcept { return closing_; }
    void beginGoaway(std::uint32_t errorCode);

    // Graceful drain (server shutdown): advertise GOAWAY(NO_ERROR) at the current last
    // stream id and keep serving streams already accepted; HEADERS for a stream above
    // the advertised id are refused (RST_STREAM(REFUSED_STREAM)). Idempotent.
    void beginDrain();
    [[nodiscard]] bool draining() const noexcept { return draining_; }

    // h2c upgrade (RFC 7540 §3.2): apply the HTTP2-Settings payload as the peer's
    // initial SETTINGS, seed stream 1 from the parsed upgraded h1 request (emitting its
    // kMessageHead/kMessageEnd events), queue local SETTINGS + the upgrade SETTINGS
    // ACK, and expect the client preface next. Returns false with GOAWAY bytes queued
    // (and closing() set) when the payload or upgraded request is invalid.
    [[nodiscard]] bool beginUpgraded(
        const HttpServerParseResult& parsed, std::string_view settingsPayload, std::string_view body);

    // True while a HEADERS block is still being assembled (awaiting CONTINUATION); the
    // I/O layer maps this to its tight header-read inactivity timeout.
    [[nodiscard]] bool headerBlockInProgress() const noexcept { return headerContinuation_.active(); }

    // Emit connection preface SETTINGS (call once after construction). Bytes land in
    // the outbound buffer.
    void queueLocalSettings();

    // Server mode: require the 24-byte client connection preface (RFC 9113 §3.4) before
    // the first frame. Call once after construction; feed() consumes + validates it.
    void expectClientPreface() noexcept { awaitingClientPreface_ = true; }

    // --- client role -------------------------------------------------------------
    // Emit the 24-byte client connection preface + our SETTINGS (+ the connection
    // WINDOW_UPDATE opening the receive window). Call once after construction.
    void queueClientPreface();
    // Open the next locally-initiated (odd) stream; returns its id, or 0 when the id
    // space is exhausted / the connection is closing / the stream table is full.
    // Concurrency slots (peer SETTINGS_MAX_CONCURRENT_STREAMS) are the OWNER's policy.
    [[nodiscard]] std::uint32_t openLocalStream();
    // Encode + queue the request HEADERS block for a stream returned by
    // openLocalStream. Headers must already be lowercase + validated (owner policy);
    // endStream marks a body-less request. The body then flows via submitData.
    void submitRequestHead(
        std::uint32_t streamId,
        std::string_view method,
        std::string_view scheme,
        std::string_view authority,
        std::string_view path,
        std::span<const HttpHeaderView> headers,
        bool endStream);
    // Streaming response consumers: bank this stream's DATA receive-window credit as
    // debt instead of re-advertising per frame (call before the response arrives)...
    void deferStreamWindowRelease(std::uint32_t streamId);
    // ...and release ALL banked debt once the consumer drained the buffered bytes:
    // credits the connection window (and the stream window while the stream is still
    // open) and queues the WINDOW_UPDATEs. Safe when the stream is gone.
    void releaseStreamWindow(std::uint32_t streamId);
    // True while submitData left a window-blocked remainder queued for this stream
    // (the owner waits for the drain report before pulling its next body chunk).
    [[nodiscard]] bool hasBlockedSend(std::uint32_t streamId) const noexcept;
    [[nodiscard]] std::uint32_t peerMaxConcurrentStreams() const noexcept;
    // Peer handshake / lifecycle observability for client drivers.
    [[nodiscard]] bool receivedPeerSettings() const noexcept { return receivedFirstSettings_; }
    [[nodiscard]] bool peerGoaway() const noexcept { return peerGoaway_; }

    // Concurrent dispatch support: a request/response built from a stream holds VIEWS
    // into that stream's decoded storage, so the stream must outlive an in-flight
    // (possibly-suspended) handler. Pin the stream before spawning its handler; while
    // pinned, a peer RST_STREAM/close only marks it reset (keeping the storage, and
    // emitting kStreamClosed so the owner can drop the response) instead of freeing it.
    // Unpin when the handler finishes; the stream is then removed. No-op / safe if the
    // stream is already gone.
    void pinStream(std::uint32_t streamId);
    void unpinStream(std::uint32_t streamId);

private:
    // Outbound frame emission: encode a 9-byte header + payload into outBuffer_.
    // Replaces the coroutine writeFramePayload; the encoders are pure.
    void appendFrame(
        Http2FrameType type, std::uint8_t flags, std::uint32_t streamId,
        std::string_view first, std::string_view second = {});
    void appendGoawayFrame(std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug);
    void appendGoaway(Http2ErrorCode error, std::string_view debug = {});
    void appendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    // Seed stream 1 from the parsed h1 request of an h2c upgrade (RFC 7540 §3.2).
    [[nodiscard]] bool seedUpgradedStream(const HttpServerParseResult& parsed, std::string_view body);

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
    // after pulling kMessageHead). Return the classification; the caller reacts.
    [[nodiscard]] HeaderDecodeStatus decodeHeaderBlock(Http2StreamState& stream);
    // Client role: decode a RESPONSE header block (:status + regular headers into the
    // stream's header table). A 1xx interim head is validated then discarded WITHOUT
    // marking the stream decoded, so the next HEADERS block decodes as the real head;
    // the callers emit events only when headersDecoded() flipped.
    [[nodiscard]] HeaderDecodeStatus decodeResponseHeaderBlock(Http2StreamState& stream);
    // Role-aware initial-head decode dispatch (request vs response semantics).
    [[nodiscard]] HeaderDecodeStatus decodeInitialHeaderBlock(Http2StreamState& stream);
    // Role-aware idle-stream test (server: above the highest peer id; client: any even
    // id or an odd id we have not opened yet).
    [[nodiscard]] bool isIdleStreamId(std::uint32_t streamId) const noexcept;
    [[nodiscard]] HeaderDecodeStatus decodeRefusedHeaderBlock(Http2StreamState& stream);
    [[nodiscard]] HeaderDecodeStatus finishTrailerBlock(Http2StreamState& stream);
    // On a decode failure: compression error is fatal (GOAWAY, returns false); anything
    // else RST_STREAMs the stream and survives (returns true).
    [[nodiscard]] bool handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status);
    // sans-I/O replacement for admitDecodedInitialStream/queueReady: emit kMessageHead
    // (and kMessageEnd when the peer already ended the stream) for the owner to dispatch.
    void emitRequestHeaders(Http2StreamState& stream);

    [[nodiscard]] Http2StreamState* findStream(std::uint32_t streamId) noexcept;
    [[nodiscard]] Http2StreamState* createStream(std::uint32_t streamId);
    [[nodiscard]] bool isPinned(std::uint32_t streamId) const noexcept;

    // Close a stream: drop it from the ready queue, mark closed, emit kStreamClosed
    // (so the owner cancels any handler), remove it, and remember it as closed.
    void closeStream(std::uint32_t streamId, Http2StreamCloseSource source);
    // Return a stream's banked receive-window debt to the connection window on removal.
    void flushWindowDebt(Http2StreamState& stream);

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
    std::pmr::vector<std::uint32_t> takenUnblockedStreams_;  // takeUnblockedStreams double buffer

    // streams with an in-flight handler; closeStream keeps these alive (see pinStream)
    std::pmr::vector<std::uint32_t> pinnedStreams_;

    std::uint32_t localMaxFrameSize_{kHttp2DefaultMaxFrameSize};
    std::uint32_t lastStreamId_{0};
    bool draining_{false};
    std::uint32_t goawayLastStreamId_{0};
    Http2Role role_{Http2Role::kServer};
    std::uint32_t nextLocalStreamId_{1};  // client role: next odd stream id to open
    bool peerGoaway_{false};
    std::int32_t connectionSendWindow_{kHttp2DefaultInitialWindowSize};
    std::int32_t connectionReceiveWindow_{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
    bool receivedFirstSettings_{false};
    bool awaitingClientPreface_{false};
    bool closing_{false};
};

}  // namespace ruvia::detail
