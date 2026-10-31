#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "test_harness.h"

namespace {
void add(std::vector<ruvia::http_header>& head, std::string_view name, std::string_view value) {
    head.emplace_back(name, value, std::pmr::get_default_resource());
}
}  // namespace

RUVIA_TEST(websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 10, .client_max_window_bits_ = 12}});
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validate_fields(head);
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(accepted).selected_subprotocol_, std::string_view("chat"));
    RUVIA_CHECK(!std::get<0>(accepted).compression_.server_no_context_takeover_);
    RUVIA_CHECK(std::get<0>(accepted).compression_.client_no_context_takeover_);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.server_max_window_bits_.value(), 10);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value(), 12);
}
RUVIA_TEST(websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::websocket_client_negotiation client({.deflate_ = {.enabled_ = true, .server_max_window_bits_ = 10, .offer_client_max_window_bits_ = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        RUVIA_CHECK(!(client.validate_fields(head).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::websocket_client_negotiation no_server_window_client({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        RUVIA_CHECK((no_server_window_client.validate_fields(head).index() == 0));
    }
    // The server may select a larger client window than the offered hint, or
    // none, but the client still never compresses with more than it offered.
    ruvia::websocket_client_negotiation limited_client_window({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .client_max_window_bits_ = 9}});
    for (auto extension : std::array{"permessage-deflate; client_max_window_bits=10", "permessage-deflate"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        auto accepted = limited_client_window.validate_fields(head);
        RUVIA_CHECK((accepted.index() == 0));
        if (accepted.index() == 0) {
            RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 9);
        }
    }
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=8");
        auto accepted = limited_client_window.validate_fields(head);
        RUVIA_CHECK((accepted.index() == 0));
        if (accepted.index() == 0) {
            RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 8);
        }
    }
    // A client takeover preference in an offer is a hint.
    ruvia::websocket_client_negotiation require_client_no_takeover({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = true}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate");
        auto accepted = require_client_no_takeover.validate_fields(head);
        RUVIA_CHECK((accepted.index() == 0));
        RUVIA_CHECK(std::get<0>(accepted).compression_.client_no_context_takeover_);
    }
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "unoffered");
    RUVIA_CHECK(!(client.validate_fields(head).index() == 0));
}
RUVIA_TEST(websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    std::array<char, 24> encoded_key{};
    ruvia::http_base64_encode(encoded_key.data(), nonce);
    const ruvia::websocket_client_negotiation negotiation({.deflate_ = {.enabled_ = true}});
    std::array<char, 2048> buffer{};
    auto prepared = ruvia::prepare_http1_websocket_client_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", encoded_key, negotiation, {}, {},
        buffer, std::pmr::get_default_resource());
    auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    RUVIA_CHECK((prepared_req->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::http1_client_response_parser parser(std::move(*prepared_req).take_exchange_state());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    RUVIA_CHECK(response.parsed() != nullptr);
    auto accepted = ruvia::validate_http1_websocket_client_response(encoded_key, negotiation, *response.parsed());
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK(std::get<0>(accepted).compression_.enabled_);
}

RUVIA_TEST(websocket_h2_client_negotiation_ignores_rfc6455_key_exchange_fields) {
    // RFC 8441 section 5: :protocol supersedes Sec-WebSocket-Accept, which an
    // Extended CONNECT client does not process.
    for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 2>{{
             {"sec-websocket-accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="},
             {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
         }}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto connection = ruvia::http2_connection::client({.resource_ = &resource});
        const auto append_frame = [&](std::pmr::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                      std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            RUVIA_CHECK(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::pmr::string settings_wire(&resource);
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        RUVIA_CHECK(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }

        ruvia::websocket_client_negotiation negotiation({}, &resource);
        const auto submitted = negotiation.submit_http2_request(connection, "https", "example.test", "/ws");
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        ruvia::hpack_encoder::encode_header(block, name, value);
        std::pmr::string wire(&resource);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, std::get<std::uint32_t>(submitted), block);
        RUVIA_CHECK(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = connection.next_event();
        const auto* head = event ? event->response_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head != nullptr) {
            RUVIA_CHECK(negotiation.validate_response(head->head(), true).index() == 0);
        }
    }

    // RFC 9113 section 8.2.2: connection-specific header fields in HTTP/2
    // responses are rejected as forbidden_field.
    ruvia::websocket_client_negotiation ws_neg({});
    for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 6>{{
             {"connection", "keep-alive"},
             {"upgrade", "websocket"},
             {"keep-alive", "timeout=5"},
             {"proxy-connection", "keep-alive"},
             {"transfer-encoding", "chunked"},
             {"te", "trailers"},
         }}) {
        ruvia::http1_client_request_writer writer;
        std::array<char, 512> req_buf{};
        auto prep = writer.prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), {.method_ = "GET", .target_ = "/ws"}, req_buf);
        ruvia::http1_client_response_parser resp_parser(
            std::move(std::get<ruvia::prepared_http1_client_request>(prep)).take_exchange_state());
        const std::string wire = "HTTP/1.1 200 OK\r\n" + std::string(name) + ": " + std::string(value) + "\r\n\r\n";
        const auto parsed = resp_parser.parse(wire);
        RUVIA_CHECK(parsed.parsed() != nullptr);
        if (parsed.parsed() != nullptr) {
            const auto res = ws_neg.validate_response(parsed.parsed()->head(), true);
            RUVIA_CHECK(std::holds_alternative<ruvia::websocket_client_negotiation_error>(res) &&
                        std::get<ruvia::websocket_client_negotiation_error>(res) ==
                            ruvia::websocket_client_negotiation_error::forbidden_field);
        }
    }
}

// RFC 8441 section 5: END_STREAM after the 2xx is the server's orderly tunnel
// close; only END_STREAM on the response HEADERS leaves no WebSocket stream.
// The fact is fixed by the HEADERS frame even when the DATA that ends the
// stream arrives in the same feed().
RUVIA_TEST(websocket_h2_client_response_head_reports_end_stream_only_from_headers) {
    for (const bool end_on_headers : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto connection = ruvia::http2_connection::client({.resource_ = &resource});
        const auto append_frame = [&](std::pmr::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                      std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            RUVIA_CHECK(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::pmr::string settings_wire(&resource);
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        RUVIA_CHECK(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({}, &resource);
        const auto submitted = negotiation.submit_http2_request(connection, "https", "example.test", "/ws");
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        std::pmr::string wire(&resource);
        append_frame(wire, ruvia::http2_frame_type::headers, end_on_headers ? 0x5 : 0x4, stream_id, block);
        if (!end_on_headers) {
            // The server's Close frame (1000, "bye") with END_STREAM.
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, std::string_view("\x88\x05\x03\xe8"
                                                                                               "bye",
                                                                                  7));
        }
        RUVIA_CHECK(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = connection.next_event();
        const auto* head = event ? event->response_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head == nullptr) {
            return;
        }
        RUVIA_CHECK_EQ(head->stream_ended(), end_on_headers);
        const auto negotiated = negotiation.validate_response(head->head(), !head->stream_ended());
        if (end_on_headers) {
            RUVIA_CHECK(std::holds_alternative<ruvia::websocket_client_negotiation_error>(negotiated) &&
                        std::get<ruvia::websocket_client_negotiation_error>(negotiated) ==
                            ruvia::websocket_client_negotiation_error::stream_closed);
        } else {
            RUVIA_CHECK(negotiated.index() == 0);
            auto data = connection.next_event();
            RUVIA_CHECK(data && data->tunnel_data() != nullptr);
            auto end = connection.next_event();
            RUVIA_CHECK(end && end->tunnel_end() != nullptr);
        }
    }
}
