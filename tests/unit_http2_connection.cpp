#include "test_harness.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"

namespace {

using ruvia::detail::Http2Connection;
using ruvia::detail::Http2ConnectForm;
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
using ruvia::detail::Http2BufferedResponseHeadSubmitResult;
using ruvia::detail::Http2ResponseHeadSubmitError;
using ruvia::detail::Http2ResponseHeadSubmitFailure;
using ruvia::detail::Http2ResponseTrailerSubmitStatus;
using ruvia::detail::Http2StreamingResponseHeadSubmitResult;
using ruvia::detail::Http2SubmittedBufferedResponseHead;
using ruvia::detail::Http2SubmittedRequestHead;
using ruvia::detail::Http2SubmittedStreamingResponseHead;
using ruvia::detail::Http2SubmitStatus;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http2TunnelState;
using ruvia::detail::ResponseStreamHeadDisposition;
using ruvia::detail::ResponseStreamTrailerFraming;
using ruvia::detail::ResponseTrailerIntent;
using ruvia::detail::HpackDecoder;
using ruvia::detail::HpackEncoder;

template <typename T>
concept HasLooseHttp2EventFields = requires(T& event) {
    event.kind = Http2EventKind::kMessageHead;
    event.streamId;
    event.bytes;
    event.error;
};

template <typename T>
concept HasHttp2EventError = requires(const T& event) {
    { event.error() } -> std::same_as<Http2ErrorCode>;
};

template <typename T>
concept HasFeedStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasFeedConsumedField = requires(const T& result) {
    result.consumed;
};

template <typename T>
concept HasRequestHeadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasRequestHeadAcceptedAccessor = requires(const T& result) {
    result.accepted();
};

template <typename T>
concept HasRequestHeadStreamIdAccessor = requires(const T& result) {
    { result.streamId() } -> std::same_as<std::uint32_t>;
};

template <typename T>
concept HasRequestHeadErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<Http2RequestHeadSubmitError>;
};

template <typename T>
concept HasResponseHeadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasResponseHeadAcceptedAccessor = requires(const T& result) {
    result.accepted();
};

template <typename T>
concept HasResponseHeadPlanAccessor = requires(const T& result) {
    result.plan();
};

template <typename T>
concept HasResponseHeadErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<Http2ResponseHeadSubmitError>;
};

template <typename T>
concept HasRequestContentMode = requires(const T& content) {
    content.mode();
};

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
static_assert(!HasRequestContentLength<
    ruvia::detail::Http2RequestWithoutContent>);
static_assert(HasRequestContentLength<
    ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!HasRequestContentLength<
    ruvia::detail::Http2StreamingRequestContent>);
static_assert(!std::default_initializable<Http2RequestContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2RequestWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2KnownLengthRequestContent>);
static_assert(!std::default_initializable<
    ruvia::detail::Http2StreamingRequestContent>);
static_assert(!HasStaleLocalContentForwarders<Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const Http2StreamState&>().localContent()),
    const Http2LocalContentState&>);
static_assert(!HasStaleTunnelForwarders<Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const Http2StreamState&>().tunnel()),
    const Http2TunnelState&>);
static_assert(!HasStaleLocalSendForwarders<Http2StreamState>);
static_assert(std::same_as<
    decltype(std::declval<const Http2StreamState&>().localSend()),
    const Http2LocalSendState&>);

static_assert(std::same_as<
    decltype(std::declval<Http2Connection&>().nextEvent()),
    std::optional<Http2Event>>);
static_assert(!std::is_default_constructible_v<Http2Event>);
static_assert(!HasLooseHttp2EventFields<Http2Event>);
static_assert(HasHttp2EventError<ruvia::detail::Http2StreamClosedEvent>);
static_assert(!HasHttp2EventError<ruvia::detail::Http2RequestUnprocessedEvent>);
static_assert(std::same_as<
    decltype(std::declval<Http2Connection&>().feed(std::string_view{})),
    Http2FeedResult>);
static_assert(std::is_enum_v<Http2FeedResult>);
static_assert(!HasFeedStatusField<Http2FeedResult>);
static_assert(!HasFeedConsumedField<Http2FeedResult>);
static_assert(!std::default_initializable<Http2RequestHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<const Http2RequestHeadSubmitResult&>().submitted()),
    const Http2SubmittedRequestHead*>);
static_assert(std::same_as<
    decltype(std::declval<const Http2RequestHeadSubmitResult&>().failure()),
    const Http2RequestHeadSubmitFailure*>);
static_assert(!HasRequestHeadStatusAccessor<Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadAcceptedAccessor<Http2RequestHeadSubmitResult>);
static_assert(!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitResult>);
static_assert(!std::constructible_from<Http2SubmittedRequestHead, std::uint32_t>);
static_assert(HasRequestHeadStreamIdAccessor<Http2SubmittedRequestHead>);
static_assert(!HasRequestHeadErrorAccessor<Http2SubmittedRequestHead>);
static_assert(HasRequestHeadErrorAccessor<Http2RequestHeadSubmitFailure>);
static_assert(!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitFailure>);
static_assert(!std::default_initializable<
    Http2BufferedResponseHeadSubmitResult>);
static_assert(!std::default_initializable<
    Http2StreamingResponseHeadSubmitResult>);
static_assert(std::same_as<
    decltype(std::declval<
        const Http2BufferedResponseHeadSubmitResult&>().submitted()),
    const Http2SubmittedBufferedResponseHead*>);
static_assert(std::same_as<
    decltype(std::declval<
        const Http2StreamingResponseHeadSubmitResult&>().submitted()),
    const Http2SubmittedStreamingResponseHead*>);
static_assert(std::same_as<
    decltype(std::declval<
        const Http2BufferedResponseHeadSubmitResult&>().failure()),
    const Http2ResponseHeadSubmitFailure*>);
static_assert(std::same_as<
    decltype(std::declval<
        const Http2StreamingResponseHeadSubmitResult&>().failure()),
    const Http2ResponseHeadSubmitFailure*>);
static_assert(!HasResponseHeadStatusAccessor<
    Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadAcceptedAccessor<
    Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadPlanAccessor<
    Http2BufferedResponseHeadSubmitResult>);
static_assert(!HasResponseHeadErrorAccessor<
    Http2BufferedResponseHeadSubmitResult>);
static_assert(HasResponseHeadPlanAccessor<
    Http2SubmittedBufferedResponseHead>);
static_assert(HasResponseHeadPlanAccessor<
    Http2SubmittedStreamingResponseHead>);
static_assert(!HasResponseHeadErrorAccessor<
    Http2SubmittedBufferedResponseHead>);
static_assert(HasResponseHeadErrorAccessor<
    Http2ResponseHeadSubmitFailure>);
static_assert(!HasResponseHeadPlanAccessor<
    Http2ResponseHeadSubmitFailure>);
static_assert(!std::constructible_from<
    Http2SubmittedBufferedResponseHead,
    ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(!std::constructible_from<
    Http2SubmittedStreamingResponseHead,
    ruvia::detail::ResponseStreamCommitPlan>);
static_assert(!std::constructible_from<
    Http2ResponseHeadSubmitFailure,
    Http2ResponseHeadSubmitError>);

const Http2LocalContentKnownLength& requireLocalKnownLength(
    const Http2StreamState& stream) {
    if (const auto* knownLength = stream.localContent().knownLength()) {
        return *knownLength;
    }
    throw std::runtime_error("HTTP/2 local content is not known-length");
}

std::uint32_t submittedRequestStreamId(
    const Http2RequestHeadSubmitResult& result) {
    if (const auto* submitted = result.submitted()) {
        return submitted->streamId();
    }
    throw std::runtime_error("HTTP/2 request head was not submitted");
}

Http2RequestHeadSubmitError requestHeadSubmitError(
    const Http2RequestHeadSubmitResult& result) {
    if (const auto* failure = result.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 request head did not fail");
}

template <typename Result>
bool responseHeadSubmitted(const Result& result) {
    return result.submitted() != nullptr;
}

template <typename Result>
Http2ResponseHeadSubmitError responseHeadSubmitError(
    const Result& result) {
    if (const auto* failure = result.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 response head did not fail");
}

template <typename Result>
const auto& submittedResponsePlan(const Result& result) {
    if (const auto* submitted = result.submitted()) {
        return submitted->plan();
    }
    throw std::runtime_error("HTTP/2 response head was not submitted");
}

struct RequestContentLengthObservation final {
    std::size_t count{0};
    std::string value;
};

bool observeRequestContentLength(
    void* target,
    std::string_view name,
    std::string_view value) {
    auto& observation = *static_cast<RequestContentLengthObservation*>(target);
    if (name == "content-length") {
        ++observation.count;
        observation.value.assign(value.data(), value.size());
    }
    return true;
}

// Encode a minimal valid request header block (HPACK literals) into `block`.
void encodeRequest(std::pmr::string& block, std::string_view method) {
    HpackEncoder::encodeHeader(block, ":method", method);
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
}

void encodeGetRequest(std::pmr::string& block) {
    encodeRequest(block, "GET");
}

// Frame a HEADERS block on `streamId` with the given flags into a fed-ready buffer.
std::pmr::string headersFrame(
    std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags,
    std::string_view block) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        hdr, static_cast<std::uint32_t>(block.size()), Http2FrameType::kHeaders, flags, streamId);
    frame.append(hdr, 9);
    frame.append(block.data(), block.size());
    return frame;
}

std::pmr::string continuationFrame(
    std::pmr::memory_resource* resource,
    std::uint32_t streamId,
    std::uint8_t flags,
    std::string_view fragment) {
    std::pmr::string frame(resource);
    char header[9];
    ruvia::detail::http2EncodeFrameHeader(
        header,
        static_cast<std::uint32_t>(fragment.size()),
        Http2FrameType::kContinuation,
        flags,
        streamId);
    frame.append(header, sizeof(header));
    frame.append(fragment.data(), fragment.size());
    return frame;
}

std::pmr::string goawayFrame(
    std::pmr::memory_resource* resource,
    std::uint32_t lastStreamId,
    Http2ErrorCode error) {
    std::pmr::string bytes(resource);
    char frame[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(
        frame, 8, Http2FrameType::kGoaway, 0, 0);
    ruvia::detail::http2WriteGoawayPayload(frame + 9, lastStreamId, error);
    bytes.append(frame, sizeof(frame));
    return bytes;
}

// Test-only HPACK literal with incremental indexing (short, non-Huffman strings).
// The next block on this connection can reference the inserted entry at index 62.
void encodeShortDynamicHeader(
    std::pmr::string& block,
    std::string_view name,
    std::string_view value) {
    block.push_back(static_cast<char>(0x40));
    block.push_back(static_cast<char>(name.size()));
    block.append(name.data(), name.size());
    block.push_back(static_cast<char>(value.size()));
    block.append(value.data(), value.size());
}

// Start the role-specific preface and leave the connection ready to receive the
// peer's first frame. Servers must consume the client magic first.
void beginPeerInput(Http2Connection& conn) {
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
void handshake(Http2Connection& conn) {
    beginPeerInput(conn);
    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = conn.feed(std::string_view(settings, sizeof(settings)));
    if (result != ruvia::detail::Http2FeedResult::kAccepted ||
        !conn.receivedPeerSettings()) {
        throw std::runtime_error("connection rejected valid initial SETTINGS");
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

void beginClient(Http2Connection& client) {
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());
}

void applyPeerMaxConcurrentStreams(
    Http2Connection& client,
    std::uint32_t limit) {
    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(
        settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kMaxConcurrentStreams, limit);
    (void)out;
    (void)client.feed(std::string_view(settings, sizeof(settings)));
    client.consumeOutput(client.pendingOutput().size());
}

// Handshake but declare a small peer SETTINGS_INITIAL_WINDOW_SIZE so freshly created
// streams start with a tiny send window (to exercise flow-control backpressure).
void handshakeWithWindow(Http2Connection& conn, std::uint32_t window) {
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
void driveGetRequest(Http2Connection& conn, std::pmr::memory_resource* res) {
    std::pmr::string block(res);
    encodeGetRequest(block);
    const auto h = headersFrame(
        res, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

void driveRequest(
    Http2Connection& conn,
    std::pmr::memory_resource* res,
    std::string_view method) {
    std::pmr::string block(res);
    encodeRequest(block, method);
    const auto h = headersFrame(
        res, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());
}

}  // namespace

// The sans-I/O core produces a SETTINGS frame (stream 0) into its outbound buffer,
// and consumeOutput drains it. Exercises the core with zero asio / zero I/O.
RUVIA_TEST(http2_connection_begin_server_connection_emits_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());

    conn.beginConnection();

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(
        out.size(),
        Http2LocalSettings::kFrameBytes +
            ruvia::detail::kHttp2WindowUpdateFrameBytes);
    RUVIA_CHECK(conn.wantsWrite());

    const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(header.streamId, static_cast<std::uint32_t>(0));
    RUVIA_CHECK_EQ(header.length, Http2LocalSettings::kPayloadBytes);

    const auto window = ruvia::detail::http2ParseFrameHeader(
        out.substr(Http2LocalSettings::kFrameBytes, 9));
    RUVIA_CHECK_EQ(window.type, static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(window.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(
            out.substr(Http2LocalSettings::kFrameBytes + 9, 4)),
        Http2LocalSettings::kInitialWindowSize -
            static_cast<std::uint32_t>(ruvia::detail::kHttp2DefaultInitialWindowSize));

    conn.beginConnection();
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), out.size());

    conn.consumeOutput(out.size());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());
}

RUVIA_TEST(http2_connection_begin_client_connection_prefixes_same_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);

    client.beginConnection();
    const auto out = client.pendingOutput();
    RUVIA_CHECK_EQ(
        out.size(),
        ruvia::detail::kHttp2ClientPreface.size() +
            Http2LocalSettings::kFrameBytes +
            ruvia::detail::kHttp2WindowUpdateFrameBytes);
    RUVIA_CHECK_EQ(
        out.substr(0, ruvia::detail::kHttp2ClientPreface.size()),
        ruvia::detail::kHttp2ClientPreface);

    const auto settingsOffset = ruvia::detail::kHttp2ClientPreface.size();
    const auto settings = ruvia::detail::http2ParseFrameHeader(
        out.substr(settingsOffset, 9));
    RUVIA_CHECK_EQ(settings.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(settings.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(settings.length, Http2LocalSettings::kPayloadBytes);

    const auto firstSize = out.size();
    client.beginConnection();
    RUVIA_CHECK_EQ(client.pendingOutput().size(), firstSize);
}

RUVIA_TEST(http2_connection_request_head_requires_started_preface_without_consuming_id) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);

    const auto beforeStart = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(beforeStart.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(beforeStart) ==
        Http2RequestHeadSubmitError::kConnectionNotStarted);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    beginClient(client);
    const auto accepted = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_outbound_extension_method_is_valid_wire_token) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto extension = client.submitRegularRequestHead(
        "PROPFIND", "https", "example.test", "/dav", {},
        Http2RequestContent::none());
    RUVIA_CHECK(extension.submitted() != nullptr);
    const auto extensionStreamId = submittedRequestStreamId(extension);
    RUVIA_CHECK_EQ(extensionStreamId, std::uint32_t{1});
    const auto* stream = client.stream(extensionStreamId);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestMethod(), std::string_view("PROPFIND"));
        RUVIA_CHECK(
            stream->requestKnownMethod() == ruvia::HttpKnownMethod::kUnknown);
    }

    client.consumeOutput(client.pendingOutput().size());
    const auto malformed = client.submitRegularRequestHead(
        "BAD METHOD", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(malformed.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(malformed) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);
}

