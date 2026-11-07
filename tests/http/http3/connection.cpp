#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"

#include "failing_memory_resource.h"
#include "http3_relay.h"
#include "test_exceptions.h"

namespace {

constexpr std::array get_request_fields{
    ruvia::http3_field_view{":method", "GET"},
    ruvia::http3_field_view{":scheme", "https"},
    ruvia::http3_field_view{":authority", "example.test"},
    ruvia::http3_field_view{":path", "/resource"},
};

constexpr std::array no_content_response_fields{
    ruvia::http3_field_view{":status", "204"},
    ruvia::http3_field_view{"server", "ruvia"},
};

// RFC 9114 §5.2: a shutdown-notice GOAWAY from a server names the largest
// client-initiated bidirectional stream identifier, 2^62 - 4.
constexpr std::int64_t shutdown_notice_stream_id = (std::int64_t{1} << 62) - 4;

// H3_REQUEST_CANCELLED, RFC 9114 §8.1.
constexpr std::uint64_t request_cancelled = 0x10c;

}  // namespace

TEST(Ruvia, http3_nghttp3_client_server_exchange_headers_and_stream_end) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::testing::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_fields, true, request));
    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    if (request.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(request.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_EQ(request.headers_[0].fields_.size(), get_request_fields.size());
    EXPECT_TRUE(request.headers_[0].stream_ended_);
    EXPECT_TRUE(request.headers_[0].field(":path") == std::string_view("/resource"));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_EQ(response.headers_.size(), std::size_t{1});
    if (response.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(response.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_EQ(response.headers_[0].fields_.size(), no_content_response_fields.size());
    EXPECT_TRUE(response.headers_[0].stream_ended_);
    EXPECT_TRUE(response.headers_[0].field(":status") == std::string_view("204"));
    EXPECT_TRUE(response.ended(0));
    EXPECT_EQ(response.sequence_, std::string("HE"));
}

TEST(Ruvia, http3_nghttp3_streaming_body_is_owned_until_acknowledged) {
    ruvia::testing::http3_peers peers({.own_data_events_ = true});
    EXPECT_TRUE(peers.bind());

    constexpr std::array request_fields{
        ruvia::http3_field_view{":method", "POST"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/upload"},
        ruvia::http3_field_view{"content-length", "3"},
    };
    EXPECT_TRUE(peers.client_.submit_request(0, request_fields, false) == ruvia::http3_status::accepted);
    const auto submitted = peers.client_.submit_data(0, ruvia::testing::http3_bytes("abc"));
    EXPECT_TRUE(submitted.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(submitted.accepted_bytes_, std::size_t{3});
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);

    // The relay scrubs every delivered input buffer, so owned DATA is the
    // only way the payload can still read "abc" after delivery.
    std::optional<ruvia::http3_data_event> retained;
    ruvia::testing::http3_event_log request;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        request(event);
        if (auto* data = std::get_if<ruvia::http3_data_event>(&event)) {
            retained.emplace(std::move(*data));
        }
    }));

    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    EXPECT_TRUE(retained.has_value() && retained->stream_id_ == 0 && retained->bytes_ == "abc");
    EXPECT_EQ(request.data(0), std::string("abc"));
    EXPECT_TRUE(request.ended(0));
    EXPECT_EQ(request.sequence_, std::string("HDE"));
}

// A request stream that QUIC reports closed is published once, with the
// application error code only when the closure carried one.
TEST(Ruvia, http3_close_stream_publishes_stream_close_event) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::testing::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_fields, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_fields, true, request));
    EXPECT_TRUE(request.ended(0) && request.ended(4));
    EXPECT_TRUE(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_TRUE(response.ended(0));

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log closed;
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id_, std::int64_t{0});
        EXPECT_TRUE(!closed.closes_[0].application_error_code_.has_value());
    }

    EXPECT_TRUE(peers.server_.close_stream(4, request_cancelled, std::nullopt) == ruvia::http3_status::accepted);
    closed.clear();
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id_, std::int64_t{4});
        EXPECT_TRUE(closed.closes_[0].application_error_code_ == std::optional<std::uint64_t>(request_cancelled));
    }
    EXPECT_EQ(closed.sequence_, std::string("C"));

    // A closed stream accepts no further response.
    EXPECT_TRUE(peers.server_.submit_response(0, no_content_response_fields, true) !=
                ruvia::http3_status::accepted);
}

// RFC 9114 §5.2 graceful shutdown: the notice still admits in-flight
// requests, the final GOAWAY names the first unprocessed request stream, and
// the server is drained only once every admitted request stream has closed.
TEST(Ruvia, http3_server_drain_sends_notice_then_final_goaway) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::testing::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_fields, true, request));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(peers.server_.begin_drain() == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    ruvia::testing::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.goaways_.size(), std::size_t{1});
    EXPECT_TRUE(!client.goaways_.empty() && client.goaways_[0] == shutdown_notice_stream_id);

    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.goaways_.size(), std::size_t{2});
    EXPECT_TRUE(client.goaways_.size() == 2 && client.goaways_[1] == 4);
    // Stream 0 was admitted before the final GOAWAY and remains open.
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_status::accepted);
    while (auto event = peers.server_.next_event()) {
        (void)event;
    }
    EXPECT_TRUE(peers.server_.drained());
    // A repeated final drain is idempotent.
    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
}

