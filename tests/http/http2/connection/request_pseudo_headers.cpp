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
#include "stl_allocation_fixture.h"
#include "test_harness.h"

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

struct observed_request final {
    std::array<pseudo_header, 5> order_{};
    std::size_t size_{0};
    std::size_t heads_{0};
    std::uint32_t stream_id_{0};

    static void receive(void* context, const ruvia::http2_receive_metadata& metadata) noexcept {
        if (const auto* head = std::get_if<ruvia::http2_received_request_head>(&metadata)) {
            auto& self = *static_cast<observed_request*>(context);
            ++self.heads_;
            self.stream_id_ = head->stream_id_;
            self.size_ = head->request_.pseudo_header_order_.size();
            std::copy_n(head->request_.pseudo_header_order_.begin(),
                std::min(self.size_, self.order_.size()), self.order_.begin());
        }
    }

    [[nodiscard]] std::span<const pseudo_header> order() const noexcept {
        return {order_.data(), std::min(size_, order_.size())};
    }
};

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

RUVIA_TEST(http2_request_pseudo_headers_preserve_every_regular_order) {
    std::array order{pseudo_header::method, pseudo_header::scheme, pseudo_header::authority, pseudo_header::path};
    do {
        observed_request observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = observed_request::receive, .receive_observer_context_ = &observed});
        std::pmr::string block;
        for (const auto header : order) {
            encode_pseudo_header(block, header);
        }
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK_EQ(observed.heads_, std::size_t{1});
        RUVIA_CHECK(same_order(observed.order(), order));
        auto head = take_request_head(server);
        RUVIA_CHECK(head.has_value());
        if (head) {
            const auto view = head->request_view();
            RUVIA_CHECK(same_order(view.pseudo_header_order_, order));
            RUVIA_CHECK(view.method_ == "GET" && view.scheme_ == "https" && view.path_ == "/" && view.authority_ == "example.test");
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

RUVIA_TEST(http2_request_pseudo_headers_do_not_synthesize_authority_from_host) {
    auto server = ruvia::http2_connection::server();
    constexpr std::array order{pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
    std::pmr::string block;
    for (const auto header : order) {
        encode_pseudo_header(block, header);
    }
    ruvia::hpack_encoder::encode_header(block, "host", "example.test");
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    RUVIA_CHECK(head.has_value());
    if (head) {
        RUVIA_CHECK(head->request_view().authority_ == "example.test");
        RUVIA_CHECK(same_order(head->request_view().pseudo_header_order_, order));
    }
}

RUVIA_TEST(http2_request_pseudo_headers_huffman_dynamic_index_and_continuation) {
    // RFC 7541 C.4.1: Huffman authority inserted at dynamic index 62.
    const std::string block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
    for (std::size_t split = 0; split <= block.size(); ++split) {
        observed_request observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = observed_request::receive, .receive_observer_context_ = &observed});
        auto first = opening_wire();
        append_frame(first, ruvia::http2_frame_type::headers, 0x1, 1, std::string_view(block).substr(0, split));
        (void)server.feed(first);
        RUVIA_CHECK_EQ(observed.heads_, std::size_t{0});
        RUVIA_CHECK(!server.next_event());
        std::string final;
        append_frame(final, ruvia::http2_frame_type::continuation, 0x4, 1, std::string_view(block).substr(split));
        RUVIA_CHECK(server.feed(final) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array expected{pseudo_header::method, pseudo_header::scheme, pseudo_header::path, pseudo_header::authority};
        RUVIA_CHECK_EQ(observed.heads_, std::size_t{1});
        RUVIA_CHECK(same_order(observed.order(), expected));
        RUVIA_CHECK(head.has_value());
        if (head) {
            RUVIA_CHECK(head->request_view().authority_ == "www.example.com");
            RUVIA_CHECK(same_order(head->request_view().pseudo_header_order_, expected));
        }
        std::string next;
        append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, "\xbe\x84\x87\x82");
        RUVIA_CHECK(server.feed(next) == ruvia::http2_feed_result::accepted);
        auto indexed = take_request_head(server);
        constexpr std::array indexed_order{pseudo_header::authority, pseudo_header::path, pseudo_header::scheme, pseudo_header::method};
        RUVIA_CHECK_EQ(observed.heads_, std::size_t{2});
        RUVIA_CHECK(same_order(observed.order(), indexed_order));
        RUVIA_CHECK(indexed.has_value());
        if (indexed) {
            RUVIA_CHECK(indexed->request_view().authority_ == "www.example.com");
            RUVIA_CHECK(same_order(indexed->request_view().pseudo_header_order_, indexed_order));
        }
        RUVIA_CHECK(!server.connection_error());
    }
}