RUVIA_TEST(http2_connection_feed_before_begin_retains_input_and_is_retryable) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);

    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(
        settings, 0, Http2FrameType::kSettings, 0, 0);
    const auto beforeBegin = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(beforeBegin ==
        ruvia::detail::Http2FeedResult::kConnectionNotStarted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(!server.receivedPeerSettings());
    RUVIA_CHECK(!server.nextEvent().has_value());

    beginPeerInput(server);
    const auto retried = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(retried == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.receivedPeerSettings());
}

RUVIA_TEST(http2_connection_peer_stream_limit_waits_for_both_half_closes) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    applyPeerMaxConcurrentStreams(client, 1);

    const auto first = client.submitRegularRequestHead(
        "POST", "https", "example.test", "/upload", {},
        Http2RequestContent::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    RUVIA_CHECK_EQ(firstStreamId, std::uint32_t{1});
    client.consumeOutput(client.pendingOutput().size());

    const auto whileOpen = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {},
        Http2RequestContent::none());
    RUVIA_CHECK(whileOpen.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(whileOpen) ==
        Http2RequestHeadSubmitError::kPeerStreamLimitReached);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(
        &resource,
        firstStreamId,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    while (client.nextEvent().has_value()) {
    }

    const auto peerHalfOnly = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {},
        Http2RequestContent::none());
    RUVIA_CHECK(peerHalfOnly.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(peerHalfOnly) ==
        Http2RequestHeadSubmitError::kPeerStreamLimitReached);
    RUVIA_CHECK(client.submitData(
        firstStreamId, {}, Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    client.consumeOutput(client.pendingOutput().size());

    const auto afterBothHalves = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {},
        Http2RequestContent::none());
    RUVIA_CHECK(afterBothHalves.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(afterBothHalves), std::uint32_t{3});
}

RUVIA_TEST(http2_connection_peer_reset_releases_peer_stream_limit_slot) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    applyPeerMaxConcurrentStreams(client, 1);

    const auto first = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/first", {},
        Http2RequestContent::none());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(requestHeadSubmitError(client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {},
        Http2RequestContent::none())) ==
        Http2RequestHeadSubmitError::kPeerStreamLimitReached);

    char reset[13];
    ruvia::detail::http2EncodeFrameHeader(
        reset, 4, Http2FrameType::kRstStream, 0, firstStreamId);
    ruvia::detail::http2Write32(
        reset + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    RUVIA_CHECK(client.feed(std::string_view(reset, sizeof(reset))) ==
        ruvia::detail::Http2FeedResult::kAccepted);

    const auto afterReset = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {},
        Http2RequestContent::none());
    RUVIA_CHECK(afterReset.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(afterReset), std::uint32_t{3});
}

// feed() drives the SETTINGS handshake with zero I/O: feed the peer's empty
// SETTINGS frame and the core must emit a SETTINGS ACK.
RUVIA_TEST(http2_connection_feed_settings_emits_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    beginPeerInput(conn);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK_EQ(ack.length, static_cast<std::uint32_t>(0));
}

