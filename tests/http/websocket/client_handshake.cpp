#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_websocket_client_handshake.h"

#include "test_harness.h"

namespace {

constexpr std::array<std::uint8_t, 16> nonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

ruvia::http1_websocket_client_handshake make_handshake(
    std::span<const ruvia::http_header_view> headers = {},
    std::span<const std::string_view> protocols = {}, std::string_view user_agent = {}) {
    return ruvia::http1_websocket_client_handshake(
        {.nonce_ = nonce, .headers_ = headers, .subprotocols_ = protocols, .user_agent_ = user_agent},
        std::pmr::get_default_resource());
}

ruvia::http1_parsed_client_response_head parse_response(ruvia::testing::test_context& ruvia_ctx,
    ruvia::http1_client_exchange_state state_value, std::string_view wire) {
    ruvia::http1_client_response_parser parser(std::move(state_value));
    auto result_value = parser.parse(wire);
    RUVIA_CHECK(result_value.parsed() != nullptr);
    if (result_value.parsed() == nullptr) {
        throw std::runtime_error("expected parsed response");
    }
    return std::move(*result_value.parsed());
}

constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

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
    std::span<const std::string_view> protocols = {}, std::string_view user_agent = {}) {
    try {
        auto handshake = make_handshake(headers, protocols, user_agent);
        (void)handshake;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(websocket_client_handshake_prepares_rfc_request) {
    const std::array protocols{std::string_view{"chat"}};
    const std::array headers{ruvia::http_header_view{"X-Trace", "one"}};
    const auto handshake = make_handshake(headers, protocols, "RuviaTest/1");
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/chat", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    const std::string wire(prepared.prepared()->head());
    RUVIA_CHECK(wire.find("GET /chat HTTP/1.1\r\n") == 0);
    RUVIA_CHECK(wire.find("Host: example.com\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
                std::string::npos);
    RUVIA_CHECK(wire.find("sec-websocket-protocol: chat\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("User-Agent: RuviaTest/1\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("X-Trace: one\r\n") != std::string::npos);
}

RUVIA_TEST(websocket_client_handshake_matches_subprotocol_case_exactly) {
    const std::array protocols{std::string_view{"chat"}, std::string_view{"Chat"}};
    const auto handshake = make_handshake({}, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
    auto response = parse_response(ruvia_ctx, prepared.prepared()->exchange_state(),
        valid_response("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result_value = handshake.validate_response(response);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(result_value).selected_subprotocol_, std::string_view("Chat"));
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    for (const auto response_protocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const auto handshake = make_handshake({}, protocols);
        std::array<char, 2048> buffer{};
        const auto prepared = handshake.prepare_request(
            ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
        const std::string extra = response_protocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        auto response = parse_response(ruvia_ctx, prepared.prepared()->exchange_state(),
            valid_response(extra));
        const auto result_value = handshake.validate_response(response);
        RUVIA_CHECK(!(result_value.index() == 0));
        if (!(result_value.index() == 0)) {
            RUVIA_CHECK(std::get<1>(result_value) ==
                        ruvia::http1_websocket_client_handshake_error::subprotocol);
        }
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const auto handshake = make_handshake();
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
    auto bad = parse_response(ruvia_ctx, prepared.prepared()->exchange_state(),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    RUVIA_CHECK(bad.plan().protocol_upgrade() != nullptr);
    const auto bad_result = handshake.validate_response(bad);
    RUVIA_CHECK(!(bad_result.index() == 0));
    if (!(bad_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(bad_result) == ruvia::http1_websocket_client_handshake_error::accept);
    }

    auto duplicate = parse_response(ruvia_ctx, prepared.prepared()->exchange_state(),
        valid_response("Sec-WebSocket-Accept: " + std::string(accept) + "\r\n"));
    const auto duplicate_result = handshake.validate_response(duplicate);
    RUVIA_CHECK(!(duplicate_result.index() == 0));
    if (!(duplicate_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(duplicate_result) ==
                    ruvia::http1_websocket_client_handshake_error::accept);
    }

    const auto extension = make_handshake();
    std::array<char, 2048> extension_buffer{};
    const auto extension_prepared = extension.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", extension_buffer);
    auto response = parse_response(ruvia_ctx, extension_prepared.prepared()->exchange_state(),
        valid_response("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extension_result = extension.validate_response(response);
    RUVIA_CHECK(!(extension_result.index() == 0));
    if (!(extension_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(extension_result) ==
                    ruvia::http1_websocket_client_handshake_error::extensions);
    }
}

RUVIA_TEST(websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const auto handshake = make_handshake();
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
    ruvia::http1_client_response_parser parser(prepared.prepared()->exchange_state());
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        RUVIA_CHECK(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(valid_response());
    RUVIA_CHECK(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result_value = handshake.validate_response(*final.parsed());
        RUVIA_CHECK((result_value.index() == 0));
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_invalid_configuration_and_reserved_headers) {
    const std::array bad_space{ruvia::http_header_view{"X-Test", "bad value\r\n"}};
    const std::array bad_comma{ruvia::http_header_view{"X-Test", "bad,\r\nvalue"}};
    const std::array reserved{ruvia::http_header_view{"Host", "evil.example"}};
    const std::array transfer_encoding{
        ruvia::http_header_view{"Transfer-Encoding", "chunked"}};
    const std::array user_agent_header{ruvia::http_header_view{"User-Agent", "custom-agent"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array bad_token{std::string_view{"bad token"}};
    const std::array comma_token{std::string_view{"chat,superchat"}};
    RUVIA_CHECK(rejects_configuration(bad_space));
    RUVIA_CHECK(rejects_configuration(bad_comma));
    RUVIA_CHECK(rejects_configuration(reserved));
    RUVIA_CHECK(rejects_configuration(transfer_encoding));
    RUVIA_CHECK(rejects_configuration(user_agent_header, {}, "another-agent"));
    RUVIA_CHECK(rejects_configuration({}, duplicate));
    RUVIA_CHECK(rejects_configuration({}, empty));
    RUVIA_CHECK(rejects_configuration({}, bad_token));
    RUVIA_CHECK(rejects_configuration({}, comma_token));
    RUVIA_CHECK(rejects_configuration({}, {}, "bad\r\nagent"));
}

RUVIA_TEST(websocket_client_handshake_prepares_ipv6_host_and_owns_configuration) {
    std::string header_name = "X-Trace";
    std::string header_value = "initial";
    std::string protocol = "chat";
    std::string user_agent = "initial-agent";
    const std::array headers{ruvia::http_header_view{header_name, header_value}};
    const std::array protocols{std::string_view(protocol)};
    const auto handshake = make_handshake(headers, protocols, user_agent);
    header_name = "Changed";
    header_value = "changed";
    protocol = "changed";
    user_agent = "changed-agent";
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "[2001:db8::1]", .port_ = 8443}), "/before", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    const std::string wire(prepared.prepared()->head());
    RUVIA_CHECK(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("X-Trace: initial\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("sec-websocket-protocol: chat\r\n") != std::string::npos);
    RUVIA_CHECK(wire.find("User-Agent: initial-agent\r\n") != std::string::npos);
}

RUVIA_TEST(websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const auto handshake = make_handshake({}, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
    auto response = parse_response(ruvia_ctx, prepared.prepared()->exchange_state(),
        valid_response("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result_value = handshake.validate_response(response);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        const auto selected = std::get<0>(result_value).selected_subprotocol_;
        RUVIA_CHECK_EQ(selected, std::string_view("chat"));
        RUVIA_CHECK_EQ(response.head().headers().back().value(), selected);
    }
}
