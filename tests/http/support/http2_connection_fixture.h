#pragma once

#include "test_harness.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/hpack/Http2HpackHuffmanTables.h"
#include "ruvia/http/detail/http2/flow/Http2ReceiveWindowCredit.h"
#include "ruvia/http/detail/http2/flow/Http2WindowUpdate.h"
#include "ruvia/http/HttpLimits.h"

namespace http2_connection_test {

using ruvia::detail::HpackDecoder;
using ruvia::detail::HpackEncoder;
using ruvia::detail::Http2BufferedResponseHeadSubmitResult;
using ruvia::detail::Http2ConnectForm;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2DataSubmitStatus;
using ruvia::detail::Http2EndStream;
using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2Event;
using ruvia::detail::Http2EventKind;
using ruvia::detail::Http2FeedResult;
using ruvia::detail::Http2FinishSubmitStatus;
using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2LocalContentKnownLength;
using ruvia::detail::Http2LocalContentState;
using ruvia::detail::Http2LocalSendState;
using ruvia::detail::Http2LocalSettings;
using ruvia::detail::Http2RequestContent;
using ruvia::detail::Http2RequestHeadSubmitError;
using ruvia::detail::Http2RequestHeadSubmitFailure;
using ruvia::detail::Http2RequestHeadSubmitResult;
using ruvia::detail::Http2ResponseHeadSubmitError;
using ruvia::detail::Http2ResponseHeadSubmitFailure;
using ruvia::detail::Http2StreamCloseSource;
using ruvia::detail::Http2StreamingResponseHeadSubmitResult;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http2SubmitStatus;
using ruvia::detail::Http2SubmittedRequestHead;
using ruvia::detail::Http2TunnelState;
using ruvia::detail::Http2WebSocketHandshakeSubmitFailure;
using ruvia::detail::Http2WebSocketHandshakeSubmitResult;
using ruvia::detail::ResponseStreamHeadDisposition;
using ruvia::detail::ResponseStreamTrailerFraming;
using ruvia::detail::ResponseTrailerIntent;

inline ruvia::detail::HttpResponseTrailerSection validatedTrailers(std::span<const ruvia::HttpHeaderView> fields) {
    const auto result = ruvia::detail::httpResponseTrailerSection(fields);
    if (result.section() == nullptr) {
        throw std::logic_error("expected valid response trailer section");
    }
    return *result.section();
}

template <typename T>
concept HasLooseHttp2EventFields = requires(T& event) {
    event.kind = Http2EventKind::kMessageHead;
    event.streamId;
    event.bytes;
    event.error;
};

template <typename T>
concept HasAnyRvalueHttp2EventBorrow = requires(T&& event) { std::move(event).messageHead(); } || requires(T&& event) { std::move(event).messageBodyChunk(); } || requires(T&& event) { std::move(event).messageEnd(); } || requires(T&& event) { std::move(event).tunnelData(); } || requires(T&& event) { std::move(event).tunnelEnd(); } || requires(T&& event) { std::move(event).streamClosed(); } || requires(T&& event) { std::move(event).requestUnprocessed(); } || requires(T&& event) { std::move(event).goaway(); } || requires(T&& event) { std::move(event).peerGoaway(); };

template <typename T>
concept ExposesRvalueHttp2ConnectionStorage = requires(T&& connection) { std::move(connection).pendingOutput(); } || requires(T&& connection) { std::move(connection).takeDrainedDataStreams(); } || requires(T&& connection) { std::move(connection).stream(std::uint32_t{}); };

static_assert(!HasAnyRvalueHttp2EventBorrow<Http2Event>);
static_assert(!HasAnyRvalueHttp2EventBorrow<ruvia::detail::Http2GoawayEvent>);
static_assert(!ExposesRvalueHttp2ConnectionStorage<Http2Connection>);

template <typename T>
concept HasHttp2EventError = requires(const T& event) {
    { event.error() } -> std::same_as<Http2ErrorCode>;
};

template <typename T>
concept HasFeedStatusField = requires(const T& result) { result.status; };

template <typename T>
concept HasFeedConsumedField = requires(const T& result) { result.consumed; };

template <typename T>
concept HasRequestHeadStatusAccessor = requires(const T& result) { result.status(); };

template <typename T>
concept HasRequestHeadAcceptedAccessor = requires(const T& result) { result.accepted(); };

template <typename T>
concept HasRequestHeadStreamIdAccessor = requires(const T& result) {
    { result.streamId() } -> std::same_as<std::uint32_t>;
};

template <typename T>
concept HasRequestHeadErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<Http2RequestHeadSubmitError>;
};

