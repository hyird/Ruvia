#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/http/websocket_handshake.h"

#include "http1_request_message_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::http_request;
using ruvia::validate_websocket_handshake;
using ruvia::test::parse_http1_request_message;

http_request parse_request(std::string_view raw_request) {
    auto parsed_value = parse_http1_request_message(raw_request);
    return std::move(parsed_value.state_).take_request();
}

[[nodiscard]] auto validate_request(std::string_view raw_request) {
    const auto parsed_value = parse_http1_request_message(raw_request);
    return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan());
}

[[nodiscard]] bool accepts_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.accepted() != nullptr;
}

[[nodiscard]] bool rejects_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.failure() != nullptr;
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

TEST(Ruvia, ws_h1_handshake_preserves_application_request_headers) {
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

    EXPECT_TRUE(request.header("Host") == "example.test");
    EXPECT_TRUE(request.header("Origin") == "https://origin.example");
    EXPECT_TRUE(request.header("Authorization") == "Bearer credential");
    EXPECT_TRUE(request.header("Cookie") == "sid=one; theme=dark");
    EXPECT_TRUE(request.header("Sec-WebSocket-Protocol") == "chat");
    EXPECT_TRUE(request.header("Sec-WebSocket-Extensions") == "permessage-deflate");
    EXPECT_TRUE(request.header("X-End-To-End") == "retained");
    EXPECT_TRUE(accepts_request(raw));
}

TEST(Ruvia, ws_public_subprotocol_set_validates_unique_tokens) {
    ruvia::websocket_subprotocol_set protocols;
    EXPECT_TRUE(protocols.append_list(", chat, superchat,"));
    EXPECT_TRUE(protocols.contains("chat"));
    EXPECT_TRUE(protocols.contains("superchat"));
    EXPECT_TRUE(!protocols.append("chat"));
    EXPECT_TRUE(!protocols.append_list("other, chat"));
    EXPECT_TRUE(!protocols.append("bad token"));

    // A received offer skips repeats instead of failing (RFC 6455 4.2.1).
    ruvia::websocket_subprotocol_set offered;
    EXPECT_TRUE(offered.append_offered_list("chat, chat"));
    EXPECT_TRUE(offered.append_offered_list("superchat, chat"));
    EXPECT_TRUE(offered.contains("chat"));
    EXPECT_TRUE(offered.contains("superchat"));
    EXPECT_TRUE(!offered.append_offered_list("chat, bad token"));
}

TEST(Ruvia, ws_subprotocol_offers_require_http_tokens_and_skip_repeats) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: , chat,, superchat,\r\n")));
    // Subprotocol identifiers are case-sensitive, so these are distinct.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, Chat\r\n")));

    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: bad token\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: \"chat\"\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: , ,\r\n")));
    // Uniqueness binds the sending client (RFC 6455 section 4.1 item 10); the
    // server's checks (section 4.2.1) have no rule for a repeated offer.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n")));
    EXPECT_TRUE(accepts_request(with_protocols(
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Protocol: superchat, chat\r\n")));
    constexpr std::array<std::string_view, 1> supported{"chat"};
    // 请求头借用原始报文，握手完成前必须保持报文存储存活。
    const auto repeated_wire = with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n");
    const auto repeated = parse_request(repeated_wire);
    const auto selected = ruvia::make_websocket_server_handshake(repeated, {.supported_subprotocols_ = supported});
    EXPECT_TRUE(selected.subprotocol() == "chat");

    std::string too_many = "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i <= ruvia::max_websocket_subprotocols; ++i) {
        if (i != 0) {
            too_many.append(", ");
        }
        too_many.append("protocol-");
        too_many.append(std::to_string(i));
    }
    too_many.append("\r\n");
    EXPECT_TRUE(rejects_request(with_protocols(too_many)));
}

TEST(Ruvia, ws_required_subprotocol_rejects_offers_without_a_supported_entry) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };
    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto validate = [&](std::string_view raw_request, bool required) {
        const auto parsed_value = parse_http1_request_message(raw_request);
        return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan(),
            {.supported_subprotocols_ = std::span<const std::string_view>(supported),
                .require_subprotocol_ = required});
    };
    const auto accepts = [&](std::string_view raw_request, bool required) {
        const auto result_value = validate(raw_request, required);
        return result_value.accepted() != nullptr;
    };

    // Not required: an offer sharing nothing is still a valid handshake.
    EXPECT_TRUE(accepts(valid_handshake(), false));
    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other\r\n"), false));

    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other, chat.v1\r\n"), true));
    for (const std::string_view raw_request :
        {valid_handshake(), std::string_view("GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
                                             "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                             "Sec-WebSocket-Protocol: other, Chat.v1\r\n\r\n")}) {
        const auto result_value = validate(raw_request, true);
        const auto* failure = result_value.failure();
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::bad_request);
            ruvia::http_response response;
            failure->apply_required_response_headers(response);
            EXPECT_TRUE(!response.header("Sec-WebSocket-Version").has_value());
        }
    }
}

