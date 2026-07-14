#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2TunnelState.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"

namespace {

using ruvia::detail::HpackDecoder;
using ruvia::detail::HpackEncoder;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2ConnectForm;
using ruvia::detail::Http2ConnectPending;
using ruvia::detail::Http2ConnectRejected;
using ruvia::detail::Http2DataSubmitStatus;
using ruvia::detail::Http2EndStream;
using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2Event;
using ruvia::detail::Http2EventKind;
using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2RequestHeadSubmitError;
using ruvia::detail::Http2RequestHeadSubmitResult;
using ruvia::detail::Http2Role;
using ruvia::detail::Http2SubmitStatus;
using ruvia::detail::Http2NotConnect;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http2TunnelOpen;
using ruvia::detail::Http2TunnelState;
using ruvia::detail::http2IsPendingWebSocketConnect;

template <typename T>
concept HasConnectForm = requires(const T& state) {
    { state.form() } -> std::same_as<Http2ConnectForm>;
};

template <typename T>
concept HasStaleTunnelKindPhase = requires(const T& state) {
    state.kind();
    state.phase();
};

static_assert(std::default_initializable<Http2TunnelState>);
static_assert(!std::default_initializable<Http2NotConnect>);
static_assert(!std::default_initializable<Http2ConnectPending>);
static_assert(!std::default_initializable<Http2TunnelOpen>);
static_assert(!std::default_initializable<Http2ConnectRejected>);
static_assert(!HasConnectForm<Http2TunnelState>);
static_assert(!HasConnectForm<Http2NotConnect>);
static_assert(HasConnectForm<Http2ConnectPending>);
static_assert(!HasConnectForm<Http2TunnelOpen>);
static_assert(!HasConnectForm<Http2ConnectRejected>);
static_assert(!HasStaleTunnelKindPhase<Http2TunnelState>);

std::uint32_t submittedRequestStreamId(
    const Http2RequestHeadSubmitResult& result) {
    if (const auto* submitted = result.submitted()) {
        return submitted->streamId();
    }
    throw std::runtime_error("HTTP/2 CONNECT head was not submitted");
}

Http2RequestHeadSubmitError requestHeadSubmitError(
    const Http2RequestHeadSubmitResult& result) {
    if (const auto* failure = result.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 CONNECT head did not fail");
}

struct HeaderObservation final {
    std::string method;
    std::string protocol;
    std::string scheme;
    std::string authority;
    std::string path;
    std::string status;
    std::size_t contentLengthCount{0};
};

bool observeHeader(void* target, std::string_view name, std::string_view value) {
    auto& observation = *static_cast<HeaderObservation*>(target);
    auto assign = [value](std::string& field) {
        field.assign(value.data(), value.size());
    };
    if (name == ":method") {
        assign(observation.method);
    } else if (name == ":protocol") {
        assign(observation.protocol);
    } else if (name == ":scheme") {
        assign(observation.scheme);
    } else if (name == ":authority") {
        assign(observation.authority);
    } else if (name == ":path") {
        assign(observation.path);
    } else if (name == ":status") {
        assign(observation.status);
    } else if (name == "content-length") {
        ++observation.contentLengthCount;
    }
    return true;
}

std::pmr::string frame(
    std::pmr::memory_resource* resource,
    Http2FrameType type,
    std::uint8_t flags,
    std::uint32_t streamId,
    std::string_view payload = {}) {
    std::pmr::string bytes(resource);
    char header[9];
    ruvia::detail::http2EncodeFrameHeader(
        header, static_cast<std::uint32_t>(payload.size()), type, flags, streamId);
    bytes.append(header, sizeof(header));
    bytes.append(payload.data(), payload.size());
    return bytes;
}

void handshake(Http2Connection& connection) {
    connection.beginConnection();
    connection.consumeOutput(connection.pendingOutput().size());
    if (connection.role() == Http2Role::kServer) {
        const auto preface = connection.feed(ruvia::detail::kHttp2ClientPreface);
        if (preface != ruvia::detail::Http2FeedResult::kAccepted) {
            throw std::runtime_error("server rejected valid client preface");
        }
    }
    const auto settings = frame(
        std::pmr::get_default_resource(), Http2FrameType::kSettings, 0, 0);
    const auto result = connection.feed(std::string_view(settings.data(), settings.size()));
    if (result != ruvia::detail::Http2FeedResult::kAccepted ||
        !connection.receivedPeerSettings()) {
        throw std::runtime_error("connection rejected valid initial SETTINGS");
    }
    connection.consumeOutput(connection.pendingOutput().size());
}

void beginClient(Http2Connection& client) {
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());
}

void enableExtendedConnect(Http2Connection& client) {
    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(
        settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kEnableConnectProtocol, 1);
    (void)out;
    (void)client.feed(std::string_view(settings, sizeof(settings)));
    client.consumeOutput(client.pendingOutput().size());
}

void drainEvents(Http2Connection& connection) {
    while (connection.nextEvent().has_value()) {
    }
}

void feedStandardConnect(
    Http2Connection& server,
    std::pmr::memory_resource* resource,
    std::uint32_t streamId = 1,
    std::uint8_t extraFlags = 0) {
    std::pmr::string block(resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":authority", "example.test:443");
    const auto request = frame(
        resource,
        Http2FrameType::kHeaders,
        static_cast<std::uint8_t>(ruvia::detail::kHttp2FlagEndHeaders | extraFlags),
        streamId,
        std::string_view(block.data(), block.size()));
    (void)server.feed(std::string_view(request.data(), request.size()));
}

void openStandardTunnel(
    Http2Connection& server,
    std::pmr::memory_resource* resource) {
    feedStandardConnect(server, resource);
    drainEvents(server);
    ruvia::HttpResponse response(resource);
    response.status(200);
    (void)server.submitConnectResponseHead(1, response);
    server.consumeOutput(server.pendingOutput().size());
}

HeaderObservation decodeSingleHeaderFrame(
    std::pmr::memory_resource* resource,
    std::string_view bytes,
    ruvia::testing::TestContext& ruvia_ctx) {
    HeaderObservation observation;
    RUVIA_CHECK(bytes.size() >= 9);
    if (bytes.size() < 9) {
        return observation;
    }
    const auto header = ruvia::detail::http2ParseFrameHeader(bytes.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK(bytes.size() >= 9 + header.length);
    if (bytes.size() < 9 + header.length) {
        return observation;
    }
    HpackDecoder decoder(resource);
    const auto decodeResult = decoder.decode(
        bytes.substr(9, header.length), &observation, &observeHeader);
    RUVIA_CHECK(decodeResult.decoded() != nullptr);
    return observation;
}

}  // namespace

RUVIA_TEST(http2_tunnel_state_alternatives_own_valid_transitions) {
    Http2TunnelState state;
    RUVIA_CHECK(state.notConnect() != nullptr);
    RUVIA_CHECK(state.pending() == nullptr);
    RUVIA_CHECK(state.open() == nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.accept());
    RUVIA_CHECK(!state.reject());
    RUVIA_CHECK(!state.begin(static_cast<Http2ConnectForm>(0xFF)));
    RUVIA_CHECK(state.notConnect() != nullptr);

    RUVIA_CHECK(state.begin(Http2ConnectForm::kStandard));
    RUVIA_CHECK(state.notConnect() == nullptr);
    RUVIA_CHECK(state.pending() != nullptr);
    RUVIA_CHECK(state.pending()->form() == Http2ConnectForm::kStandard);
    RUVIA_CHECK(state.open() == nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.begin(Http2ConnectForm::kExtended));
    RUVIA_CHECK(state.accept());
    RUVIA_CHECK(state.pending() == nullptr);
    RUVIA_CHECK(state.open() != nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.accept());
    RUVIA_CHECK(!state.reject());
    RUVIA_CHECK(!state.begin(Http2ConnectForm::kExtended));

    Http2TunnelState rejected;
    RUVIA_CHECK(rejected.begin(Http2ConnectForm::kExtended));
    RUVIA_CHECK(rejected.pending() != nullptr);
    RUVIA_CHECK(rejected.pending()->form() == Http2ConnectForm::kExtended);
    RUVIA_CHECK(rejected.reject());
    RUVIA_CHECK(rejected.pending() == nullptr);
    RUVIA_CHECK(rejected.open() == nullptr);
    RUVIA_CHECK(rejected.rejected() != nullptr);
    RUVIA_CHECK(!rejected.accept());
}

RUVIA_TEST(http2_connect_client_standard_head_owns_shape_and_gates_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    beginClient(client);

    const ruvia::HttpHeaderView host[] = {{"host", "example.test:443"}};
    const ruvia::HttpHeaderView length[] = {{"content-length", "0"}};
    const ruvia::HttpHeaderView transfer[] = {{"te", "trailers"}};
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitConnectRequestHead("example.test")) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitConnectRequestHead("example.test:0")) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitConnectRequestHead("example.test:443", host)) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitConnectRequestHead("example.test:443", length)) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitConnectRequestHead("example.test:443", transfer)) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    const auto submitted = client.submitConnectRequestHead("example.test:443");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(submitted);
    RUVIA_CHECK_EQ(streamId, std::uint32_t{1});
    const auto out = client.pendingOutput();
    const auto frameHeader = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK((frameHeader.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto observed = decodeSingleHeaderFrame(&resource, out, ruvia_ctx);
    RUVIA_CHECK_EQ(observed.method, std::string("CONNECT"));
    RUVIA_CHECK_EQ(observed.authority, std::string("example.test:443"));
    RUVIA_CHECK(observed.scheme.empty());
    RUVIA_CHECK(observed.path.empty());
    RUVIA_CHECK(observed.protocol.empty());
    RUVIA_CHECK_EQ(observed.contentLengthCount, std::size_t{0});

    const auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == Http2ConnectForm::kStandard);
    RUVIA_CHECK(stream->localSend().connectPending() != nullptr);
    RUVIA_CHECK(stream->localSend().tunnelOpen() == nullptr);
    RUVIA_CHECK(stream->localContent().forbidden() != nullptr);
    RUVIA_CHECK(client.submitData(streamId, "early", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kInvalidState);
}

RUVIA_TEST(http2_connect_client_extended_head_requires_setting_and_protocol_contract) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    beginClient(client);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitExtendedConnectRequestHead(
            "connect-udp", "https", "example.test", "/masque")) ==
        Http2RequestHeadSubmitError::kPeerCapabilityUnavailable);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    enableExtendedConnect(client);
    const ruvia::HttpHeaderView rawLength[] = {{"content-length", "0"}};
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitExtendedConnectRequestHead(
            "bad protocol", "https", "example.test", "/masque")) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitExtendedConnectRequestHead(
            "connect-udp", "https", "example.test", "relative")) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(requestHeadSubmitError(
        client.submitExtendedConnectRequestHead(
            "connect-udp", "https", "example.test", "/masque", rawLength)) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    const auto generic = client.submitExtendedConnectRequestHead(
        "connect-udp", "https", "example.test", "/masque");
    RUVIA_CHECK(generic.submitted() != nullptr);
    const auto genericStream = submittedRequestStreamId(generic);
    RUVIA_CHECK_EQ(genericStream, std::uint32_t{1});
    auto out = client.pendingOutput();
    const auto frameHeader = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK((frameHeader.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto observed = decodeSingleHeaderFrame(&resource, out, ruvia_ctx);
    RUVIA_CHECK_EQ(observed.method, std::string("CONNECT"));
    RUVIA_CHECK_EQ(observed.protocol, std::string("connect-udp"));
    RUVIA_CHECK_EQ(observed.scheme, std::string("https"));
    RUVIA_CHECK_EQ(observed.authority, std::string("example.test"));
    RUVIA_CHECK_EQ(observed.path, std::string("/masque"));
    const auto* genericPending =
        client.stream(genericStream)->tunnel().pending();
    RUVIA_CHECK(genericPending != nullptr);
    RUVIA_CHECK(genericPending->form() == Http2ConnectForm::kExtended);
    RUVIA_CHECK_EQ(
        client.stream(genericStream)->requestProtocol(), std::string_view("connect-udp"));
    client.consumeOutput(out.size());

    RUVIA_CHECK(requestHeadSubmitError(
        client.submitExtendedConnectRequestHead(
            "websocket", "https", "example.test", "/ws")) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    const ruvia::HttpHeaderView websocketHeaders[] = {
        {"sec-websocket-version", "13"}};
    const auto websocket = client.submitExtendedConnectRequestHead(
        "websocket",
        "https",
        "example.test",
        "/ws",
        websocketHeaders);
    RUVIA_CHECK(websocket.submitted() != nullptr);
    const auto websocketStream = submittedRequestStreamId(websocket);
    RUVIA_CHECK_EQ(websocketStream, std::uint32_t{3});
    RUVIA_CHECK(http2IsPendingWebSocketConnect(
        *client.stream(websocketStream)));
}

RUVIA_TEST(http2_connect_server_accepts_standard_tunnel_and_preserves_half_close) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    feedStandardConnect(server, &resource);

    auto event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!server.nextEvent().has_value());
    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == Http2ConnectForm::kStandard);

    ruvia::HttpResponse invalidBody(&resource);
    invalidBody.status(200);
    invalidBody.body("not tunnel metadata");
    RUVIA_CHECK(server.submitConnectResponseHead(1, invalidBody) ==
        Http2SubmitStatus::kInvalidMessage);
    ruvia::HttpResponse invalidLength(&resource);
    invalidLength.status(200);
    invalidLength.header("Content-Length", "0");
    RUVIA_CHECK(server.submitConnectResponseHead(1, invalidLength) ==
        Http2SubmitStatus::kInvalidMessage);
    ruvia::HttpResponse invalidConnection(&resource);
    invalidConnection.status(200);
    invalidConnection.header("Connection", "close");
    RUVIA_CHECK(server.submitConnectResponseHead(1, invalidConnection) ==
        Http2SubmitStatus::kInvalidMessage);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(stream->tunnel().pending() != nullptr);

    ruvia::HttpResponse accepted(&resource);
    accepted.status(200);
    accepted.header("X-Tunnel", "ready");
    RUVIA_CHECK(server.submitConnectResponseHead(1, accepted) ==
        Http2SubmitStatus::kAccepted);
    const auto responseBytes = server.pendingOutput();
    const auto responseFrame = ruvia::detail::http2ParseFrameHeader(responseBytes.substr(0, 9));
    RUVIA_CHECK((responseFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto observed = decodeSingleHeaderFrame(&resource, responseBytes, ruvia_ctx);
    RUVIA_CHECK_EQ(observed.status, std::string("200"));
    RUVIA_CHECK_EQ(observed.contentLengthCount, std::size_t{0});
    server.consumeOutput(responseBytes.size());
    RUVIA_CHECK(stream->tunnel().open() != nullptr);
    RUVIA_CHECK(stream->localSend().tunnelOpen() != nullptr);

    const auto peerData = frame(&resource, Http2FrameType::kData, 0, 1, "peer");
    (void)server.feed(std::string_view(peerData.data(), peerData.size()));
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kTunnelData);
    RUVIA_CHECK_EQ(event.tunnelData()->bytes(), std::string_view("peer"));
    drainEvents(server);
    server.consumeOutput(server.pendingOutput().size());

    const auto peerFin = frame(
        &resource, Http2FrameType::kData, ruvia::detail::kHttp2FlagEndStream, 1, "fin");
    (void)server.feed(std::string_view(peerFin.data(), peerFin.size()));
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kTunnelData);
    RUVIA_CHECK_EQ(event.tunnelData()->bytes(), std::string_view("fin"));
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kTunnelEnd);
    RUVIA_CHECK(!server.nextEvent().has_value());
    server.consumeOutput(server.pendingOutput().size());
    RUVIA_CHECK(stream->remoteReceive().endStream() != nullptr);

    // Peer FIN closes only its send half; the server can still finish its own half.
    RUVIA_CHECK(server.submitData(1, "reply", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    server.consumeOutput(server.pendingOutput().size());
    RUVIA_CHECK(server.submitData(1, {}, Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    server.consumeOutput(server.pendingOutput().size());

    const auto afterFin = frame(&resource, Http2FrameType::kData, 0, 1, "late");
    (void)server.feed(std::string_view(afterFin.data(), afterFin.size()));
    const auto resetBytes = server.pendingOutput();
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kStreamClosed));
    RUVIA_CHECK(!server.connectionError().has_value());
}

RUVIA_TEST(http2_connect_client_success_ignores_length_and_uses_tunnel_events) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitConnectRequestHead("example.test:443");
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-length", "not-a-number");
    const auto head = frame(
        &resource,
        Http2FrameType::kHeaders,
        ruvia::detail::kHttp2FlagEndHeaders,
        streamId,
        std::string_view(response.data(), response.size()));
    (void)client.feed(std::string_view(head.data(), head.size()));
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->tunnel().open() != nullptr);
    RUVIA_CHECK(stream->remoteContent().allowedWithoutLength() != nullptr);
    RUVIA_CHECK(stream->localSend().tunnelOpen() != nullptr);

    const auto data = frame(
        &resource,
        Http2FrameType::kData,
        ruvia::detail::kHttp2FlagEndStream,
        streamId,
        "opaque");
    (void)client.feed(std::string_view(data.data(), data.size()));
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kTunnelData);
    RUVIA_CHECK_EQ(event.tunnelData()->bytes(), std::string_view("opaque"));
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kTunnelEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    client.consumeOutput(client.pendingOutput().size());

    RUVIA_CHECK(client.submitData(streamId, "last", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
}

RUVIA_TEST(http2_connect_client_rejection_closes_request_half_and_decodes_response_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitConnectRequestHead("example.test:443");
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "407");
    HpackEncoder::encodeHeader(response, "content-length", "3");
    const auto head = frame(
        &resource,
        Http2FrameType::kHeaders,
        ruvia::detail::kHttp2FlagEndHeaders,
        streamId,
        std::string_view(response.data(), response.size()));
    (void)client.feed(std::string_view(head.data(), head.size()));
    const auto requestFin = client.pendingOutput();
    const auto fin = ruvia::detail::http2ParseFrameHeader(requestFin.substr(0, 9));
    RUVIA_CHECK_EQ(fin.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(fin.length, std::uint32_t{0});
    RUVIA_CHECK((fin.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    client.consumeOutput(requestFin.size());
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());

    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->tunnel().rejected() != nullptr);
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
    RUVIA_CHECK(stream->localSend().connectPending() == nullptr);
    RUVIA_CHECK(stream->localSend().tunnelOpen() == nullptr);
    RUVIA_CHECK(client.submitData(streamId, "tunnel?", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kInvalidState);

    const auto body = frame(
        &resource,
        Http2FrameType::kData,
        ruvia::detail::kHttp2FlagEndStream,
        streamId,
        "bad");
    (void)client.feed(std::string_view(body.data(), body.size()));
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(event.messageBodyChunk()->bytes(), std::string_view("bad"));
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
}

RUVIA_TEST(http2_connect_server_rejection_accepts_empty_terminal_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    feedStandardConnect(server, &resource);
    drainEvents(server);

    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remoteReceive().connectPending() != nullptr);

    ruvia::HttpResponse rejected(&resource);
    rejected.status(403);
    const auto submitted = server.submitResponseHead(
        1,
        rejected,
        ruvia::detail::httpBufferedResponseWritePlan(
            ruvia::HttpKnownMethod::kConnect,
            rejected));
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK(
        stream->remoteReceive().connectRejectedAwaitingEndStream() != nullptr);
    server.consumeOutput(server.pendingOutput().size());

    const auto emptyKeepOpen = frame(
        &resource, Http2FrameType::kData, 0, 1);
    RUVIA_CHECK(server.feed(
        std::string_view(emptyKeepOpen.data(), emptyKeepOpen.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(
        stream->remoteReceive().connectRejectedAwaitingEndStream() != nullptr);

    // A client that receives the non-2xx response closes its still-open CONNECT
    // request half with an empty DATA(END_STREAM). This is normal completion, not a
    // second request body and not a STREAM_CLOSED error.
    const auto terminal = frame(
        &resource,
        Http2FrameType::kData,
        ruvia::detail::kHttp2FlagEndStream,
        1);
    RUVIA_CHECK(server.feed(
        std::string_view(terminal.data(), terminal.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(!server.nextEvent().has_value());
    RUVIA_CHECK(stream->remoteReceive().endStream() != nullptr);
    RUVIA_CHECK(!server.connectionError().has_value());
}

RUVIA_TEST(http2_connect_pending_accepts_empty_request_half_close) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    feedStandardConnect(server, &resource);
    drainEvents(server);

    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remoteReceive().connectPending() != nullptr);
    const auto terminal = frame(
        &resource,
        Http2FrameType::kData,
        ruvia::detail::kHttp2FlagEndStream,
        1);
    RUVIA_CHECK(server.feed(
        std::string_view(terminal.data(), terminal.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(!server.nextEvent().has_value());
    RUVIA_CHECK(
        stream->remoteReceive().connectPendingEndStream() != nullptr);

    ruvia::HttpResponse accepted(&resource);
    accepted.status(200);
    RUVIA_CHECK(server.submitConnectResponseHead(1, accepted) ==
        Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(stream->remoteReceive().endStream() != nullptr);
    RUVIA_CHECK(server.nextEvent().value().kind() ==
        Http2EventKind::kTunnelEnd);
    RUVIA_CHECK(!server.nextEvent().has_value());
}

RUVIA_TEST(http2_connect_open_tunnel_replenishes_owner_released_stream_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    openStandardTunnel(server, &resource);
    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remoteReceive().tunnelOpen() != nullptr);

    const auto data = frame(&resource, Http2FrameType::kData, 0, 1, "peer");
    RUVIA_CHECK(server.feed(std::string_view(data.data(), data.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kTunnelData);
    RUVIA_CHECK(!server.nextEvent().has_value());
    RUVIA_CHECK(server.pendingOutput().empty());

    server.releaseReceivedData(1);
    const auto updates = server.pendingOutput();
    RUVIA_CHECK_EQ(
        updates.size(),
        std::size_t{2 * ruvia::detail::kHttp2WindowUpdateFrameBytes});
    const auto connectionUpdate =
        ruvia::detail::http2ParseFrameHeader(updates.substr(0, 9));
    const auto streamUpdate = ruvia::detail::http2ParseFrameHeader(
        updates.substr(ruvia::detail::kHttp2WindowUpdateFrameBytes, 9));
    RUVIA_CHECK_EQ(
        connectionUpdate.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(connectionUpdate.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        streamUpdate.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(streamUpdate.streamId, std::uint32_t{1});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(updates.substr(9, 4)),
        std::uint32_t{4});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(
            updates.substr(ruvia::detail::kHttp2WindowUpdateFrameBytes + 9, 4)),
        std::uint32_t{4});
}

RUVIA_TEST(http2_connect_server_rejects_data_before_acceptance) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    feedStandardConnect(server, &resource);
    drainEvents(server);

    const auto data = frame(&resource, Http2FrameType::kData, 0, 1, "early");
    (void)server.feed(std::string_view(data.data(), data.size()));
    const auto out = server.pendingOutput();
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    RUVIA_CHECK(server.stream(1) == nullptr);
    RUVIA_CHECK(!server.connectionError().has_value());
}

RUVIA_TEST(http2_connect_pending_stream_cannot_hide_invalid_data_padding) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    feedStandardConnect(server, &resource);
    drainEvents(server);

    const char malformedPayload[] = {5, 'x'};
    const auto malformed = frame(
        &resource,
        Http2FrameType::kData,
        ruvia::detail::kHttp2FlagPadded,
        1,
        std::string_view(malformedPayload, sizeof(malformedPayload)));
    const auto result = server.feed(
        std::string_view(malformed.data(), malformed.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(server.connectionError().has_value());
    const auto out = server.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connect_open_tunnel_rejects_headers_and_unknown_stream_frames) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection server(&resource);
        handshake(server);
        openStandardTunnel(server, &resource);
        std::pmr::string block(&resource);
        HpackEncoder::encodeHeader(block, "x-trailer", "forbidden");
        const auto headers = frame(
            &resource,
            Http2FrameType::kHeaders,
            static_cast<std::uint8_t>(
                ruvia::detail::kHttp2FlagEndHeaders |
                ruvia::detail::kHttp2FlagEndStream),
            1,
            std::string_view(block.data(), block.size()));
        (void)server.feed(std::string_view(headers.data(), headers.size()));
        const auto out = server.pendingOutput();
        const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2Read32(
                reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
        RUVIA_CHECK(!server.connectionError().has_value());
    }
    {
        Http2Connection server(&resource);
        handshake(server);
        openStandardTunnel(server, &resource);
        const auto unknown = frame(
            &resource, static_cast<Http2FrameType>(0xa), 0, 1);
        (void)server.feed(std::string_view(unknown.data(), unknown.size()));
        const auto out = server.pendingOutput();
        const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2Read32(
                reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
        RUVIA_CHECK(!server.connectionError().has_value());
    }
}

RUVIA_TEST(http2_connect_server_rejects_extended_before_advertising_capability) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "connect-udp");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":authority", "example.test");
    HpackEncoder::encodeHeader(block, ":path", "/masque");
    const auto request = frame(
        &resource,
        Http2FrameType::kHeaders,
        ruvia::detail::kHttp2FlagEndHeaders,
        1,
        std::string_view(block.data(), block.size()));
    const auto result = server.feed(std::string_view(request.data(), request.size()));

    RUVIA_CHECK(result ==
        ruvia::detail::Http2FeedResult::kConnectionNotStarted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(server.stream(1) == nullptr);
    RUVIA_CHECK(!server.connectionError().has_value());
}

RUVIA_TEST(http2_connect_server_retains_generic_extended_protocol) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "connect-udp");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":authority", "example.test");
    HpackEncoder::encodeHeader(block, ":path", "/.well-known/masque/udp");
    const auto request = frame(
        &resource,
        Http2FrameType::kHeaders,
        ruvia::detail::kHttp2FlagEndHeaders,
        1,
        std::string_view(block.data(), block.size()));
    (void)server.feed(std::string_view(request.data(), request.size()));
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!server.nextEvent().has_value());
    const auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == Http2ConnectForm::kExtended);
    RUVIA_CHECK(!http2IsPendingWebSocketConnect(*stream));
    RUVIA_CHECK_EQ(stream->requestProtocol(), std::string_view("connect-udp"));
    RUVIA_CHECK_EQ(stream->requestPath(), std::string_view("/.well-known/masque/udp"));
}
