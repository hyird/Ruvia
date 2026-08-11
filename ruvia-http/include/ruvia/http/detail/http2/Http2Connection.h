#pragma once

#include "ruvia/http/HttpHeader.h"

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
// pressure has explicit ownership: submitData either accepts the full input (possibly
// copying a deferred suffix) or accepts none until the prior queued input drains.
// Final response Content-Length uses the same ownership boundary: accepted bytes are
// counted once for the whole input, committed bytes advance only when DATA is framed,
// and a mismatch is rejected before output/window/state mutation.
//
// All protocol primitives it builds on (frame codec, HPACK, stream state, flow
// control, input buffer, settings) are already pure and reused as-is. Generic frame
// progression, the connection-global header-block lifecycle, and local submission
// live in separate implementation units; Http2OutputBuffer is the sole owner of
// outbound storage and its consumed cursor.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/detail/http2/stream/Http2ClosedStreams.h"
#include "ruvia/http/detail/http2/Http2Event.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderContinuation.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderDecode.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/settings/Http2LocalSettings.h"
#include "ruvia/http/detail/http2/settings/Http2LocalConnectionState.h"
#include "ruvia/http/detail/http2/frame/Http2OutputBuffer.h"
#include "ruvia/http/detail/http2/settings/Http2PeerSettings.h"
#include "ruvia/http/detail/http2/flow/Http2ReceiveWindowCredit.h"
#include "ruvia/http/detail/http2/flow/Http2ReadyQueue.h"
#include "ruvia/http/detail/http2/message/Http2RequestContent.h"
#include "ruvia/http/detail/http2/Http2Role.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/http2/stream/Http2StreamTable.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// feed() has all-or-nothing ownership for each supplied span; it never partially
// consumes caller input. The result itself therefore carries the complete state
// instead of pairing a status with a redundant byte count.
enum class Http2FeedResult : std::uint8_t {
    // The exact span remains caller-owned and retryable after beginConnection().
    kConnectionNotStarted,
    // The exact span remains caller-owned and retryable after nextEvent() drains.
    kEventsPending,
    // The whole span was accepted and no partial preface/frame remains buffered.
    kAccepted,
    // The whole span was accepted; a partial preface/frame awaits another span.
    kNeedInput,
    // The connection is terminal. The current span must be dropped, never retried;
    // GOAWAY/other final bytes can still be present in pendingOutput().
    kProtocolFailure,
};

enum class Http2EndStream : std::uint8_t { kKeepOpen, kEndStream };

[[nodiscard]] constexpr bool http2EndsStream(Http2EndStream value) noexcept {
    return value == Http2EndStream::kEndStream;
}

// Initial-head/control submission status. kClosed is an expected race with a
// reset peer; kInvalidState is a caller contract violation and emits no bytes.
enum class Http2SubmitStatus : std::uint8_t {
    kAccepted,
    kClosed,
    kInvalidState,
    // The stream phase is valid, but the submitted HTTP message metadata cannot
    // be serialized as a conformant message (currently an invalid content-length).
    kInvalidMessage,
    // Extended CONNECT was requested before the peer advertised RFC 8441 support.
    kPeerCapabilityUnavailable
};

enum class Http2WebSocketHandshakeSubmitError : std::uint8_t {
    kClosed,
    kInvalidState,
};

class Http2WebSocketHandshakeSubmitResult;

class Http2WebSocketHandshakeSubmitFailure final {
public:
    [[nodiscard]] constexpr Http2WebSocketHandshakeSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2WebSocketHandshakeSubmitResult;

    explicit constexpr Http2WebSocketHandshakeSubmitFailure(Http2WebSocketHandshakeSubmitError error) noexcept
        : error_(error) {}

    Http2WebSocketHandshakeSubmitError error_;
};

// The successful alternative directly owns the exact negotiation encoded in
// the 200 response. The runtime must configure WsConnection from that committed
// value, never from a separately recomputed compression/subprotocol tuple.
class Http2WebSocketHandshakeSubmitResult final {
public:
    Http2WebSocketHandshakeSubmitResult(const Http2WebSocketHandshakeSubmitResult&) = delete;
    Http2WebSocketHandshakeSubmitResult& operator=(const Http2WebSocketHandshakeSubmitResult&) = delete;
    Http2WebSocketHandshakeSubmitResult(Http2WebSocketHandshakeSubmitResult&&) noexcept = default;
    Http2WebSocketHandshakeSubmitResult& operator=(Http2WebSocketHandshakeSubmitResult&&) = delete;

    [[nodiscard]] const WebSocketServerNegotiation* submitted() const& noexcept {
        return std::get_if<WebSocketServerNegotiation>(&value_);
    }
    [[nodiscard]] const WebSocketServerNegotiation* submitted() const&& = delete;

    [[nodiscard]] const Http2WebSocketHandshakeSubmitFailure* failure() const& noexcept {
        return std::get_if<Http2WebSocketHandshakeSubmitFailure>(&value_);
    }
    [[nodiscard]] const Http2WebSocketHandshakeSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;

