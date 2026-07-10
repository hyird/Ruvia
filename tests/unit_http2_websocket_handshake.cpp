#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::Http2StreamState;
using ruvia::detail::HttpServerParser;
using ruvia::detail::http2ChooseWebSocketSubprotocol;
using ruvia::detail::http2IsValidWebSocketRequest;

HttpRequest parseRequest(std::string_view rawRequest) {
    HttpServerParser parser;
    const auto parsed = parser.parse(rawRequest);
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
    RUVIA_CHECK_EQ(http2ChooseWebSocketSubprotocol(request, "superchat, chat"),
                   std::string_view("superchat"));
    RUVIA_CHECK_EQ(http2ChooseWebSocketSubprotocol(request, "chat"), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(http2ChooseWebSocketSubprotocol(request, "binary").empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parseRequest("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    RUVIA_CHECK(http2ChooseWebSocketSubprotocol(none, "chat").empty());
}

RUVIA_TEST(websocket_request_validity_requires_all_conditions) {
    const auto request = requestWithVersion();

    auto valid = makeStream();
    valid.markExtendedConnectWebSocket();
    valid.markWebSocketTunnel();
    RUVIA_CHECK(http2IsValidWebSocketRequest(valid, request));

    // Missing the tunnel flag invalidates it.
    auto noTunnel = makeStream();
    noTunnel.markExtendedConnectWebSocket();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(noTunnel, request));

    // Without the extended-CONNECT websocket marker (:method CONNECT +
    // :protocol websocket, RFC 8441) it is not a WebSocket handshake at all.
    auto noExtendedConnect = makeStream();
    noExtendedConnect.markWebSocketTunnel();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(noExtendedConnect, request));

    // A Content-Length must not be present on a WebSocket CONNECT.
    auto withContentLength = makeStream();
    withContentLength.markExtendedConnectWebSocket();
    withContentLength.markWebSocketTunnel();
    RUVIA_CHECK(withContentLength.setContentLength(5));
    RUVIA_CHECK(!http2IsValidWebSocketRequest(withContentLength, request));

    // The Sec-WebSocket-Version must be exactly 13.
    const auto badVersion = requestWithBadVersion();
    auto stream = makeStream();
    stream.markExtendedConnectWebSocket();
    stream.markWebSocketTunnel();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(stream, badVersion));
}
