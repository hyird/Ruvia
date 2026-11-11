#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "failing_memory_resource.h"
#include "http3_relay.h"
#include "memory_resource_fixture.h"
#include "stl_allocation_fixture.h"
#include "test_exceptions.h"

namespace {

template <typename bytes_type>
constexpr bool can_construct_http3_data_event = std::is_constructible_v<ruvia::http3_data_event, std::int64_t, bytes_type, std::pmr::memory_resource*, bool>;

template <typename bytes_type>
constexpr bool can_read_http3_stream = requires(ruvia::http3_connection& connection, bytes_type&& bytes) {
    connection.read_stream(0, std::forward<bytes_type>(bytes), false, 0);
};

template <typename connection_type>
constexpr bool can_read_http3_stream_braced_bytes = requires(connection_type& connection) {
    connection.read_stream(0, {std::byte{1}, std::byte{2}}, false, 0);
};

static_assert(can_construct_http3_data_event<std::string&>);
static_assert(can_construct_http3_data_event<const std::string&>);
static_assert(can_construct_http3_data_event<std::pmr::string&>);
static_assert(can_construct_http3_data_event<const std::pmr::string&>);
static_assert(can_construct_http3_data_event<char (&)[4]>);
static_assert(can_construct_http3_data_event<const char (&)[4]>);
static_assert(can_construct_http3_data_event<std::string_view>);
static_assert(can_construct_http3_data_event<ruvia::borrowed_text>);
static_assert(!can_construct_http3_data_event<std::string>);
static_assert(!can_construct_http3_data_event<const std::string>);
static_assert(!can_construct_http3_data_event<std::pmr::string>);
static_assert(!can_construct_http3_data_event<const std::pmr::string>);

static_assert(can_read_http3_stream<std::vector<std::byte>&>);
static_assert(can_read_http3_stream<const std::vector<std::byte>&>);
static_assert(can_read_http3_stream<std::pmr::vector<std::byte>&>);
static_assert(can_read_http3_stream<const std::pmr::vector<std::byte>&>);
static_assert(can_read_http3_stream<std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<const std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<std::byte (&)[3]>);
static_assert(can_read_http3_stream<const std::byte (&)[3]>);
static_assert(can_read_http3_stream<std::span<std::byte>>);
static_assert(can_read_http3_stream<std::span<const std::byte>>);
static_assert(can_read_http3_stream<std::span<std::byte, 3>>);
static_assert(can_read_http3_stream<std::span<const std::byte, 3>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<const ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>&>);
static_assert(!can_read_http3_stream<std::vector<std::byte>>);
static_assert(!can_read_http3_stream<const std::vector<std::byte>>);
static_assert(!can_read_http3_stream<std::pmr::vector<std::byte>>);
static_assert(!can_read_http3_stream<const std::pmr::vector<std::byte>>);
static_assert(!can_read_http3_stream<std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<const std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream<const std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream_braced_bytes<ruvia::http3_connection>);

constexpr ruvia::http3_regular_request_head_view get_request_head{
    .authority_ = "example.test", .target_ = "/resource"};

[[nodiscard]] auto submit_no_content_response(ruvia::http3_connection& connection, std::int64_t stream_id) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::no_content);
    response.header("Server", "ruvia");
    return connection.submit_response_head(stream_id, response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
}

// RFC 9114 §5.2: a shutdown-notice GOAWAY from a server names the largest
// client-initiated bidirectional stream identifier, 2^62 - 4.
constexpr std::int64_t shutdown_notice_stream_id = (std::int64_t{1} << 62) - 4;

// H3_REQUEST_CANCELLED, RFC 9114 §8.1.
constexpr std::uint64_t request_cancelled = 0x10c;

ruvia::http3_headers_event make_owned_headers_event(std::pmr::memory_resource* resource) {
    ruvia::http3_headers_event event(4, std::pmr::vector<ruvia::http3_field>(resource), true, true, true);
    event.fields_.emplace_back("x-first", "one", resource);
    event.fields_.emplace_back("x-second", "two", resource);
    return event;
}

}  // namespace