    using Value = std::variant<WebSocketServerNegotiation, Http2WebSocketHandshakeSubmitFailure>;

    template <typename Alternative>
    explicit Http2WebSocketHandshakeSubmitResult(Alternative&& alternative) noexcept
        : value_(std::forward<Alternative>(alternative)) {}

    [[nodiscard]] static Http2WebSocketHandshakeSubmitResult makeSubmitted(WebSocketServerNegotiation&& negotiation) noexcept {
        return Http2WebSocketHandshakeSubmitResult(std::move(negotiation));
    }

    [[nodiscard]] static Http2WebSocketHandshakeSubmitResult makeFailure(Http2WebSocketHandshakeSubmitError error) noexcept {
        return Http2WebSocketHandshakeSubmitResult(Http2WebSocketHandshakeSubmitFailure(error));
    }

    Value value_;
};

// Opening a client request is one transaction: semantic validation and peer/local
// capacity checks happen before the core allocates a stream ID or emits HPACK bytes.
// Success is not an enum member because only success owns a real request stream.
enum class Http2RequestHeadSubmitError : std::uint8_t {
    kInvalidState,            // this connection is not in client role
    kConnectionNotStarted,    // beginConnection() has not queued the client preface
    kConnectionUnavailable,   // connection error, peer GOAWAY, or stream-ID exhaustion
    kPeerStreamLimitReached,  // peer SETTINGS_MAX_CONCURRENT_STREAMS is exhausted
    kLocalStreamCapacityReached,
    kPeerCapabilityUnavailable,  // Extended CONNECT was not advertised by the peer
    kInvalidMessage
};

class Http2RequestHeadSubmitResult;

// A successfully submitted HEADERS transaction. The stream ID exists only in this
// alternative; failure can never expose connection-control stream zero as a sentinel.
class Http2SubmittedRequestHead final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2RequestHeadSubmitResult;

    explicit constexpr Http2SubmittedRequestHead(std::uint32_t streamId) noexcept
        : streamId_(streamId) {
        if (streamId_ == 0 || streamId_ > 0x7fffffffU || (streamId_ & 1U) == 0) {
            std::terminate();
        }
    }

    std::uint32_t streamId_;
};

class Http2RequestHeadSubmitFailure final {
public:
    [[nodiscard]] constexpr Http2RequestHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2RequestHeadSubmitResult;

    explicit constexpr Http2RequestHeadSubmitFailure(Http2RequestHeadSubmitError error) noexcept
        : error_(error) {}

    Http2RequestHeadSubmitError error_;
};

// Exactly one alternative is observable: submitted() owns a nonzero request stream
// ID, while failure() owns the refusal reason. There is no accepted status paired
// with a default stream ID and no top-level streamId() accessor.
class Http2RequestHeadSubmitResult final {
public:
    [[nodiscard]] constexpr const Http2SubmittedRequestHead* submitted() const& noexcept {
        return std::get_if<Http2SubmittedRequestHead>(&value_);
    }
    [[nodiscard]] constexpr const Http2SubmittedRequestHead* submitted() const&& = delete;

    [[nodiscard]] constexpr const Http2RequestHeadSubmitFailure* failure() const& noexcept {
        return std::get_if<Http2RequestHeadSubmitFailure>(&value_);
    }
    [[nodiscard]] constexpr const Http2RequestHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;

    using Value = std::variant<Http2SubmittedRequestHead, Http2RequestHeadSubmitFailure>;

    template <typename Alternative>
    explicit constexpr Http2RequestHeadSubmitResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeSubmitted(std::uint32_t streamId) noexcept {
        return Http2RequestHeadSubmitResult(Http2SubmittedRequestHead(streamId));
    }

    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeFailure(Http2RequestHeadSubmitError error) noexcept {
        return Http2RequestHeadSubmitResult(Http2RequestHeadSubmitFailure(error));
    }

    Value value_;
};

// DATA ownership is explicit:
// - kAccepted: the whole input was accepted and no deferred remainder exists.
// - kQueued: the whole input was accepted; the core copied the unsent suffix and
//   will drain it automatically. The caller MUST NOT submit that input again.
// - kBackpressured: an older queued submission still owns the stream; this call
//   accepted zero bytes, so the caller retains and retries this input after drain.
enum class Http2DataSubmitStatus : std::uint8_t {
    kAccepted,
    kQueued,
    kBackpressured,
    // Expect: 100-continue still gates this request body. The caller retains
    // the complete input and retries after a Continue signal or explicit release.
    kExpectationPending,
    // The stream disappeared or was reset; the caller drops the input.
    kClosed,
    // The stream exists but its local message is not in the body-open phase.
    kInvalidState,
    // The whole input is rejected before any frame/window/counter mutation.
    kContentLengthExceeded,
    // END_STREAM was requested before the declared content length would be met.
    kContentLengthIncomplete
};

enum class Http2RequestContentReleaseStatus : std::uint8_t {
    kReleased,
    kNotPending,
    kClosed,
};

