#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1WebSocketClientHandshake.h"

#include "test_harness.h"

namespace {

constexpr std::array<std::uint8_t, 16> kNonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

ruvia::Http1WebSocketClientHandshake makeHandshake(
    std::span<const ruvia::HttpHeaderView> headers = {},
    std::span<const std::string_view> protocols = {}, std::string_view userAgent = {}) {
    return ruvia::Http1WebSocketClientHandshake(
        {.nonce = kNonce, .headers = headers, .subprotocols = protocols, .userAgent = userAgent},
        std::pmr::get_default_resource());
}

ruvia::Http1ParsedClientResponseHead parseResponse(ruvia::testing::TestContext& ruvia_ctx,
    ruvia::Http1ClientExchangeState state, std::string_view wire) {
    ruvia::Http1ClientResponseParser parser(std::move(state));
    auto result = parser.parse(wire);
    RUVIA_CHECK(result.parsed() != nullptr);
    if (result.parsed() == nullptr) {
        throw std::runtime_error("expected parsed response");
    }
    return std::move(*result.parsed());
}

constexpr std::string_view kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string validResponse(std::string_view extra = {}) {
    std::string wire =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    wire += kAccept;
    wire += "\r\n";
    wire += extra;
    wire += "\r\n";
    return wire;
}

bool rejectsConfiguration(std::span<const ruvia::HttpHeaderView> headers = {},
    std::span<const std::string_view> protocols = {}, std::string_view userAgent = {}) {
    try {
        auto handshake = makeHandshake(headers, protocols, userAgent);
        (void)handshake;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(websocket_client_handshake_prepares_rfc_request) {
    const std::array protocols{std::string_view{"chat"}};
    const std::array headers{ruvia::HttpHeaderView{"X-Trace", "one"}};
    const auto handshake = makeHandshake(headers, protocols, "RuviaTest/1");
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/chat", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    const std::string wire(prepared.prepared()->head());
    RUVIA_CHECK(wire.find("GET /chat HTTP/1.1\r\n") == 0);
    RUVIA_CHECK(wire.find("Host: example.com\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
                std::string::npos);
    RUVIA_CHECK(wire.find("Sec-WebSocket-Protocol: chat\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("User-Agent: RuviaTest/1\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("X-Trace: one\r\n") != std::string::npos);
}

RUVIA_TEST(websocket_client_handshake_matches_subprotocol_case_exactly) {
    const std::array protocols{std::string_view{"chat"}, std::string_view{"Chat"}};
    const auto handshake = makeHandshake({}, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
    auto response = parseResponse(ruvia_ctx, prepared.prepared()->exchangeState(),
        validResponse("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result = handshake.validateResponse(response);
    RUVIA_CHECK(result.has_value());
    if (result.has_value()) {
        RUVIA_CHECK_EQ(result->selectedSubprotocol, std::string_view("Chat"));
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    for (const auto responseProtocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const auto handshake = makeHandshake({}, protocols);
        std::array<char, 2048> buffer{};
        const auto prepared = handshake.prepareRequest(
            ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
        const std::string extra = responseProtocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        auto response = parseResponse(ruvia_ctx, prepared.prepared()->exchangeState(),
            validResponse(extra));
        const auto result = handshake.validateResponse(response);
        RUVIA_CHECK(!result.has_value());
        if (!result.has_value()) {
            RUVIA_CHECK(result.error() ==
                        ruvia::Http1WebSocketClientHandshakeError::kSubprotocol);
        }
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const auto handshake = makeHandshake();
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
    auto bad = parseResponse(ruvia_ctx, prepared.prepared()->exchangeState(),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    RUVIA_CHECK(bad.plan().protocolUpgrade() != nullptr);
    const auto badResult = handshake.validateResponse(bad);
    RUVIA_CHECK(!badResult.has_value());
    if (!badResult.has_value()) {
        RUVIA_CHECK(badResult.error() == ruvia::Http1WebSocketClientHandshakeError::kAccept);
    }

    auto duplicate = parseResponse(ruvia_ctx, prepared.prepared()->exchangeState(),
        validResponse("Sec-WebSocket-Accept: " + std::string(kAccept) + "\r\n"));
    const auto duplicateResult = handshake.validateResponse(duplicate);
    RUVIA_CHECK(!duplicateResult.has_value());
    if (!duplicateResult.has_value()) {
        RUVIA_CHECK(duplicateResult.error() ==
                    ruvia::Http1WebSocketClientHandshakeError::kAccept);
    }

    const auto extension = makeHandshake();
    std::array<char, 2048> extensionBuffer{};
    const auto extensionPrepared = extension.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/", extensionBuffer);
    auto response = parseResponse(ruvia_ctx, extensionPrepared.prepared()->exchangeState(),
        validResponse("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extensionResult = extension.validateResponse(response);
    RUVIA_CHECK(!extensionResult.has_value());
    if (!extensionResult.has_value()) {
        RUVIA_CHECK(extensionResult.error() ==
                    ruvia::Http1WebSocketClientHandshakeError::kExtensions);
    }
}

RUVIA_TEST(websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const auto handshake = makeHandshake();
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
    ruvia::Http1ClientResponseParser parser(prepared.prepared()->exchangeState());
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        RUVIA_CHECK(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(validResponse());
    RUVIA_CHECK(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result = handshake.validateResponse(*final.parsed());
        RUVIA_CHECK(result.has_value());
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_invalid_configuration_and_reserved_headers) {
    const std::array badSpace{ruvia::HttpHeaderView{"X-Test", "bad value\r\n"}};
    const std::array badComma{ruvia::HttpHeaderView{"X-Test", "bad,\r\nvalue"}};
    const std::array reserved{ruvia::HttpHeaderView{"Host", "evil.example"}};
    const std::array transferEncoding{
        ruvia::HttpHeaderView{"Transfer-Encoding", "chunked"}};
    const std::array userAgentHeader{ruvia::HttpHeaderView{"User-Agent", "custom-agent"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array badToken{std::string_view{"bad token"}};
    const std::array commaToken{std::string_view{"chat,superchat"}};
    RUVIA_CHECK(rejectsConfiguration(badSpace));
    RUVIA_CHECK(rejectsConfiguration(badComma));
    RUVIA_CHECK(rejectsConfiguration(reserved));
    RUVIA_CHECK(rejectsConfiguration(transferEncoding));
    RUVIA_CHECK(rejectsConfiguration(userAgentHeader, {}, "another-agent"));
    RUVIA_CHECK(rejectsConfiguration({}, duplicate));
    RUVIA_CHECK(rejectsConfiguration({}, empty));
    RUVIA_CHECK(rejectsConfiguration({}, badToken));
    RUVIA_CHECK(rejectsConfiguration({}, commaToken));
    RUVIA_CHECK(rejectsConfiguration({}, {}, "bad\r\nagent"));
}

RUVIA_TEST(websocket_client_handshake_prepares_ipv6_host_and_owns_configuration) {
    std::string headerName = "X-Trace";
    std::string headerValue = "initial";
    std::string protocol = "chat";
    std::string userAgent = "initial-agent";
    const std::array headers{ruvia::HttpHeaderView{headerName, headerValue}};
    const std::array protocols{std::string_view(protocol)};
    const auto handshake = makeHandshake(headers, protocols, userAgent);
    headerName = "Changed";
    headerValue = "changed";
    protocol = "changed";
    userAgent = "changed-agent";
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "[2001:db8::1]", .port = 8443}), "/before", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    const std::string wire(prepared.prepared()->head());
    RUVIA_CHECK(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("X-Trace: initial\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("Sec-WebSocket-Protocol: chat\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("User-Agent: initial-agent\r\n") != std::string::npos);
}

RUVIA_TEST(websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const auto handshake = makeHandshake({}, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepareRequest(
        ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
    auto response = parseResponse(ruvia_ctx, prepared.prepared()->exchangeState(),
        validResponse("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result = handshake.validateResponse(response);
    RUVIA_CHECK(result.has_value());
    if (result.has_value()) {
        const auto selected = result->selectedSubprotocol;
        RUVIA_CHECK_EQ(selected, std::string_view("chat"));
        RUVIA_CHECK_EQ(response.head().headers().back().value(), selected);
    }
}
