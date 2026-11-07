#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "failing_memory_resource.h"
#include "output_drain.h"

namespace {

using pseudo_header = ruvia::http2_request_pseudo_header;

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

std::string opening_wire() {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    return wire;
}

void encode_pseudo_header(std::pmr::string& block, pseudo_header header) {
    switch (header) {
        case pseudo_header::method:
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            break;
        case pseudo_header::scheme:
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            break;
        case pseudo_header::authority:
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            break;
        case pseudo_header::path:
            ruvia::hpack_encoder::encode_header(block, ":path", "/");
            break;
        case pseudo_header::protocol:
            ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
            break;
    }
}

bool same_order(std::span<const pseudo_header> actual, std::span<const pseudo_header> expected) {
    return std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

std::optional<ruvia::http2_request_head_event> take_request_head(ruvia::http2_connection& server) {
    std::optional<ruvia::http2_request_head_event> head;
    while (auto event = server.next_event()) {
        if (auto* request = event->request_head()) {
            if (head) {
                throw std::logic_error("unexpected duplicate request head");
            }
            head.emplace(std::move(*request));
        }
    }
    return head;
}

}  // namespace

TEST(Ruvia, http2_request_pseudo_headers_preserve_every_regular_order) {
    std::array order{pseudo_header::method, pseudo_header::scheme, pseudo_header::authority, pseudo_header::path};
    do {
        auto server = ruvia::http2_connection::server();
        std::pmr::string block;
        for (const auto header : order) {
            encode_pseudo_header(block, header);
        }
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (head) {
            const auto view = head->request_view();
            EXPECT_TRUE(same_order(view.pseudo_header_order_, order));
            EXPECT_TRUE(view.method_ == "GET" && view.scheme_ == "https" && view.path_ == "/" && view.authority_ == "example.test");
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(Ruvia, http2_request_pseudo_headers_do_not_synthesize_authority_from_host) {
    auto server = ruvia::http2_connection::server();
    constexpr std::array order{pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
    std::pmr::string block;
    for (const auto header : order) {
        encode_pseudo_header(block, header);
    }
    ruvia::hpack_encoder::encode_header(block, "host", "example.test");
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_TRUE(head->request_view().authority_ == "example.test");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, order));
    }
}

TEST(Ruvia, http2_request_pseudo_headers_huffman_dynamic_index_and_continuation) {
    // RFC 7541 C.4.1: Huffman authority inserted at dynamic index 62.
    const std::string block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
    for (std::size_t split = 0; split <= block.size(); ++split) {
        auto server = ruvia::http2_connection::server();
        auto first = opening_wire();
        append_frame(first, ruvia::http2_frame_type::headers, 0x1, 1, std::string_view(block).substr(0, split));
        (void)server.feed(first);
        EXPECT_TRUE(!server.next_event());
        std::string final;
        append_frame(final, ruvia::http2_frame_type::continuation, 0x4, 1, std::string_view(block).substr(split));
        EXPECT_TRUE(server.feed(final) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array expected{pseudo_header::method, pseudo_header::scheme, pseudo_header::path, pseudo_header::authority};
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
        std::string next;
        append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, "\xbe\x84\x87\x82");
        EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
        auto indexed = take_request_head(server);
        constexpr std::array indexed_order{pseudo_header::authority, pseudo_header::path, pseudo_header::scheme, pseudo_header::method};
        EXPECT_TRUE(indexed.has_value());
        if (indexed) {
            EXPECT_TRUE(indexed->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(indexed->request_view().pseudo_header_order_, indexed_order));
        }
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_request_pseudo_headers_preserve_both_connect_forms) {
    for (const bool extended : {false, true}) {
        auto server = ruvia::http2_connection::server();
        (void)ruvia::testing::drain_output(server);
        std::pmr::string block;
        if (extended) {
            encode_pseudo_header(block, pseudo_header::protocol);
            encode_pseudo_header(block, pseudo_header::path);
            encode_pseudo_header(block, pseudo_header::scheme);
        }
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test:443");
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        if (extended) {
            ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        }
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array extended_order{pseudo_header::protocol, pseudo_header::path, pseudo_header::scheme, pseudo_header::authority, pseudo_header::method};
        const auto expected = std::span(extended_order).subspan(extended ? 0 : 3);
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->snapshot().connect_pending_);
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
    }
}

TEST(Ruvia, http2_invalid_pseudo_headers_do_not_publish_but_keep_dynamic_table_sync) {
    auto server = ruvia::http2_connection::server();
    auto wire = opening_wire();
    // A duplicate :method fails request semantics, but the later incremental
    // :path literal must still become dynamic index 62 (RFC 7541 section 2.3.2).
    const std::string malformed("\x82\x82\x44\x07/synced", 11);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, malformed);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!take_request_head(server));
    EXPECT_TRUE(server.stream_aborted(1));
    std::pmr::string indexed("\xbe\x87\x82", 3);
    encode_pseudo_header(indexed, pseudo_header::authority);
    std::string next;
    append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, indexed);
    EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    constexpr std::array expected{pseudo_header::path, pseudo_header::scheme, pseudo_header::method, pseudo_header::authority};
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_EQ(head->stream_id(), std::uint32_t{3});
        EXPECT_TRUE(head->request_view().path_ == "/synced");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
    }
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_request_pseudo_header_lease_survives_reset_move_and_later_input) {
    failing_memory_resource resource;
    {
        std::optional<ruvia::http2_request_head_event> lease;
        ruvia::http2_server_request_view saved;
        constexpr std::array expected{pseudo_header::authority, pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
        {
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            std::pmr::string block;
            for (const auto header : expected) {
                encode_pseudo_header(block, header);
            }
            auto wire = opening_wire();
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto received = take_request_head(server);
            EXPECT_TRUE(received.has_value());
            if (!received) {
                return;
            }
            lease.emplace(std::move(*received));
            saved = lease->request_view();
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            auto moved = std::move(server);
            std::string next;
            append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, block);
            EXPECT_TRUE(moved.feed(next) == ruvia::http2_feed_result::accepted);
            auto following = take_request_head(moved);
            EXPECT_TRUE(following.has_value());
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
            moved = ruvia::http2_connection::server({.resource_ = &resource});
        }
        EXPECT_TRUE(saved.authority_ == "example.test");
        EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
        EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http2_locally_synthesized_push_has_no_received_pseudo_header_order) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
    relay.transfer(client, server);
    auto parent = take_request_head(server);
    EXPECT_TRUE(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() == 0) {
        EXPECT_TRUE(std::get<0>(pushed).request_view().pseudo_header_order_.empty());
        EXPECT_TRUE(std::get<0>(pushed).request_view().path_ == "/asset");
    }
}
