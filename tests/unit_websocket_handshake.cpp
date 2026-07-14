#include "test_harness.h"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::chooseWebSocketSubprotocol;
using ruvia::detail::isValidWebSocketRequest;
using ruvia::detail::webSocketProtocolOffered;

template <typename T>
concept HasRvalueWebSocketHandshakeNegotiation =
    requires(T&& handshake) { std::move(handshake).negotiation(); };

static_assert(!HasRvalueWebSocketHandshakeNegotiation<
    ruvia::detail::HttpWebSocketServerHandshake>);

HttpRequest parseRequest(std::string_view rawRequest) {
    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(rawRequest);
    return parsed.request;
}

HttpRequest offering() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "\r\n");
}

HttpRequest validHandshake() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest postHandshake() {
    return parseRequest(
        "POST /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest http10Handshake() {
    return parseRequest(
        "GET /ws HTTP/1.0\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest badVersionHandshake() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 8\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest contentLengthZeroHandshake() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
}

}  // namespace

RUVIA_TEST(ws_subprotocol_negotiation_prefers_server_order) {
    const auto request = offering();
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, "superchat, chat"),
                   std::string_view("superchat"));
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, "chat"), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(chooseWebSocketSubprotocol(request, "binary").empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parseRequest("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    RUVIA_CHECK(chooseWebSocketSubprotocol(none, "chat").empty());
}

RUVIA_TEST(ws_protocol_offered_matches_whole_tokens_only) {
    const auto request = offering();
    RUVIA_CHECK(webSocketProtocolOffered(request, "chat"));
    RUVIA_CHECK(webSocketProtocolOffered(request, "superchat"));
    RUVIA_CHECK(!webSocketProtocolOffered(request, "super"));   // prefix, not a whole token
    RUVIA_CHECK(!webSocketProtocolOffered(request, "binary"));
}

RUVIA_TEST(ws_valid_request_requires_all_conditions) {
    RUVIA_CHECK(isValidWebSocketRequest(validHandshake()));

    // Every individual requirement is necessary.
    RUVIA_CHECK(!isValidWebSocketRequest(postHandshake()));        // not GET
    RUVIA_CHECK(!isValidWebSocketRequest(http10Handshake()));      // not 1.1
    RUVIA_CHECK(!isValidWebSocketRequest(badVersionHandshake()));  // version != 13
    RUVIA_CHECK(!isValidWebSocketRequest(contentLengthZeroHandshake())); // no HTTP body framing

    const auto noConnectionUpgrade = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(noConnectionUpgrade));

    const auto duplicateKey = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(duplicateKey));

    const auto duplicateVersion = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(duplicateVersion));

    // The Upgrade header must name "websocket", not another protocol token.
    const auto wrongUpgrade = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(wrongUpgrade));

    // Sec-WebSocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    const auto badKey = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(badKey));
}

RUVIA_TEST(ws_upgrade_uses_the_shared_recipient_list_semantics) {
    const auto request = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , custom/1, websocket,\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
    RUVIA_CHECK(isValidWebSocketRequest(request));
}

RUVIA_TEST(ws_server_handshake_response_serialization_is_http_owned) {
    const auto request = parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15\r\n"
        "\r\n");
    const auto handshake = ruvia::detail::makeHttpWebSocketServerHandshake(
        request, "chat");

    std::string response;
    handshake.forEachResponsePart([&response](std::string_view part) {
        response.append(part);
    });
    RUVIA_CHECK_EQ(
        response,
        std::string(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
            "Sec-WebSocket-Protocol: chat\r\n"
            "Sec-WebSocket-Extensions: permessage-deflate; "
            "server_no_context_takeover; client_no_context_takeover; "
            "server_max_window_bits=15\r\n"
            "\r\n"));
    RUVIA_CHECK(
        handshake.negotiation().deflate() ==
        ruvia::detail::WebSocketDeflateNegotiation::
            kAcceptedWithServerMaxWindowBits);
    RUVIA_CHECK_EQ(handshake.negotiation().subprotocol(), "chat");
    RUVIA_CHECK_EQ(
        handshake.negotiation().extensions(),
        ruvia::detail::kWebSocketDeflateResponseExtensionsMaxWindow);
}