TEST(Ruvia, http3_nghttp3_client_server_exchange_headers_and_stream_end) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    if (request.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(request.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_EQ(request.headers_[0].fields_.size(), std::size_t{4});
    EXPECT_TRUE(request.headers_[0].stream_ended_);
    EXPECT_TRUE(request.headers_[0].field(":path") == std::string_view("/resource"));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_EQ(response.headers_.size(), std::size_t{1});
    if (response.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(response.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_TRUE(response.headers_[0].field("server") == "ruvia");
    EXPECT_TRUE(response.headers_[0].stream_ended_);
    EXPECT_TRUE(response.headers_[0].status_ && response.headers_[0].status_->value() == 204);
    EXPECT_TRUE(response.ended(0));
    EXPECT_EQ(response.sequence_, std::string("HE"));
}

TEST(Ruvia, http3_owned_event_copies_inherit_source_resource) {
    for (const bool empty : {false, true}) {
        SCOPED_TRACE(empty);
        ruvia::test::failing_memory_resource resource;
        {
            auto source = make_owned_headers_event(&resource);
            if (empty) {
                source.fields_.clear();
            }
            const auto copied = source;
            EXPECT_EQ(copied.fields_.get_allocator().resource(), &resource);
            EXPECT_EQ(copied.stream_id_, 4);
            EXPECT_TRUE(copied.stream_ended_);
            EXPECT_TRUE(copied.trailers_);
            EXPECT_TRUE(copied.early_data_);
            ASSERT_EQ(copied.fields_.size(), empty ? std::size_t{0} : std::size_t{2});
            if (!empty) {
                source.fields_.clear();
                EXPECT_EQ(copied.fields_[0].name(), "x-first");
                EXPECT_EQ(copied.fields_[0].value(), "one");
                EXPECT_EQ(copied.fields_[1].name(), "x-second");
                EXPECT_EQ(copied.fields_[1].value(), "two");
            }
            const std::string payload(empty ? 0 : 128, 'd');
            ruvia::http3_datagram_event datagram(8, std::pmr::string(payload, &resource));
            const auto copied_datagram = datagram;
            datagram.payload_.clear();
            EXPECT_EQ(copied_datagram.payload_.get_allocator().resource(), &resource);
            EXPECT_EQ(copied_datagram.stream_id_, 8);
            EXPECT_EQ(copied_datagram.payload_, std::string_view(payload));
        }
        EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http3_headers_event_assignment_rebinds_equivalent_resources) {
    for (const bool move : {false, true}) {
        SCOPED_TRACE(move);
        ruvia::test::failing_memory_resource upstream;
        ruvia::test::equivalent_tracking_resource source_resource(upstream);
        ruvia::test::equivalent_tracking_resource target_resource(upstream);
        {
            ruvia::http3_headers_event target(8, std::pmr::vector<ruvia::http3_field>(&target_resource));
            target.fields_.emplace_back("x-old", "before", &target_resource);
            {
                auto source = make_owned_headers_event(&source_resource);
                if (ruvia::test::stl_allocation_failure_injection_enabled()) {
                    source_resource.reject_allocations_after(0);
                }
                if (move) {
                    target = std::move(source);
                } else {
                    target = source;
                }
                source_resource.allow_allocations();
            }
            source_resource.retire();
            EXPECT_EQ(target.fields_.get_allocator().resource(), &target_resource);
            EXPECT_EQ(target.stream_id_, 4);
            EXPECT_TRUE(target.stream_ended_);
            EXPECT_TRUE(target.trailers_);
            EXPECT_TRUE(target.early_data_);
            ASSERT_EQ(target.fields_.size(), std::size_t{2});
            EXPECT_EQ(target.fields_[0].name(), "x-first");
            EXPECT_EQ(target.fields_[0].value(), "one");
            EXPECT_EQ(target.fields_[1].name(), "x-second");
            EXPECT_EQ(target.fields_[1].value(), "two");
        }
        EXPECT_EQ(source_resource.callbacks_after_retirement(), std::size_t{0});
        EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http3_headers_event_assignment_rolls_back_each_allocation_failure) {
    for (const bool move : {false, true}) {
        SCOPED_TRACE(move);
        for (std::size_t fail_after = 0;; ++fail_after) {
            SCOPED_TRACE(fail_after);
            ruvia::test::failing_memory_resource source_resource;
            ruvia::test::failing_memory_resource target_resource;
            bool failed = false;
            {
                auto source = make_owned_headers_event(&source_resource);
                ruvia::http3_headers_event target(8, std::pmr::vector<ruvia::http3_field>(&target_resource));
                target.fields_.emplace_back("x-old", "before", &target_resource);
                const auto* source_fields = source.fields_.data();
                const auto* target_fields = target.fields_.data();
                const auto source_live = source_resource.live_allocations();
                const auto target_live = target_resource.live_allocations();
                target_resource.fail_after_stl_operation(fail_after);
                try {
                    if (move) {
                        target = std::move(source);
                    } else {
                        target = source;
                    }
                } catch (const std::bad_alloc&) {
                    failed = true;
                }
                target_resource.allow_allocations();
                if (failed) {
                    EXPECT_EQ(source.stream_id_, 4);
                    EXPECT_TRUE(source.stream_ended_);
                    EXPECT_TRUE(source.trailers_);
                    EXPECT_TRUE(source.early_data_);
                    EXPECT_EQ(source.fields_.data(), source_fields);
                    ASSERT_EQ(source.fields_.size(), std::size_t{2});
                    EXPECT_EQ(source.fields_[0].value(), "one");
                    EXPECT_EQ(source.fields_[1].value(), "two");
                    EXPECT_EQ(target.stream_id_, 8);
                    EXPECT_FALSE(target.stream_ended_);
                    EXPECT_FALSE(target.trailers_);
                    EXPECT_FALSE(target.early_data_);
                    EXPECT_EQ(target.fields_.data(), target_fields);
                    ASSERT_EQ(target.fields_.size(), std::size_t{1});
                    EXPECT_EQ(target.fields_[0].name(), "x-old");
                    EXPECT_EQ(target.fields_[0].value(), "before");
                    EXPECT_EQ(source_resource.live_allocations(), source_live);
                    EXPECT_EQ(target_resource.live_allocations(), target_live);
                    if (move) {
                        target = std::move(source);
                    } else {
                        target = source;
                    }
                }
                EXPECT_EQ(target.stream_id_, 4);
                EXPECT_TRUE(target.stream_ended_);
                EXPECT_TRUE(target.trailers_);
                EXPECT_TRUE(target.early_data_);
                ASSERT_EQ(target.fields_.size(), std::size_t{2});
                EXPECT_EQ(target.fields_[0].value(), "one");
                EXPECT_EQ(target.fields_[1].value(), "two");
            }
            EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
            EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
            if (!failed) {
                break;
            }
        }
    }
}

TEST(Ruvia, http3_datagram_event_assignment_retains_resource_and_allows_retry) {
    const std::string previous(64, 'p');
    const std::string incoming(2048, 'n');
    for (const bool move : {false, true}) {
        SCOPED_TRACE(move);
        ruvia::test::failing_memory_resource source_resource;
        ruvia::test::failing_memory_resource target_resource;
        {
            ruvia::http3_datagram_event source(4, std::pmr::string(incoming, &source_resource));
            ruvia::http3_datagram_event target(8, std::pmr::string(previous, &target_resource));
            if (ruvia::test::stl_allocation_failure_injection_enabled()) {
                target_resource.fail_after(0);
                if (move) {
                    EXPECT_THROW(target = std::move(source), std::bad_alloc);
                } else {
                    EXPECT_THROW(target = source, std::bad_alloc);
                }
                EXPECT_EQ(source.stream_id_, 4);
                EXPECT_EQ(source.payload_, std::string_view(incoming));
                EXPECT_EQ(target.stream_id_, 8);
                EXPECT_EQ(target.payload_, std::string_view(previous));
            }
            target_resource.allow_allocations();
            if (move) {
                target = std::move(source);
            } else {
                target = source;
            }
            EXPECT_EQ(target.stream_id_, 4);
            EXPECT_EQ(target.payload_, std::string_view(incoming));
            EXPECT_EQ(target.payload_.get_allocator().resource(), &target_resource);
        }
        EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
        EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http3_owned_events_move_storage_within_one_resource) {
    ruvia::test::failing_memory_resource resource;
    const std::string expected_payload(128, 'd');
    {
        auto source = make_owned_headers_event(&resource);
        ruvia::http3_headers_event target(8, std::pmr::vector<ruvia::http3_field>(&resource));
        target.fields_.emplace_back("x-old", "before", &resource);
        ruvia::http3_datagram_event datagram(4, std::pmr::string(128, 'd', &resource));
        ruvia::http3_datagram_event received(8, std::pmr::string(64, 'p', &resource));
        const auto* fields = source.fields_.data();
        const auto* payload = datagram.payload_.data();
        resource.fail_after_stl_operation(0);
        target = std::move(source);
        received = std::move(datagram);
        resource.allow_allocations();
        EXPECT_EQ(target.stream_id_, 4);
        EXPECT_EQ(target.fields_.data(), fields);
        ASSERT_EQ(target.fields_.size(), std::size_t{2});
        EXPECT_EQ(target.fields_[0].value(), "one");
        EXPECT_EQ(target.fields_[1].value(), "two");
        EXPECT_EQ(received.stream_id_, 4);
        EXPECT_EQ(received.payload_.data(), payload);
        EXPECT_EQ(received.payload_, std::string_view(expected_payload));
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http3_data_event_borrows_or_copies_named_input) {
    std::string input = "abc";
    ruvia::http3_data_event borrowed(0, input, nullptr, false);
    ruvia::http3_data_event owned(4, input, nullptr, true);

    EXPECT_EQ(borrowed.bytes_.data(), input.data());
    input[0] = 'z';
    EXPECT_EQ(borrowed.bytes_, std::string_view("zbc"));
    EXPECT_EQ(owned.bytes_, std::string_view("abc"));
}

TEST(Ruvia, http3_data_event_move_assignment_preserves_self_aliased_body) {
    for (const std::size_t length : {std::size_t{3}, std::size_t{256}}) {
        for (const bool slice : {false, true}) {
            SCOPED_TRACE(length);
            SCOPED_TRACE(slice);
            ruvia::test::reentrant_memory_resource resource;
            bool retired = false;
            {
                const std::string original(length, 'x');
                const auto expected = slice ? original.substr(1, length - 2) : original;
                ruvia::http3_data_event target(4, original, &resource, true);
                const auto old_address = reinterpret_cast<std::uintptr_t>(target.bytes_.data());
                resource.deallocation_context_ = &retired;
                resource.before_deallocate_ = [](void* context) noexcept {
                    *static_cast<bool*>(context) = true;
                };
                {
                    const auto bytes = slice ? target.bytes_.substr(1, length - 2) : target.bytes_;
                    ruvia::http3_data_event source(8, bytes, &resource, false);
                    target = std::move(source);
                }
                resource.before_deallocate_ = nullptr;
                const auto address = reinterpret_cast<std::uintptr_t>(target.bytes_.data());
                const bool expired = retired && address >= old_address && address - old_address < length;
                EXPECT_FALSE(expired);
                if (!expired) {
                    EXPECT_EQ(target.stream_id_, 8);
                    EXPECT_EQ(target.bytes_, std::string_view(expected));
                }
            }
            EXPECT_EQ(resource.live_bytes(), std::size_t{0});
        }
    }
}

TEST(Ruvia, http3_data_event_self_alias_snapshot_failure_preserves_both_events) {
    ruvia::test::failing_memory_resource resource;
    {
        const std::string original(256, 'x');
        ruvia::http3_data_event target(4, original, &resource, true);
        const auto bytes = target.bytes_;
        ruvia::http3_data_event source(8, bytes, &resource, false);
        if (ruvia::test::stl_allocation_failure_injection_enabled()) {
            const auto live = resource.live_allocations();
            resource.fail_after(0);
            bool failed = false;
            try {
                target = std::move(source);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            ASSERT_TRUE(failed);
            EXPECT_EQ(target.stream_id_, 4);
            EXPECT_EQ(target.bytes_, std::string_view(original));
            EXPECT_EQ(target.bytes_.data(), bytes.data());
            EXPECT_EQ(source.stream_id_, 8);
            EXPECT_EQ(source.bytes_, std::string_view(original));
            EXPECT_EQ(source.bytes_.data(), bytes.data());
            EXPECT_EQ(resource.live_allocations(), live);
        }
        resource.allow_allocations();
        target = std::move(source);
        EXPECT_EQ(target.stream_id_, 8);
        EXPECT_EQ(target.bytes_, std::string_view(original));
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http3_data_event_move_assignment_keeps_external_body_borrowed) {
    ruvia::test::failing_memory_resource resource;
    const std::string external(256, 'e');
    {
        const std::string original(256, 'x');
        ruvia::http3_data_event target(4, original, &resource, true);
        ruvia::http3_data_event source(8, external, &resource, false);
        resource.fail_after_stl_operation(0);
        target = std::move(source);
        resource.allow_allocations();
        EXPECT_EQ(target.stream_id_, 8);
        EXPECT_EQ(target.bytes_, std::string_view(external));
        EXPECT_EQ(target.bytes_.data(), external.data());
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http3_data_event_move_assignment_preserves_state_on_allocation_failure) {
    const std::string previous(64, 'p');
    const std::string incoming(2048, 'n');
    for (const bool owned_target : {false, true}) {
        SCOPED_TRACE(owned_target);
        ruvia::test::failing_memory_resource source_resource;
        ruvia::test::failing_memory_resource target_resource;
        {
            ruvia::http3_data_event target(4, previous, &target_resource, owned_target);
            {
                ruvia::http3_data_event source(8, incoming, &source_resource, true);
                if (ruvia::test::stl_allocation_failure_injection_enabled()) {
                    const auto source_bytes = source.bytes_;
                    const auto target_bytes = target.bytes_;
                    const auto source_live = source_resource.live_allocations();
                    const auto target_live = target_resource.live_allocations();
                    target_resource.fail_after(0);
                    EXPECT_THROW(target = std::move(source), std::bad_alloc);
                    EXPECT_EQ(target.stream_id_, 4);
                    EXPECT_EQ(target.bytes_, std::string_view(previous));
                    EXPECT_EQ(target.bytes_.data(), target_bytes.data());
                    EXPECT_EQ(source.stream_id_, 8);
                    EXPECT_EQ(source.bytes_, std::string_view(incoming));
                    EXPECT_EQ(source.bytes_.data(), source_bytes.data());
                    EXPECT_EQ(source_resource.live_allocations(), source_live);
                    EXPECT_EQ(target_resource.live_allocations(), target_live);
                }
                target_resource.allow_allocations();
                target = std::move(source);
            }
            EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
            EXPECT_EQ(target.stream_id_, 8);
            EXPECT_EQ(target.bytes_, std::string_view(incoming));
        }
        EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
        EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http3_nghttp3_streaming_body_is_owned_until_acknowledged) {
    ruvia::test::http3_peers peers({.own_data_events_ = true});
    EXPECT_TRUE(peers.bind());

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_length_ = 3, .streaming_ = true}) == ruvia::http3_status::accepted);
    const auto submitted = peers.client_.submit_data(0, ruvia::test::http3_bytes("abc"));
    EXPECT_TRUE(submitted.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(submitted.accepted_bytes_, std::size_t{3});
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);

    // The relay scrubs every delivered input buffer, so owned DATA is the
    // only way the payload can still read "abc" after delivery.
    std::optional<ruvia::http3_data_event> retained;
    ruvia::test::http3_event_log request;
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
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0) && request.ended(4));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_TRUE(response.ended(0));

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_status::accepted);
    ruvia::test::http3_event_log closed;
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http3_response_head_error>(
        submit_no_content_response(peers.server_, 0)));
}

// RFC 9114 §5.2 graceful shutdown: the notice still admits in-flight
// requests, the final GOAWAY names the first unprocessed request stream, and
// the server is drained only once every admitted request stream has closed.
TEST(Ruvia, http3_server_drain_sends_notice_then_final_goaway) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(peers.server_.begin_drain() == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    ruvia::test::http3_event_log client;
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

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
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
    ruvia::test::http3_peers peers(
        {.enable_connect_protocol_ = true, .enable_h3_datagram_ = true},
        {.enable_h3_datagram_ = true});
    EXPECT_TRUE(peers.bind());
    EXPECT_TRUE(!peers.client_.peer_settings().has_value());
    ruvia::test::http3_event_log log;
    EXPECT_TRUE(peers.to_client(log));
    EXPECT_TRUE(peers.to_server(log));
    const auto settings = peers.client_.peer_settings();
    EXPECT_TRUE(settings && settings->enable_connect_protocol_ && settings->h3_datagram_);
    EXPECT_TRUE(!peers.client_.datagram_prefix(0));
    EXPECT_TRUE(peers.send_request(0, get_request_head, false, log));
    const auto prefix = peers.client_.datagram_prefix(0);
    EXPECT_TRUE(prefix && prefix->size_ == 1 && prefix->bytes_[0] == std::byte{0});
    const std::string wire("\0\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_status::accepted);
    // Public input calls must drain the prior owned event before appending more.
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_status::events_pending);
    auto event = peers.server_.next_event();
    const auto* datagram = event ? std::get_if<ruvia::http3_datagram_event>(&*event) : nullptr;
    EXPECT_TRUE(datagram && datagram->stream_id_ == 0 && datagram->payload_ == std::string_view("\0echo", 5));
    EXPECT_TRUE(!peers.server_.next_event());
    // An unknown request stream is dropped, not retained for future streams.
    const std::string unknown("\1\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(unknown)).status_ == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
    EXPECT_TRUE(peers.server_.close_stream(0, request_cancelled, request_cancelled) == ruvia::http3_status::accepted);
    while (peers.server_.next_event()) {
    }
    EXPECT_TRUE(!peers.server_.datagram_prefix(0));
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
}

TEST(Ruvia, http3_datagram_truncated_prefix_and_disabled_setting_are_connection_errors) {
    auto enabled = ruvia::http3_connection::server({.enable_h3_datagram_ = true});
    const auto truncated = enabled.read_datagram(ruvia::test::http3_bytes("\x40"));
    EXPECT_TRUE(truncated.status_ == ruvia::http3_status::protocol_error);
    EXPECT_EQ(truncated.error_code_, static_cast<std::uint64_t>(ruvia::http3_error_code::datagram_error));
    auto disabled = ruvia::http3_connection::server();
    const std::string wire("\0hello", 6);
    EXPECT_TRUE(disabled.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_status::protocol_error);
}

TEST(Ruvia, http3_origin_frame_advertises_initial_and_dynamic_origins) {
    constexpr std::array<std::string_view, 2> initial_origins{
        "https://example.test",
        "https://api.example.test:8443",
    };
    ruvia::test::http3_peers peers({.origins_ = initial_origins});
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log client_log;
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
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array priority_fields{ruvia::http_header_view{"priority", "u=1, i"}};
    const ruvia::http3_regular_request_head_view prioritized_request_head{
        .authority_ = "example.test", .target_ = "/resource", .headers_ = priority_fields};
    EXPECT_TRUE(peers.send_request(0, prioritized_request_head, true, server_log));
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

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.header("Priority", "u=2, i");
    response.body("prioritized");
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
    auto overridden = peers.server_.stream_priority(0);
    EXPECT_TRUE(overridden.has_value());
    if (overridden) {
        EXPECT_EQ(overridden->urgency_.value(), std::uint8_t{2});
        EXPECT_TRUE(overridden->incremental_);
    }
    EXPECT_EQ(peers.server_.submit_data(0, ruvia::test::http3_bytes(response.body())).accepted_bytes_, response.body().size());
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_client(client_log));
    ASSERT_EQ(client_log.headers_.size(), std::size_t{1});
    EXPECT_TRUE(client_log.headers_[0].field("priority") == "u=2, i");
}

TEST(Ruvia, http3_connections_route_c_library_allocations_through_pmr) {
    ruvia::test::failing_memory_resource resource;
    {
        auto client = ruvia::http3_connection::client({.resource_ = &resource});
        auto server = ruvia::http3_connection::server({.resource_ = &resource});
        EXPECT_TRUE(resource.live_allocations() > std::size_t{0});
        EXPECT_TRUE(client.bind_control_streams(2, 6, 10) == ruvia::http3_status::accepted);
        EXPECT_TRUE(server.bind_control_streams(3, 7, 11) == ruvia::http3_status::accepted);
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});

    resource.fail_after(0);
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::http3_connection::client({.resource_ = &resource});
    }));
    resource.allow_allocations();
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http3_request_enforces_declared_content_length) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_length_ = 5, .streaming_ = true}) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).status_ ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)peers.client_.finish_stream(0); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)peers.client_.submit_data(0, ruvia::test::http3_bytes("def"));
    }));
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("de")).status_ ==
                ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    EXPECT_EQ(server_log.data(0), "abcde");
    EXPECT_TRUE(server_log.ended(0));
}

TEST(Ruvia, http3_concurrent_streams_interleave_partial_header_blocks) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    EXPECT_TRUE(peers.client_.submit_request_head(0, get_request_head) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.client_.submit_request_head(4, get_request_head) == ruvia::http3_status::accepted);

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