enum class Http2FinishSubmitStatus : std::uint8_t {
    kAccepted,
    kQueued,
    // The stream disappeared or was reset while its owner was finishing it.
    kClosed,
    // The stream is not in an open response body/trailers phase, or a
    // trailers-only response has no submitted terminal section.
    kInvalidState,
    // The response remains body-open so the caller can submit the missing bytes.
    kContentLengthIncomplete
};

// A final response HEADERS transaction either commits one body/stream plan or
// rejects the submission without exposing a plan that was never committed.
// The refusal reason is a plain enum, matching the request-head submission
// contract; protocol layers never manufacture exception objects.
enum class Http2ResponseHeadSubmitError : std::uint8_t {
    kClosed,
    kInvalidState,
    kResponsePlanMismatch,
    kInvalidMessage,
};

class Http2ResponseHeadSubmitFailure final {
public:
    [[nodiscard]] constexpr bool peerClosed() const noexcept {
        return error_ == Http2ResponseHeadSubmitError::kClosed;
    }

    [[nodiscard]] constexpr Http2ResponseHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    template <typename>
    friend class Http2ResponseHeadSubmitResult;

    explicit constexpr Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError error) noexcept
        : error_(error) {}

    Http2ResponseHeadSubmitError error_;
};

[[nodiscard]] inline std::string_view http2ResponseHeadSubmitErrorMessage(Http2ResponseHeadSubmitError error) noexcept {
    switch (error) {
        case Http2ResponseHeadSubmitError::kClosed:
            return "HTTP/2 response stream is closed";
        case Http2ResponseHeadSubmitError::kInvalidState:
            return "invalid HTTP/2 response head submission state";
        case Http2ResponseHeadSubmitError::kResponsePlanMismatch:
            return "HTTP/2 response head does not match its write plan";
        case Http2ResponseHeadSubmitError::kInvalidMessage:
            return "invalid HTTP/2 response head message";
    }
    return "unknown HTTP/2 response head submission failure";
}

// The successful alternative directly owns the plan that now governs
// DATA/END_STREAM. A failure owns only its refusal reason, so callers cannot
// observe body metadata from a rejected transaction or forget a parallel status.
template <typename Plan>
class Http2ResponseHeadSubmitResult final {
public:
    [[nodiscard]] const Plan* submitted() const& noexcept {
        return std::get_if<Plan>(&value_);
    }
    [[nodiscard]] const Plan* submitted() const&& = delete;

    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const& noexcept {
        return std::get_if<Http2ResponseHeadSubmitFailure>(&value_);
    }
    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;

    using Value = std::variant<Plan, Http2ResponseHeadSubmitFailure>;

    template <typename Alternative>
    explicit Http2ResponseHeadSubmitResult(Alternative alternative)
        : value_(std::move(alternative)) {}

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeSubmitted(Plan plan) {
        return Http2ResponseHeadSubmitResult(std::move(plan));
    }

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeClosedFailure() {
        return Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError::kClosed));
    }

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeInvalidStateFailure() {
        return Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError::kInvalidState));
    }

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeResponsePlanMismatchFailure() {
        return Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError::kResponsePlanMismatch));
    }

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeInvalidMessageFailure() {
        return Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError::kInvalidMessage));
    }

    Value value_;
};

using Http2BufferedResponseHeadSubmitResult = Http2ResponseHeadSubmitResult<HttpBufferedResponseWritePlan>;
using Http2StreamingResponseHeadSubmitResult = Http2ResponseHeadSubmitResult<ResponseStreamCommitPlan>;

// A response body the send window could not fully drain: the core keeps the unsent
// remainder and flushes it as WINDOW_UPDATE/SETTINGS reopen the window (nghttp2-style
// deferred data). At most one queued submission exists per stream.
struct Http2PendingSend final {
    std::uint32_t streamId{0};
    std::pmr::string bytes;
    std::size_t offset{0};
    Http2EndStream endStream{Http2EndStream::kKeepOpen};
    // A terminal trailer HEADERS block queued atomically by finishResponse behind
    // flow-control-deferred DATA. It goes out AFTER that DATA drains (RFC 9113
    // §8.1), carrying END_STREAM in place of the body.
    std::pmr::string trailerBlock;
};

class Http2Connection final {
    // One role-aware receive phase owns connection startup. Keeping this as one enum
    // prevents combinations such as "started but not awaiting magic or SETTINGS".
    enum class PrefacePhase : std::uint8_t { kNotStarted, kAwaitingClientMagic, kAwaitingPeerSettings, kReady };

public:
    explicit Http2Connection(std::pmr::memory_resource* resource, Http2Role role = Http2Role::kServer);