template <typename T>
concept HasResponseHeadStatusAccessor = requires(const T& result) { result.status(); };

template <typename T>
concept HasResponseHeadAcceptedAccessor = requires(const T& result) { result.accepted(); };

template <typename T>
concept HasResponseHeadPlanAccessor = requires(const T& result) { result.plan(); };

template <typename T>
concept HasResponseHeadErrorAccessor = requires(const T& result) { result.error(); };

template <typename T>
concept HasResponseHeadFailureContract = requires(const T& failure) {
    { failure.peerClosed() } -> std::same_as<bool>;
    { failure.exception() } -> std::same_as<Http2ResponseHeadSubmitError>;
};

template <typename T>
concept HasRequestContentMode = requires(const T& content) { content.mode(); };

template <typename T>
concept HasRequestContentLength = requires(const T& content) {
    { content.length() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasStaleLocalContentForwarders = requires(const T& stream) {
    stream.localContentMode();
    stream.localContentHasKnownLength();
    stream.localContentDeclaredLength();
    stream.localContentAcceptedBytes();
    stream.localContentCommittedBytes();
    stream.localContentLengthComplete();
};

template <typename T>
concept HasStaleTunnelForwarders = requires(const T& stream) {
    stream.standardConnect();
    stream.extendedConnect();
    stream.extendedConnectWebSocket();
    stream.webSocketTunnel();
    stream.connectRequest();
    stream.connectPending();
    stream.tunnelOpen();
    stream.connectRejected();
};

template <typename T>
concept HasStaleLocalSendForwarders = requires(const T& stream) {
    stream.localSendPhase();
    stream.localMessageKind();
    stream.localEndStream();
    stream.localEndStreamCommitted();
    stream.canSubmitLocalHead();
    stream.localBodyOpen();
    stream.localTrailersOnly();
};

static_assert(!HasRequestContentMode<Http2RequestContent>);
static_assert(!HasRequestContentLength<Http2RequestContent>);
static_assert(!HasRequestContentLength<ruvia::detail::Http2RequestWithoutContent>);
static_assert(HasRequestContentLength<ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!HasRequestContentLength<ruvia::detail::Http2StreamingRequestContent>);
static_assert(!std::default_initializable<Http2RequestContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2RequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!std::default_initializable<ruvia::detail::Http2StreamingRequestContent>);
static_assert(!HasStaleLocalContentForwarders<Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const Http2StreamState&>().localContent()), const Http2LocalContentState&>);
static_assert(!HasStaleTunnelForwarders<Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const Http2StreamState&>().tunnel()), const Http2TunnelState&>);
static_assert(!HasStaleLocalSendForwarders<Http2StreamState>);
static_assert(std::same_as<decltype(std::declval<const Http2StreamState&>().localSend()), const Http2LocalSendState&>);

