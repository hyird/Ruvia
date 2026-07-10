#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/HttpRequestFlags.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::HttpRequestFlags;
using ruvia::detail::HttpServerParser;
using ruvia::detail::chooseWebSocketSubprotocol;
using ruvia::detail::isValidWebSocketRequest;
using ruvia::detail::webSocketProtocolOffered;

HttpRequest parseRequest(std::string_view rawRequest) {
    HttpServerParser parser;
    const auto parsed = parser.parse(rawRequest);
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
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest postHandshake() {
    return parseRequest(
        "POST /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest http10Handshake() {
    return parseRequest(
        "GET /ws HTTP/1.0\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest badVersionHandshake() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 8\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");
}

HttpRequest contentLengthZeroHandshake() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
}

HttpRequestFlags validHandshakeFlags() {
    HttpRequestFlags flags;
    flags.upgrade = true;
    flags.secWebSocketKeyCount = 1;
    flags.secWebSocketVersionCount = 1;
    return flags;
}

}  // namespace

RUVIA_TEST(ws_subprotocol_negotiation_prefers_server_order) {
    const auto request = offering();
    HttpRequestFlags flags;
    flags.secWebSocketProtocolCount = 1;
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, flags, "superchat, chat"),
                   std::string_view("superchat"));
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, flags, "chat"), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(chooseWebSocketSubprotocol(request, flags, "binary").empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parseRequest("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    RUVIA_CHECK(chooseWebSocketSubprotocol(none, flags, "chat").empty());
}

RUVIA_TEST(ws_protocol_offered_matches_whole_tokens_only) {
    const auto request = offering();
    RUVIA_CHECK(webSocketProtocolOffered(request, "chat"));
    RUVIA_CHECK(webSocketProtocolOffered(request, "superchat"));
    RUVIA_CHECK(!webSocketProtocolOffered(request, "super"));   // prefix, not a whole token
    RUVIA_CHECK(!webSocketProtocolOffered(request, "binary"));
}

RUVIA_TEST(ws_valid_request_requires_all_conditions) {
    const auto flags = validHandshakeFlags();

    RUVIA_CHECK(isValidWebSocketRequest(validHandshake(), flags));

    // Every individual requirement is necessary.
    RUVIA_CHECK(!isValidWebSocketRequest(postHandshake(), flags));        // not GET
    RUVIA_CHECK(!isValidWebSocketRequest(http10Handshake(), flags));      // not 1.1
    RUVIA_CHECK(!isValidWebSocketRequest(badVersionHandshake(), flags));  // version != 13
    RUVIA_CHECK(!isValidWebSocketRequest(contentLengthZeroHandshake(), flags)); // no HTTP body framing

    {
        HttpRequestFlags noUpgrade = flags;
        noUpgrade.upgrade = false;  // Connection: Upgrade absent
        RUVIA_CHECK(!isValidWebSocketRequest(validHandshake(), noUpgrade));
    }
    {
        HttpRequestFlags duplicateKey = flags;
        duplicateKey.secWebSocketKeyCount = 2;  // Sec-WebSocket-Key must appear exactly once
        RUVIA_CHECK(!isValidWebSocketRequest(validHandshake(), duplicateKey));
    }
    {
        HttpRequestFlags duplicateVersion = flags;
        duplicateVersion.secWebSocketVersionCount = 2;  // Version must appear exactly once
        RUVIA_CHECK(!isValidWebSocketRequest(validHandshake(), duplicateVersion));
    }

    // The Upgrade header must name "websocket", not another protocol token.
    const auto wrongUpgrade = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: h2c\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(wrongUpgrade, flags));

    // Sec-WebSocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    const auto badKey = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n");
    RUVIA_CHECK(!isValidWebSocketRequest(badKey, flags));
}