    [[nodiscard]] Http2Role role() const noexcept {
        return role_;
    }

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer; advances the protocol. Before
    // beginConnection(), kConnectionNotStarted retains the exact input for retry.
    // Calling while events remain returns kEventsPending with the same retained-input
    // guarantee; drain nextEvent() first. kAccepted and kNeedInput both accept the
    // whole input, with the latter buffering a partial preface/frame. kProtocolFailure
    // is terminal, so that input must be dropped. Every accepted call can emit events,
    // which must be drained before the next input is offered.
    [[nodiscard]] Http2FeedResult feed(std::string_view in);
    template <HttpTemporaryOwningCharString Input>
    Http2FeedResult feed(Input&&) = delete;
    // Pull the next protocol event. nullopt means the queue is drained; every
    // materialized event contains exactly one typed payload.
    [[nodiscard]] std::optional<Http2Event> nextEvent();
    // Facades that must perform fallible event materialization use peek/commit so
    // an exception never consumes the only protocol notification for a stream.
    [[nodiscard]] Http2Event* peekEvent() & noexcept;
    [[nodiscard]] Http2Event* peekEvent() && = delete;
    void consumeEvent() noexcept;

    // Access an assembled request head / stream for the owner to build an HttpRequest.
    [[nodiscard]] Http2StreamState* stream(std::uint32_t streamId) & noexcept;
    [[nodiscard]] Http2StreamState* stream(std::uint32_t) && = delete;

    // --- outbound --------------------------------------------------------------
    // Bytes the core wants written to the peer (frame headers + payloads, batched).
    [[nodiscard]] std::string_view pendingOutput() const& noexcept;
    [[nodiscard]] std::string_view pendingOutput() const&& = delete;
    // Acknowledges at most the current pending size. An out-of-range count is
    // rejected without clearing bytes or advancing the cursor.
    Http2OutputConsumeStatus consumeOutput(std::size_t bytes) noexcept;
    // Move ALL pending outbound bytes into `into` (allocator-matching swap when nothing
    // was partially consumed, so the common path is copy-free) and reset the buffer.
    // REQUIRED for any writer that awaits mid-write: a pendingOutput() view dangles if
    // a concurrent submit reallocates the buffer during the write.
    void takeOutput(std::pmr::string& into);
    [[nodiscard]] bool wantsWrite() const noexcept {
        return output_.wantsWrite();
    }

    // Submit a final response for `streamId`. The caller must pass the write plan
    // prepared after its last body/header transformation; method provenance,
    // status, and representation length are checked against the live stream and
    // response. A stale/mismatched plan is rejected distinctly before HPACK or
    // stream mutation. Informational status codes and HTTP/2-unrepresentable
    // control semantics (notably 426, whose mandatory Upgrade field is forbidden
    // here) are likewise rejected transactionally. An exclusive
    // Http2ResponseHeadPlan owns canonical, explicit, absent, or forbidden
    // Content-Length metadata before the encoder and local DATA state advance.
    [[nodiscard]] Http2BufferedResponseHeadSubmitResult submitResponseHead(std::uint32_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan writePlan);
    // Submit a STREAMING response head: no Content-Length is generated automatically;
    // an explicit value is strictly parsed once and the same plan binds both HPACK
    // metadata and all later DATA. With no explicit value the body is unbounded.
    // The stream stays open for subsequent submitData
    // chunks unless the method/status suppresses a body. A declared trailer section
    // keeps an HTTP/2 content-forbidden response open in a trailers-only phase; without
    // one, END_STREAM is carried by the initial HEADERS. The owner then streams DATA
    // (when allowed) and terminates through finishResponse(streamId, trailers).
    [[nodiscard]] Http2StreamingResponseHeadSubmitResult submitStreamingResponseHead(std::uint32_t streamId, HttpResponse head, ResponseStreamKind kind, ResponseTrailerIntent trailerIntent);
    [[nodiscard]] Http2DataSubmitStatus submitData(std::uint32_t streamId, std::string_view chunk, Http2EndStream endStream);
    // Submit a typed interim 1xx head. HttpInterimResponseHead excludes 101 and
    // cannot carry content; the initial-head phase remains open for the required
    // final response. Invalid HTTP/2 fields reject transactionally.
    [[nodiscard]] Http2SubmitStatus submitInterimResponseHead(std::uint32_t streamId, const HttpInterimResponseHead& response);
    // Queue the RFC 8441 successful response (:status 200, Date and the exact
    // negotiated fields, without END_STREAM) and open the stream as a tunnel.
    // Only the submitted alternative exposes the negotiation committed on wire.
    // Ownership moves into that alternative only after validation succeeds; a
    // rejected submission leaves the caller's negotiation unchanged.
    [[nodiscard]] Http2WebSocketHandshakeSubmitResult submitWebSocketHandshake(std::uint32_t streamId, WebSocketServerNegotiation&& negotiation);
    // Accept a pending standard or extended CONNECT with a successful final response.
    // The head must be bodyless and contain neither Content-Length nor
    // Transfer-Encoding. DATA becomes opaque tunnel bytes only after this succeeds.
    [[nodiscard]] Http2SubmitStatus submitConnectResponseHead(std::uint32_t streamId, const HttpResponse& response);
    // Finish a response exactly once with its complete terminal trailer section
    // (possibly empty). Its typed value proves shared validation already completed;
    // HPACK encoding, DATA/trailer ordering, and END_STREAM are one transaction.
    // An incomplete declared Content-Length is rejected without changing the
    // body-open phase. A flow-control-blocked body keeps the
    // terminal marker queued behind it once the full length is core-owned.
    [[nodiscard]] Http2FinishSubmitStatus finishResponse(std::uint32_t streamId, const HttpResponseTrailerSection& trailers);
    [[nodiscard]] Http2SubmitStatus submitReset(std::uint32_t streamId, Http2ErrorCode error);