RUVIA_TEST(http2_connection_enable_push_validation_uses_peer_direction) {
    char frame[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(
        frame, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kEnablePush, 1);
    RUVIA_CHECK_EQ(out, frame + sizeof(frame));

    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginPeerInput(client);
    const auto clientResult = client.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(clientResult == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError().has_value());
    const auto goaway = client.pendingOutput();
    const auto goawayHeader = ruvia::detail::http2ParseFrameHeader(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(
        goawayHeader.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(goaway.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));

    Http2Connection server(&resource);
    beginPeerInput(server);
    const auto serverResult = server.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(serverResult == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    const auto ack = ruvia::detail::http2ParseFrameHeader(
        server.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
}

// A non-SETTINGS first frame is a protocol error (GOAWAY emitted, feed reports error).
RUVIA_TEST(http2_connection_feed_rejects_non_settings_first_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    beginPeerInput(conn);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kPing, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

RUVIA_TEST(http2_connection_server_requires_client_magic_before_frames) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    server.consumeOutput(server.pendingOutput().size());

    char bytes[ruvia::detail::kHttp2ClientPreface.size()]{};
    ruvia::detail::http2EncodeFrameHeader(
        bytes, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = server.feed(std::string_view(bytes, sizeof(bytes)));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(server.connectionError().has_value());
    RUVIA_CHECK(!server.receivedPeerSettings());
}

RUVIA_TEST(http2_connection_first_peer_settings_must_not_be_ack_for_either_role) {
    char ack[9];
    ruvia::detail::http2EncodeFrameHeader(
        ack, 0, Http2FrameType::kSettings, ruvia::detail::kHttp2FlagAck, 0);

    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginPeerInput(client);
        const auto result = client.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(client.connectionError().has_value());
        RUVIA_CHECK(!client.receivedPeerSettings());
    }
    {
        Http2Connection server(&resource);
        beginPeerInput(server);
        const auto result = server.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(server.connectionError().has_value());
        RUVIA_CHECK(!server.receivedPeerSettings());
    }
}

// After the handshake, a PING is echoed back with the ACK flag and the same payload.
RUVIA_TEST(http2_connection_feed_ping_echoes_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    const char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(ping + 9, data, 8);
    (void)conn.feed(std::string_view(ping, sizeof(ping)));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(9 + 8));
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK(out.substr(9, 8) == std::string_view(data, 8));
}

RUVIA_TEST(http2_connection_partial_frame_reports_need_more_until_complete) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(
        ping, 8, Http2FrameType::kPing, 0, 0);
    std::memcpy(ping + 9, "12345678", 8);

    constexpr std::size_t kFirstBytes = 12;  // full header + partial payload
    const auto partial = conn.feed(std::string_view(ping, kFirstBytes));
    RUVIA_CHECK(partial == ruvia::detail::Http2FeedResult::kNeedInput);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto complete = conn.feed(std::string_view(
        ping + kFirstBytes, sizeof(ping) - kFirstBytes));
    RUVIA_CHECK(complete == ruvia::detail::Http2FeedResult::kAccepted);
    const auto ack = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
}

// A valid connection-level WINDOW_UPDATE just opens the send window: no error, no
// output frame.
RUVIA_TEST(http2_connection_feed_connection_window_update_ok) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 1000);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A zero-increment connection WINDOW_UPDATE is a protocol error (GOAWAY).
RUVIA_TEST(http2_connection_feed_zero_window_update_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 0);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto goaway = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

// RST_STREAM referencing an idle (never-opened) stream is a protocol error (GOAWAY).
RUVIA_TEST(http2_connection_feed_rst_on_idle_stream_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char frame[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(frame, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(frame + 9, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto g = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(g.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

// RFC 9113 deprecated the RFC 7540 priority tree. Dependency and weight are ignored
// after validating frame shape, including the old self-dependency case.
RUVIA_TEST(http2_connection_feed_priority_payload_is_ignored) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // A self-dependent advisory payload on a live stream has no stream-state effect.
    driveGetRequest(conn, &resource);  // stream 1 open
    conn.consumeOutput(conn.pendingOutput().size());
    char live[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(live, 5, Http2FrameType::kPriority, 0, 1);
    ruvia::detail::http2Write32(live + 9, 1);  // depends on stream 1 (itself)
    live[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(live, sizeof(live))) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && !conn.stream(1)->isAborted());

    // The same is true on an idle stream; PRIORITY never opens it.
    char idle[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(idle, 5, Http2FrameType::kPriority, 0, 7);
    ruvia::detail::http2Write32(idle + 9, 7);  // idle stream depends on itself
    idle[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(idle, sizeof(idle))) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());  // ignored: no RST, no GOAWAY
}

// A complete HEADERS frame (END_HEADERS + END_STREAM) decodes the request head and the
// sans-I/O core emits kMessageHead then kMessageEnd; the head is exposed via stream().
RUVIA_TEST(http2_connection_event_queue_is_optional_and_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto frame = headersFrame(
        &resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    const auto result = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());

    const auto e1 = conn.nextEvent().value();
    RUVIA_CHECK(e1.kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(e1.messageBodyChunk() == nullptr);
    RUVIA_CHECK(e1.goaway() == nullptr);
    RUVIA_CHECK_EQ(
        e1.messageHead()->streamId(), static_cast<std::uint32_t>(1));
    const auto e2 = conn.nextEvent().value();
    RUVIA_CHECK(e2.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(e2.messageHead() == nullptr);
    RUVIA_CHECK_EQ(
        e2.messageEnd()->streamId(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.nextEvent().has_value());

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);
    RUVIA_CHECK_EQ(s->requestMethod(), std::string_view("GET"));
    RUVIA_CHECK(s->requestKnownMethod() == ruvia::HttpKnownMethod::kGet);
}

RUVIA_TEST(http2_connection_feed_extension_method_emits_request_event) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "PROPFIND");
    const auto frame = headersFrame(
        &resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    const auto result = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestMethod(), std::string_view("PROPFIND"));
        RUVIA_CHECK(
            stream->requestKnownMethod() == ruvia::HttpKnownMethod::kUnknown);
    }
}

// A HEADERS frame WITHOUT END_HEADERS leaves the block open (awaiting CONTINUATION); a
// CONTINUATION carrying the rest with END_HEADERS completes the head and emits the event.
RUVIA_TEST(http2_connection_feed_headers_continuation_completes_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string first(&resource);
    HpackEncoder::encodeHeader(first, ":method", "GET");
    HpackEncoder::encodeHeader(first, ":scheme", "https");
    std::pmr::string second(&resource);
    HpackEncoder::encodeHeader(second, ":path", "/");
    HpackEncoder::encodeHeader(second, ":authority", "example.com");

    // HEADERS with END_STREAM but no END_HEADERS -> no event yet.
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream,
        std::string_view(first.data(), first.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    // CONTINUATION with END_HEADERS -> head completes.
    char chdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        chdr, static_cast<std::uint32_t>(second.size()), Http2FrameType::kContinuation,
        ruvia::detail::kHttp2FlagEndHeaders, 1);
    std::pmr::string cont(&resource);
    cont.append(chdr, 9);
    cont.append(second.data(), second.size());
    RUVIA_CHECK(conn.feed(std::string_view(cont.data(), cont.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr && s->requestMethod() == "GET");
    RUVIA_CHECK(s != nullptr &&
        s->requestKnownMethod() == ruvia::HttpKnownMethod::kGet);
}

// RFC 9113 requires field blocks received after our RST_STREAM to be minimally
// processed. A pinned reset stream must not capture the fragments, and the dynamic
// entry created by the discarded block must remain usable by the next stream.
RUVIA_TEST(http2_connection_local_reset_discards_multiframe_headers_and_keeps_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());

    std::pmr::string dynamic(&resource);
    encodeShortDynamicHeader(dynamic, "x-discarded", "indexed");
    const auto split = dynamic.size() / 2;
    const auto first = headersFrame(
        &resource, 1, 0, std::string_view(dynamic.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());  // no second RST

    const auto last = continuationFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(dynamic.data() + split, dynamic.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(!conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && conn.stream(1)->isAborted());

    std::pmr::string nextBlock(&resource);
    encodeGetRequest(nextBlock);
    HpackEncoder::encodeIndexed(nextBlock, 62);  // x-discarded: indexed
    const auto next = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextBlock.data(), nextBlock.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next.data(), next.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.connectionError().has_value());
    conn.unpinStream(1);
}

// If a discarded field block exceeds the buffering budget, the core cannot satisfy
// RFC 9113's mandatory decompression step. The connection error is COMPRESSION_ERROR,
// not an application/load-shedding code that would imply HPACK remains usable.
RUVIA_TEST(http2_connection_undecodable_discarded_block_is_compression_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());

    const std::string fullFrame(ruvia::detail::kHttp2DefaultMaxFrameSize, '\0');
    const auto first = headersFrame(&resource, 1, 0, fullFrame);
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    for (int i = 0; i < 3; ++i) {
        const auto continuation = continuationFrame(&resource, 1, 0, fullFrame);
        RUVIA_CHECK(conn.feed(
            std::string_view(continuation.data(), continuation.size())) ==
            ruvia::detail::Http2FeedResult::kAccepted);
    }
    const auto overflow = continuationFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders, "x");
    RUVIA_CHECK(conn.feed(std::string_view(overflow.data(), overflow.size())) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto out = conn.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kCompressionError));
    conn.unpinStream(1);
}

// A client can cancel while a multi-frame response head is incomplete. The partial
// compressed block must move out of the stream before removal, then CONTINUATION
// completes silently and updates HPACK for later responses.
RUVIA_TEST(http2_connection_client_reset_detaches_partial_response_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());
    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    client.consumeOutput(client.pendingOutput().size());

    const auto firstHead = client.submitRegularRequestHead(
        "GET",
        "https",
        "example.test",
        "/",
        {},
        Http2RequestContent::none());
    RUVIA_CHECK(firstHead.submitted() != nullptr);
    const auto firstStream = submittedRequestStreamId(firstHead);
    RUVIA_CHECK_EQ(firstStream, static_cast<std::uint32_t>(1));
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string responseBlock(&resource);
    HpackEncoder::encodeHeader(responseBlock, ":status", "200");
    const auto statusBytes = responseBlock.size();
    encodeShortDynamicHeader(responseBlock, "x-response", "indexed");
    const auto first = headersFrame(
        &resource,
        firstStream,
        0,
        std::string_view(responseBlock.data(), statusBytes));
    RUVIA_CHECK(client.feed(std::string_view(first.data(), first.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.headerBlockInProgress());

    RUVIA_CHECK(client.submitReset(firstStream, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(client.stream(firstStream) == nullptr);
    client.consumeOutput(client.pendingOutput().size());

    const auto continuation = continuationFrame(
        &resource,
        firstStream,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(
            responseBlock.data() + statusBytes,
            responseBlock.size() - statusBytes));
    RUVIA_CHECK(client.feed(
        std::string_view(continuation.data(), continuation.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());  // no second RST
    RUVIA_CHECK(!client.nextEvent().has_value());

    const auto nextHead = client.submitRegularRequestHead(
        "GET",
        "https",
        "example.test",
        "/next",
        {},
        Http2RequestContent::none());
    RUVIA_CHECK(nextHead.submitted() != nullptr);
    const auto nextStream = submittedRequestStreamId(nextHead);
    RUVIA_CHECK_EQ(nextStream, static_cast<std::uint32_t>(3));
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string nextResponse(&resource);
    HpackEncoder::encodeHeader(nextResponse, ":status", "200");
    HpackEncoder::encodeIndexed(nextResponse, 62);  // x-response: indexed
    const auto final = headersFrame(
        &resource,
        nextStream,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextResponse.data(), nextResponse.size()));
    RUVIA_CHECK(client.feed(std::string_view(final.data(), final.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.connectionError().has_value());
}

// Build a POST request head (no END_STREAM) with optional content-length; body follows.
std::pmr::string postHeadFrame(
    std::pmr::memory_resource* resource, std::string_view contentLength) {
    std::pmr::string block(resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    if (!contentLength.empty()) {
        HpackEncoder::encodeHeader(block, "content-length", contentLength);
    }
    return headersFrame(
        resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
}

// Frame a DATA payload on `streamId` with the given flags.
std::pmr::string dataFrame(
    std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags,
    std::string_view body) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        hdr, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, flags, streamId);
    frame.append(hdr, 9);
    frame.append(body.data(), body.size());
    return frame;
}

// A trailer section without END_STREAM is a stream error, but its complete field
// block still has to update HPACK before the reset is emitted. Splitting it proves the
// core neither sends an early RST nor mistakes the required CONTINUATION for a new frame.
RUVIA_TEST(http2_connection_invalid_multiframe_trailer_resets_after_hpack_decode) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto request = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    conn.consumeOutput(conn.pendingOutput().size());

    std::pmr::string trailer(&resource);
    encodeShortDynamicHeader(trailer, "x-invalid-trailer", "indexed");
    const auto split = trailer.size() / 2;
    const auto first = headersFrame(
        &resource, 1, 0, std::string_view(trailer.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());

    const auto last = continuationFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(trailer.data() + split, trailer.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    const auto resetBytes = conn.pendingOutput();
    RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(resetBytes.size());

    std::pmr::string nextBlock(&resource);
    encodeGetRequest(nextBlock);
    HpackEncoder::encodeIndexed(nextBlock, 62);  // x-invalid-trailer: indexed
    const auto next = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextBlock.data(), nextBlock.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next.data(), next.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.connectionError().has_value());
}

// A DATA frame after the head yields a kMessageBodyChunk carrying the bytes, then
// (on END_STREAM) kMessageEnd; the core also credits the peer back with WINDOW_UPDATE.
RUVIA_TEST(http2_connection_feed_data_emits_body_chunk_and_end) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "");
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const char body[5] = {'h', 'e', 'l', 'l', 'o'};
    const auto d = dataFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    const auto chunk = conn.nextEvent().value();
    RUVIA_CHECK(chunk.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(
        chunk.messageBodyChunk()->streamId(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(chunk.messageBodyChunk()->bytes() == std::string_view(body, 5));
    const auto end = conn.nextEvent().value();
    RUVIA_CHECK(end.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK_EQ(
        end.messageEnd()->streamId(), static_cast<std::uint32_t>(1));

    const auto wu = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(wu.type, static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
}

RUVIA_TEST(http2_connection_feed_preserves_pending_events_and_retries_input) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto data = dataFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagEndStream,
        "body");
    RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    const auto outputBeforeRetry = conn.pendingOutput().size();

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(
        ping, 8, Http2FrameType::kPing, 0, 0);
    std::memcpy(ping + 9, "retry-me", 8);
    const auto blocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(blocked == ruvia::detail::Http2FeedResult::kEventsPending);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), outputBeforeRetry);

    const auto chunk = conn.nextEvent().value();
    RUVIA_CHECK(chunk.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(chunk.messageBodyChunk()->bytes(), std::string_view("body"));
    const auto stillBlocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(stillBlocked ==
        ruvia::detail::Http2FeedResult::kEventsPending);
    RUVIA_CHECK_EQ(chunk.messageBodyChunk()->bytes(), std::string_view("body"));

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    const auto retried = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(retried == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), outputBeforeRetry + sizeof(ping));
    const auto ack = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(outputBeforeRetry, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK(!conn.nextEvent().has_value());
}

// A DATA/END_STREAM that falls short of a declared content-length is a protocol error:
// the core RST_STREAMs the stream and does NOT emit kMessageEnd.
RUVIA_TEST(http2_connection_feed_data_short_of_content_length_resets) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "10");  // promises 10 bytes
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);

    const char body[5] = {'s', 'h', 'o', 'r', 't'};  // only 5, with END_STREAM
    const auto d = dataFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageBodyChunk);
    // The length mismatch aborts the stream: kStreamClosed (never kMessageEnd), and
    // the (unpinned) stream is removed from the table.
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// submitResponseHead emits a HEADERS block (END_HEADERS, no END_STREAM when a body
// follows); submitData then sends the buffered body as a terminal DATA frame.
RUVIA_TEST(http2_connection_submit_response_head_and_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp(&resource);
    resp.status(200);
    resp.setBodyCopy("hello");
    const auto headResult = conn.submitResponseHead(1, resp);
    RUVIA_CHECK(responseHeadSubmitted(headResult));

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(head.size());

    const auto r = conn.submitData(1, "hello", Http2EndStream::kEndStream);
    RUVIA_CHECK(r == Http2DataSubmitStatus::kAccepted);
    const auto body = conn.pendingOutput();
    const auto dd = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(dd.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(dd.length, static_cast<std::uint32_t>(5));
    RUVIA_CHECK((dd.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_buffered_response_length_is_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("hello");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitResponseHead(1, response)));
    conn.consumeOutput(conn.pendingOutput().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});

    // All mismatches are rejected before output, flow-window, counters, or phase
    // change. The caller can correct the submission and continue the same stream.
    RUVIA_CHECK(conn.finishResponse(1) ==
        Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "four", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "sixsix", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);

    RUVIA_CHECK(conn.submitData(1, "he", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{2});
    RUVIA_CHECK(conn.submitData(1, "llo", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->localContent().lengthComplete());
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

RUVIA_TEST(http2_connection_rejects_data_before_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const auto before = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "body", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), before);
}

RUVIA_TEST(http2_connection_response_head_submit_result_is_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("ok");

    Http2Connection missingStream(&resource);
    const auto closed = missingStream.submitResponseHead(1, response);
    RUVIA_CHECK(closed.submitted() == nullptr);
    RUVIA_CHECK(closed.failure() != nullptr);
    RUVIA_CHECK(
        closed.failure()->error() ==
        Http2ResponseHeadSubmitError::kClosed);
    RUVIA_CHECK(missingStream.pendingOutput().empty());

    Http2Connection buffered(&resource);
    handshake(buffered);
    driveGetRequest(buffered, &resource);
    const auto submitted = buffered.submitResponseHead(1, response);
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK(submitted.failure() == nullptr);
    RUVIA_CHECK_EQ(
        submitted.submitted()->plan().contentLength(),
        std::uint64_t{2});

    Http2Connection streaming(&resource);
    handshake(streaming);
    driveGetRequest(streaming, &resource);
    ruvia::HttpResponse streamingHead(&resource);
    streamingHead.status(200);
    const auto streamingSubmitted = streaming.submitStreamingResponseHead(
        1,
        std::move(streamingHead),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(streamingSubmitted.submitted() != nullptr);
    RUVIA_CHECK(streamingSubmitted.failure() == nullptr);
}

RUVIA_TEST(http2_connection_rejects_duplicate_response_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse first(&resource);
    first.status(200);
    const auto firstResult = conn.submitStreamingResponseHead(
        1,
        std::move(first),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(firstResult));
    conn.consumeOutput(conn.pendingOutput().size());

    ruvia::HttpResponse duplicate(&resource);
    duplicate.status(200);
    const auto duplicateResult = conn.submitResponseHead(1, duplicate);
    RUVIA_CHECK(
        responseHeadSubmitError(duplicateResult) ==
        Http2ResponseHeadSubmitError::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_rejects_head_api_for_wrong_role) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    driveGetRequest(server, &resource);
    RUVIA_CHECK(requestHeadSubmitError(server.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none())) ==
        Http2RequestHeadSubmitError::kInvalidState);
    RUVIA_CHECK(server.pendingOutput().empty());

    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());
    ruvia::HttpResponse response(&resource);
    response.status(200);
    const auto result = client.submitResponseHead(streamId, response);
    RUVIA_CHECK(
        responseHeadSubmitError(result) ==
        Http2ResponseHeadSubmitError::kInvalidState);
    RUVIA_CHECK(client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_request_content_alternatives_own_wire_framing) {
    std::pmr::monotonic_buffer_resource resource;

    const auto withoutContent = Http2RequestContent::none();
    RUVIA_CHECK(withoutContent.withoutContent() != nullptr);
    RUVIA_CHECK(withoutContent.knownLengthContent() == nullptr);
    RUVIA_CHECK(withoutContent.streamingContent() == nullptr);

    const auto zeroLength = Http2RequestContent::knownLength(0);
    RUVIA_CHECK(zeroLength.withoutContent() == nullptr);
    RUVIA_CHECK(zeroLength.knownLengthContent() != nullptr);
    RUVIA_CHECK(zeroLength.streamingContent() == nullptr);
    if (const auto* knownLength = zeroLength.knownLengthContent()) {
        RUVIA_CHECK_EQ(knownLength->length(), std::uint64_t{0});
    }

    const auto streaming = Http2RequestContent::streaming();
    RUVIA_CHECK(streaming.withoutContent() == nullptr);
    RUVIA_CHECK(streaming.knownLengthContent() == nullptr);
    RUVIA_CHECK(streaming.streamingContent() != nullptr);

    const auto check = [&resource, &ruvia_ctx](
                           std::string_view method,
                           Http2RequestContent content,
                           bool expectEndStream,
                           std::string_view expectedContentLength,
                           auto&& verifyLocalContent) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto submit = client.submitRegularRequestHead(
            method,
            "https",
            "example.test",
            "/upload",
            {},
            content);
        RUVIA_CHECK(submit.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(submit);
        RUVIA_CHECK_EQ(streamId, static_cast<std::uint32_t>(1));

        const auto out = client.pendingOutput();
        RUVIA_CHECK(out.size() >= 9);
        const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
        RUVIA_CHECK_EQ(frame.streamId, streamId);
        RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
        RUVIA_CHECK(
            ((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0) ==
            expectEndStream);
        RUVIA_CHECK_EQ(
            out.size(), static_cast<std::size_t>(9 + frame.length));

        RequestContentLengthObservation observation;
        HpackDecoder decoder(&resource);
        RUVIA_CHECK(decoder.decode(
            out.substr(9, frame.length),
            &observation,
            &observeRequestContentLength).ok());
        if (expectedContentLength.empty()) {
            RUVIA_CHECK_EQ(observation.count, static_cast<std::size_t>(0));
        } else {
            RUVIA_CHECK_EQ(observation.count, static_cast<std::size_t>(1));
            RUVIA_CHECK_EQ(observation.value, std::string(expectedContentLength));
        }

        const auto* stream = client.stream(streamId);
        RUVIA_CHECK(stream != nullptr);
        verifyLocalContent(stream->localContent());
        RUVIA_CHECK_EQ(
            stream->localSend().endStreamCommitted() != nullptr,
            expectEndStream);
    };

    check(
        "GET",
        Http2RequestContent::none(),
        true,
        {},
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            RUVIA_CHECK(localContent.forbidden() != nullptr);
            RUVIA_CHECK(localContent.knownLength() == nullptr);
        });
    check(
        "POST",
        Http2RequestContent::knownLength(0),
        true,
        "0",
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            const auto* knownLength = localContent.knownLength();
            RUVIA_CHECK(knownLength != nullptr);
            if (knownLength != nullptr) {
                RUVIA_CHECK_EQ(
                    knownLength->declaredLength(), std::uint64_t{0});
            }
        });
    check(
        "POST",
        Http2RequestContent::knownLength(5),
        false,
        "5",
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            const auto* knownLength = localContent.knownLength();
            RUVIA_CHECK(knownLength != nullptr);
            if (knownLength != nullptr) {
                RUVIA_CHECK_EQ(
                    knownLength->declaredLength(), std::uint64_t{5});
            }
        });
    check(
        "POST",
        Http2RequestContent::streaming(),
        false,
        {},
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            RUVIA_CHECK(localContent.unbounded() != nullptr);
            RUVIA_CHECK(localContent.knownLength() == nullptr);
        });
}

RUVIA_TEST(http2_connection_rejects_raw_request_content_length_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    const auto checkRejected =
        [&resource, &ruvia_ctx](std::span<const ruvia::HttpHeaderView> headers) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto rejected = client.submitRegularRequestHead(
            "POST",
            "https",
            "example.test",
            "/upload",
            headers,
            Http2RequestContent::knownLength(5));
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submitRegularRequestHead(
            "POST",
            "https",
            "example.test",
            "/upload",
            {},
            Http2RequestContent::knownLength(5));
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
        RUVIA_CHECK(!client.pendingOutput().empty());
    };

    const ruvia::HttpHeaderView matching[] = {{"content-length", "5"}};
    const ruvia::HttpHeaderView conflicting[] = {{"content-length", "4"}};
    const ruvia::HttpHeaderView duplicate[] = {
        {"content-length", "5"},
        {"content-length", "5"}};
    const ruvia::HttpHeaderView invalid[] = {{"content-length", "invalid"}};
    checkRejected(matching);
    checkRejected(conflicting);
    checkRejected(duplicate);
    checkRejected(invalid);
}

RUVIA_TEST(http2_connection_rejects_invalid_request_head_before_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto reject = [&](std::string_view method,
                            std::string_view scheme,
                            std::string_view authority,
                            std::string_view path,
                            std::span<const ruvia::HttpHeaderView> headers) {
        const auto result = client.submitRegularRequestHead(
            method,
            scheme,
            authority,
            path,
            headers,
            Http2RequestContent::none());
        RUVIA_CHECK(result.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(result) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const ruvia::HttpHeaderView uppercase[] = {{"X-Test", "value"}};
    const ruvia::HttpHeaderView connection[] = {{"connection", "keep-alive"}};
    const ruvia::HttpHeaderView mismatchedHost[] = {{"host", "other.test"}};
    reject("CONNECT", "https", "example.test:443", "/", {});
    reject("GET bad", "https", "example.test", "/", {});
    reject("GET", "ftp", "example.test", "/", {});
    reject("GET", "https", "example.test", "relative", {});
    reject("GET", "https", "example.test", "/", uppercase);
    reject("GET", "https", "example.test", "/", connection);
    reject("GET", "https", "example.test", "/", mismatchedHost);

    const auto accepted = client.submitRegularRequestHead(
        "GET",
        "https",
        "example.test",
        "/",
        {},
        Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_exposes_negotiated_extended_connect_capability) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    RUVIA_CHECK(!client.peerExtendedConnectEnabled());
    beginPeerInput(client);

    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(
        settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kEnableConnectProtocol, 1);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.peerExtendedConnectEnabled());

    Http2Connection server(&resource);
    beginPeerInput(server);
    RUVIA_CHECK(server.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.peerExtendedConnectEnabled());
}

RUVIA_TEST(http2_connection_request_known_length_is_exact_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "POST",
        "https",
        "example.test",
        "/upload",
        {},
        Http2RequestContent::knownLength(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(client.submitData(streamId, "four", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(client.submitData(streamId, "sixsix", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->localSend().requestContentOpen() != nullptr);

    RUVIA_CHECK(client.submitData(streamId, "he", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    auto out = client.pendingOutput();
    auto data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(2));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{2});
    client.consumeOutput(out.size());

    RUVIA_CHECK(client.submitData(streamId, "llo", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    out = client.pendingOutput();
    data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(3));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->localContent().lengthComplete());
    RUVIA_CHECK(stream->localSend().requestContentOpen() == nullptr);
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
    client.consumeOutput(out.size());

    RUVIA_CHECK(client.submitData(streamId, "again", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK(client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_request_streaming_content_has_no_length_contract) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "POST",
        "https",
        "example.test",
        "/upload",
        {},
        Http2RequestContent::streaming());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    RUVIA_CHECK(client.submitData(streamId, "chunk-a", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(client.submitData(streamId, "chunk-b", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    const auto out = client.pendingOutput();
    const auto data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(7));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_interim_head_preserves_final_head_phase) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const ruvia::HttpHeaderView invalidHeaders[] = {
        {"Content-Length", "0"},
    };
    const ruvia::HttpInterimResponseHead invalidEarlyHints(
        103,
        invalidHeaders);
    RUVIA_CHECK(conn.submitInterimResponseHead(1, invalidEarlyHints) ==
        Http2SubmitStatus::kInvalidMessage);
    RUVIA_CHECK(conn.pendingOutput().empty());

    const ruvia::HttpHeaderView earlyHintHeaders[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(103, earlyHintHeaders);
    RUVIA_CHECK(conn.submitInterimResponseHead(1, earlyHints) ==
        Http2SubmitStatus::kAccepted);
    const auto informational = conn.pendingOutput();
    const auto infoFrame = ruvia::detail::http2ParseFrameHeader(informational.substr(0, 9));
    RUVIA_CHECK_EQ(infoFrame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((infoFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(informational.size());

    ruvia::HttpResponse finalResponse(&resource);
    finalResponse.status(200);
    const auto finalResult = conn.submitResponseHead(1, finalResponse);
    RUVIA_CHECK(responseHeadSubmitted(finalResult));
    const auto finalHead = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(finalHead.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((finalHead.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_rejects_upgrade_required_final_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse buffered(&resource);
    buffered.status(426);
    buffered.header("Upgrade", "websocket");
    const auto bufferedResult = conn.submitResponseHead(1, buffered);
    RUVIA_CHECK(
        responseHeadSubmitError(bufferedResult) ==
        Http2ResponseHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(conn.pendingOutput().empty());

    ruvia::HttpResponse streaming(&resource);
    streaming.status(426);
    streaming.header("Upgrade", "websocket");
    const auto streamingResult = conn.submitStreamingResponseHead(
        1,
        std::move(streaming),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ruvia::detail::ResponseTrailerIntent::kNone);
    RUVIA_CHECK(
        responseHeadSubmitError(streamingResult) ==
        Http2ResponseHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(conn.pendingOutput().empty());

    // Both failures occur before HPACK/stream mutation, so a conformant final
    // response can still be submitted on the same stream.
    ruvia::HttpResponse fallback(&resource);
    fallback.status(400);
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitResponseHead(1, fallback)));
    RUVIA_CHECK(!conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_terminal_large_head_sets_end_stream_only_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(204);
    const std::string largeValue(20 * 1024, 'a');
    response.header("X-Large", largeValue);
    const auto result = conn.submitResponseHead(1, response);
    RUVIA_CHECK(responseHeadSubmitted(result));

    auto out = conn.pendingOutput();
    bool first = true;
    bool sawContinuation = false;
    bool sawEndHeaders = false;
    while (out.size() >= 9) {
        const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK(out.size() >= 9 + frame.length);
        if (first) {
            RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
            first = false;
        } else {
            RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kContinuation));
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
            sawContinuation = true;
        }
        if ((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0) {
            sawEndHeaders = true;
        }
        out.remove_prefix(9 + frame.length);
    }
    RUVIA_CHECK(!first);
    RUVIA_CHECK(sawContinuation);
    RUVIA_CHECK(sawEndHeaders);
    RUVIA_CHECK(out.empty());
}

// HEAD carries the representation metadata (including Content-Length) but the
// protocol core terminates the stream on HEADERS and tells the runtime not to
// submit DATA, even when the application response contains bytes.
RUVIA_TEST(http2_connection_head_buffered_response_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("hello");
    const auto headResult = conn.submitResponseHead(1, response);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodySuppressed());
    RUVIA_CHECK(!submittedResponsePlan(headResult).sendBody());
    RUVIA_CHECK_EQ(submittedResponsePlan(headResult).contentLength(), static_cast<std::uint64_t>(5));
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

// 205 is not a normal zero-byte response whose body phase may remain open: RFC
// 9110 §15.3.6 forbids content. The shared response plan therefore terminates the
// HTTP/2 stream on HEADERS even when the application attached buffered bytes.
RUVIA_TEST(http2_connection_reset_content_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(205);
    response.setBodyCopy("must-not-be-sent");
    response.header("Content-Length", "16");
    const auto headResult = conn.submitResponseHead(1, response);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(!submittedResponsePlan(headResult).statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodySuppressed());
    RUVIA_CHECK(!submittedResponsePlan(headResult).sendBody());
    RUVIA_CHECK_EQ(submittedResponsePlan(headResult).contentLength(), static_cast<std::uint64_t>(0));
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

// submitStreamingResponseHead emits HEADERS with NO Content-Length and leaves the
// stream open; subsequent submitData chunks stream the body, the last with END_STREAM.
RUVIA_TEST(http2_connection_submit_streaming_response_head_and_chunks) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp(&resource);
    resp.status(200);
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(resp),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(headResult));

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stays open
    conn.consumeOutput(head.size());

    RUVIA_CHECK(conn.submitData(1, "chunk1", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.submitData(1, "chunk2", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    const auto body = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(6));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto d2 = ruvia::detail::http2ParseFrameHeader(body.substr(9 + 6, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_streaming_rejects_invalid_content_length_before_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    for (const std::string_view invalid : {
             std::string_view{"x"},
             std::string_view{"-1"},
             std::string_view{"5,5"},
             std::string_view{"18446744073709551616"}}) {
        ruvia::HttpResponse response(&resource);
        response.status(200);
        response.header("Content-Length", invalid);
        const auto result = conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone);
        RUVIA_CHECK(
            responseHeadSubmitError(result) ==
            Http2ResponseHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(conn.pendingOutput().empty());
        auto* stream = conn.stream(1);
        RUVIA_CHECK(stream != nullptr);
        RUVIA_CHECK(stream->localSend().headPending() != nullptr);
        RUVIA_CHECK(stream->localContent().unset() != nullptr);
    }

    // A valid retry still owns the initial-head transition.
    ruvia::HttpResponse valid(&resource);
    valid.status(200);
    valid.header("Content-Length", "5");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(valid),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{5});
}

RUVIA_TEST(http2_connection_streaming_content_length_finish_and_trailers_are_exact) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "5");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);

    RUVIA_CHECK(conn.submitData(1, "hey", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "toolong", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});

    RUVIA_CHECK(conn.submitData(1, "hel", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());
    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, trailers) ==
        Http2ResponseTrailerSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(stream->responseTrailerBlock().empty());
    RUVIA_CHECK(conn.finishResponse(1) ==
        Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());

    RUVIA_CHECK(conn.submitData(1, "lo", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, trailers) ==
        Http2ResponseTrailerSubmitStatus::kAccepted);
    const auto trailerBytes = stream->responseTrailerBlock().size();
    RUVIA_CHECK(trailerBytes != 0);
    // A direct terminal DATA would strand the accepted trailer section, so only
    // finishResponse() can own the terminal END_STREAM transition now.
    RUVIA_CHECK(conn.submitData(1, {}, Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(stream->responseTrailerBlock().size(), trailerBytes);
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kAccepted);
    const auto trailer = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(trailer.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
}

RUVIA_TEST(http2_connection_streaming_zero_content_length_stays_open_for_finish) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "0");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    const auto head = conn.pendingOutput();
    const auto headFrame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK((headFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(head.size());

    RUVIA_CHECK(conn.submitData(1, "x", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kAccepted);
    const auto terminal = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(terminal.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(terminal.length, static_cast<std::uint32_t>(0));
    RUVIA_CHECK((terminal.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

// An explicitly registered streaming HEAD route still cannot emit a payload.
// The method/status decision belongs to the HTTP/2 core, not the Web sink.
RUVIA_TEST(http2_connection_head_streaming_response_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        submittedResponsePlan(headResult).headDisposition() ==
        ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(conn.stream(1)->localContent().forbidden() != nullptr);
    RUVIA_CHECK(conn.stream(1)->localContent().knownLength() == nullptr);
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

RUVIA_TEST(http2_connection_head_response_can_end_with_trailers_only) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kPresent);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        submittedResponsePlan(headResult).headDisposition() ==
        ResponseStreamHeadDisposition::kTrailersOnly);
    RUVIA_CHECK(
        submittedResponsePlan(headResult).trailerFraming() ==
        ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
    const auto initialHead = conn.pendingOutput();
    const auto initialFrame = ruvia::detail::http2ParseFrameHeader(
        initialHead.substr(0, 9));
    RUVIA_CHECK_EQ(
        initialFrame.type,
        static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((initialFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(initialHead.size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->localSend().responseContentOpen() == nullptr);
    RUVIA_CHECK(stream->localSend().responseTrailersOnly() != nullptr);
    RUVIA_CHECK(stream->localContent().forbidden() != nullptr);
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());

    // Declaring trailer intent cannot fall back to an empty DATA terminator.
    RUVIA_CHECK(
        conn.finishResponse(1) ==
        Http2FinishSubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(stream->localSend().responseTrailersOnly() != nullptr);

    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"Server-Timing", "db;dur=4"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, trailers) ==
        Http2ResponseTrailerSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kAccepted);
    const auto terminal = conn.pendingOutput();
    const auto terminalFrame = ruvia::detail::http2ParseFrameHeader(
        terminal.substr(0, 9));
    RUVIA_CHECK_EQ(
        terminalFrame.type,
        static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((terminalFrame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

RUVIA_TEST(http2_response_trailer_section_is_phase_typed_and_atomic) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const std::array<ruvia::HttpHeaderView, 1> validTrailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, validTrailers) ==
        Http2ResponseTrailerSubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.stream(1)->responseTrailerBlock().empty());

    ruvia::HttpResponse response(&resource);
    response.status(200);
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());

    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, {}) ==
        Http2ResponseTrailerSubmitStatus::kEmpty);
    const std::array<ruvia::HttpHeaderView, 2> mixedTrailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"},
        ruvia::HttpHeaderView{"Content-Length", "2"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, mixedTrailers) ==
        Http2ResponseTrailerSubmitStatus::kInvalidField);
    RUVIA_CHECK(conn.stream(1)->responseTrailerBlock().empty());

    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, validTrailers) ==
        Http2ResponseTrailerSubmitStatus::kAccepted);
    const auto acceptedBytes = conn.stream(1)->responseTrailerBlock().size();
    RUVIA_CHECK(acceptedBytes != 0);
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, validTrailers) ==
        Http2ResponseTrailerSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(
        conn.stream(1)->responseTrailerBlock().size(),
        acceptedBytes);
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kAccepted);
}

RUVIA_TEST(http2_connection_reset_content_streaming_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(205);
    response.header("Content-Length", "9");
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(!submittedResponsePlan(headResult).bodyPlan().statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        submittedResponsePlan(headResult).headDisposition() ==
        ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(conn.stream(1)->localContent().forbidden() != nullptr);
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

// A body larger than the send window is partially sent and the remainder queued. The
// core owns that remainder; a second submission is rejected without growing output.
// WINDOW_UPDATE drains it with END_STREAM and reports the stream ready for new input.
RUVIA_TEST(http2_connection_submit_data_blocks_then_drains_on_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 3);  // stream 1 starts with a 3-byte send window
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(headResult));
    conn.consumeOutput(conn.pendingOutput().size());

    const char body[5] = {'a', 'b', 'c', 'd', 'e'};
    const auto r1 = conn.submitData(
        1, std::string_view(body, 5), Http2EndStream::kKeepOpen);
    RUVIA_CHECK(r1 == Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(conn.hasQueuedData(1));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});

    const auto out1 = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out1.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(3));            // only 3 fit
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);   // not terminal
    const auto queuedBytes = out1.size();
    RUVIA_CHECK(conn.submitData(1, "later", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kBackpressured);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), queuedBytes);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});
    conn.consumeOutput(out1.size());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 10);  // reopen stream 1's window
    (void)conn.feed(std::string_view(wu, sizeof(wu)));

    const auto out2 = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out2.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(2));            // remaining 2
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) == 0);

    const auto drained = conn.takeDrainedDataStreams();
    RUVIA_CHECK_EQ(drained.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK_EQ(drained[0], static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.hasQueuedData(1));
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(conn.submitData(1, "later", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{10});
}

RUVIA_TEST(http2_connection_short_finish_does_not_mutate_queued_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 3);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.header("Content-Length", "8");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "12345", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kQueued);
    conn.consumeOutput(conn.pendingOutput().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});
    RUVIA_CHECK(conn.finishResponse(1) ==
        Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);
    RUVIA_CHECK(conn.hasQueuedData(1));
    RUVIA_CHECK(conn.pendingOutput().empty());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 10);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    const auto drainedOutput = conn.pendingOutput();
    const auto drainedFrame = ruvia::detail::http2ParseFrameHeader(
        drainedOutput.substr(0, 9));
    RUVIA_CHECK_EQ(drainedFrame.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((drainedFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(drainedOutput.size());
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});

    RUVIA_CHECK(conn.submitData(1, "678", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{8});
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

// submitReset emits a RST_STREAM and marks the stream reset so no further response
// bytes are produced for it.
RUVIA_TEST(http2_connection_submit_reset_emits_rst) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    const auto out = conn.pendingOutput();
    const auto r = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(r.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(r.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_local_reset_unknown_and_repeat_emit_no_illegal_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    RUVIA_CHECK(conn.submitReset(99, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());

    driveGetRequest(conn, &resource);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    const auto firstResetBytes = conn.pendingOutput().size();
    RUVIA_CHECK_EQ(firstResetBytes, static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));

    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kClosed);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), firstResetBytes);

    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    RUVIA_CHECK(client.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kInvalidState);
    RUVIA_CHECK(client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_peer_reset_discards_queued_data_and_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 0);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(200);
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "deferred", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(conn.hasQueuedData(1));
    RUVIA_CHECK(conn.pendingOutput().empty());

    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, trailers) ==
        Http2ResponseTrailerSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kQueued);

    char rst[13];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.nextEvent().has_value()) {
    }
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(!conn.hasQueuedData(1));
    RUVIA_CHECK(conn.takeDrainedDataStreams().empty());
    RUVIA_CHECK(conn.pendingOutput().empty());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.takeDrainedDataStreams().empty());
}

// A pinned stream (handler in flight) is NOT freed by a peer RST_STREAM: it stays in
// the table (so the handler's request views survive) but is marked reset, and
// kStreamClosed is emitted so the owner can drop the response. unpin then frees it.
RUVIA_TEST(http2_connection_pinned_stream_survives_peer_reset) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 created + decoded
    RUVIA_CHECK(conn.stream(1) != nullptr);

    conn.pinStream(1);

    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);   // kept alive because pinned
    RUVIA_CHECK(s->isAborted());  // retained storage, but protocol ownership ended
    bool sawClosed = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* closed = event->streamClosed();
            closed != nullptr && closed->streamId() == 1) {
            sawClosed = true;
            RUVIA_CHECK(
                closed->source() ==
                ruvia::detail::Http2StreamCloseSource::kPeer);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kCancel);
        }
    }
    RUVIA_CHECK(sawClosed);

    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);  // freed once the handler finished
}