TEST(Ruvia, ws_extension_offers_must_match_the_rfc6455_abnf) {
    const auto with_extensions = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(
        with_extensions("Sec-WebSocket-Extensions: , x-test; flag; value=token,,\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\"to\\ken\"\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions(
        "Sec-WebSocket-Extensions: x-test\r\n"
        "Sec-WebSocket-Extensions: y-test; value=token\r\n")));

    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: , ,\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: \"x-test\"\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x test\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;; flag\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; =value\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"bad value\"\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"unterminated\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"token\"junk\r\n")));
}

TEST(Ruvia, ws_valid_request_requires_all_conditions) {
    EXPECT_TRUE(accepts_request(valid_handshake()));

    // Every individual requirement is necessary.
    EXPECT_TRUE(rejects_request(post_handshake()));
    EXPECT_TRUE(rejects_request(http10_handshake()));
    // RFC 6455 permits additional HTTP fields, and RFC 9112 framing makes a
    // zero Content-Length an empty request. Its mere presence must not block
    // an otherwise valid protocol switch.
    EXPECT_TRUE(accepts_request(content_length_zero_handshake()));
    // Actual request content still prevents switching protocols because those
    // octets belong to the HTTP message rather than to the websocket stream.
    EXPECT_TRUE(rejects_request(content_length_one_handshake()));

    constexpr std::string_view no_connection_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(no_connection_upgrade));

    constexpr std::string_view duplicate_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_key));

    constexpr std::string_view duplicate_version =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_version));

    // The Upgrade header must name "websocket", not another protocol token.
    constexpr std::string_view wrong_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(wrong_upgrade));

    // Sec-websocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    constexpr std::string_view bad_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n";
    EXPECT_TRUE(rejects_request(bad_key));

    // "...ZR==" decodes to the same 16 bytes as the canonical "...ZQ==",
    // but sets unused base64 padding bits and must therefore be rejected.
    constexpr std::string_view non_canonical_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n";
    EXPECT_TRUE(rejects_request(non_canonical_key));

    const auto unsupported_version = validate_request(bad_version_handshake());
    EXPECT_TRUE(unsupported_version.failure() != nullptr);
    if (const auto* failure = unsupported_version.failure()) {
        const auto error = failure->protocol_error();
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::http_response response;
        failure->apply_required_response_headers(response);
        EXPECT_EQ(response.header("Sec-WebSocket-Version"), std::string_view("13"));
    }
}

TEST(Ruvia, ws_upgrade_uses_the_shared_recipient_list_semantics) {
    constexpr std::string_view request =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , custom/1, websocket,\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    EXPECT_TRUE(accepts_request(request));
}