static_assert(std::same_as<decltype(std::declval<Http2Connection&>().nextEvent()), std::optional<Http2Event>>);
static_assert(!std::is_default_constructible_v<Http2Event>);
static_assert(!HasLooseHttp2EventFields<Http2Event>);
static_assert(HasHttp2EventError<ruvia::detail::Http2StreamClosedEvent>);
static_assert(!HasHttp2EventError<ruvia::detail::Http2RequestUnprocessedEvent>);
static_assert(std::same_as<decltype(std::declval<Http2Connection&>().feed(std::string_view{})), Http2FeedResult>);
static_assert(std::is_enum_v<Http2FeedResult>);
static_assert(!HasFeedStatusField<Http2FeedResult>);
static_assert(!HasFeedConsumedField<Http2FeedResult>);
static_assert(!std::default_initializable<Http2WebSocketHandshakeSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const Http2WebSocketHandshakeSubmitResult&>().submitted()), const ruvia::detail::WebSocketServerNegotiation*>);
static_assert(std::same_as<decltype(std::declval<const Http2WebSocketHandshakeSubmitResult&>().failure()), const Http2WebSocketHandshakeSubmitFailure*>);
static_assert(!std::default_initializable<Http2RequestHeadSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const Http2RequestHeadSubmitResult&>().submitted()), const Http2SubmittedRequestHead*>);
static_assert(std::same_as<decltype(std::declval<const Http2RequestHeadSubmitResult&>().failure()), const Http2RequestHeadSubmitFailure*>);
static_assert(!HasRequestHeadStatusAccessor<Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadAcceptedAccessor<Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitResult>);
static_assert(!std::constructible_from<Http2SubmittedRequestHead, std::uint32_t>);
static_assert(HasRequestHeadStreamIdAccessor<Http2SubmittedRequestHead>);
static_assert(!HasRequestHeadErrorAccessor<Http2SubmittedRequestHead>);
static_assert(HasRequestHeadErrorAccessor<Http2RequestHeadSubmitFailure>);
static_assert(!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitFailure>);
static_assert(!std::default_initializable<Http2BufferedResponseHeadSubmitResult>);
static_assert(!std::default_initializable<Http2StreamingResponseHeadSubmitResult>);
static_assert(std::same_as<decltype(std::declval<const Http2BufferedResponseHeadSubmitResult&>().submitted()), const ruvia::detail::HttpBufferedResponseWritePlan*>);
static_assert(std::same_as<decltype(std::declval<const Http2StreamingResponseHeadSubmitResult&>().submitted()), const ruvia::detail::ResponseStreamCommitPlan*>);
static_assert(std::same_as<decltype(std::declval<const Http2BufferedResponseHeadSubmitResult&>().failure()), const Http2ResponseHeadSubmitFailure*>);
static_assert(std::same_as<decltype(std::declval<const Http2StreamingResponseHeadSubmitResult&>().failure()), const Http2ResponseHeadSubmitFailure*>);
static_assert(!HasResponseHeadStatusAccessor<Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadAcceptedAccessor<Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadPlanAccessor<Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadErrorAccessor<Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadErrorAccessor<Http2ResponseHeadSubmitFailure>);
static_assert(HasResponseHeadFailureContract<Http2ResponseHeadSubmitFailure>);
static_assert(std::derived_from<Http2ResponseHeadSubmitError, std::exception>);
static_assert(std::is_trivially_copyable_v<Http2ResponseHeadSubmitFailure>);
static_assert(sizeof(Http2ResponseHeadSubmitFailure) <= 1);
static_assert(!HasResponseHeadPlanAccessor<Http2ResponseHeadSubmitFailure>);
static_assert(!std::constructible_from<Http2ResponseHeadSubmitFailure, Http2ResponseHeadSubmitError>);

inline const Http2LocalContentKnownLength& requireLocalKnownLength(const Http2StreamState& stream) {
    if (const auto* knownLength = stream.localContent().knownLength()) {
        return *knownLength;
    }
    throw std::runtime_error("HTTP/2 local content is not known-length");
}

inline std::uint32_t submittedRequestStreamId(const Http2RequestHeadSubmitResult& result) {
    if (const auto* submitted = result.submitted()) {
        return submitted->streamId();
    }
    throw std::runtime_error("HTTP/2 request head was not submitted");
}

