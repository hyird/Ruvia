#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/websocket_handshake.h"
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
