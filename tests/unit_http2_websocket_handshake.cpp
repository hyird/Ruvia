#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::HpackDecoder;
using ruvia::detail::http2EncodeWebSocketHandshakeHeaders;
using ruvia::detail::http2IsValidWebSocketRequest;
using ruvia::detail::makeWebSocketServerNegotiation;

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

}  // namespace

RUVIA_TEST(websocket_subprotocol_negotiation) {
    const auto request = requestWithProtocol();
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(
        makeWebSocketServerNegotiation(request, "superchat, chat")
            .subprotocol(),
        std::string_view("superchat"));
    RUVIA_CHECK_EQ(
        makeWebSocketServerNegotiation(request, "chat").subprotocol(),
        std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(
        makeWebSocketServerNegotiation(request, "binary")
            .subprotocol()
            .empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parseRequest("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    RUVIA_CHECK(
        makeWebSocketServerNegotiation(none, "chat").subprotocol().empty());
}

RUVIA_TEST(websocket_request_validity_requires_all_conditions) {
    const auto request = requestWithVersion();

    auto valid = makeStream();
    valid.setProtocol("websocket");
    RUVIA_CHECK(valid.beginExtendedConnect());
    RUVIA_CHECK(http2IsValidWebSocketRequest(valid, request));

    // A completed/rejected CONNECT is no longer an opening handshake.
    auto openTunnel = makeStream();
    openTunnel.setProtocol("websocket");
    RUVIA_CHECK(openTunnel.beginExtendedConnect());
    RUVIA_CHECK(openTunnel.acceptConnect());
    RUVIA_CHECK(!http2IsValidWebSocketRequest(openTunnel, request));

    // Without the extended-CONNECT websocket marker (:method CONNECT +
    // :protocol websocket, RFC 8441) it is not a WebSocket handshake at all.
    auto noExtendedConnect = makeStream();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(noExtendedConnect, request));

    // A Content-Length must not be present on a WebSocket CONNECT.
    auto withContentLength = makeStream();
    withContentLength.setProtocol("websocket");
    RUVIA_CHECK(withContentLength.beginExtendedConnect());
    RUVIA_CHECK(withContentLength.declareRemoteContentLength(5));
    RUVIA_CHECK(!http2IsValidWebSocketRequest(withContentLength, request));

    // The Sec-WebSocket-Version must be exactly 13.
    const auto badVersion = requestWithBadVersion();
    auto stream = makeStream();
    stream.setProtocol("websocket");
    RUVIA_CHECK(stream.beginExtendedConnect());
    RUVIA_CHECK(!http2IsValidWebSocketRequest(stream, badVersion));
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
    http2EncodeWebSocketHandshakeHeaders(
        block, negotiation);

    Collector fields;
    HpackDecoder decoder(std::pmr::get_default_resource());
    const auto decodeResult = decoder.decode(block, &fields, &collect);
    RUVIA_CHECK(decodeResult.decoded() != nullptr);
    RUVIA_CHECK(hasHeader(fields, ":status", "200"));
    RUVIA_CHECK(hasHeader(fields, "sec-websocket-protocol", "chat"));
    RUVIA_CHECK(hasHeader(
        fields,
        "sec-websocket-extensions",
        ruvia::detail::kWebSocketDeflateResponseExtensions));
    RUVIA_CHECK(hasHeaderName(fields, "date"));
    RUVIA_CHECK(!hasHeaderName(fields, "server"));
}
