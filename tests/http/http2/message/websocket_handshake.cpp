#include "test_harness.h"

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/message/Http2WebSocketHandshake.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::HpackDecoder;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::http2EncodeWebSocketHandshakeHeaders;
using ruvia::detail::Http2StreamState;
using ruvia::detail::makeWebSocketServerNegotiation;
using ruvia::detail::validateHttp2WebSocketHandshake;

template <typename T>
concept ExposesRvalueWebSocketServerSubprotocol = requires(T&& negotiation) { std::move(negotiation).subprotocol(); };

static_assert(!ExposesRvalueWebSocketServerSubprotocol<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::copy_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(std::move_constructible<ruvia::detail::WebSocketServerNegotiation>);

struct Collector final {
    std::vector<std::pair<std::string, std::string>> headers;
};

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<Collector*>(target)->headers.emplace_back(name, value);
    return true;
}

bool hasHeader(const Collector& fields, std::string_view name, std::string_view value) {
    for (const auto& field : fields.headers) {
        if (field.first == name && field.second == value) {
            return true;
        }
    }
    return false;
}

bool hasHeaderName(const Collector& fields, std::string_view name) {
    for (const auto& field : fields.headers) {
        if (field.first == name) {
            return true;
        }
    }
    return false;
}

HttpRequest parseRequest(std::string_view rawRequest) {
    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(rawRequest);
    return parsed.request;
}

HttpRequest requestWithProtocol() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "\r\n");
}

HttpRequest requestWithVersion() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");
}

HttpRequest requestWithBadVersion() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 8\r\n"
        "\r\n");
}

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

[[nodiscard]] bool acceptsWebSocketHandshake(const Http2StreamState& stream, const HttpRequest& request) {
    const auto result = validateHttp2WebSocketHandshake(stream, request);
    return result.accepted() != nullptr;
}

[[nodiscard]] bool rejectsWebSocketHandshake(const Http2StreamState& stream, const HttpRequest& request) {
    const auto result = validateHttp2WebSocketHandshake(stream, request);
    return result.failure() != nullptr;
}

}  // namespace

