#include "test_io_context.h"
#include "test_harness.h"

#include <concepts>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/WebSocketHandshake.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"

namespace {

using ruvia::HttpRequest;
using ruvia::detail::chooseWebSocketSubprotocol;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::validateWebSocketHandshake;
using ruvia::detail::webSocketProtocolOffered;

template <typename T>
concept HasRvalueWebSocketHandshakeSubprotocol = requires(T&& handshake) { std::move(handshake).subprotocol(); };

static_assert(!HasRvalueWebSocketHandshakeSubprotocol<ruvia::WebSocketServerHandshake>);
static_assert(!std::copy_constructible<ruvia::WebSocketServerHandshake>);
static_assert(std::move_constructible<ruvia::WebSocketServerHandshake>);

class FailingHandshakeWriteStream final {
public:
    using executor_type = asio::io_context::executor_type;

    explicit FailingHandshakeWriteStream(asio::io_context& io) noexcept
        : executor_(io.get_executor()) {}

    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }

    template <typename ConstBufferSequence, typename Handler>
    void async_write_some(const ConstBufferSequence&, Handler&& handler) {
        asio::post(executor_, [handler = std::forward<Handler>(handler)]() mutable { std::move(handler)(std::make_error_code(std::errc::broken_pipe), std::size_t{0}); });
    }

private:
    executor_type executor_;
};

HttpRequest parseRequest(std::string_view rawRequest) {
    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(rawRequest);
    return parsed.request;
}

[[nodiscard]] auto validateRequest(std::string_view rawRequest) {
    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(rawRequest);
    return validateWebSocketHandshake(parsed.request, parsed.bodyPlan);
}

[[nodiscard]] bool acceptsRequest(std::string_view rawRequest) {
    const auto result = validateRequest(rawRequest);
    return result.accepted() != nullptr;
}

[[nodiscard]] bool rejectsRequest(std::string_view rawRequest) {
    const auto result = validateRequest(rawRequest);
    return result.failure() != nullptr;
}

HttpRequest offering() {
    return parseRequest(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "\r\n");
}

std::string_view validHandshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view postHandshake() {
    return "POST /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view http10Handshake() {
    return "GET /ws HTTP/1.0\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view badVersionHandshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 8\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view contentLengthZeroHandshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

std::string_view contentLengthOneHandshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 1\r\n"
           "\r\n"
           "x";
}

}  // namespace

RUVIA_TEST(ws_subprotocol_negotiation_prefers_server_order) {
    const auto request = offering();
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, "superchat, chat"), std::string_view("superchat"));
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
    RUVIA_CHECK(!webSocketProtocolOffered(request, "super"));  // prefix, not a whole token
    RUVIA_CHECK(!webSocketProtocolOffered(request, "binary"));

    const auto malformed = parseRequest(
        "GET /ws HTTP/1.1\r\nHost: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, bad token\r\n\r\n");
    RUVIA_CHECK(!webSocketProtocolOffered(malformed, "chat"));
    RUVIA_CHECK(chooseWebSocketSubprotocol(malformed, "chat").empty());
    RUVIA_CHECK(chooseWebSocketSubprotocol(request, "chat, bad token").empty());
}

RUVIA_TEST(ws_subprotocol_offers_require_unique_http_tokens) {
    const auto withProtocols = [](std::string_view fields) {
        std::string request(validHandshake());
        request.insert(request.size() - 2, fields);
        return request;
    };

    RUVIA_CHECK(acceptsRequest(withProtocols("Sec-WebSocket-Protocol: , chat,, superchat,\r\n")));
    // Subprotocol identifiers are case-sensitive, so these are distinct.
    RUVIA_CHECK(acceptsRequest(withProtocols("Sec-WebSocket-Protocol: chat, Chat\r\n")));

    RUVIA_CHECK(rejectsRequest(withProtocols("Sec-WebSocket-Protocol: bad token\r\n")));
    RUVIA_CHECK(rejectsRequest(withProtocols("Sec-WebSocket-Protocol: \"chat\"\r\n")));
    RUVIA_CHECK(rejectsRequest(withProtocols("Sec-WebSocket-Protocol: , ,\r\n")));
    RUVIA_CHECK(rejectsRequest(withProtocols("Sec-WebSocket-Protocol: chat, chat\r\n")));
    RUVIA_CHECK(
        rejectsRequest(withProtocols("Sec-WebSocket-Protocol: chat\r\n"
                                     "Sec-WebSocket-Protocol: superchat, chat\r\n")));

    std::string tooMany = "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        if (i != 0) {
            tooMany.append(", ");
        }
        tooMany.append("protocol-");
        tooMany.append(std::to_string(i));
    }
    tooMany.append("\r\n");
    RUVIA_CHECK(rejectsRequest(withProtocols(tooMany)));
}