    // Returns streams whose core-owned DATA remainder just fully drained after a
    // WINDOW_UPDATE/SETTINGS change. Their owner may now submit the next source chunk.
    [[nodiscard]] std::span<const std::uint32_t> takeDrainedDataStreams() & noexcept;
    [[nodiscard]] std::span<const std::uint32_t> takeDrainedDataStreams() && = delete;

    // --- lifecycle / timeout ---------------------------------------------------
    // A local connection error is terminal: its GOAWAY has been queued and the I/O
    // owner must close the transport after flushing it. Graceful local/peer GOAWAY is
    // deliberately absent from this value; those connections keep established streams
    // alive while draining.
    [[nodiscard]] std::optional<Http2ErrorCode> connectionError() const noexcept {
        const auto* failure = localConnectionState_.fatalFailure();
        return failure != nullptr ? std::optional<Http2ErrorCode>(failure->error()) : std::nullopt;
    }

    // Graceful local drain: advertise GOAWAY(NO_ERROR) at the current last peer stream
    // id and keep serving streams already accepted; HEADERS for a stream above the
    // advertised id are refused (RST_STREAM(REFUSED_STREAM)). Idempotent. Receiving a
    // valid peer GOAWAY enters this state through the same path so shutdown is bilateral.
    void beginDrain();
    [[nodiscard]] bool draining() const noexcept {
        return localConnectionState_.gracefulDrain() != nullptr;
    }

    // True while a HEADERS block is still being assembled (awaiting CONTINUATION); the
    // I/O layer maps this to its tight header-read inactivity timeout.
    [[nodiscard]] bool headerBlockInProgress() const noexcept {
        return headerContinuation_.active();
    }

    // Begin the role-specific HTTP/2 connection preface exactly once. Client role
    // queues the 24-byte magic plus SETTINGS; server role queues SETTINGS and makes
    // feed() require the peer magic before its first frame. In both roles the first
    // peer frame must then be a non-ACK SETTINGS frame. Both append the matching
    // connection WINDOW_UPDATE. Repeated calls are idempotent.
    void beginConnection();

