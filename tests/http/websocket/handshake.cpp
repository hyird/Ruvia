#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http1/http1_server_request_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_subprotocol_set.h"

#include "test_harness.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace {

using ruvia::http_request;
using ruvia::validate_websocket_handshake;
using ruvia::detail::choose_websocket_subprotocol;
using ruvia::detail::http1_server_request_parser;
using ruvia::detail::websocket_protocol_offered;

http_request parse_request(std::string_view raw_request) {
    http1_server_request_parser parser;
    auto parsed_value = parser.parse_message(raw_request);
    return std::move(parsed_value.request_);
}

[[nodiscard]] auto validate_request(std::string_view raw_request) {
    http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(raw_request);
    return validate_websocket_handshake(parsed_value.request_, parsed_value.body_plan_);
}

[[nodiscard]] bool accepts_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.accepted() != nullptr;
}

[[nodiscard]] bool rejects_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.failure() != nullptr;
}

http_request offering() {
    return parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "\r\n");
}

std::string_view valid_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view post_handshake() {
    return "POST /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view http10_handshake() {
    return "GET /ws HTTP/1.0\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view bad_version_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 8\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view content_length_zero_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

std::string_view content_length_one_handshake() {
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

RUVIA_TEST(ws_h1_handshake_preserves_application_request_headers) {
    constexpr std::string_view raw =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Origin: https://origin.example\r\n"
        "Authorization: Bearer credential\r\n"
        "Cookie: sid=one; theme=dark\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "X-End-To-End: retained\r\n"
        "\r\n";
    const auto request = parse_request(raw);

    RUVIA_CHECK(request.header("Host") == "example.test");
    RUVIA_CHECK(request.header("Origin") == "https://origin.example");
    RUVIA_CHECK(request.header("Authorization") == "Bearer credential");
    RUVIA_CHECK(request.header("Cookie") == "sid=one; theme=dark");
    RUVIA_CHECK(request.header("Sec-WebSocket-Protocol") == "chat");
    RUVIA_CHECK(request.header("Sec-WebSocket-Extensions") == "permessage-deflate");
    RUVIA_CHECK(request.header("X-End-To-End") == "retained");
    RUVIA_CHECK(accepts_request(raw));
}

RUVIA_TEST(ws_subprotocol_negotiation_prefers_server_order) {
    const auto request = offering();
    constexpr std::array<std::string_view, 2> supported{"superchat", "chat"};
    constexpr std::array<std::string_view, 1> chat{"chat"};
    constexpr std::array<std::string_view, 1> binary{"binary"};
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(choose_websocket_subprotocol(request, supported), std::string_view("superchat"));
    RUVIA_CHECK_EQ(choose_websocket_subprotocol(request, chat), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(choose_websocket_subprotocol(request, binary).empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parse_request("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    RUVIA_CHECK(choose_websocket_subprotocol(none, chat).empty());
}

RUVIA_TEST(ws_protocol_offered_matches_whole_tokens_only) {
    const auto request = offering();
    RUVIA_CHECK(websocket_protocol_offered(request, "chat"));
    RUVIA_CHECK(websocket_protocol_offered(request, "superchat"));
    RUVIA_CHECK(!websocket_protocol_offered(request, "super"));  // prefix, not a whole token
    RUVIA_CHECK(!websocket_protocol_offered(request, "binary"));

    const auto malformed = parse_request(
        "GET /ws HTTP/1.1\r\nHost: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, bad token\r\n\r\n");
    RUVIA_CHECK(!websocket_protocol_offered(malformed, "chat"));
    constexpr std::array<std::string_view, 1> chat{"chat"};
    constexpr std::array<std::string_view, 2> malformed_supported{"chat", "bad token"};
    constexpr std::array<std::string_view, 2> duplicate_supported{"chat", "chat"};
    RUVIA_CHECK(choose_websocket_subprotocol(malformed, chat).empty());
    RUVIA_CHECK(choose_websocket_subprotocol(request, malformed_supported).empty());
    RUVIA_CHECK(choose_websocket_subprotocol(request, duplicate_supported).empty());
}

RUVIA_TEST(ws_public_subprotocol_set_validates_unique_tokens) {
    ruvia::websocket_subprotocol_set protocols;
    RUVIA_CHECK(protocols.append_list(", chat, superchat,"));
    RUVIA_CHECK(protocols.contains("chat"));
    RUVIA_CHECK(protocols.contains("superchat"));
    RUVIA_CHECK(!protocols.append("chat"));
    RUVIA_CHECK(!protocols.append("bad token"));
}

RUVIA_TEST(ws_subprotocol_offers_require_unique_http_tokens) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    RUVIA_CHECK(accepts_request(with_protocols("Sec-WebSocket-Protocol: , chat,, superchat,\r\n")));
    // Subprotocol identifiers are case-sensitive, so these are distinct.
    RUVIA_CHECK(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, Chat\r\n")));

    RUVIA_CHECK(rejects_request(with_protocols("Sec-WebSocket-Protocol: bad token\r\n")));
    RUVIA_CHECK(rejects_request(with_protocols("Sec-WebSocket-Protocol: \"chat\"\r\n")));
    RUVIA_CHECK(rejects_request(with_protocols("Sec-WebSocket-Protocol: , ,\r\n")));
    RUVIA_CHECK(rejects_request(with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n")));
    RUVIA_CHECK(
        rejects_request(with_protocols("Sec-WebSocket-Protocol: chat\r\n"
                                       "Sec-WebSocket-Protocol: superchat, chat\r\n")));

    std::string too_many = "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        if (i != 0) {
            too_many.append(", ");
        }
        too_many.append("protocol-");
        too_many.append(std::to_string(i));
    }
    too_many.append("\r\n");
    RUVIA_CHECK(rejects_request(with_protocols(too_many)));
}

RUVIA_TEST(ws_extension_offers_must_match_the_rfc6455_abnf) {
    const auto with_extensions = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    RUVIA_CHECK(accepts_request(
        with_extensions("Sec-WebSocket-Extensions: , x-test; flag; value=token,,\r\n")));
    RUVIA_CHECK(
        accepts_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\"to\\ken\"\r\n")));
    RUVIA_CHECK(
        accepts_request(with_extensions("Sec-WebSocket-Extensions: x-test\r\n"
                                        "Sec-WebSocket-Extensions: y-test; value=token\r\n")));

    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: , ,\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: \"x-test\"\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: x test\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;; flag\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; =value\r\n")));
    RUVIA_CHECK(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\r\n")));
    RUVIA_CHECK(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"bad value\"\r\n")));
    RUVIA_CHECK(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"unterminated\r\n")));
    RUVIA_CHECK(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"token\"junk\r\n")));
}

