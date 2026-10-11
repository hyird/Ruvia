#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_websocket_client_handshake.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "test_harness.h"

namespace {
void add(ruvia::http3_message_head& head, std::string_view name, std::string_view value) {
    head.headers_.push_back(ruvia::http_header::copy_of(name, value, head.headers_.get_allocator().resource()));
}
void ignore_event(void*, const ruvia::http3_connection_event&) {}
void apply_http3_peer_settings(
    ruvia::testing::test_context& ruvia_ctx, ruvia::http3_connection& connection, ruvia::http3_settings settings) {
    const auto prefixes = ruvia::http3_local_critical_streams::create(settings);
    RUVIA_CHECK(prefixes.index() == 0);
    const auto fed = connection.feed(3, std::get<0>(prefixes).control_prefix(), false, false, ignore_event, nullptr);
    RUVIA_CHECK(fed.scope_ == ruvia::http3_connection_error_scope::none);
}
}  // namespace
RUVIA_TEST(websocket_client_http2_tunnel_peer_fin_keeps_local_send_open) {
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    std::string wire(server.pending_output());
    (void)server.consume_output(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    ruvia::websocket_client_negotiation negotiation({});
    const auto submitted = negotiation.submit_http2_request(client, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    wire.assign(client.pending_output());
    (void)client.consume_output(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head());
    if (!request || !request->request_head()) {
        return;
    }
    const auto validation = ruvia::validate_http2_websocket_handshake(server, 1, request->request_head()->request());
    const auto handshake = server.submit_websocket_handshake(1, request->request_head()->request(), validation);
    RUVIA_CHECK(handshake.submitted() != nullptr);
    wire.assign(server.pending_output());
    (void)server.consume_output(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    auto response = client.next_event();
    RUVIA_CHECK(response && response->response_head());
    RUVIA_CHECK(server.submit_data(1, "peer", ruvia::http2_end_stream::end_stream) == ruvia::http2_data_submit_status::accepted);
    wire.assign(server.pending_output());
    (void)server.consume_output(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    auto data = client.next_event();
    RUVIA_CHECK(data && data->tunnel_data());
    auto end = client.next_event();
    RUVIA_CHECK(end && end->tunnel_end());
    data.reset();
    RUVIA_CHECK(!client.stream_aborted(1));
    RUVIA_CHECK(client.submit_data(1, "local", ruvia::http2_end_stream::keep_open) == ruvia::http2_data_submit_status::accepted);
    wire.assign(client.pending_output());
    (void)client.consume_output(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    auto local_data = server.next_event();
    RUVIA_CHECK(local_data && local_data->tunnel_data());
    if (local_data && local_data->tunnel_data()) {
        RUVIA_CHECK_EQ(local_data->tunnel_data()->bytes(), std::string_view("local"));
    }
    RUVIA_CHECK(client.submit_data(1, {}, ruvia::http2_end_stream::end_stream) == ruvia::http2_data_submit_status::accepted);
    wire.assign(client.pending_output());
    (void)client.consume_output(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::http2_feed_result::protocol_failure);
    auto local_end = server.next_event();
    RUVIA_CHECK(local_end && local_end->tunnel_end());
    RUVIA_CHECK(client.submit_data(1, "late", ruvia::http2_end_stream::keep_open) == ruvia::http2_data_submit_status::closed);
}

RUVIA_TEST(websocket_client_extended_connect_requires_peer_setting_and_emits_protocol) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.enabled_ = true}});
    ruvia::http3_connection http3(ruvia::http3_peer_role::client, std::pmr::get_default_resource());
    auto disabled = client.encode_http3_request(http3, 0, "https", "example.com", "/chat");
    RUVIA_CHECK(!(disabled.index() == 0));
    RUVIA_CHECK(std::get<1>(disabled).kind_ == ruvia::http3_client_request_head_error::connect_protocol_disabled);
    apply_http3_peer_settings(ruvia_ctx, http3, {.enable_connect_protocol_ = true});
    auto encoded = client.encode_http3_request(http3, 0, "https", "example.com", "/chat");
    RUVIA_CHECK((encoded.index() == 0));
    auto head = ruvia::decode_http3_message_head(std::get<0>(encoded).headers_.field_section(), ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((head.index() == 0));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(head).protocol_), std::string_view("websocket"));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(head).method_), std::string_view("CONNECT"));
    RUVIA_CHECK(!std::get<0>(head).content_length_);
    auto connection = ruvia::http2_connection::client();
    auto submitted = client.submit_http2_request(connection, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.failure() != nullptr);
    auto server = ruvia::http2_connection::server();
    const std::string peer_settings(server.pending_output());
    RUVIA_CHECK(connection.feed(peer_settings) != ruvia::http2_feed_result::protocol_failure);
    submitted = client.submit_http2_request(connection, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const std::string request_wire(connection.pending_output());
    RUVIA_CHECK(server.feed(request_wire) != ruvia::http2_feed_result::protocol_failure);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head());
    if (request && request->request_head()) {
        const auto validation = ruvia::validate_http2_websocket_handshake(server,
            request->request_head()->stream_id(), request->request_head()->request());
        RUVIA_CHECK(validation.accepted() != nullptr);
    }
}
RUVIA_TEST(websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 10, .client_max_window_bits_ = 12}});
    ruvia::http3_message_head head(std::pmr::get_default_resource());
    head.status_ = 200;
    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validate_response(head, true);
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(accepted).selected_subprotocol_, std::string_view("chat"));
    RUVIA_CHECK(!std::get<0>(accepted).compression_.server_no_context_takeover_);
    RUVIA_CHECK(std::get<0>(accepted).compression_.client_no_context_takeover_);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.server_max_window_bits_.value(), 10);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value(), 12);
    RUVIA_CHECK(!(client.validate_response(head, false).index() == 0));
}
RUVIA_TEST(websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::websocket_client_negotiation client({.deflate_ = {.enabled_ = true, .server_max_window_bits_ = 10, .offer_client_max_window_bits_ = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        ruvia::http3_message_head head(std::pmr::get_default_resource());
        head.status_ = 200;
        add(head, "sec-websocket-extensions", extension);
        RUVIA_CHECK(!(client.validate_response(head, true).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::websocket_client_negotiation no_server_window_client({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}});
    {
        ruvia::http3_message_head head(std::pmr::get_default_resource());
        head.status_ = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        RUVIA_CHECK((no_server_window_client.validate_response(head, true).index() == 0));
    }
    // The client window in an offer is a hint; the server may choose a larger one.
    ruvia::websocket_client_negotiation limited_client_window({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .client_max_window_bits_ = 9}});
    {
        ruvia::http3_message_head head(std::pmr::get_default_resource());
        head.status_ = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=10");
        RUVIA_CHECK((limited_client_window.validate_response(head, true).index() == 0));
    }
    // A client takeover preference in an offer is a hint.
    ruvia::websocket_client_negotiation require_client_no_takeover({.deflate_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = true}});
    {
        ruvia::http3_message_head head(std::pmr::get_default_resource());
        head.status_ = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate");
        RUVIA_CHECK((require_client_no_takeover.validate_response(head, true).index() == 0));
    }
    ruvia::http3_message_head head(std::pmr::get_default_resource());
    head.status_ = 200;
    add(head, "sec-websocket-protocol", "unoffered");
    RUVIA_CHECK(!(client.validate_response(head, true).index() == 0));
}
RUVIA_TEST(websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    ruvia::http1_websocket_client_handshake client({.nonce_ = nonce, .deflate_ = {.enabled_ = true}});
    std::array<char, 2048> buffer{};
    auto prepared = client.prepare_request(ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::http1_client_response_parser parser(prepared.prepared()->exchange_state());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    RUVIA_CHECK(response.parsed() != nullptr);
    auto accepted = client.validate_response(*response.parsed());
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK(std::get<0>(accepted).compression_.enabled_);
}

namespace {
struct negotiation_resource final : std::pmr::memory_resource {
    std::size_t live_{0};
    bool fail_{false};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Fail request field storage, not noexcept debug iterator bookkeeping.
        if (fail_ && bytes_value >= 32) {
            throw std::bad_alloc();
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_ += bytes_value;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        live_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(websocket_client_negotiation_preserves_owned_offers_and_retained_results_across_operations) {
    negotiation_resource resource;
    {
        const std::array<std::string_view, 1> protocols{"channel.protocol.v1"};
        ruvia::websocket_client_negotiation negotiation({.subprotocols_ = protocols, .deflate_ = {.enabled_ = true}}, &resource);
        ruvia::http3_connection connection(ruvia::http3_peer_role::client, &resource);
        apply_http3_peer_settings(ruvia_ctx, connection, {.enable_connect_protocol_ = true});
        const auto retained = negotiation.encode_http3_request(connection, 0, "https", "example.test", "/socket");
        RUVIA_CHECK((retained.index() == 0));
        const auto retained_section = std::get<0>(retained).headers_.field_section();
        const std::vector<char> snapshot(retained_section.begin(), retained_section.end());
        const auto baseline = resource.live_;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto head = negotiation.encode_http3_request(connection, 4, "https", "example.test", "/other");
                RUVIA_CHECK((head.index() == 0));
            }
            RUVIA_CHECK_EQ(resource.live_, baseline);
            const auto current = std::get<0>(retained).headers_.field_section();
            RUVIA_CHECK(std::equal(snapshot.begin(), snapshot.end(), current.begin(), current.end()));
        }
        resource.fail_ = true;
        bool threw = false;
        try {
            (void)negotiation.encode_http3_request(connection, 8, "https", "example.test", "/allocation");
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail_ = false;
        RUVIA_CHECK_EQ(resource.live_, baseline);
    }
    RUVIA_CHECK_EQ(resource.live_, 0u);
    {
        // Preparing an offer without sending it owns no transport or hidden job.
        ruvia::websocket_client_negotiation discarded({.deflate_ = {.enabled_ = true}}, &resource);
        RUVIA_CHECK(resource.live_ > 0);
    }
    RUVIA_CHECK_EQ(resource.live_, 0u);
}

RUVIA_TEST(websocket_client_http3_uses_connection_settings_and_retains_its_request_after_offer_retirement) {
    negotiation_resource connection_resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::client, &connection_resource);
        const auto begun = connection.begin_early_data({.max_field_section_size_ = 1, .enable_connect_protocol_ = true});
        RUVIA_CHECK(begun.scope_ == ruvia::http3_connection_error_scope::none);
        std::optional<ruvia::http3_client_request_head> retained;
        {
            negotiation_resource offer_resource;
            const std::array protocols{std::string_view("retained.protocol")};
            ruvia::websocket_client_negotiation negotiation({.subprotocols_ = protocols}, &offer_resource);
            auto limited = negotiation.encode_http3_request(connection, 0, "https", "example.test", "/socket");
            RUVIA_CHECK(limited.index() != 0);
            RUVIA_CHECK(std::get<1>(limited).field_section_error_ == ruvia::http3_field_section_error::field_list_too_large);
            const auto rejected = connection.complete_early_data(false);
            RUVIA_CHECK(rejected.scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(!connection.peer_settings());
            apply_http3_peer_settings(ruvia_ctx, connection, {.enable_connect_protocol_ = true});
            auto encoded = negotiation.encode_http3_request(connection, 4, "https", "example.test", "/socket");
            RUVIA_CHECK(encoded.index() == 0);
            retained.emplace(std::move(std::get<0>(encoded)));
        }
        const auto decoded = ruvia::decode_http3_message_head(retained->headers_.field_section(), ruvia::http3_message_head_kind::request);
        RUVIA_CHECK(decoded.index() == 0);
        const auto& headers = std::get<0>(decoded).headers_;
        const auto protocol = std::ranges::find_if(headers, [](const auto& field) { return field.name() == "sec-websocket-protocol"; });
        RUVIA_CHECK(protocol != headers.end() && protocol->value() == "retained.protocol");
    }
    RUVIA_CHECK_EQ(connection_resource.live_, std::size_t{0});
}