TEST(Ruvia, http3_settings_and_datagrams_are_negotiated_and_stream_scoped) {
    ruvia::testing::http3_peers peers(
        {.enable_connect_protocol_ = true, .enable_h3_datagram_ = true},
        {.enable_h3_datagram_ = true});
    EXPECT_TRUE(peers.bind());
    EXPECT_TRUE(!peers.client_.peer_settings().has_value());
    ruvia::testing::http3_event_log log;
    EXPECT_TRUE(peers.to_client(log));
    EXPECT_TRUE(peers.to_server(log));
    const auto settings = peers.client_.peer_settings();
    EXPECT_TRUE(settings && settings->enable_connect_protocol_ && settings->h3_datagram_);
    EXPECT_TRUE(!peers.client_.datagram_prefix(0));
    EXPECT_TRUE(peers.send_request(0, get_request_fields, false, log));
    const auto prefix = peers.client_.datagram_prefix(0);
    EXPECT_TRUE(prefix && prefix->size_ == 1 && prefix->bytes_[0] == std::byte{0});
    const std::string wire("\0\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::testing::http3_bytes(wire)).status_ == ruvia::http3_status::accepted);
    // Public input calls must drain the prior owned event before appending more.
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::testing::http3_bytes(wire)).status_ == ruvia::http3_status::events_pending);
    auto event = peers.server_.next_event();
    const auto* datagram = event ? std::get_if<ruvia::http3_datagram_event>(&*event) : nullptr;
    EXPECT_TRUE(datagram && datagram->stream_id_ == 0 && datagram->payload_ == std::string_view("\0echo", 5));
    EXPECT_TRUE(!peers.server_.next_event());
    // An unknown request stream is dropped, not retained for future streams.
    const std::string unknown("\1\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::testing::http3_bytes(unknown)).status_ == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
    EXPECT_TRUE(peers.server_.close_stream(0, request_cancelled, request_cancelled) == ruvia::http3_status::accepted);
    while (peers.server_.next_event()) {
    }
    EXPECT_TRUE(!peers.server_.datagram_prefix(0));
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::testing::http3_bytes(wire)).status_ == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
}

TEST(Ruvia, http3_datagram_truncated_prefix_and_disabled_setting_are_connection_errors) {
    auto enabled = ruvia::http3_connection::server({.enable_h3_datagram_ = true});
    const auto truncated = enabled.read_datagram(ruvia::testing::http3_bytes("\x40"));
    EXPECT_TRUE(truncated.status_ == ruvia::http3_status::protocol_error);
    EXPECT_EQ(truncated.error_code_, static_cast<std::uint64_t>(ruvia::http3_error_code::datagram_error));
    auto disabled = ruvia::http3_connection::server();
    const std::string wire("\0hello", 6);
    EXPECT_TRUE(disabled.read_datagram(ruvia::testing::http3_bytes(wire)).status_ == ruvia::http3_status::protocol_error);
}

TEST(Ruvia, http3_origin_frame_advertises_initial_and_dynamic_origins) {
    constexpr std::array<std::string_view, 2> initial_origins{
        "https://example.test",
        "https://api.example.test:8443",
    };
    ruvia::testing::http3_peers peers({.origins_ = initial_origins});
    EXPECT_TRUE(peers.bind());

    ruvia::testing::http3_event_log client_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{1});
    if (client_log.origins_.size() == 1) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{2});
        if (client_log.origins_[0].size() == 2) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://example.test"));
            EXPECT_EQ(client_log.origins_[0][1], std::string("https://api.example.test:8443"));
        }
    }

    constexpr std::array<std::string_view, 1> dynamic_origins{"https://cdn.example.test"};
    EXPECT_TRUE(peers.client_.submit_origin_advertisement(dynamic_origins) ==
                ruvia::http3_status::invalid_state);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(dynamic_origins) ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(std::span<const std::string_view>{}) ==
                ruvia::http3_status::accepted);
    constexpr std::array<std::string_view, 1> invalid_origin{"not-an-origin"};
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(invalid_origin) ==
                ruvia::http3_status::invalid_state);

    client_log.clear();
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{2});
    if (client_log.origins_.size() == 2) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{1});
        if (!client_log.origins_[0].empty()) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://cdn.example.test"));
        }
        EXPECT_TRUE(client_log.origins_[1].empty());
    }
}

