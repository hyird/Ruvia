#include <array>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "output_drain.h"
#include "test_harness.h"
RUVIA_TEST(http2_connection_origin_and_alternative_service_events_are_owned_and_role_checked) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const std::array<std::string_view, 1> origins{"https://example.test"};
    RUVIA_CHECK(server.submit_origin_advertisement(origins) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.submit_alternative_service_advertisement(0, origins[0], "h3=\":443\"") == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    auto origin = client.next_event();
    auto alt = client.next_event();
    RUVIA_CHECK(origin && origin->origin_advertisement() && origin->origin_advertisement()->origins_[0] == origins[0]);
    RUVIA_CHECK(alt && alt->alternative_service_advertisement() && alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
    RUVIA_CHECK(client.submit_origin_advertisement(origins) == ruvia::http2_submit_status::invalid_state);
    for (std::size_t i = 0; i < 16; ++i) {
        RUVIA_CHECK(server.submit_alternative_service_advertisement(0, origins[0], "clear") == ruvia::http2_submit_status::accepted);
        relay.transfer(server, client);
        RUVIA_CHECK(client.next_event()->alternative_service_advertisement());
    }
    RUVIA_CHECK(alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
}
RUVIA_TEST(http2_connection_ignores_origin_without_authenticated_origin_context) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    RUVIA_CHECK(server.submit_origin_advertisement(std::array<std::string_view, 1>{"https://example.test"}) == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    RUVIA_CHECK(!client.next_event());
}

RUVIA_TEST(http2_connection_advertisement_validates_stream_state_and_rejects_tunnel_and_reserved_frames) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // 1. Server submit_alternative_service_advertisement rejects incomplete head, CONNECT, reserved push, and closed stream.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    (void)ruvia::testing::drain_output(client);
    std::pmr::string partial_block(&resource);
    ruvia::hpack_encoder::encode_header(partial_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(partial_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(partial_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(partial_block, ":path", "/");
    std::string partial_frame(ruvia::http2_frame_header_bytes, '\0');
    RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(partial_frame).first(9),
        static_cast<std::uint32_t>(partial_block.size()), ruvia::http2_frame_type::headers, 0x1, 1));
    partial_frame.append(partial_block);
    RUVIA_CHECK(server.feed(partial_frame) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    std::array<char, 9> cont{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    RUVIA_CHECK(server.feed(std::string_view(cont.data(), cont.size())) == ruvia::http2_feed_result::accepted);
    auto req1 = server.next_event();
    (void)server.next_event();
    RUVIA_CHECK(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::accepted);

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    RUVIA_CHECK(pushed.index() == 0);
    // Pushed stream in reserved (local) state cannot send ALTSVC before response headers open it.
    if (pushed.index() == 0) {
        RUVIA_CHECK(server.submit_alternative_service_advertisement(std::get<0>(pushed).stream_id(), "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::invalid_state);
        (void)server.release(std::move(std::get<0>(pushed)));
    }

    ruvia::http_response resp(&resource);
    resp.status(ruvia::http_status::no_content);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp))));
    // Stream 1 is now closed (retained only by req1 lease); ALTSVC submission must be rejected.
    RUVIA_CHECK(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    if (req1 && req1->request_head()) {
        (void)server.release(std::move(*req1->request_head()));
    }
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    // 2. Open CONNECT tunnel rejects ALTSVC on stream with RST_STREAM(PROTOCOL_ERROR) and rejects server submission.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> req3_lease;
    std::optional<ruvia::http2_request_head_event> connect_lease;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            if (rh->stream_id() == connect_id) {
                RUVIA_CHECK(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                ruvia::http_response ok_resp(&resource);
                ok_resp.status(ruvia::http_status::ok);
                RUVIA_CHECK(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
                RUVIA_CHECK(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                connect_lease.emplace(std::move(*rh));
            } else {
                req3_lease.emplace(std::move(*rh));
            }
        }
    }
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    std::string altsvc_on_tunnel(ruvia::http2_frame_header_bytes + 8, '\0');
    RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(altsvc_on_tunnel).first(9),
        8, ruvia::http2_frame_type::alternative_service, 0, connect_id));
    RUVIA_CHECK(client.feed(altsvc_on_tunnel) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!client.connection_error());
    const auto tunnel_closed = client.next_event();
    RUVIA_CHECK(tunnel_closed && tunnel_closed->stream_closed() &&
                tunnel_closed->stream_closed()->stream_id() == connect_id &&
                tunnel_closed->stream_closed()->error() == ruvia::http2_error_code::protocol_error);

    // 3. Reserved push stream receiving ALTSVC triggers connection GOAWAY(PROTOCOL_ERROR).
    auto pushed2 = server.submit_push_request(3, {.authority_ = "example.test", .path_ = "/reserved"});
    RUVIA_CHECK(pushed2.index() == 0);
    if (pushed2.index() == 0) {
        const auto pushed2_id = std::get<0>(pushed2).stream_id();
        relay.transfer(server, client);
        while (client.next_event()) {
        }
        std::string altsvc_on_reserved(ruvia::http2_frame_header_bytes + 8, '\0');
        RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(altsvc_on_reserved).first(9),
            8, ruvia::http2_frame_type::alternative_service, 0, pushed2_id));
        RUVIA_CHECK(client.feed(altsvc_on_reserved) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::protocol_error);
        (void)server.release(std::move(std::get<0>(pushed2)));
    }
    if (connect_lease) {
        (void)server.release(std::move(*connect_lease));
    }
    if (req3_lease) {
        (void)server.release(std::move(*req3_lease));
    }
}