RUVIA_TEST(websocket_subprotocol_negotiation) {
    const auto request = requestWithProtocol();
    // Server preference wins: the first supported token the client also offered.
    const auto preferred = makeWebSocketServerNegotiation(request, "superchat, chat");
    RUVIA_CHECK_EQ(preferred.subprotocol(), std::string_view("superchat"));
    const auto chat = makeWebSocketServerNegotiation(request, "chat");
    RUVIA_CHECK_EQ(chat.subprotocol(), std::string_view("chat"));
    // No overlap yields no subprotocol.
    const auto noOverlap = makeWebSocketServerNegotiation(request, "binary");
    RUVIA_CHECK(noOverlap.subprotocol().empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parseRequest("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    const auto noOffer = makeWebSocketServerNegotiation(none, "chat");
    RUVIA_CHECK(noOffer.subprotocol().empty());
}

RUVIA_TEST(websocket_server_negotiation_owns_selected_subprotocol) {
    const auto request = requestWithProtocol();
    std::string supported = "chat";
    const auto negotiation = makeWebSocketServerNegotiation(request, supported);

    supported.front() = 'X';

    RUVIA_CHECK_EQ(negotiation.subprotocol(), std::string_view("chat"));
}

RUVIA_TEST(websocket_request_validity_requires_all_conditions) {
    const auto request = requestWithVersion();

    auto valid = makeStream();
    valid.setProtocol("websocket");
    RUVIA_CHECK(valid.beginExtendedConnect());
    RUVIA_CHECK(valid.finalizeRemoteConnectHead());
    RUVIA_CHECK(acceptsWebSocketHandshake(valid, request));

    // The complete HTTP/2 field section must be decoded before the helper can
    // validate WebSocket-specific request headers. A synthetically pending
    // CONNECT is not yet a complete opening handshake.
    auto unfinalized = makeStream();
    unfinalized.setProtocol("websocket");
    RUVIA_CHECK(unfinalized.beginExtendedConnect());
    RUVIA_CHECK(rejectsWebSocketHandshake(unfinalized, request));

    // A completed/rejected CONNECT is no longer an opening handshake.
    auto openTunnel = makeStream();
    openTunnel.setProtocol("websocket");
    RUVIA_CHECK(openTunnel.beginExtendedConnect());
    RUVIA_CHECK(openTunnel.finalizeRemoteConnectHead());
    RUVIA_CHECK(openTunnel.acceptConnect());
    RUVIA_CHECK(rejectsWebSocketHandshake(openTunnel, request));

    // Without the extended-CONNECT websocket marker (:method CONNECT +
    // :protocol websocket, RFC 8441) it is not a WebSocket handshake at all.
    auto noExtendedConnect = makeStream();
    RUVIA_CHECK(rejectsWebSocketHandshake(noExtendedConnect, request));

    // A Content-Length must not be present on a WebSocket CONNECT.
    auto withContentLength = makeStream();
    withContentLength.setProtocol("websocket");
    RUVIA_CHECK(withContentLength.beginExtendedConnect());
    RUVIA_CHECK(withContentLength.finalizeRemoteConnectHead());
    RUVIA_CHECK(withContentLength.declareRemoteContentLength(5));
    RUVIA_CHECK(rejectsWebSocketHandshake(withContentLength, request));

    // A WebSocket opening handshake must leave the client-to-server send half
    // open for WebSocket frames. A CONNECT head that already carried END_STREAM
    // is a half-closed CONNECT decision, not an admissible WebSocket tunnel start.
    auto halfClosed = makeStream();
    halfClosed.setProtocol("websocket");
    RUVIA_CHECK(halfClosed.recordRemoteHeadEndStream());
    RUVIA_CHECK(halfClosed.beginExtendedConnect());
    RUVIA_CHECK(halfClosed.finalizeRemoteConnectHead());
    RUVIA_CHECK(halfClosed.remoteReceive().connectPendingEndStream() != nullptr);
    RUVIA_CHECK(rejectsWebSocketHandshake(halfClosed, request));

    // The Sec-WebSocket-Version must be exactly 13.
    const auto badVersion = requestWithBadVersion();
    auto stream = makeStream();
    stream.setProtocol("websocket");
    RUVIA_CHECK(stream.beginExtendedConnect());
    RUVIA_CHECK(stream.finalizeRemoteConnectHead());
    const auto unsupportedVersion = validateHttp2WebSocketHandshake(stream, badVersion);
    RUVIA_CHECK(unsupportedVersion.failure() != nullptr);
    if (const auto* failure = unsupportedVersion.failure()) {
        const auto error = failure->protocolError();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kBadRequest);
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::HttpResponse response;
        failure->applyRequiredResponseHeaders(response);
        RUVIA_CHECK_EQ(response.header("Sec-WebSocket-Version"), std::string_view("13"));
    }
}

RUVIA_TEST(websocket_subprotocol_offers_are_validated_for_extended_connect) {
    const auto malformed = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat, bad token\r\n"
        "\r\n");
    auto malformedStream = makeStream();
    malformedStream.setProtocol("websocket");
    RUVIA_CHECK(malformedStream.beginExtendedConnect());
    RUVIA_CHECK(malformedStream.finalizeRemoteConnectHead());
    RUVIA_CHECK(rejectsWebSocketHandshake(malformedStream, malformed));

    const auto duplicate = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Protocol: superchat, chat\r\n"
        "\r\n");
    auto duplicateStream = makeStream();
    duplicateStream.setProtocol("websocket");
    RUVIA_CHECK(duplicateStream.beginExtendedConnect());
    RUVIA_CHECK(duplicateStream.finalizeRemoteConnectHead());
    RUVIA_CHECK(rejectsWebSocketHandshake(duplicateStream, duplicate));
}

RUVIA_TEST(websocket_extension_offers_are_validated_for_extended_connect) {
    const auto malformed = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; =value\r\n"
        "\r\n");
    auto stream = makeStream();
    stream.setProtocol("websocket");
    RUVIA_CHECK(stream.beginExtendedConnect());
    RUVIA_CHECK(stream.finalizeRemoteConnectHead());
    RUVIA_CHECK(rejectsWebSocketHandshake(stream, malformed));
}

RUVIA_TEST(http2_websocket_handshake_does_not_invent_server_product) {
    const auto request = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "\r\n");
    const auto negotiation = makeWebSocketServerNegotiation(request, "chat");
    std::pmr::string block(std::pmr::get_default_resource());
    http2EncodeWebSocketHandshakeHeaders(block, negotiation);

    Collector fields;
    HpackDecoder decoder(std::pmr::get_default_resource());
    const auto decodeResult = decoder.decode(block, &fields, &collect);
    RUVIA_CHECK(decodeResult.decoded() != nullptr);
    RUVIA_CHECK(hasHeader(fields, ":status", "200"));
    RUVIA_CHECK(hasHeader(fields, "sec-websocket-protocol", "chat"));
    RUVIA_CHECK(hasHeader(fields, "sec-websocket-extensions", ruvia::detail::kWebSocketDeflateResponseExtensions));
    RUVIA_CHECK(hasHeaderName(fields, "date"));
    RUVIA_CHECK(!hasHeaderName(fields, "server"));
}