inline Http2RequestHeadSubmitError requestHeadSubmitError(const Http2RequestHeadSubmitResult& result) {
    if (const auto* failure = result.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 request head did not fail");
}

template <typename Result>
inline bool responseHeadSubmitted(const Result& result) {
    return result.submitted() != nullptr;
}

template <typename Result>
inline std::string_view responseHeadSubmitFailureMessage(const Result& result) {
    if (const auto* failure = result.failure()) {
        return failure->exception().what();
    }
    throw std::runtime_error("HTTP/2 response head did not fail");
}

inline Http2BufferedResponseHeadSubmitResult submitBufferedResponseHead(Http2Connection& connection, std::uint32_t streamId, const ruvia::HttpResponse& response) {
    const auto* stream = connection.stream(streamId);
    const auto requestMethod = stream == nullptr ? ruvia::HttpKnownMethod::kUnknown : stream->requestKnownMethod();
    return connection.submitResponseHead(streamId, response, ruvia::detail::httpBufferedResponseWritePlan(requestMethod, response));
}

template <typename Result>
inline const auto& submittedResponsePlan(const Result& result) {
    if (const auto* submitted = result.submitted()) {
        return *submitted;
    }
    throw std::runtime_error("HTTP/2 response head was not submitted");
}

struct RequestContentLengthObservation final {
    std::size_t count{0};
    std::size_t authorityCount{0};
    std::size_t pathCount{0};
    std::string value;
    std::string authority;
    std::string scheme;
    std::string path;
};

inline bool observeRequestContentLength(void* target, std::string_view name, std::string_view value) {
    auto& observation = *static_cast<RequestContentLengthObservation*>(target);
    if (name == "content-length") {
        ++observation.count;
        observation.value.assign(value.data(), value.size());
    } else if (name == ":scheme") {
        observation.scheme.assign(value.data(), value.size());
    } else if (name == ":authority") {
        ++observation.authorityCount;
        observation.authority.assign(value.data(), value.size());
    } else if (name == ":path") {
        ++observation.pathCount;
        observation.path.assign(value.data(), value.size());
    }
    return true;
}

// Encode a minimal valid request header block (HPACK literals) into `block`.
inline void encodeRequest(std::pmr::string& block, std::string_view method, std::string_view scheme = "https", std::string_view path = "/", std::optional<std::string_view> authority = "example.com") {
    HpackEncoder::encodeHeader(block, ":method", method);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    HpackEncoder::encodeHeader(block, ":path", path);
    if (authority.has_value()) {
        HpackEncoder::encodeHeader(block, ":authority", *authority);
    }
}

inline void encodeGetRequest(std::pmr::string& block) {
    encodeRequest(block, "GET");
}

// Frame a HEADERS block on `streamId` with the given flags into a fed-ready buffer.
inline std::pmr::string headersFrame(std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags, std::string_view block) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(hdr, static_cast<std::uint32_t>(block.size()), Http2FrameType::kHeaders, flags, streamId);
    frame.append(hdr, 9);
    frame.append(block.data(), block.size());
    return frame;
}

inline std::pmr::string continuationFrame(std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags, std::string_view fragment) {
    std::pmr::string frame(resource);
    char header[9];
    ruvia::detail::http2EncodeFrameHeader(header, static_cast<std::uint32_t>(fragment.size()), Http2FrameType::kContinuation, flags, streamId);
    frame.append(header, sizeof(header));
    frame.append(fragment.data(), fragment.size());
    return frame;
}