    // --- client role -------------------------------------------------------------
    // Validate, open the next odd stream, and queue a regular request HEADERS block as
    // one transaction. Failure consumes neither a stream ID nor a peer concurrency
    // slot. `content` is the sole Content-Length/END_STREAM contract; later content
    // flows via submitData(result.submitted()->streamId(), ...).
    // CONNECT has different pseudo-header and tunnel semantics and is deliberately
    // rejected here; it uses the dedicated CONNECT submission path. `scheme`
    // accepts the complete RFC 3986 grammar, including non-HTTP schemes. `path`
    // is origin-form, except that only OPTIONS can use asterisk-form. `authority`
    // is absent exactly when the target URI has no authority information. HTTP(S)
    // origin-form requires Host-compatible authority without userinfo; other
    // schemes use the complete RFC 3986 authority grammar and may carry an empty
    // path. Asterisk-form OPTIONS itself has no authority component, but a direct
    // HTTP/2 sender may still carry request authority in :authority.
    [[nodiscard]] Http2RequestHeadSubmitResult submitRegularRequestHead(std::string_view method, std::string_view scheme, std::optional<std::string_view> authority, std::string_view path, std::span<const HttpHeaderView> headers, Http2RequestContent content, HttpClientRequestExpectation expectation = HttpClientRequestExpectation::kNone);
    // The submitted head is queued for later HPACK encoding, so every view must
    // outlive the call; a temporary owning string would be destroyed while the
    // borrowed head is still pending. Owning temporaries are rejected at
    // compile time, one deleted overload per view parameter.
    template <detail::HttpTemporaryOwningCharString Method>
    Http2RequestHeadSubmitResult submitRegularRequestHead(Method&&, std::string_view, std::optional<std::string_view>, std::string_view, std::span<const HttpHeaderView>, Http2RequestContent, HttpClientRequestExpectation = HttpClientRequestExpectation::kNone) = delete;
    template <detail::HttpTemporaryOwningCharString Scheme>
    Http2RequestHeadSubmitResult submitRegularRequestHead(std::string_view, Scheme&&, std::optional<std::string_view>, std::string_view, std::span<const HttpHeaderView>, Http2RequestContent, HttpClientRequestExpectation = HttpClientRequestExpectation::kNone) = delete;
    template <typename Authority>
        requires detail::HttpTemporaryOwningCharString<Authority>
    Http2RequestHeadSubmitResult submitRegularRequestHead(std::string_view, std::string_view, std::optional<Authority>&&, std::string_view, std::span<const HttpHeaderView>, Http2RequestContent, HttpClientRequestExpectation = HttpClientRequestExpectation::kNone) = delete;
    template <detail::HttpTemporaryOwningCharString Path>
    Http2RequestHeadSubmitResult submitRegularRequestHead(std::string_view, std::string_view, std::optional<std::string_view>, Path&&, std::span<const HttpHeaderView>, Http2RequestContent, HttpClientRequestExpectation = HttpClientRequestExpectation::kNone) = delete;
    // Standard CONNECT uses only :method and an authority-form :authority. Its
    // initial HEADERS never ends the stream; DATA is gated until a final 2xx response.
    [[nodiscard]] Http2RequestHeadSubmitResult submitConnectRequestHead(std::string_view authority, std::span<const HttpHeaderView> headers = {});
    template <detail::HttpTemporaryOwningCharString Authority>
    Http2RequestHeadSubmitResult submitConnectRequestHead(Authority&&, std::span<const HttpHeaderView> = {}) = delete;
    // RFC 8441 Extended CONNECT. The peer must first advertise
    // SETTINGS_ENABLE_CONNECT_PROTOCOL=1. :protocol is a protocol-name token and the
    // ordinary target pseudo-headers are generated by the core. Generic protocols
    // accept any RFC 3986 scheme and allow its path to be empty; otherwise the path
    // is origin-form. WebSocket requires an HTTP(S) scheme. WebSocket protocol names
    // are matched case-insensitively and encoded as `websocket`.
    [[nodiscard]] Http2RequestHeadSubmitResult submitExtendedConnectRequestHead(std::string_view protocol, std::string_view scheme, std::string_view authority, std::string_view path, std::span<const HttpHeaderView> headers = {});
    template <detail::HttpTemporaryOwningCharString Protocol>
    Http2RequestHeadSubmitResult submitExtendedConnectRequestHead(Protocol&&, std::string_view, std::string_view, std::string_view, std::span<const HttpHeaderView> = {}) = delete;
    template <detail::HttpTemporaryOwningCharString Scheme>
    Http2RequestHeadSubmitResult submitExtendedConnectRequestHead(std::string_view, Scheme&&, std::string_view, std::string_view, std::span<const HttpHeaderView> = {}) = delete;
    template <detail::HttpTemporaryOwningCharString Authority>
    Http2RequestHeadSubmitResult submitExtendedConnectRequestHead(std::string_view, std::string_view, Authority&&, std::string_view, std::span<const HttpHeaderView> = {}) = delete;
    template <detail::HttpTemporaryOwningCharString Path>
    Http2RequestHeadSubmitResult submitExtendedConnectRequestHead(std::string_view, std::string_view, std::string_view, Path&&, std::span<const HttpHeaderView> = {}) = delete;
    // A DATA event borrows bytes from the accepted input and retains the matching
    // receive-window debt. Once the owner has copied/consumed every currently
    // delivered DATA event for this stream, transfer the debt into batched
    // connection/stream credit. WINDOW_UPDATE is emitted only at the shared
    // half-window threshold. Safe when the stream is gone.
    [[nodiscard]] bool releaseReceivedData(std::uint32_t streamId, std::uint32_t bytes);
    void releaseAllReceivedData(std::uint32_t streamId);
    // Runtime timeout/manual release for an Expect-gated request body. A later
    // 100 response remains observable but emits no duplicate Continue signal.
    [[nodiscard]] Http2RequestContentReleaseStatus releaseRequestContent(std::uint32_t streamId) noexcept;
    // True while submitData left a window-blocked remainder queued for this stream
    // (the owner waits for the drain report before pulling its next body chunk).
    [[nodiscard]] bool hasQueuedData(std::uint32_t streamId) const noexcept;
    // RFC 8441 capability only. A dedicated CONNECT/tunnel submission API must still
    // own pseudo-header shape and tunnel lifecycle; regular requests never infer it.
    [[nodiscard]] bool peerExtendedConnectEnabled() const noexcept {
        return role_ == Http2Role::kClient && peerSettings_.enableConnectProtocol();
    }
    // Peer handshake / lifecycle observability for client drivers.
    [[nodiscard]] bool receivedPeerSettings() const noexcept {
        return prefacePhase_ == PrefacePhase::kReady;
    }
    [[nodiscard]] std::optional<Http2PeerGoaway> peerGoaway() const noexcept {
        return peerGoaway_;
    }

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
    void appendGoaway(Http2ErrorCode error, std::string_view debug = {});

    // A WINDOW_UPDATE/SETTINGS change drains core-owned DATA remainders into the
    // outbound buffer. A stream whose remainder fully drains is reported through
    // drainedDataStreams_ so the owner can pull its next source chunk.
    void markSendWindowOpened();