RUVIA_TEST(http2_request_pseudo_headers_preserve_both_connect_forms) {
    for (const bool extended : {false, true}) {
        observed_request observed;
        auto server = ruvia::http2_connection::server({.receive_observer_ = observed_request::receive, .receive_observer_context_ = &observed});
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
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array extended_order{pseudo_header::protocol, pseudo_header::path, pseudo_header::scheme, pseudo_header::authority, pseudo_header::method};
        const auto expected = std::span(extended_order).subspan(extended ? 0 : 3);
        RUVIA_CHECK_EQ(observed.heads_, std::size_t{1});
        RUVIA_CHECK(same_order(observed.order(), expected));
        RUVIA_CHECK(head.has_value());
        if (head) {
            RUVIA_CHECK(head->snapshot().connect_pending_);
            RUVIA_CHECK(same_order(head->request_view().pseudo_header_order_, expected));
        }
    }
}

RUVIA_TEST(http2_invalid_pseudo_headers_do_not_publish_but_keep_dynamic_table_sync) {
    observed_request observed;
    auto server = ruvia::http2_connection::server({.receive_observer_ = observed_request::receive, .receive_observer_context_ = &observed});
    auto wire = opening_wire();
    // A duplicate :method fails request semantics, but the later incremental
    // :path literal must still become dynamic index 62 (RFC 7541 section 2.3.2).
    const std::string malformed("\x82\x82\x44\x07/synced", 11);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, malformed);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(observed.heads_, std::size_t{0});
    RUVIA_CHECK(!take_request_head(server));
    RUVIA_CHECK(server.stream_aborted(1));
    std::pmr::string indexed("\xbe\x87\x82", 3);
    encode_pseudo_header(indexed, pseudo_header::authority);
    std::string next;
    append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, indexed);
    RUVIA_CHECK(server.feed(next) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    constexpr std::array expected{pseudo_header::path, pseudo_header::scheme, pseudo_header::method, pseudo_header::authority};
    RUVIA_CHECK_EQ(observed.heads_, std::size_t{1});
    RUVIA_CHECK_EQ(observed.stream_id_, std::uint32_t{3});
    RUVIA_CHECK(same_order(observed.order(), expected));
    RUVIA_CHECK(head.has_value());
    if (head) {
        RUVIA_CHECK(head->request_view().path_ == "/synced");
        RUVIA_CHECK(same_order(head->request_view().pseudo_header_order_, expected));
    }
    RUVIA_CHECK(!server.connection_error());
}