TEST(Ruvia, ws_server_handshake_response_serialization_is_http_owned) {
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
    EXPECT_EQ(response, std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                    "Sec-WebSocket-Protocol: chat\r\n"
                                    "Sec-WebSocket-Extensions: permessage-deflate; "
                                    "server_no_context_takeover; client_no_context_takeover; "
                                    "server_max_window_bits=15\r\n"
                                    "\r\n"));
    EXPECT_TRUE(handshake.compression() ==
                (ruvia::websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    EXPECT_EQ(handshake.subprotocol(), "chat");
}

TEST(Ruvia, ws_server_handshake_negotiates_directional_context_takeover_independently) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n"
        "\r\n");
    const auto handshake = ruvia::make_websocket_server_handshake(
        request, {.deflate_ = {.enabled_ = true, .context_takeover_ = true}});
    EXPECT_TRUE(handshake.compression().enabled_);
    EXPECT_TRUE(!handshake.compression().server_no_context_takeover_);
    EXPECT_TRUE(handshake.compression().client_no_context_takeover_);
}

TEST(Ruvia, ws_handshake_copies_application_headers_and_preserves_multiple_cookies) {
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
    EXPECT_TRUE((response.find("set-cookie: sid=0123456789abcdef; HttpOnly\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("set-cookie: theme=dark\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("x-request-id: request-1\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("alt-svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(Ruvia, ws_handshake_rejects_application_framing_and_invalid_fields) {
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
        EXPECT_TRUE(rejected);
    }
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", "sid=a\r\nInjected: value")};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_bounds_application_header_size) {
    const auto request = parse_request(valid_handshake());
    const std::vector<ruvia::http_header_view> many(80, {"x", "v"});
    (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = many});
    const std::string oversized(ruvia::max_http_header_bytes, 'a');
    const std::array fields_value{ruvia::http_header_view("x", oversized)};
    bool size_rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::length_error&) {
        size_rejected = true;
    }
    EXPECT_TRUE(size_rejected);
}

TEST(Ruvia, ws_handshake_budget_covers_status_line_and_final_empty_line) {
    const auto request = parse_request(valid_handshake());
    const auto serialize = [](const ruvia::websocket_server_handshake& handshake) {
        std::string response;
        handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
        return response;
    };
    const auto base_size = serialize(ruvia::make_websocket_server_handshake(request, {})).size();
    // "x: " + value + CRLF fills the head to exactly max_http_header_bytes.
    const std::string largest(ruvia::max_http_header_bytes - base_size - 5, 'a');
    const std::array fitting{ruvia::http_header_view("x", largest)};
    EXPECT_EQ(serialize(ruvia::make_websocket_server_handshake(request, {.response_headers_ = fitting})).size(), ruvia::max_http_header_bytes);
    const std::string oversized = largest + 'a';
    const std::array too_large{ruvia::http_header_view("x", oversized)};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = too_large});
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_application_values_are_valid_for_both_http_versions) {
    const auto request = parse_request(valid_handshake());
    for (const auto value : {" leading", "trailing ", "\tleading", "trailing\t"}) {
        const std::array headers{ruvia::http_header_view("X-Test", value)};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const std::array headers{ruvia::http_header_view("X-Test", "")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    EXPECT_TRUE((response.find("x-test: \r\n") != std::string_view::npos));
}

namespace {

template <typename subprotocols_type>
concept ws_server_handshake_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_server_handshake_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

template <typename subprotocols_type>
concept ws_handshake_validation_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_handshake_validation_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

static_assert(ws_server_handshake_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::array<std::string_view, 1>>);
static_assert(ws_handshake_validation_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::array<std::string_view, 1>>);

template <typename protocol_type>
concept ws_subprotocol_set_accepts_append = requires(ruvia::websocket_subprotocol_set& set, protocol_type&& protocol) {
    set.append(std::forward<protocol_type>(protocol));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_list = requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
    set.append_list(std::forward<list_type>(list));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_offered_list =
    requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
        set.append_offered_list(std::forward<list_type>(list));
    };

static_assert(ws_subprotocol_set_accepts_append<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append<std::string>);
static_assert(ws_subprotocol_set_accepts_append_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_list<std::string>);
static_assert(ws_subprotocol_set_accepts_append_offered_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_offered_list<std::string>);

}  // namespace

namespace {

constexpr std::array<std::uint8_t, 16> nonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// What a client holds across one opening handshake: the encoded key, the
// negotiation and the application headers it offers.
class client_handshake final {
public:
    explicit client_handshake(std::span<const ruvia::http_header_view> headers = {},
        std::span<const std::string_view> protocols = {})
        : negotiation_({.subprotocols_ = protocols}) {
        ruvia::validate_http1_websocket_client_handshake_configuration(headers);
        ruvia::http_base64_encode(key_.data(), nonce);
        for (const auto header : headers) {
            headers_.emplace_back(header.name(), header.value());
        }
    }

    [[nodiscard]] std::variant<ruvia::prepared_http1_client_request,
        ruvia::http1_client_request_buffer_too_small, ruvia::http1_client_request_prepare_error>
    prepare(const ruvia::http_origin_view& origin, std::string_view target, std::span<char> buffer) const {
        return ruvia::prepare_http1_websocket_client_request(origin, target, key_, negotiation_, headers_, buffer);
    }

    // The response parser takes a freshly prepared request's exchange state.
    [[nodiscard]] ruvia::http1_client_response_parser parser() const {
        std::array<char, 2048> buffer{};
        auto prepared = prepare(ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
        if (prepared.index() != 0) {
            throw std::runtime_error("expected prepared handshake request");
        }
        return ruvia::http1_client_response_parser(std::get<0>(std::move(prepared)).take_exchange_state());
    }

    [[nodiscard]] ruvia::http1_parsed_client_response_head parse(std::string_view wire) const {
        auto response_parser = parser();
        auto result_value = response_parser.parse(wire);
        if (result_value.parsed() == nullptr) {
            throw std::runtime_error("expected parsed response");
        }
        return std::move(*result_value.parsed());
    }

    [[nodiscard]] std::variant<ruvia::websocket_client_negotiation_result_view,
        ruvia::websocket_client_handshake_error>
    validate(const ruvia::http1_parsed_client_response_head& response) const {
        return ruvia::validate_http1_websocket_client_response(key_, negotiation_, response);
    }

private:
    std::array<char, 24> key_{};
    ruvia::websocket_client_negotiation negotiation_;
    std::vector<ruvia::http_header> headers_;
};

std::string valid_response(std::string_view extra = {}) {
    std::string wire =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    wire += accept;
    wire += "\r\n";
    wire += extra;
    wire += "\r\n";
    return wire;
}

bool rejects_configuration(std::span<const ruvia::http_header_view> headers = {},
    std::span<const std::string_view> protocols = {}) {
    try {
        const client_handshake handshake(headers, protocols);
        (void)handshake;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, websocket_client_handshake_prepares_rfc_request) {
    const std::array protocols{std::string_view{"chat"}};
    const std::array headers{
        ruvia::http_header_view{"X-Trace", "one"}, ruvia::http_header_view{"User-Agent", "RuviaTest/1"}};
    const client_handshake handshake(headers, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/chat", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("GET /chat HTTP/1.1\r\n") == 0);
    EXPECT_TRUE(wire.find("Host: example.com\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
                std::string::npos);
    EXPECT_TRUE(wire.find("sec-websocket-protocol: chat\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("User-Agent: RuviaTest/1\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("X-Trace: one\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_matches_subprotocol_case_exactly) {
    const std::array protocols{std::string_view{"chat"}, std::string_view{"Chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        EXPECT_EQ(std::get<0>(result_value).selected_subprotocol_, std::string_view("Chat"));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    for (const auto response_protocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const std::string extra = response_protocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        const auto response = handshake.parse(valid_response(extra));
        const auto result_value = handshake.validate(response);
        EXPECT_TRUE(!(result_value.index() == 0));
        if (!(result_value.index() == 0)) {
            EXPECT_TRUE(std::get<1>(result_value) == ruvia::websocket_client_handshake_error::subprotocol);
        }
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const client_handshake handshake;
    const auto bad = handshake.parse(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    EXPECT_TRUE(bad.plan().protocol_upgrade() != nullptr);
    const auto bad_result = handshake.validate(bad);
    EXPECT_TRUE(!(bad_result.index() == 0));
    if (!(bad_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(bad_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto duplicate =
        handshake.parse(valid_response("Sec-WebSocket-Accept: " + std::string(accept) + "\r\n"));
    const auto duplicate_result = handshake.validate(duplicate);
    EXPECT_TRUE(!(duplicate_result.index() == 0));
    if (!(duplicate_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(duplicate_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto response =
        handshake.parse(valid_response("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extension_result = handshake.validate(response);
    EXPECT_TRUE(!(extension_result.index() == 0));
    if (!(extension_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(extension_result) == ruvia::websocket_client_handshake_error::extensions);
    }
}

TEST(Ruvia, websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const client_handshake handshake;
    auto parser = handshake.parser();
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        EXPECT_TRUE(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(valid_response());
    EXPECT_TRUE(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result_value = handshake.validate(*final.parsed());
        EXPECT_TRUE((result_value.index() == 0));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_invalid_configuration_and_reserved_headers) {
    const std::array bad_space{ruvia::http_header_view{"X-Test", "bad value\r\n"}};
    const std::array bad_comma{ruvia::http_header_view{"X-Test", "bad,\r\nvalue"}};
    const std::array reserved{ruvia::http_header_view{"Host", "evil.example"}};
    const std::array transfer_encoding{
        ruvia::http_header_view{"Transfer-Encoding", "chunked"}};
    const std::array bad_user_agent{ruvia::http_header_view{"User-Agent", "bad\r\nagent"}};
    const std::array trailer{ruvia::http_header_view{"Trailer", "X-End"}};
    const std::array expect{ruvia::http_header_view{"Expect", "100-continue"}};
    const std::array pseudo{ruvia::http_header_view{":authority", "evil.example"}};
    const std::array extensions{ruvia::http_header_view{"Sec-WebSocket-Extensions", "permessage-deflate"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array bad_token{std::string_view{"bad token"}};
    const std::array comma_token{std::string_view{"chat,superchat"}};
    EXPECT_TRUE(rejects_configuration(bad_space));
    EXPECT_TRUE(rejects_configuration(bad_comma));
    EXPECT_TRUE(rejects_configuration(reserved));
    EXPECT_TRUE(rejects_configuration(transfer_encoding));
    EXPECT_TRUE(rejects_configuration(trailer));
    EXPECT_TRUE(rejects_configuration(expect));
    EXPECT_TRUE(rejects_configuration(pseudo));
    EXPECT_TRUE(rejects_configuration(extensions));
    EXPECT_TRUE(rejects_configuration({}, duplicate));
    EXPECT_TRUE(rejects_configuration({}, empty));
    EXPECT_TRUE(rejects_configuration({}, bad_token));
    EXPECT_TRUE(rejects_configuration({}, comma_token));
    EXPECT_TRUE(rejects_configuration(bad_user_agent));
}

TEST(Ruvia, websocket_client_handshake_prepares_ipv6_host) {
    const client_handshake handshake;
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "[2001:db8::1]", .port_ = 8443}), "/before", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        const auto selected = std::get<0>(result_value).selected_subprotocol_;
        EXPECT_EQ(selected, std::string_view("chat"));
        EXPECT_EQ(response.head().headers().back().value(), selected);
    }
}

namespace {
void add(std::vector<ruvia::http_header>& head, std::string_view name, std::string_view value) {
    head.emplace_back(name, value);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 10, .client_max_window_bits_ = 12}}});
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validate_fields(head);
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_EQ(std::get<0>(accepted).selected_subprotocol_, std::string_view("chat"));
    EXPECT_TRUE(!std::get<0>(accepted).compression_.server_no_context_takeover_);
    EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_.value(), 10);
    EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value(), 12);
}
TEST(Ruvia, websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::websocket_client_negotiation client({.deflate_ = {.compression_ = {.enabled_ = true, .server_max_window_bits_ = 10}, .offer_client_max_window_bits_ = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::websocket_client_negotiation no_server_window_client({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        EXPECT_TRUE((no_server_window_client.validate_fields(head).index() == 0));
    }
    // The server may select a larger client window than the offered hint, or
    // none, but the client still never compresses with more than it offered.
    ruvia::websocket_client_negotiation limited_client_window({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .client_max_window_bits_ = 9}}});
    for (auto extension : std::array{"permessage-deflate; client_max_window_bits=10", "permessage-deflate"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 9);
        }
    }
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=8");
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 8);
        }
    }
    // A client takeover preference in an offer is a hint.
    ruvia::websocket_client_negotiation require_client_no_takeover({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = true}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate");
        auto accepted = require_client_no_takeover.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    }
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "unoffered");
    EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
}
TEST(Ruvia, websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    std::array<char, 24> encoded_key{};
    ruvia::http_base64_encode(encoded_key.data(), nonce);
    const ruvia::websocket_client_negotiation negotiation({.deflate_ = {.compression_ = {.enabled_ = true}}});
    std::array<char, 2048> buffer{};
    auto prepared = ruvia::prepare_http1_websocket_client_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", encoded_key, negotiation, std::span<const ruvia::http_header>{}, buffer);
    auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::http1_client_response_parser parser(std::move(*prepared_req).take_exchange_state());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    EXPECT_TRUE(response.parsed() != nullptr);
    auto accepted = ruvia::validate_http1_websocket_client_response(encoded_key, negotiation, *response.parsed());
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
}

TEST(Ruvia, websocket_client_negotiation_supports_multiline_and_comma_extensions_while_rejecting_duplicates) {
    ruvia::websocket_client_negotiation client(
        {.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .server_max_window_bits_ = 12}}});

    // Multi-line Sec-WebSocket-Extensions with empty/comma lines per RFC 6455 §9.1 / RFC 9110 §5.3.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", ",");
        add(head, "sec-websocket-extensions", " , permessage-deflate; server_max_window_bits=10 , ");
        auto accepted = client.validate_fields(head);
        ASSERT_EQ(accepted.index(), 0U);
        EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
        EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_, std::optional<int>(10));
    }

    // Duplicate permessage-deflate across multiple header lines is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Duplicate permessage-deflate in a single comma-separated line is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions",
            "permessage-deflate; server_max_window_bits=10, permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Unknown extension on a second header line or comma-separated item is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10, x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Present but empty Sec-WebSocket-Extensions violates 1#extension.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", " , ");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
}

namespace {

template <typename fields_type>
concept ws_client_negotiation_accepts_fields =
    requires(const ruvia::websocket_client_negotiation& negotiation, fields_type&& fields) {
        negotiation.validate_fields(std::forward<fields_type>(fields));
    };

static_assert(ws_client_negotiation_accepts_fields<const std::vector<ruvia::http_header>&>);
static_assert(!ws_client_negotiation_accepts_fields<std::vector<ruvia::http_header>>);
static_assert(!ws_client_negotiation_accepts_fields<std::array<ruvia::http_header, 1>>);

}  // namespace

namespace {
using namespace ruvia;

bool zero_mask(void*, websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

struct client_endpoint final {
    explicit client_endpoint(websocket_compression compression)
        : protocol_(input_, {.message_limit_ = protocol_byte_limit::unlimited(),
                                .compression_ = compression,
                                .role_ = websocket_connection_role::client,
                                .mask_key_generator_ = &zero_mask}) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

// Takes every pending output byte of a sender.
template <typename sender_type>
[[nodiscard]] std::string drain(sender_type& sender) {
    const auto plan = sender.output_plan();
    std::string bytes_value(plan.bytes());
    bytes_value.append(plan.borrowed_bytes());
    EXPECT_TRUE(sender.consume_output(bytes_value.size()) == websocket_output_consume_status::drained);
    return bytes_value;
}

struct server_endpoint final {
    explicit server_endpoint(websocket_connection_options options)
        : input_(),
          protocol_(input_, [&] {
              options.message_limit_ = protocol_byte_limit::unlimited();
              return options;
          }()) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

}  // namespace

// A direction without context takeover produces the same wire bytes whether
// it borrows the shared codecs or owns a per-connection codec, and with the
// shared codecs the connection's resource never holds codec state.

namespace {
using namespace ruvia;

}  // namespace