    // Emit a response header block as HEADERS + CONTINUATION frames (atomic sequence,
    // RFC 9113 §6.10) into the outbound buffer, ending the stream when endStream is set.
    void appendResponseHeaderFrames(Http2StreamState& stream, std::string_view headerBlock, Http2EndStream endStream);
    // Emit DATA frames for data.substr(offset) while the send window allows, returning
    // the new offset (== data.size() when fully sent). Consumes send-window credit.
    [[nodiscard]] std::size_t sendDataUpToWindow(Http2StreamState& stream, std::string_view data, std::size_t offset, Http2EndStream endStream);

    // Consume complete frames from `buffer` starting at `offset` (advanced past each
    // consumed frame; a trailing partial frame is left for the caller). Returns false on
    // a fatal protocol error (GOAWAY queued). Shared by feed()'s fast (parse over the
    // caller's bytes) and slow (parse the buffered input_) paths.
    [[nodiscard]] bool consumeFrames(std::string_view buffer, std::size_t& offset);
    // Synchronous per-frame dispatch (ported 1:1 from processFrame/*; returns false
    // on a fatal protocol error, having appended GOAWAY and transitioned the
    // local connection state to fatal failure).
    [[nodiscard]] bool processFrame(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processSettings(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processPing(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processWindowUpdate(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processRstStream(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processPriority(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processGoaway(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processHeaders(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processTrailerHeaders(Http2StreamState& stream, const Http2FrameHeader& header, std::string_view fragment);
    [[nodiscard]] bool processContinuation(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool processData(const Http2FrameHeader& header, std::string_view payload);
    // Release a successfully debited DATA payload that protocol semantics discard.
    // Only connection credit survives because the stream is closed/being abandoned.
    void releaseDroppedDataConnectionWindow(std::int32_t flowBytes);
    void queueConsumedDataCredit(Http2StreamState* stream, std::uint32_t bytes);
    [[nodiscard]] bool applySettingsPayload(std::string_view payload);

    // HPACK header-block decode (all pure; ported 1:1 from the coroutine session but
    // WITHOUT resolveStreamRoute -- route resolution is application policy the owner runs
    // after pulling kMessageHead). Return the classification; the caller reacts.
    [[nodiscard]] HeaderDecodeStatus decodeHeaderBlock(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction& streamTransaction, HpackDecoder::DecodeTransaction& hpackTransaction);
    // Client role: decode a RESPONSE header block (:status + regular headers into the
    // stream's header table). A 1xx interim head is validated then discarded WITHOUT
    // leaving the remote head-pending alternative active, so the next HEADERS block
    // decodes as the real head; callers emit events only after that alternative changes.
    [[nodiscard]] HeaderDecodeStatus decodeResponseHeaderBlock(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction& streamTransaction, HpackDecoder::DecodeTransaction& hpackTransaction);
    // Role-aware initial-head decode dispatch (request vs response semantics).
    [[nodiscard]] HeaderDecodeStatus decodeInitialHeaderBlock(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction& streamTransaction, HpackDecoder::DecodeTransaction& hpackTransaction);
    // Role-aware idle-stream test (server: above the highest peer id; client: any even
    // id or an odd id we have not opened yet).
    [[nodiscard]] bool isIdleStreamId(std::uint32_t streamId) const noexcept;
    [[nodiscard]] HeaderDecodeStatus decodeRefusedHeaderBlock(Http2StreamState& stream, HpackDecoder::DecodeTransaction& hpackTransaction);
    [[nodiscard]] HeaderDecodeStatus decodeDiscardedHeaderBlock(Http2StreamState& stream, HpackDecoder::DecodeTransaction& hpackTransaction);
    [[nodiscard]] HeaderDecodeStatus finishTrailerBlock(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction& streamTransaction, HpackDecoder::DecodeTransaction& hpackTransaction);
    // On a decode failure: compression error is fatal (GOAWAY, returns false); anything
    // else RST_STREAMs the stream and survives (returns true).
    [[nodiscard]] bool handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status, HpackDecoder::DecodeTransaction* hpackTransaction);
    // sans-I/O replacement for admitDecodedInitialStream/queueReady: emit kMessageHead
    // (and kMessageEnd when the peer already ended the stream) for the owner to dispatch.
    void emitRequestHeaders(Http2StreamState& stream);

    // A stream error or a locally-sent RST does not permit skipping a HEADERS block:
    // HPACK is connection-scoped, so every fragment must be accumulated and decoded.
    // The detached scratch prevents discarded fields from mutating a live/reset stream;
    // `DiscardedHeaderAction` is applied only after END_HEADERS.
    enum class DiscardedHeaderAction : std::uint8_t { kIgnore, kResetProtocolError, kResetStreamClosed, kRefuseStream };
    [[nodiscard]] bool startDiscardedHeaderBlock(const Http2FrameHeader& header, std::string_view fragment, DiscardedHeaderAction action);
    [[nodiscard]] bool finishDiscardedHeaderBlock();
    void detachActiveHeaderBlock(Http2StreamState& stream);

    [[nodiscard]] Http2StreamState* findStream(std::uint32_t streamId) noexcept;
    [[nodiscard]] Http2StreamState* createStream(std::uint32_t streamId);
    [[nodiscard]] std::optional<Http2RequestHeadSubmitError> localRequestAdmissionError() const noexcept;
    [[nodiscard]] Http2StreamState* admitLocalRequestStream();
    void activateLocalRequestStream(Http2StreamState& stream) noexcept;
    void releaseLocalRequestStreamIfClosed(Http2StreamState& stream) noexcept;
    void releaseLocalRequestStream(Http2StreamState& stream) noexcept;
    [[nodiscard]] bool isPinned(std::uint32_t streamId) const noexcept;

    void reserveEventSlots(std::size_t count);

    // Close a stream: drop it from the ready queue, mark closed, emit kStreamClosed
    // (so the owner cancels any handler), remove it, and remember it as closed.
    enum class CloseNotification : std::uint8_t { kEmitEvent, kOwnerAlreadyKnows };
    bool closeStreamImpl(std::uint32_t streamId, Http2StreamCloseSource source, Http2ErrorCode error, CloseNotification notification);
    bool closeStream(std::uint32_t streamId, Http2StreamCloseSource source, Http2ErrorCode error);
    bool closeStreamByOwner(std::uint32_t streamId);
    [[nodiscard]] bool wasClosedByPeerReset(std::uint32_t streamId, const Http2StreamState* retainedStream) const noexcept;
    void discardDeferredStreamState(std::uint32_t streamId);
    // Preserve a removed stream's banked debt as batched connection credit.
    void flushWindowDebt(Http2StreamState& stream);
    void reserveStreamCloseEffects(Http2StreamState& stream);

    std::pmr::memory_resource* resource_;

    // Inbound byte buffer (reused across feeds; inputOffset_ = consumed cursor).
    // A slow-path feed owns the supplied span here before dispatching it. If a later
    // frame throws, retryInput_ keeps that owned batch and its committed cursor so a
    // retry of the same caller span resumes at the first uncommitted frame instead of
    // appending and replaying the already committed prefix.
    std::pmr::string input_;
    std::size_t inputOffset_{0};
    bool retryInput_{false};

    // Outbound serialization and consumed-prefix ownership are isolated from
    // connection state transitions.
    Http2OutputBuffer output_;

    // pure protocol state (all reused as-is)
    Http2StreamTable streams_;
    Http2ClosedStreamHistory closedStreams_;
    Http2ReadyQueue readyQueue_;
    HpackDecoder decoder_;
    Http2HeaderContinuation headerContinuation_;
    Http2PeerSettings peerSettings_;
    // The static-only encoder starts with HPACK's implicit 4096-byte maximum even
    // though it never inserts entries. A peer reduction below this value must still
    // be acknowledged on the wire at the beginning of the next field block.
    std::uint32_t encoderDynamicTableSize_{Http2LocalSettings::kHeaderTableSize};
    bool encoderTableSizeUpdatePending_{false};
    std::optional<Http2StreamState> discardedHeaderStream_;
    DiscardedHeaderAction discardedHeaderAction_{DiscardedHeaderAction::kIgnore};

    // event queue drained by nextEvent()
    std::pmr::vector<Http2Event> events_;
    std::size_t eventOffset_{0};

    // flow-control-deferred response bodies + streams that just fully drained
    std::pmr::vector<Http2PendingSend> pendingSends_;
    std::pmr::vector<std::uint32_t> drainedDataStreams_;
    std::pmr::vector<std::uint32_t> takenDrainedDataStreams_;  // double buffer for returned spans

    // streams with an in-flight handler; closeStream keeps these alive (see pinStream)
    std::pmr::vector<std::uint32_t> pinnedStreams_;

    std::uint32_t localMaxFrameSize_{Http2LocalSettings::kMaxFrameSize};
    std::uint32_t lastStreamId_{0};
    Http2LocalConnectionState localConnectionState_;
    Http2Role role_{Http2Role::kServer};
    std::uint32_t nextLocalStreamId_{1};  // client role: next odd stream id to open
    std::uint32_t activeLocalRequestStreams_{0};
    std::optional<Http2PeerGoaway> peerGoaway_;
    std::int32_t connectionSendWindow_{kHttp2DefaultInitialWindowSize};
    std::int32_t connectionReceiveWindow_{static_cast<std::int32_t>(Http2LocalSettings::kInitialWindowSize)};
    Http2ReceiveWindowCredit connectionReceiveCredit_;
    PrefacePhase prefacePhase_{PrefacePhase::kNotStarted};

    // Defense-in-depth flood budgets (see Http2Connection.cpp). No clock in the core, so
    // these are per-connection counters that trip GOAWAY(ENHANCE_YOUR_CALM).
    std::uint32_t peerResetStreams_{0};     // new stream aborts from peer RST_STREAM
    std::uint32_t completedResponses_{0};   // streams finished without reset (refills budget)
    std::uint32_t consecutivePings_{0};     // inbound PINGs since output was last drained
    std::uint32_t consecutiveSettings_{0};  // inbound non-ACK SETTINGS since output drained
};

}  // namespace ruvia::detail