RUVIA_TEST(http2_request_pseudo_header_lease_survives_reset_move_and_later_input) {
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
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto received = take_request_head(server);
            RUVIA_CHECK(received.has_value());
            if (!received) {
                return;
            }
            lease.emplace(std::move(*received));
            saved = lease->request_view();
            RUVIA_CHECK(same_order(saved.pseudo_header_order_, expected));
            auto moved = std::move(server);
            std::string next;
            append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, block);
            RUVIA_CHECK(moved.feed(next) == ruvia::http2_feed_result::accepted);
            auto following = take_request_head(moved);
            RUVIA_CHECK(following.has_value());
            RUVIA_CHECK(same_order(saved.pseudo_header_order_, expected));
            RUVIA_CHECK(same_order(lease->request_view().pseudo_header_order_, expected));
            moved = ruvia::http2_connection::server({.resource_ = &resource});
        }
        RUVIA_CHECK(saved.authority_ == "example.test");
        RUVIA_CHECK(same_order(saved.pseudo_header_order_, expected));
        RUVIA_CHECK(same_order(lease->request_view().pseudo_header_order_, expected));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http2_locally_synthesized_push_has_no_received_pseudo_header_order) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
    relay.transfer(client, server);
    auto parent = take_request_head(server);
    RUVIA_CHECK(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
    RUVIA_CHECK(pushed.index() == 0);
    if (pushed.index() == 0) {
        RUVIA_CHECK(std::get<0>(pushed).request_view().pseudo_header_order_.empty());
        RUVIA_CHECK(std::get<0>(pushed).request_view().path_ == "/asset");
    }
}

RUVIA_TEST(http2_pseudo_header_order_rolls_back_on_initial_and_final_continuation_allocation_failure) {
    constexpr std::array expected{pseudo_header::method, pseudo_header::authority, pseudo_header::scheme, pseudo_header::path};
    std::pmr::string block;
    const std::string method(64, 'M');
    const std::string authority = std::string(64, 'a') + ".test";
    const std::string path = "/" + std::string(128, 'p');
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":authority", authority);
    encode_pseudo_header(block, pseudo_header::scheme);
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    ruvia::hpack_encoder::encode_header(block, "x-long", std::string(256, 'v'));
    for (const bool continuation : {false, true}) {
        bool completed = false;
        bool saw_failure = false;
        for (std::size_t failure_index = 0; failure_index < 128 && !completed; ++failure_index) {
            failing_memory_resource resource;
            {
                observed_request observed;
                auto server = ruvia::http2_connection::server({.resource_ = &resource,
                    .receive_observer_ = observed_request::receive,
                    .receive_observer_context_ = &observed});
                (void)ruvia::testing::drain_output(server);
                auto opening = opening_wire();
                if (continuation) {
                    append_frame(opening, ruvia::http2_frame_type::headers, 0x1, 1, std::string_view(block).substr(0, 1));
                }
                (void)server.feed(opening);
                RUVIA_CHECK_EQ(observed.heads_, std::size_t{0});
                (void)ruvia::testing::drain_output(server);
                std::string wire;
                append_frame(wire, continuation ? ruvia::http2_frame_type::continuation : ruvia::http2_frame_type::headers,
                    continuation ? 0x4 : 0x5, 1, std::string_view(block).substr(continuation ? 1 : 0));
                if (ruvia::test::should_inject_stl_allocation_failure(failure_index)) {
                    resource.fail_after(failure_index);
                }
                bool failed = false;
                try {
                    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
                } catch (const std::bad_alloc&) {
                    failed = true;
                }
                resource.allow_allocations();
                if (failed) {
                    saw_failure = true;
                    RUVIA_CHECK_EQ(observed.heads_, std::size_t{0});
                    RUVIA_CHECK(!server.connection_error());
                    RUVIA_CHECK(!server.next_event());
                    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
                } else {
                    completed = true;
                }
                RUVIA_CHECK_EQ(observed.heads_, std::size_t{1});
                RUVIA_CHECK_EQ(observed.size_, expected.size());
                RUVIA_CHECK(same_order(observed.order(), expected));
                auto head = take_request_head(server);
                RUVIA_CHECK(head.has_value());
                if (head) {
                    const auto view = head->request_view();
                    RUVIA_CHECK(same_order(view.pseudo_header_order_, expected));
                    RUVIA_CHECK(view.method_ == method && view.authority_ == authority && view.path_ == path);
                    RUVIA_CHECK(head->build_request().header("x-long") == std::string(256, 'v'));
                }
                RUVIA_CHECK(!server.connection_error());
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
        RUVIA_CHECK(completed);
        RUVIA_CHECK(saw_failure);
    }
}
