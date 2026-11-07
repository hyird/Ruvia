#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "output_drain.h"
TEST(Ruvia, http2_connection_origin_and_alternative_service_events_are_owned_and_role_checked) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const std::array<std::string_view, 1> origins{"https://example.test"};
    EXPECT_TRUE(server.submit_origin_advertisement(origins) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(0, origins[0], "h3=\":443\"") == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    auto origin = client.next_event();
    auto alt = client.next_event();
    EXPECT_TRUE(origin && origin->origin_advertisement() && origin->origin_advertisement()->origins_[0] == origins[0]);
    EXPECT_TRUE(alt && alt->alternative_service_advertisement() && alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
    EXPECT_TRUE(client.submit_origin_advertisement(origins) == ruvia::http2_submit_status::invalid_state);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_TRUE(server.submit_alternative_service_advertisement(0, origins[0], "clear") == ruvia::http2_submit_status::accepted);
        relay.transfer(server, client);
        EXPECT_TRUE(client.next_event()->alternative_service_advertisement());
    }
    EXPECT_TRUE(alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
}
TEST(Ruvia, http2_connection_ignores_origin_without_authenticated_origin_context) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(server.submit_origin_advertisement(std::array<std::string_view, 1>{"https://example.test"}) == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    EXPECT_TRUE(!client.next_event());
}

TEST(Ruvia, http2_connection_server_rejects_client_only_options) {
    const auto rejected = [](ruvia::http2_connection_options options) {
        try {
            static_cast<void>(ruvia::http2_connection::server(options));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(rejected({.enable_push_ = true}));
    EXPECT_TRUE(rejected({.receive_origin_advertisements_ = true}));
    EXPECT_TRUE(!rejected({}));
}

TEST(Ruvia, http2_connection_advertisement_validates_stream_state_and_ignores_tunnel_and_reserved_frames) {
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_frame).first(9),
        static_cast<std::uint32_t>(partial_block.size()), ruvia::http2_frame_type::headers, 0x1, 1));
    partial_frame.append(partial_block);
    EXPECT_TRUE(server.feed(partial_frame) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    std::array<char, 9> cont{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    EXPECT_TRUE(server.feed(std::string_view(cont.data(), cont.size())) == ruvia::http2_feed_result::accepted);
    auto req1 = server.next_event();
    (void)server.next_event();
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::accepted);

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    // Pushed stream in reserved (local) state cannot send ALTSVC before response headers open it.
    // An interim response is a response HEADERS frame too (RFC 9113 §8.4.2): the
    // stream is half-closed (remote) afterwards and the server may advertise on it.
    std::uint32_t pushed_id = 0;
    if (pushed.index() == 0) {
        pushed_id = std::get<0>(pushed).stream_id();
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::invalid_state);
        EXPECT_TRUE(server.submit_interim_response_head(pushed_id, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) ==
                    ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::accepted);
        (void)server.release(std::move(std::get<0>(pushed)));
    }

    ruvia::http_response resp(&resource);
    resp.status(ruvia::http_status::no_content);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp))));
    // Stream 1 is now closed (retained only by req1 lease); ALTSVC submission must be rejected.
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    if (req1 && req1->request_head()) {
        (void)server.release(std::move(*req1->request_head()));
    }
    relay.transfer(server, client);
    bool pushed_altsvc_seen = false;
    while (auto event = client.next_event()) {
        if (const auto* advertisement = event->alternative_service_advertisement();
            advertisement != nullptr && advertisement->stream_id_ == pushed_id) {
            pushed_altsvc_seen = advertisement->field_value_ == "h3=\":443\"";
        }
    }
    EXPECT_TRUE(pushed_altsvc_seen);
    EXPECT_TRUE(!client.connection_error());

    // 2. Open CONNECT tunnel ignores ALTSVC on stream (RFC 7838 section 4 / RFC 9113
    // section 5.5 discard) and rejects server submission.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> req3_lease;
    std::optional<ruvia::http2_request_head_event> connect_lease;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            if (rh->stream_id() == connect_id) {
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                ruvia::http_response ok_resp(&resource);
                ok_resp.status(ruvia::http_status::ok);
                EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_tunnel).first(9),
        8, ruvia::http2_frame_type::alternative_service, 0, connect_id));
    EXPECT_TRUE(client.feed(altsvc_on_tunnel) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error());
    while (auto event = client.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
    }
    EXPECT_TRUE(!client.stream_aborted(connect_id));
    // No RST_STREAM (or any other frame) answers the ignored frame.
    EXPECT_TRUE(ruvia::testing::drain_output(client).empty());

    // 3. ALTSVC on a reserved push stream is an extension frame the stream cannot
    //    use (RFC 7838 section 4, RFC 9113 section 5.5): ignored without a reply.
    auto pushed2 = server.submit_push_request(3, {.authority_ = "example.test", .path_ = "/reserved"});
    EXPECT_TRUE(pushed2.index() == 0);
    if (pushed2.index() == 0) {
        const auto pushed2_id = std::get<0>(pushed2).stream_id();
        relay.transfer(server, client);
        while (client.next_event()) {
        }
        std::string altsvc_on_reserved(ruvia::http2_frame_header_bytes + 8, '\0');
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_reserved).first(9),
            8, ruvia::http2_frame_type::alternative_service, 0, pushed2_id));
        EXPECT_TRUE(client.feed(altsvc_on_reserved) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!client.connection_error());
        EXPECT_TRUE(!client.next_event().has_value());
        EXPECT_TRUE(ruvia::testing::drain_output(client).empty());
        (void)server.release(std::move(std::get<0>(pushed2)));
    }
    if (connect_lease) {
        (void)server.release(std::move(*connect_lease));
    }
    if (req3_lease) {
        (void)server.release(std::move(*req3_lease));
    }
}

