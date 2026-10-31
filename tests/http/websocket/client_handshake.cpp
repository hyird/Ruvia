#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "test_harness.h"

namespace {

constexpr std::array<std::uint8_t, 16> nonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// What a client holds across one opening handshake: the encoded key, the
// negotiation and the application headers it offers.
class client_handshake final {
public:
    explicit client_handshake(std::span<const ruvia::http_header_view> headers = {},
        std::span<const std::string_view> protocols = {}, std::string_view user_agent = {})
        : negotiation_({.subprotocols_ = protocols}),
          user_agent_(user_agent) {
        ruvia::validate_http1_websocket_client_handshake_configuration(headers, user_agent);
        ruvia::http_base64_encode(key_.data(), nonce);
        for (const auto header : headers) {
            headers_.emplace_back(header.name(), header.value(), std::pmr::get_default_resource());
        }
    }

    [[nodiscard]] std::variant<ruvia::prepared_http1_client_request,
        ruvia::http1_client_request_buffer_too_small, ruvia::http1_client_request_prepare_error>
    prepare(const ruvia::http_origin_view& origin, std::string_view target, std::span<char> buffer) const {
        return ruvia::prepare_http1_websocket_client_request(origin, target, key_, negotiation_, headers_,
            user_agent_, buffer, std::pmr::get_default_resource());
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
        ruvia::http1_websocket_client_handshake_error>
    validate(const ruvia::http1_parsed_client_response_head& response) const {
        return ruvia::validate_http1_websocket_client_response(key_, negotiation_, response);
    }

private:
    std::array<char, 24> key_{};
    ruvia::websocket_client_negotiation negotiation_;
    std::vector<ruvia::http_header> headers_;
    std::string user_agent_;
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
    std::span<const std::string_view> protocols = {}, std::string_view user_agent = {}) {
    try {
        const client_handshake handshake(headers, protocols, user_agent);
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
    const client_handshake handshake(headers, protocols, "RuviaTest/1");
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/chat", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
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
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result_value = handshake.validate(response);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(result_value).selected_subprotocol_, std::string_view("Chat"));
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    for (const auto response_protocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const std::string extra = response_protocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        const auto response = handshake.parse(valid_response(extra));
        const auto result_value = handshake.validate(response);
        RUVIA_CHECK(!(result_value.index() == 0));
        if (!(result_value.index() == 0)) {
            RUVIA_CHECK(std::get<1>(result_value) ==
                        ruvia::http1_websocket_client_handshake_error::subprotocol);
        }
    }
}

RUVIA_TEST(websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const client_handshake handshake;
    const auto bad = handshake.parse(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    RUVIA_CHECK(bad.plan().protocol_upgrade() != nullptr);
    const auto bad_result = handshake.validate(bad);
    RUVIA_CHECK(!(bad_result.index() == 0));
    if (!(bad_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(bad_result) == ruvia::http1_websocket_client_handshake_error::accept);
    }

    const auto duplicate =
        handshake.parse(valid_response("Sec-WebSocket-Accept: " + std::string(accept) + "\r\n"));
    const auto duplicate_result = handshake.validate(duplicate);
    RUVIA_CHECK(!(duplicate_result.index() == 0));
    if (!(duplicate_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(duplicate_result) ==
                    ruvia::http1_websocket_client_handshake_error::accept);
    }

    const auto response =
        handshake.parse(valid_response("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extension_result = handshake.validate(response);
    RUVIA_CHECK(!(extension_result.index() == 0));
    if (!(extension_result.index() == 0)) {
        RUVIA_CHECK(std::get<1>(extension_result) ==
                    ruvia::http1_websocket_client_handshake_error::extensions);
    }
}

RUVIA_TEST(websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const client_handshake handshake;
    auto parser = handshake.parser();
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        RUVIA_CHECK(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(valid_response());
    RUVIA_CHECK(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result_value = handshake.validate(*final.parsed());
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
    const std::array trailer{ruvia::http_header_view{"Trailer", "X-End"}};
    const std::array expect{ruvia::http_header_view{"Expect", "100-continue"}};
    const std::array pseudo{ruvia::http_header_view{":authority", "evil.example"}};
    const std::array extensions{ruvia::http_header_view{"Sec-WebSocket-Extensions", "permessage-deflate"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array bad_token{std::string_view{"bad token"}};
    const std::array comma_token{std::string_view{"chat,superchat"}};
    RUVIA_CHECK(rejects_configuration(bad_space));
    RUVIA_CHECK(rejects_configuration(bad_comma));
    RUVIA_CHECK(rejects_configuration(reserved));
    RUVIA_CHECK(rejects_configuration(transfer_encoding));
    RUVIA_CHECK(rejects_configuration(user_agent_header, {}, "another-agent"));
    RUVIA_CHECK(rejects_configuration(trailer));
    RUVIA_CHECK(rejects_configuration(expect));
    RUVIA_CHECK(rejects_configuration(pseudo));
    RUVIA_CHECK(rejects_configuration(extensions));
    RUVIA_CHECK(rejects_configuration({}, duplicate));
    RUVIA_CHECK(rejects_configuration({}, empty));
    RUVIA_CHECK(rejects_configuration({}, bad_token));
    RUVIA_CHECK(rejects_configuration({}, comma_token));
    RUVIA_CHECK(rejects_configuration({}, {}, "bad\r\nagent"));
}

RUVIA_TEST(websocket_client_handshake_prepares_ipv6_host) {
    const client_handshake handshake;
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "[2001:db8::1]", .port_ = 8443}), "/before", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    RUVIA_CHECK(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
}

RUVIA_TEST(websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result_value = handshake.validate(response);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        const auto selected = std::get<0>(result_value).selected_subprotocol_;
        RUVIA_CHECK_EQ(selected, std::string_view("chat"));
        RUVIA_CHECK_EQ(response.head().headers().back().value(), selected);
    }
}