inline std::pmr::string goawayFrame(std::pmr::memory_resource* resource, std::uint32_t lastStreamId, Http2ErrorCode error) {
    std::pmr::string bytes(resource);
    char frame[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(frame, 8, Http2FrameType::kGoaway, 0, 0);
    ruvia::detail::http2WriteGoawayPayload(frame + 9, lastStreamId, error);
    bytes.append(frame, sizeof(frame));
    return bytes;
}

// Test-only HPACK literal with incremental indexing (short, non-Huffman strings).
// The next block on this connection can reference the inserted entry at index 62.
inline void encodeShortDynamicHeader(std::pmr::string& block, std::string_view name, std::string_view value) {
    block.push_back(static_cast<char>(0x40));
    block.push_back(static_cast<char>(name.size()));
    block.append(name.data(), name.size());
    block.push_back(static_cast<char>(value.size()));
    block.append(value.data(), value.size());
}

inline void appendHpackInteger(std::pmr::string& block, std::size_t value, std::uint8_t prefixBits, std::uint8_t firstBits) {
    const auto prefixMask = static_cast<std::uint8_t>((1U << prefixBits) - 1U);
    if (value < prefixMask) {
        block.push_back(static_cast<char>(firstBits | value));
        return;
    }

    block.push_back(static_cast<char>(firstBits | prefixMask));
    value -= prefixMask;
    while (value >= 128) {
        block.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    block.push_back(static_cast<char>(value));
}

inline void encodeRepeatedHuffmanHeader(std::pmr::string& block, std::string_view name, unsigned char value, std::size_t count) {
    appendHpackInteger(block, 0, 4, ruvia::detail::kHpackLiteralWithoutIndexing);
    appendHpackInteger(block, name.size(), 7, 0);
    block.append(name.data(), name.size());

    const auto code = ruvia::detail::kHpackHuffmanCodes[value];
    const auto bitLength = ruvia::detail::kHpackHuffmanLengths[value];
    const auto encodedBytes = (count * bitLength + 7) / 8;
    appendHpackInteger(block, encodedBytes, 7, 0x80);

    std::uint64_t pending = 0;
    std::uint8_t pendingBits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        pending = (pending << bitLength) | code;
        pendingBits = static_cast<std::uint8_t>(pendingBits + bitLength);
        while (pendingBits >= 8) {
            pendingBits = static_cast<std::uint8_t>(pendingBits - 8);
            block.push_back(static_cast<char>((pending >> pendingBits) & 0xffU));
        }
        if (pendingBits == 0) {
            pending = 0;
        } else {
            pending &= (std::uint64_t{1} << pendingBits) - 1;
        }
    }
    if (pendingBits != 0) {
        const auto paddingBits = static_cast<std::uint8_t>(8 - pendingBits);
        const auto padding = (std::uint16_t{1} << paddingBits) - 1;
        block.push_back(static_cast<char>((pending << paddingBits) | padding));
    }
}

// Start the role-specific preface and leave the connection ready to receive the
// peer's first frame. Servers must consume the client magic first.
inline void beginPeerInput(Http2Connection& conn) {
    conn.beginConnection();
    conn.consumeOutput(conn.pendingOutput().size());
    if (conn.role() == ruvia::detail::Http2Role::kServer) {
        const auto result = conn.feed(ruvia::detail::kHttp2ClientPreface);
        if (result != ruvia::detail::Http2FeedResult::kAccepted) {
            throw std::runtime_error("server rejected valid client preface");
        }
    }
}

// Feed the peer's empty non-ACK SETTINGS frame and drain the resulting ACK, leaving
// the connection ready for post-handshake frames.
inline void handshake(Http2Connection& conn) {
    beginPeerInput(conn);
    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = conn.feed(std::string_view(settings, sizeof(settings)));
    if (result != ruvia::detail::Http2FeedResult::kAccepted || !conn.receivedPeerSettings()) {
        throw std::runtime_error("connection rejected valid initial SETTINGS");
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

inline void beginClient(Http2Connection& client) {
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());
}

inline void applyPeerMaxConcurrentStreams(Http2Connection& client, std::uint32_t limit) {
    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(out, ruvia::detail::Http2SettingId::kMaxConcurrentStreams, limit);
    (void)out;
    (void)client.feed(std::string_view(settings, sizeof(settings)));
    client.consumeOutput(client.pendingOutput().size());
}

// Handshake but declare a small peer SETTINGS_INITIAL_WINDOW_SIZE so freshly created
// streams start with a tiny send window (to exercise flow-control backpressure).
inline void handshakeWithWindow(Http2Connection& conn, std::uint32_t window) {
    beginPeerInput(conn);
    char s[9 + 6];
    ruvia::detail::http2EncodeFrameHeader(s, 6, Http2FrameType::kSettings, 0, 0);
    s[9] = 0;
    s[10] = 4;  // SETTINGS_INITIAL_WINDOW_SIZE
    s[11] = static_cast<char>((window >> 24) & 0xFF);
    s[12] = static_cast<char>((window >> 16) & 0xFF);
    s[13] = static_cast<char>((window >> 8) & 0xFF);
    s[14] = static_cast<char>(window & 0xFF);
    (void)conn.feed(std::string_view(s, sizeof(s)));
    conn.consumeOutput(conn.pendingOutput().size());
}

// Feed a complete GET on stream 1, drain its events and any output, leaving stream 1
// open (half-closed remote) and ready to receive a response.
inline void driveGetRequest(Http2Connection& conn, std::pmr::memory_resource* res) {
    std::pmr::string block(res);
    encodeGetRequest(block);
    const auto h = headersFrame(res, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

inline void driveRequest(Http2Connection& conn, std::pmr::memory_resource* res, std::string_view method) {
    std::pmr::string block(res);
    encodeRequest(block, method);
    const auto h = headersFrame(res, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

// Open stream 1 and let the peer reset it. `pinned` retains the aborted stream
// object to exercise the request-view lifetime branch; the wire state is closed
// in both cases.
inline void openThenPeerReset(Http2Connection& conn, std::pmr::memory_resource* resource, bool pinned) {
    std::pmr::string block(resource);
    encodeGetRequest(block);
    const auto head = headersFrame(resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(head.data(), head.size()));
    while (conn.nextEvent().has_value()) {
    }
    if (pinned) {
        conn.pinStream(1);
    }

    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

// Open stream 1 and let this endpoint reset it. DATA that was already in flight
// before the peer observes our RST_STREAM can still arrive and must be minimally
// processed without sending another stream frame.
inline void openThenLocalReset(Http2Connection& conn, std::pmr::memory_resource* resource, bool pinned = false) {
    std::pmr::string block(resource);
    encodeGetRequest(block);
    const auto head = headersFrame(resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(head.data(), head.size()));
    while (conn.nextEvent().has_value()) {
    }
    if (pinned) {
        conn.pinStream(1);
    }
    (void)conn.submitReset(1, Http2ErrorCode::kCancel);
    conn.consumeOutput(conn.pendingOutput().size());  // flush the one legal reset
}

using ruvia::detail::Http2Role;

// Byte shuttle between two cores (no sockets): move pending output of `from` into
// `to`, draining `to`'s events into the collectors first would lose them -- so the
// caller passes a per-hop event sink invoked after every feed.
template <typename OnEvent>
inline void shuttleOnce(Http2Connection& from, Http2Connection& to, OnEvent&& onEvent) {
    while (from.wantsWrite()) {
        const auto out = from.pendingOutput();
        std::pmr::string copy(out.data(), out.size(), std::pmr::get_default_resource());
        from.consumeOutput(out.size());
        (void)to.feed(std::string_view(copy.data(), copy.size()));
        while (const auto event = to.nextEvent()) {
            onEvent(*event);
        }
    }
}

// Walk the outbound buffer frame-by-frame and return the error code of the first GOAWAY,
// or 0xffffffff if none is present.
inline std::uint32_t firstGoawayError(std::string_view out) {
    std::size_t pos = 0;
    while (pos + 9 <= out.size()) {
        const auto h = ruvia::detail::http2ParseFrameHeader(out.substr(pos, 9));
        if (h.type == static_cast<std::uint8_t>(Http2FrameType::kGoaway) && h.length >= 8) {
            const auto* p = reinterpret_cast<const unsigned char*>(out.data() + pos + 9);
            return (static_cast<std::uint32_t>(p[4]) << 24) | (static_cast<std::uint32_t>(p[5]) << 16) | (static_cast<std::uint32_t>(p[6]) << 8) | static_cast<std::uint32_t>(p[7]);
        }
        pos += 9 + h.length;
    }
    return 0xffffffffU;
}
constexpr std::uint32_t kEnhanceYourCalm = static_cast<std::uint32_t>(ruvia::detail::Http2ErrorCode::kEnhanceYourCalm);

// Build a POST request head (no END_STREAM) with optional content-length; body follows.
inline std::pmr::string postHeadFrame(std::pmr::memory_resource* resource, std::string_view contentLength) {
    std::pmr::string block(resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    if (!contentLength.empty()) {
        HpackEncoder::encodeHeader(block, "content-length", contentLength);
    }
    return headersFrame(resource, 1, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(block.data(), block.size()));
}

// Frame a DATA payload on `streamId` with the given flags.
inline std::pmr::string dataFrame(std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags, std::string_view body) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(hdr, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, flags, streamId);
    frame.append(hdr, 9);
    frame.append(body.data(), body.size());
    return frame;
}

}  // namespace http2_connection_test

using namespace http2_connection_test;  // NOLINT(google-build-using-namespace)