RUVIA_TEST(ws_extension_offers_must_match_the_rfc6455_abnf) {
    const auto withExtensions = [](std::string_view fields) {
        std::string request(validHandshake());
        request.insert(request.size() - 2, fields);
        return request;
    };

    RUVIA_CHECK(acceptsRequest(withExtensions("Sec-WebSocket-Extensions: , x-test; flag; value=token,,\r\n")));
    RUVIA_CHECK(acceptsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; value=\"to\\ken\"\r\n")));
    RUVIA_CHECK(
        acceptsRequest(withExtensions("Sec-WebSocket-Extensions: x-test\r\n"
                                      "Sec-WebSocket-Extensions: y-test; value=token\r\n")));

    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: , ,\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: \"x-test\"\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x test\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test;\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test;; flag\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; =value\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; value=\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; value=\"bad value\"\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; value=\"unterminated\r\n")));
    RUVIA_CHECK(rejectsRequest(withExtensions("Sec-WebSocket-Extensions: x-test; value=\"token\"junk\r\n")));
}

RUVIA_TEST(ws_valid_request_requires_all_conditions) {
    RUVIA_CHECK(acceptsRequest(validHandshake()));

    // Every individual requirement is necessary.
    RUVIA_CHECK(rejectsRequest(postHandshake()));
    RUVIA_CHECK(rejectsRequest(http10Handshake()));
    // RFC 6455 permits additional HTTP fields, and RFC 9112 framing makes a
    // zero Content-Length an empty request. Its mere presence must not block
    // an otherwise valid protocol switch.
    RUVIA_CHECK(acceptsRequest(contentLengthZeroHandshake()));
    // Actual request content still prevents switching protocols because those
    // octets belong to the HTTP message rather than to the WebSocket stream.
    RUVIA_CHECK(rejectsRequest(contentLengthOneHandshake()));

    constexpr std::string_view noConnectionUpgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(noConnectionUpgrade));

    constexpr std::string_view duplicateKey =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(duplicateKey));

    constexpr std::string_view duplicateVersion =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(duplicateVersion));

    // The Upgrade header must name "websocket", not another protocol token.
    constexpr std::string_view wrongUpgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(wrongUpgrade));

    // Sec-WebSocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    constexpr std::string_view badKey =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(badKey));

    // "...ZR==" decodes to the same 16 bytes as the canonical "...ZQ==",
    // but sets unused base64 padding bits and must therefore be rejected.
    constexpr std::string_view nonCanonicalKey =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n";
    RUVIA_CHECK(rejectsRequest(nonCanonicalKey));

    const auto unsupportedVersion = validateRequest(badVersionHandshake());
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

RUVIA_TEST(ws_upgrade_uses_the_shared_recipient_list_semantics) {
    constexpr std::string_view request =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , custom/1, websocket,\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    RUVIA_CHECK(acceptsRequest(request));
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
    std::string supported = "chat";
    const auto handshake = ruvia::makeWebSocketServerHandshake(request, {.supportedSubprotocols = supported});
    supported.front() = 'X';

    std::string response;
    handshake.forEachResponsePart([&response](std::string_view part) { response.append(part); });
    RUVIA_CHECK_EQ(response, std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                         "Upgrade: websocket\r\n"
                                         "Connection: Upgrade\r\n"
                                         "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                         "Sec-WebSocket-Protocol: chat\r\n"
                                         "Sec-WebSocket-Extensions: permessage-deflate; "
                                         "server_no_context_takeover; client_no_context_takeover; "
                                         "server_max_window_bits=15\r\n"
                                         "\r\n"));
    RUVIA_CHECK(handshake.compression() == ruvia::WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);
    RUVIA_CHECK_EQ(handshake.subprotocol(), "chat");
}

RUVIA_TEST(ws_handshake_writer_preserves_transport_error) {
    const auto request = parseRequest(validHandshake());
    const auto handshake = ruvia::makeWebSocketServerHandshake(request, {});
    asio::io_context& io = ruvia::test::newTestIoContext();
    FailingHandshakeWriteStream stream(io);
    auto result = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(ruvia::detail::writeWebSocketHandshake(stream, handshake)), asio::use_future);
    io.run();
    RUVIA_CHECK_EQ(result.get(), std::make_error_code(std::errc::broken_pipe));
}