TEST(Ruvia, http2_connection_ignores_valid_altsvc_on_tunnel_idle_and_closed_streams_and_origin_on_reserved_push) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client(
        {.resource_ = &resource, .enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> connect_req;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            connect_req.emplace(std::move(*rh));
        }
    }
    ruvia::http_response ok_resp(&resource);
    ok_resp.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    const auto altsvc_tunnel =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(connect_id, "", "h3=\":443\"", 16384, &resource));
    const auto altsvc_idle =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(99, "", "h3=\":443\"", 16384, &resource));
    EXPECT_TRUE(client.feed(std::string_view(altsvc_tunnel.data(), altsvc_tunnel.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(client.feed(std::string_view(altsvc_idle.data(), altsvc_idle.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(!client.connection_error().has_value());

    const auto get_submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(get_submitted));
    const auto get_id = std::get<std::uint32_t>(get_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> get_req;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            get_req.emplace(std::move(*rh));
        }
    }
    auto pushed = server.submit_push_request(get_id, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    const auto push_id = std::get<0>(pushed).stream_id();
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    // Server ignores client-sent ALTSVC on reserved(local) push stream (RFC 7838 section 4).
    std::pmr::string client_altsvc(&resource);
    std::array<char, 9> hdr{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(hdr, 11, ruvia::http2_frame_type::alternative_service, 0, push_id));
    client_altsvc.append(hdr.data(), hdr.size());
    client_altsvc.append("\0\0h3=\":443\"", 11);
    EXPECT_TRUE(server.feed(client_altsvc) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());

    // Client ignores server-sent ORIGIN on reserved(remote) push stream (RFC 8336 section 2.2).
    std::pmr::string server_origin(&resource);
    EXPECT_TRUE(ruvia::encode_http2_frame_header(hdr, 0, ruvia::http2_frame_type::origin, 0, push_id));
    server_origin.append(hdr.data(), hdr.size());
    EXPECT_TRUE(client.feed(server_origin) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error().has_value());

    (void)server.release(std::move(std::get<0>(pushed)));
    if (get_req) {
        (void)server.release(std::move(*get_req));
    }
    if (connect_req) {
        (void)server.release(std::move(*connect_req));
    }
}