RUVIA_TEST(ws_valid_request_requires_all_conditions) {
    RUVIA_CHECK(accepts_request(valid_handshake()));

    // Every individual requirement is necessary.
    RUVIA_CHECK(rejects_request(post_handshake()));
    RUVIA_CHECK(rejects_request(http10_handshake()));
    // RFC 6455 permits additional HTTP fields, and RFC 9112 framing makes a
    // zero Content-Length an empty request. Its mere presence must not block
    // an otherwise valid protocol switch.
    RUVIA_CHECK(accepts_request(content_length_zero_handshake()));
    // Actual request content still prevents switching protocols because those
    // octets belong to the HTTP message rather than to the websocket stream.
    RUVIA_CHECK(rejects_request(content_length_one_handshake()));

    constexpr std::string_view no_connection_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejects_request(no_connection_upgrade));

    constexpr std::string_view duplicate_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejects_request(duplicate_key));

    constexpr std::string_view duplicate_version =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejects_request(duplicate_version));

    // The Upgrade header must name "websocket", not another protocol token.
    constexpr std::string_view wrong_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    RUVIA_CHECK(rejects_request(wrong_upgrade));

    // Sec-websocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    constexpr std::string_view bad_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n";
    RUVIA_CHECK(rejects_request(bad_key));

    // "...ZR==" decodes to the same 16 bytes as the canonical "...ZQ==",
    // but sets unused base64 padding bits and must therefore be rejected.
    constexpr std::string_view non_canonical_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n";
    RUVIA_CHECK(rejects_request(non_canonical_key));

    const auto unsupported_version = validate_request(bad_version_handshake());
    RUVIA_CHECK(unsupported_version.failure() != nullptr);
    if (const auto* failure = unsupported_version.failure()) {
        const auto error = failure->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::http_response response;
        failure->apply_required_response_headers(response);
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
    RUVIA_CHECK(accepts_request(request));
}