// Unpinning a stream that completed normally on both halves frees it without a reset.
RUVIA_TEST(http2_connection_unpin_frees_completed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    ruvia::HttpResponse response(&resource);
    response.status(204);
    const auto headResult = conn.submitResponseHead(1, response);
    RUVIA_CHECK(responseHeadSubmitted(headResult));
    conn.consumeOutput(conn.pendingOutput().size());
    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// Dropping the last owner while the local response is still open must produce an
// explicit terminal transition, rather than silently erasing a peer-visible stream.
RUVIA_TEST(http2_connection_unpin_incomplete_stream_emits_cancel_reset) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);

    conn.unpinStream(1);

    RUVIA_CHECK(conn.stream(1) == nullptr);
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
}

// RFC 8441 Extended CONNECT: a CONNECT + :protocol=websocket head emits kMessageHead
// with NO kMessageEnd (the tunnel stays open), and the stream carries the
// extendedConnectWebSocket mark for the owner's route policy. The handshake atomically
// opens the tunnel, tunnel DATA flows as kTunnelData events (no content-length
// required), and submitData carries frames
// back on the still-open stream.
RUVIA_TEST(http2_connection_websocket_tunnel_handshake_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "websocket");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/ws");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));

    bool sawHeaders = false;
    bool sawEnd = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* head = event->messageHead();
            head != nullptr && head->streamId() == 1) {
            sawHeaders = true;
        }
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
        }
    }
    RUVIA_CHECK(sawHeaders);
    RUVIA_CHECK(!sawEnd);  // the tunnel must stay open

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == Http2ConnectForm::kExtended);
    RUVIA_CHECK(stream->protocolIsWebSocket());

    // Owner route policy admitted a WebSocket route: answer 200 and open the tunnel.
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitWebSocketHandshake(1, "chat") ==
        Http2SubmitStatus::kAccepted);

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() > 9);
    const auto head = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(head.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK_EQ(head.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stream open
    conn.consumeOutput(out.size());

    // Inbound tunnel bytes (a would-be masked frame) surface as tunnel DATA even with
    // no content-length: the tunnel is exempt from body accounting.
    char data[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(data, 4, Http2FrameType::kData, 0, 1);
    std::memcpy(data + 9, "\x81\x80\x01\x02", 4);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    bool sawChunk = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* tunnelData = event->tunnelData();
            tunnelData != nullptr && tunnelData->streamId() == 1 &&
            tunnelData->bytes().size() == 4) {
            sawChunk = true;
        }
    }
    RUVIA_CHECK(sawChunk);

    // Outbound tunnel frames ride submitData on the still-open stream.
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "\x81\x02hi", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    const auto frameOut = conn.pendingOutput();
    const auto dataHead = ruvia::detail::http2ParseFrameHeader(frameOut.substr(0, 9));
    RUVIA_CHECK_EQ(dataHead.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((dataHead.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    RUVIA_CHECK(frameOut.substr(9) == std::string_view("\x81\x02hi"));
}

namespace {

using ruvia::detail::Http2Role;

// Byte shuttle between two cores (no sockets): move pending output of `from` into
// `to`, draining `to`'s events into the collectors first would lose them -- so the
// caller passes a per-hop event sink invoked after every feed.
template <typename OnEvent>
void shuttleOnce(Http2Connection& from, Http2Connection& to, OnEvent&& onEvent) {
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

}  // namespace

// Client role end-to-end against the server core with ZERO I/O: the client core opens
// stream 1, sends a GET, the server core dispatches a 200 "pong", and the client core
// surfaces the response head (status via the stream state), body chunk, and end.
RUVIA_TEST(http2_connection_client_role_get_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();

    std::string clientBody;
    std::uint16_t status = 0;
    bool clientSawHead = false;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (const auto* messageEnd = event.messageEnd()) {
            const auto streamId = messageEnd->streamId();
            ruvia::HttpResponse response(&resource);
            response.status(200);
            response.setBodyCopy("pong");
            RUVIA_CHECK(responseHeadSubmitted(
                server.submitResponseHead(streamId, response)));
            RUVIA_CHECK(server.submitData(
                streamId, "pong", Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kAccepted);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (const auto* messageHead = event.messageHead()) {
            clientSawHead = true;
            if (auto* stream = client.stream(messageHead->streamId())) {
                status = stream->responseStatus();
            }
        } else if (const auto* bodyChunk = event.messageBodyChunk()) {
            clientBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (event.messageEnd() != nullptr) {
            clientSawEnd = true;
        }
    };

    const auto request = client.submitRegularRequestHead(
        "GET", "http", "example.com", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    RUVIA_CHECK_EQ(streamId, static_cast<std::uint32_t>(1));
    client.pinStream(streamId);
    const auto requestBytes = client.pendingOutput().size();
    RUVIA_CHECK(client.submitData(streamId, "forbidden", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(client.pendingOutput().size(), requestBytes);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(clientSawHead);
    RUVIA_CHECK_EQ(status, static_cast<std::uint16_t>(200));
    RUVIA_CHECK(clientBody == "pong");
    RUVIA_CHECK(clientSawEnd);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    // content-length was decoded into the stream (auto CL from the server head).
    const auto* remoteKnownLength = stream->remoteContent().knownLength();
    RUVIA_CHECK(remoteKnownLength != nullptr);
    RUVIA_CHECK_EQ(
        remoteKnownLength->declaredLength(), std::size_t{4});
    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

// Client role POST: the request body flows through submitData with END_STREAM, the
// server core buffers it (owner-side append) and answers; both directions complete.
RUVIA_TEST(http2_connection_client_role_post_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();

    std::string serverBody;
    std::string clientBody;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (const auto* bodyChunk = event.messageBodyChunk()) {
            serverBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (const auto* messageEnd = event.messageEnd()) {
            const auto streamId = messageEnd->streamId();
            ruvia::HttpResponse response(&resource);
            response.status(200);
            response.setBodyCopy(serverBody);
            RUVIA_CHECK(responseHeadSubmitted(
                server.submitResponseHead(streamId, response)));
            RUVIA_CHECK(server.submitData(
                streamId,
                std::string_view(serverBody.data(), serverBody.size()),
                Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (const auto* bodyChunk = event.messageBodyChunk()) {
            clientBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (event.messageEnd() != nullptr) {
            clientSawEnd = true;
        }
    };

    const auto request = client.submitRegularRequestHead(
        "POST", "http", "example.com", "/echo", {},
        Http2RequestContent::knownLength(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    RUVIA_CHECK(client.submitData(streamId, "hello", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(serverBody == "hello");
    RUVIA_CHECK(clientBody == "hello");
    RUVIA_CHECK(clientSawEnd);
}

// A 1xx interim head is validated and skipped (no events, stream not decoded); the
// following 200 head is the one surfaced. Hand-encoded server bytes drive the client.
RUVIA_TEST(http2_connection_client_role_interim_response_skipped) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());

    const auto request = client.submitRegularRequestHead(
        "GET", "http", "example.com", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    // Server bytes: SETTINGS, then HEADERS(103), then HEADERS(200) + DATA END_STREAM.
    std::pmr::string bytes(&resource);
    {
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        bytes.append(settings, sizeof(settings));
        std::pmr::string interim(&resource);
        HpackEncoder::encodeHeader(interim, ":status", "103");
        HpackEncoder::encodeHeader(interim, "link", "</style.css>; rel=preload");
        const auto interimFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(interim.data(), interim.size()));
        bytes.append(interimFrame.data(), interimFrame.size());
        std::pmr::string final_(&resource);
        HpackEncoder::encodeHeader(final_, ":status", "200");
        HpackEncoder::encodeHeader(final_, "content-length", "2");
        const auto finalFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(final_.data(), final_.size()));
        bytes.append(finalFrame.data(), finalFrame.size());
        char data[9 + 2];
        ruvia::detail::http2EncodeFrameHeader(data, 2, Http2FrameType::kData,
            ruvia::detail::kHttp2FlagEndStream, streamId);
        std::memcpy(data + 9, "ok", 2);
        bytes.append(data, sizeof(data));
    }
    (void)client.feed(std::string_view(bytes.data(), bytes.size()));

    int heads = 0;
    std::string body;
    bool end = false;
    while (const auto event = client.nextEvent()) {
        if (event->messageHead() != nullptr) {
            ++heads;
        }
        if (const auto* bodyChunk = event->messageBodyChunk()) {
            body.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        }
        if (event->messageEnd() != nullptr) {
            end = true;
        }
    }
    RUVIA_CHECK_EQ(heads, 1);  // only the final head is surfaced
    RUVIA_CHECK(body == "ok");
    RUVIA_CHECK(end);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->responseStatus(), static_cast<std::uint16_t>(200));
    RUVIA_CHECK_EQ(static_cast<int>(stream->interimResponseCount()), 1);
    RUVIA_CHECK(!client.connectionError().has_value());
}

// A HEAD response's Content-Length is representation metadata, not a DATA
// contract. The same exemption must apply when trailing HEADERS, rather than the
// initial response HEADERS, carries END_STREAM.
RUVIA_TEST(http2_connection_client_head_representation_length_survives_trailer_terminal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "HEAD", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-length", "10");
    const auto responseHead = headersFrame(
        &resource,
        streamId,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() ==
        Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());
    const auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    const auto* known = stream->remoteContent().knownLength();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{10});
    RUVIA_CHECK_EQ(stream->remoteContent().receivedBytes(), std::size_t{0});

    std::pmr::string trailers(&resource);
    HpackEncoder::encodeHeader(trailers, "server-timing", "db;dur=4");
    const auto trailerHead = headersFrame(
        &resource,
        streamId,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(trailers.data(), trailers.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(trailerHead.data(), trailerHead.size())) ==
        Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() ==
        Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(streamId)->peerEndStream());

    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

// Client role protocol errors: HEADERS on an odd stream never opened is a connection
// error, and HEADERS on an even (server-initiated) stream is one too (push disabled).
RUVIA_TEST(http2_connection_client_role_rejects_unexpected_streams) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection client(&resource, Http2Role::kClient);
        client.beginConnection();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto idle = headersFrame(
            &resource, 5, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(idle.data(), idle.size()));
        RUVIA_CHECK(client.connectionError().has_value());  // HEADERS on idle stream -> GOAWAY
    }
    {
        Http2Connection client(&resource, Http2Role::kClient);
        client.beginConnection();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto even = headersFrame(
            &resource, 2, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(even.data(), even.size()));
        RUVIA_CHECK(client.connectionError().has_value());  // no push: even ids are never valid
    }
}

RUVIA_TEST(http2_connection_goaway_rejects_unprocessed_requests_in_core) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshakeWithWindow(client, 0);

    const auto first = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/first", {},
        Http2RequestContent::none());
    const auto second = client.submitRegularRequestHead(
        "POST", "https", "example.test", "/upload", {},
        Http2RequestContent::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    RUVIA_CHECK(second.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    const auto secondStreamId = submittedRequestStreamId(second);
    RUVIA_CHECK_EQ(firstStreamId, std::uint32_t{1});
    RUVIA_CHECK_EQ(secondStreamId, std::uint32_t{3});
    client.pinStream(secondStreamId);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(client.submitData(
        secondStreamId, "queued", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(client.hasQueuedData(secondStreamId));

    const auto goaway = goawayFrame(
        &resource, firstStreamId, Http2ErrorCode::kNoError);
    const auto result = client.feed(
        std::string_view(goaway.data(), goaway.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.draining());
    const auto reciprocal = client.pendingOutput();
    const auto reciprocalHead = ruvia::detail::http2ParseFrameHeader(
        reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(
        reciprocalHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kNoError));
    client.consumeOutput(reciprocal.size());

    const auto info = client.peerGoaway();
    RUVIA_CHECK(info.has_value());
    RUVIA_CHECK_EQ(info->lastStreamId(), firstStreamId);
    RUVIA_CHECK(info->error() == Http2ErrorCode::kNoError);
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), firstStreamId);
    RUVIA_CHECK(event.goaway()->error() == Http2ErrorCode::kNoError);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kRequestUnprocessed);
    RUVIA_CHECK_EQ(
        event.requestUnprocessed()->streamId(), secondStreamId);
    RUVIA_CHECK(!client.nextEvent().has_value());

    auto* firstStream = client.stream(firstStreamId);
    auto* secondStream = client.stream(secondStreamId);
    RUVIA_CHECK(firstStream != nullptr && !firstStream->isAborted());
    RUVIA_CHECK(secondStream != nullptr && secondStream->isAborted());
    RUVIA_CHECK(secondStream->localSend().aborted() != nullptr);
    RUVIA_CHECK(secondStream->localSend().aborted()->source() ==
        ruvia::detail::Http2StreamCloseSource::kPeerGoaway);
    RUVIA_CHECK(!secondStream->releasePeerConcurrencySlot());
    RUVIA_CHECK(!client.hasQueuedData(secondStreamId));
    RUVIA_CHECK(requestHeadSubmitError(client.submitRegularRequestHead(
        "GET", "https", "example.test", "/new", {},
        Http2RequestContent::none())) ==
        Http2RequestHeadSubmitError::kConnectionUnavailable);

    client.unpinStream(secondStreamId);
    RUVIA_CHECK(client.stream(secondStreamId) == nullptr);

    // GOAWAY does not abort a request at or below the peer boundary. Its response can
    // still complete after the reciprocal local drain has started.
    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(
        &resource,
        firstStreamId,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(!client.connectionError().has_value());
}

RUVIA_TEST(http2_connection_goaway_last_stream_id_is_monotonic) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    for (const std::string_view path : {"/one", "/two", "/three"}) {
        RUVIA_CHECK(client.submitRegularRequestHead(
            "GET", "https", "example.test", path, {},
            Http2RequestContent::none()).submitted() != nullptr);
    }
    client.consumeOutput(client.pendingOutput().size());

    const auto notice = goawayFrame(
        &resource, 0x7fffffffU, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(notice.data(), notice.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(
        event.goaway()->lastStreamId(), std::uint32_t{0x7fffffffU});
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.stream(5) != nullptr);
    RUVIA_CHECK(client.draining());
    RUVIA_CHECK(!client.pendingOutput().empty());  // reciprocal GOAWAY(NO_ERROR)
    client.consumeOutput(client.pendingOutput().size());

    const auto narrowed = goawayFrame(
        &resource, 3, Http2ErrorCode::kInternalError);
    RUVIA_CHECK(client.feed(
        std::string_view(narrowed.data(), narrowed.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), std::uint32_t{3});
    RUVIA_CHECK(event.goaway()->error() == Http2ErrorCode::kInternalError);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kRequestUnprocessed);
    RUVIA_CHECK_EQ(
        event.requestUnprocessed()->streamId(), std::uint32_t{5});
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.stream(5) == nullptr);
    const auto narrowedInfo = client.peerGoaway();
    RUVIA_CHECK(narrowedInfo.has_value());
    RUVIA_CHECK_EQ(narrowedInfo->lastStreamId(), std::uint32_t{3});
    RUVIA_CHECK(narrowedInfo->error() == Http2ErrorCode::kInternalError);
    RUVIA_CHECK(client.pendingOutput().empty());  // local drain is idempotent

    const auto invalidIncrease = goawayFrame(
        &resource, 5, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(invalidIncrease.data(), invalidIncrease.size())) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError() == Http2ErrorCode::kProtocolError);
    RUVIA_CHECK_EQ(client.peerGoaway()->lastStreamId(), std::uint32_t{3});
    const auto localGoaway = client.pendingOutput();
    const auto head = ruvia::detail::http2ParseFrameHeader(
        localGoaway.substr(0, 9));
    RUVIA_CHECK_EQ(head.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(localGoaway.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_goaway_cannot_exclude_a_started_response) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(
        &resource,
        streamId,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());

    const auto contradictory = goawayFrame(
        &resource, 0, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(contradictory.data(), contradictory.size())) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError() == Http2ErrorCode::kProtocolError);
    RUVIA_CHECK(!client.peerGoaway().has_value());
    RUVIA_CHECK(client.stream(streamId) != nullptr);
    RUVIA_CHECK(!client.stream(streamId)->isAborted());
    RUVIA_CHECK(!client.nextEvent().has_value());
}

RUVIA_TEST(http2_connection_peer_goaway_drains_without_truncating_server_request) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource, Http2Role::kServer);
    handshake(server);

    const auto requestHead = postHeadFrame(&resource, "4");
    RUVIA_CHECK(server.feed(
        std::string_view(requestHead.data(), requestHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    auto event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK_EQ(event.messageHead()->streamId(), std::uint32_t{1});
    RUVIA_CHECK(!server.nextEvent().has_value());

    // The client GOAWAY and the rest of an already established request can share one
    // transport read. The core must consume both frames and preserve event order.
    const auto peerGoaway = goawayFrame(
        &resource, 0, Http2ErrorCode::kNoError);
    const auto requestBody = dataFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagEndStream,
        "body");
    std::pmr::string batch(&resource);
    batch.append(peerGoaway.data(), peerGoaway.size());
    batch.append(requestBody.data(), requestBody.size());
    const auto result = server.feed(
        std::string_view(batch.data(), batch.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    RUVIA_CHECK(server.draining());
    RUVIA_CHECK(server.peerGoaway().has_value());

    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), std::uint32_t{0});
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK(event.messageBodyChunk()->bytes() == "body");
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK_EQ(event.messageEnd()->streamId(), std::uint32_t{1});
    RUVIA_CHECK(!server.nextEvent().has_value());

    // Reciprocal GOAWAY uses the highest accepted client stream (1), not the peer's
    // directional last-stream-id (which refers to server-initiated streams).
    const auto reciprocal = server.pendingOutput();
    const auto reciprocalHead = ruvia::detail::http2ParseFrameHeader(
        reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(
        reciprocalHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{1});
    server.consumeOutput(reciprocal.size());

    // A stream opened after our advertised boundary is safely refused, while stream 1
    // remains response-capable.
    std::pmr::string lateBlock(&resource);
    encodeGetRequest(lateBlock);
    const auto late = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(lateBlock.data(), lateBlock.size()));
    RUVIA_CHECK(server.feed(
        std::string_view(late.data(), late.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.nextEvent().has_value());
    const auto reset = server.pendingOutput();
    const auto resetHead = ruvia::detail::http2ParseFrameHeader(reset.substr(0, 9));
    RUVIA_CHECK_EQ(
        resetHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(resetHead.streamId, std::uint32_t{3});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kRefusedStream));
    server.consumeOutput(reset.size());

    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("ok");
    RUVIA_CHECK(responseHeadSubmitted(
        server.submitResponseHead(1, response)));
    RUVIA_CHECK(server.submitData(
        1, "ok", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
}

// Graceful drain (RFC 9113 §6.8): beginDrain emits GOAWAY(NO_ERROR) at the last
// accepted stream id; streams already open keep working, HEADERS for a higher id are
// refused with RST_STREAM(REFUSED_STREAM), and beginDrain is idempotent.
RUVIA_TEST(http2_connection_begin_drain_refuses_new_streams) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 open (half-closed remote)
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();
    // GOAWAY(NO_ERROR, lastStreamId=1) emitted without a connection error.
    const auto goaway = conn.pendingOutput();
    RUVIA_CHECK(goaway.size() >= 9);
    const auto gh = ruvia::detail::http2ParseFrameHeader(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(gh.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.draining());
    conn.consumeOutput(goaway.size());

    // A new stream ABOVE the advertised id (3) is refused only after its complete
    // multi-frame field block is decoded. The inserted dynamic entry must survive.
    std::pmr::string block(&resource);
    encodeGetRequest(block);
    encodeShortDynamicHeader(block, "x-refused", "indexed");
    const auto split = block.size() / 2;
    const auto h = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), split));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());
    const auto continuation = continuationFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data() + split, block.size() - split));
    (void)conn.feed(std::string_view(continuation.data(), continuation.size()));
    while (conn.nextEvent().has_value()) {
        // stream 3 must NOT surface as a request (it was refused)
    }
    const auto rst = conn.pendingOutput();
    RUVIA_CHECK(rst.size() >= 9);
    const auto rh = ruvia::detail::http2ParseFrameHeader(rst.substr(0, 9));
    RUVIA_CHECK_EQ(rh.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rh.streamId, static_cast<std::uint32_t>(3));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(rst.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kRefusedStream));
    RUVIA_CHECK(!conn.connectionError().has_value());  // refusal is not a connection error
    conn.consumeOutput(rst.size());

    // A later refused stream can reference the dynamic entry created by stream 3;
    // successful decode yields another REFUSED_STREAM, never COMPRESSION_ERROR.
    std::pmr::string dependent(&resource);
    encodeGetRequest(dependent);
    HpackEncoder::encodeIndexed(dependent, 62);
    const auto h5 = headersFrame(
        &resource,
        5,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(dependent.data(), dependent.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h5.data(), h5.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    const auto rst5 = conn.pendingOutput();
    const auto rh5 = ruvia::detail::http2ParseFrameHeader(rst5.substr(0, 9));
    RUVIA_CHECK_EQ(rh5.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rh5.streamId, static_cast<std::uint32_t>(5));
    RUVIA_CHECK(!conn.connectionError().has_value());
    conn.consumeOutput(rst5.size());

    // Stream 1 (opened before the drain) can still be answered.
    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("ok");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitResponseHead(1, response)));
    RUVIA_CHECK(conn.submitData(1, "ok", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.pendingOutput().size() > 9);  // response frames produced
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();  // idempotent: no further GOAWAY
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A streaming consumer's banked receive-window debt (deferStreamWindowRelease) must
// return to the CONNECTION window when the stream is removed, even if the owner never
// calls releaseStreamWindow -- otherwise the connection window shrinks permanently.
RUVIA_TEST(http2_connection_window_debt_flushed_on_removal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, content-length) and mark it deferred-release.
    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "content-length", "5");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {}
    conn.deferStreamWindowRelease(1);
    conn.consumeOutput(conn.pendingOutput().size());

    // Feed 5 body bytes: the receive-window credit is BANKED (deferred), so NO
    // per-frame WINDOW_UPDATE is emitted.
    char data[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(data, 5, Http2FrameType::kData, 0, 1);
    std::memcpy(data + 9, "hello", 5);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    while (conn.nextEvent().has_value()) {}
    RUVIA_CHECK(conn.pendingOutput().empty());  // debt banked, not advertised

    // Peer RST_STREAM removes the (unpinned) stream. The banked 5 bytes must be
    // returned to the CONNECTION window via a stream-0 WINDOW_UPDATE.
    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.nextEvent().has_value()) {}

    bool sawConnWindowUpdate = false;
    auto out = conn.pendingOutput();
    while (out.size() >= 9) {
        const auto fh = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        if (fh.type == static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate) && fh.streamId == 0) {
            const auto inc = ruvia::detail::http2WindowUpdateIncrement(out.substr(9, 4));
            if (inc == 5) sawConnWindowUpdate = true;
        }
        out = out.substr(9 + fh.length);
    }
    RUVIA_CHECK(sawConnWindowUpdate);  // connection window self-healed on removal
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_discarded_data_returns_full_payload_credit_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    conn.consumeOutput(conn.pendingOutput().size());

    // The flow-controlled length is 6: one Pad Length byte, two data bytes, and
    // three padding bytes. A closed stream has no stream window to restore, but the
    // accepted connection debit must be returned exactly once at connection scope.
    constexpr char paddedPayload[] = {3, 'o', 'k', 0, 0, 0};
    const auto discarded = dataFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagPadded,
        std::string_view(paddedPayload, sizeof(paddedPayload)));
    RUVIA_CHECK(conn.feed(
        std::string_view(discarded.data(), discarded.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());

    auto out = conn.pendingOutput();
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(
        reset.type,
        static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, std::uint32_t{1});
    out.remove_prefix(9 + reset.length);

    const auto update = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(
        update.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(update.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(out.substr(9, 4)),
        std::uint32_t{sizeof(paddedPayload)});
    out.remove_prefix(9 + update.length);
    RUVIA_CHECK(out.empty());
}

RUVIA_TEST(http2_connection_discarded_data_still_enforces_connection_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Stream 1 banks valid DATA instead of returning WINDOW_UPDATE, allowing the test
    // to exhaust the shared connection receive window without exceeding its stream
    // window or application body limit.
    const auto streamingHead = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(
        std::string_view(streamingHead.data(), streamingHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    conn.deferStreamWindowRelease(1);

    // Open then locally reset stream 3 so later DATA targets a known closed stream.
    std::pmr::string closedBlock(&resource);
    encodeGetRequest(closedBlock);
    const auto closedHead = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(closedBlock.data(), closedBlock.size()));
    RUVIA_CHECK(conn.feed(
        std::string_view(closedHead.data(), closedHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.submitReset(3, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(conn.stream(3) == nullptr);
    conn.consumeOutput(conn.pendingOutput().size());

    std::string chunk(Http2LocalSettings::kMaxFrameSize, 'x');
    std::uint32_t remaining = Http2LocalSettings::kInitialWindowSize;
    while (remaining != 0) {
        const auto chunkBytes = static_cast<std::size_t>(
            remaining < chunk.size() ? remaining : chunk.size());
        const auto data = dataFrame(
            &resource,
            1,
            0,
            std::string_view(chunk.data(), chunkBytes));
        RUVIA_CHECK(conn.feed(
            std::string_view(data.data(), data.size())) ==
            ruvia::detail::Http2FeedResult::kAccepted);
        const auto event = conn.nextEvent().value();
        RUVIA_CHECK(event.kind() == Http2EventKind::kMessageBodyChunk);
        RUVIA_CHECK_EQ(event.messageBodyChunk()->bytes().size(), chunkBytes);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());
        remaining -= static_cast<std::uint32_t>(chunkBytes);
    }

    // Even though stream 3 is closed and its DATA will be discarded, the DATA must
    // first fit the connection window. At zero remaining credit this is a connection
    // FLOW_CONTROL_ERROR, not RST_STREAM plus an unearned WINDOW_UPDATE.
    const auto overflow = dataFrame(&resource, 3, 0, "x");
    const auto result = conn.feed(
        std::string_view(overflow.data(), overflow.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kFlowControlError);
    const auto goaway = conn.pendingOutput();
    const auto goawayHead = ruvia::detail::http2ParseFrameHeader(
        goaway.substr(0, 9));
    RUVIA_CHECK_EQ(
        goawayHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(goaway.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kFlowControlError));
}

// Server-role trailers: a trailing HEADERS block WITHOUT END_STREAM is a protocol
// error on that stream (RFC 9113 §8.1) -- the core RSTs and closes it, no kMessageEnd.
RUVIA_TEST(http2_connection_server_trailers_without_end_stream_rejected) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, no END_STREAM on HEADERS).
    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {}
    conn.consumeOutput(conn.pendingOutput().size());

    // A trailer HEADERS block with END_HEADERS but NO END_STREAM -> stream error.
    std::pmr::string trailer(&resource);
    HpackEncoder::encodeHeader(trailer, "x-checksum", "abc");
    const auto t = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,  // deliberately no END_STREAM
        std::string_view(trailer.data(), trailer.size()));
    (void)conn.feed(std::string_view(t.data(), t.size()));

    bool sawClosed = false;
    bool sawEnd = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* closed = event->streamClosed()) {
            sawClosed = true;
            RUVIA_CHECK(
                closed->source() ==
                ruvia::detail::Http2StreamCloseSource::kLocal);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
        }
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
        }
    }
    RUVIA_CHECK(sawClosed);
    RUVIA_CHECK(!sawEnd);            // never completes the request
    RUVIA_CHECK(!conn.connectionError().has_value());    // stream error, not connection error
    const auto rst = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK(conn.stream(1) == nullptr);  // removed, not leaked
}

// Semantic response trailers queued behind a window-blocked body: the HEADERS must be
// emitted AFTER the deferred DATA drains (RFC 9113 §8.1), carrying END_STREAM in place
// of it -- never ahead of the body bytes.
RUVIA_TEST(http2_connection_trailers_wait_for_blocked_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 4);  // tiny 4-byte stream send window
    driveGetRequest(conn, &resource);
    conn.consumeOutput(conn.pendingOutput().size());

    // Streaming response head declares an exact 8-byte content length. Only 4 DATA
    // bytes fit the window, so the other 4 are core-owned and deferred -> kQueued.
    ruvia::HttpResponse head(&resource);
    head.status(200);
    head.header("Content-Length", "8");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(head),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "AAAABBBB", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kQueued);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{4});

    // First 4 bytes went out as DATA (no END_STREAM).
    auto out = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(4));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(out.size());

    // Queue trailers while the remaining 4 bytes are still window-blocked.
    const std::array<ruvia::HttpHeaderView, 1> invalidTrailers{
        ruvia::HttpHeaderView{"Content-Length", "8"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, invalidTrailers) ==
        Http2ResponseTrailerSubmitStatus::kInvalidField);
    RUVIA_CHECK(stream->responseTrailerBlock().empty());
    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.submitResponseTrailerSection(1, trailers) ==
        Http2ResponseTrailerSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.finishResponse(1) == Http2FinishSubmitStatus::kQueued);
    RUVIA_CHECK(conn.pendingOutput().empty());  // nothing emitted yet (still blocked)

    // Peer WINDOW_UPDATE reopens the window: the deferred DATA drains, THEN the trailer
    // HEADERS(END_STREAM) follows -- in that order.
    char wu[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(wu, 4, Http2FrameType::kWindowUpdate, 0, 1);
    ruvia::detail::http2Write32(wu + 9, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    while (conn.nextEvent().has_value()) {}

    out = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(4));  // the remaining body
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // NOT on the DATA
    out = out.substr(9 + d2.length);
    const auto th = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(th.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));  // trailer
    RUVIA_CHECK((th.flags & ruvia::detail::kHttp2FlagEndStream) != 0);  // END_STREAM here
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{8});
    const auto trailerPayload = out.substr(9, th.length);
    RUVIA_CHECK(trailerPayload.find("x-checksum") != std::string_view::npos);
    RUVIA_CHECK(trailerPayload.find("X-Checksum") == std::string_view::npos);
}

// --- flood defense-in-depth budgets (GOAWAY ENHANCE_YOUR_CALM) -----------------

namespace {
// Walk the outbound buffer frame-by-frame and return the error code of the first GOAWAY,
// or 0xffffffff if none is present.
std::uint32_t firstGoawayError(std::string_view out) {
    std::size_t pos = 0;
    while (pos + 9 <= out.size()) {
        const auto h = ruvia::detail::http2ParseFrameHeader(out.substr(pos, 9));
        if (h.type == static_cast<std::uint8_t>(Http2FrameType::kGoaway) && h.length >= 8) {
            const auto* p = reinterpret_cast<const unsigned char*>(out.data() + pos + 9);
            return (static_cast<std::uint32_t>(p[4]) << 24) |
                   (static_cast<std::uint32_t>(p[5]) << 16) |
                   (static_cast<std::uint32_t>(p[6]) << 8) |
                   static_cast<std::uint32_t>(p[7]);
        }
        pos += 9 + h.length;
    }
    return 0xffffffffU;
}
constexpr std::uint32_t kEnhanceYourCalm =
    static_cast<std::uint32_t>(ruvia::detail::Http2ErrorCode::kEnhanceYourCalm);
}  // namespace

// A peer that floods PINGs without ever letting us flush the echoed ACKs is cut off with
// GOAWAY(ENHANCE_YOUR_CALM) instead of accumulating unbounded ACK bytes.
RUVIA_TEST(http2_connection_ping_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memset(ping + 9, 0, 8);

    bool tripped = false;
    for (int i = 0; i < 1200 && !tripped; ++i) {
        // Deliberately do NOT drain output between pings, so the un-drained PING budget
        // accumulates (consumeOutput would reset it -- see the keepalive test below).
        tripped = conn.feed(std::string_view(ping, sizeof(ping))) ==
                  ruvia::detail::Http2FeedResult::kProtocolFailure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}

// Healthy keepalive PINGs (ACKs drained each round) never trip the budget, however many.
RUVIA_TEST(http2_connection_drained_pings_never_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memset(ping + 9, 0, 8);

    for (int i = 0; i < 5000; ++i) {
        const auto r = conn.feed(std::string_view(ping, sizeof(ping)));
        RUVIA_CHECK(r == ruvia::detail::Http2FeedResult::kAccepted);
        conn.consumeOutput(conn.pendingOutput().size());  // flush ACK -> resets budget
        RUVIA_CHECK(!conn.connectionError().has_value());
    }
}

// A rapid-reset flood (open a stream, RST it, repeat -- never letting a response finish)
// is cut off with GOAWAY(ENHANCE_YOUR_CALM); the 128-stream cap alone never trips because
// each RST immediately frees the slot (CVE-2023-44487).
RUVIA_TEST(http2_connection_rapid_reset_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);

    bool tripped = false;
    for (std::uint32_t sid = 1; sid < 1U + 2U * 1200U; sid += 2) {
        const auto h = headersFrame(
            &resource, sid,
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));
        if (conn.feed(std::string_view(h.data(), h.size())) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure) {
            tripped = true;
            break;
        }
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());

        char rst[9 + 4];
        ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, sid);
        ruvia::detail::http2Write32(rst + 9, 0);
        if (conn.feed(std::string_view(rst, sizeof(rst))) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure) {
            tripped = true;
            break;  // leave the GOAWAY in the outbound buffer for inspection
        }
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}