TEST(Ruvia, http3_priority_headers_and_priority_update_frames_reprioritize_streams) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::testing::http3_event_log client_log;
    ruvia::testing::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array prioritized_request_fields{
        ruvia::http3_field_view{":method", "GET"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/resource"},
        ruvia::http3_field_view{"priority", "u=1, i"},
    };
    EXPECT_TRUE(peers.send_request(0, prioritized_request_fields, true, server_log));
    auto initial_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(initial_priority.has_value());
    if (initial_priority) {
        EXPECT_EQ(initial_priority->urgency_.value(), std::uint8_t{1});
        EXPECT_TRUE(initial_priority->incremental_);
    }

    EXPECT_TRUE(peers.client_.submit_priority_update(
                    0, {.urgency_ = ruvia::http_priority_urgency(5), .incremental_ = false}) ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    auto updated_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(updated_priority.has_value());
    if (updated_priority) {
        EXPECT_EQ(updated_priority->urgency_.value(), std::uint8_t{5});
        EXPECT_TRUE(!updated_priority->incremental_);
    }

    EXPECT_TRUE(peers.server_.set_server_stream_priority(
                    0, {.urgency_ = ruvia::http_priority_urgency(2), .incremental_ = true}) ==
                ruvia::http3_status::accepted);
    auto overridden = peers.server_.stream_priority(0);
    EXPECT_TRUE(overridden.has_value());
    if (overridden) {
        EXPECT_EQ(overridden->urgency_.value(), std::uint8_t{2});
        EXPECT_TRUE(overridden->incremental_);
    }
}

TEST(Ruvia, http3_connections_route_c_library_allocations_through_pmr) {
    failing_memory_resource resource;
    {
        auto client = ruvia::http3_connection::client({.resource_ = &resource});
        auto server = ruvia::http3_connection::server({.resource_ = &resource});
        EXPECT_TRUE(resource.live_allocations() > std::size_t{0});
        EXPECT_TRUE(client.bind_control_streams(2, 6, 10) == ruvia::http3_status::accepted);
        EXPECT_TRUE(server.bind_control_streams(3, 7, 11) == ruvia::http3_status::accepted);
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});

    resource.fail_after(0);
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::http3_connection::client({.resource_ = &resource});
    }));
    resource.allow_allocations();
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http3_request_enforces_declared_content_length) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::testing::http3_event_log client_log;
    ruvia::testing::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array post_fields{
        ruvia::http3_field_view{":method", "POST"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/upload"},
        ruvia::http3_field_view{"content-length", "5"},
    };
    EXPECT_TRUE(peers.client_.submit_request(0, post_fields, false) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::testing::http3_bytes("abc")).status_ ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)peers.client_.finish_stream(0); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)peers.client_.submit_data(0, ruvia::testing::http3_bytes("def"));
    }));
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::testing::http3_bytes("de")).status_ ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);
}

TEST(Ruvia, http3_concurrent_streams_interleave_partial_header_blocks) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::testing::http3_event_log client_log;
    ruvia::testing::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    EXPECT_TRUE(peers.client_.submit_request(0, get_request_fields, true) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.submit_request(4, get_request_fields, true) == ruvia::http3_status::accepted);

    std::vector<std::byte> stream0;
    std::vector<std::byte> stream4;
    ruvia::http3_stream_output out;
    while (peers.client_.peek_output(64 * 1024, out).status_ == ruvia::http3_status::accepted) {
        const auto size = out.size();
        if (out.stream_id_ == 0 || out.stream_id_ == 4) {
            auto& dst = out.stream_id_ == 0 ? stream0 : stream4;
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                dst.insert(dst.end(), out.buffers_[idx].begin(), out.buffers_[idx].end());
            }
        } else {
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                EXPECT_TRUE(peers.server_.read_stream(out.stream_id_, out.buffers_[idx], out.fin_, 1).status_ ==
                            ruvia::http3_status::accepted);
            }
        }
        EXPECT_TRUE(peers.client_.commit_output(out.stream_id_, size) == ruvia::http3_status::accepted);
        if (size != 0) {
            EXPECT_TRUE(peers.client_.acknowledge_output(out.stream_id_, size) == ruvia::http3_status::accepted);
        }
    }
    EXPECT_TRUE(stream0.size() > std::size_t{4});
    EXPECT_TRUE(stream4.size() > std::size_t{4});

    // Feed the first half of stream 0's HEADERS frame, then all of stream 4's
    // HEADERS frame, then the rest of stream 0's HEADERS frame.
    const auto split = stream0.size() / 2;
    EXPECT_TRUE(peers.server_.read_stream(0, std::span(stream0.data(), split), false, 1).status_ ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(4, stream4, true, 2).status_ == ruvia::http3_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(
                                 0, std::span(stream0.data() + split, stream0.size() - split), true, 3)
                    .status_ == ruvia::http3_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(!peers.server_.header_block_in_progress());
    EXPECT_EQ(server_log.headers_.size(), std::size_t{2});
}