RUVIA_TEST(ws_server_handshake_response_serialization_is_http_owned) {
    const auto request = parse_request(
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
    const std::array<std::string_view, 1> supported_views{supported};
    const auto handshake =
        ruvia::make_websocket_server_handshake(request, {.supported_subprotocols_ = supported_views});
    supported.front() = 'X';

    std::vector<std::string_view> response_parts;
    handshake.for_each_response_part(
        [&response_parts](std::string_view part) { response_parts.push_back(part); });
    std::string response;
    for (const auto part : response_parts) {
        response.append(part);
    }
    RUVIA_CHECK_EQ(response, std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                         "Upgrade: websocket\r\n"
                                         "Connection: Upgrade\r\n"
                                         "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                         "Sec-WebSocket-Protocol: chat\r\n"
                                         "Sec-WebSocket-Extensions: permessage-deflate; "
                                         "server_no_context_takeover; client_no_context_takeover; "
                                         "server_max_window_bits=15\r\n"
                                         "\r\n"));
    RUVIA_CHECK(handshake.compression() ==
                (ruvia::websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    RUVIA_CHECK_EQ(handshake.subprotocol(), "chat");
}

RUVIA_TEST(ws_handshake_copies_application_headers_and_preserves_multiple_cookies) {
    const auto request = parse_request(valid_handshake());
    std::string cookie = "sid=0123456789abcdef; HttpOnly";
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", cookie),
        ruvia::http_header_view("Set-Cookie", "theme=dark"),
        ruvia::http_header_view("X-Request-Id", "request-1"),
        ruvia::http_header_view("Alt-Svc", "h3=\":443\"; ma=86400")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    cookie.assign(cookie.size(), 'x');
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    RUVIA_CHECK((response.find("set-cookie: sid=0123456789abcdef; HttpOnly\r\n") != std::string_view::npos));
    RUVIA_CHECK((response.find("set-cookie: theme=dark\r\n") != std::string_view::npos));
    RUVIA_CHECK((response.find("x-request-id: request-1\r\n") != std::string_view::npos));
    RUVIA_CHECK((response.find("alt-svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    RUVIA_CHECK(response.ends_with("\r\n\r\n"));
}

RUVIA_TEST(ws_handshake_rejects_application_framing_and_invalid_fields) {
    const auto request = parse_request(valid_handshake());
    for (const auto name : {"Connection", "Upgrade", "Content-Length", "Transfer-Encoding",
             "Sec-WebSocket-Accept", "Sec-WebSocket-Protocol", "TE", "Trailer", "bad name"}) {
        const std::array fields_value{ruvia::http_header_view(name, "value")};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", "sid=a\r\nInjected: value")};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(ws_handshake_bounds_application_header_count_and_size) {
    const auto request = parse_request(valid_handshake());
    const std::vector<ruvia::http_header_view> too_many(ruvia::max_http_header_fields, {"x", "v"});
    bool count_rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = too_many});
    } catch (const std::length_error&) {
        count_rejected = true;
    }
    RUVIA_CHECK(count_rejected);
    const std::string oversized(ruvia::max_http_header_bytes, 'a');
    const std::array fields_value{ruvia::http_header_view("x", oversized)};
    bool size_rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::length_error&) {
        size_rejected = true;
    }
    RUVIA_CHECK(size_rejected);
}

RUVIA_TEST(ws_handshake_application_values_are_valid_for_both_http_versions) {
    const auto request = parse_request(valid_handshake());
    for (const auto value : {" leading", "trailing ", "\tleading", "trailing\t"}) {
        const std::array headers{ruvia::http_header_view("X-Test", value)};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    const std::array headers{ruvia::http_header_view("X-Test", "")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    RUVIA_CHECK((response.find("x-test: \r\n") != std::string_view::npos));
}
