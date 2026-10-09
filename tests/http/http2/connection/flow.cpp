#include <new>

#include "ruvia/http/http_response_stream.h"

#include "http2/http2_output_buffer.h"
#include "http2_connection_fixture.h"

namespace {

#if !defined(_MSC_VER)
class toggle_rejecting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool value = true) noexcept {
        reject_ = value;
    }

    void reject_allocations_of_size(std::size_t bytes_value) noexcept {
        rejected_size_ = bytes_value;
    }

    void clear_allocation_size_rejection() noexcept {
        rejected_size_ = 0;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_ || (rejected_size_ != 0 && bytes_value == rejected_size_)) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
    std::size_t rejected_size_{0};
};
#endif  // !_MSC_VER

}  // namespace

// http2_connection: flow control windows and GOAWAY draining.

// A valid connection-level WINDOW_UPDATE just opens the send window: no error, no
// output frame.

RUVIA_TEST(http2_connection_feed_connection_window_update_ok) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 0, 1000);
    const auto result_value = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_connection_begin_client_connection_is_atomic_on_second_segment_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource, ruvia::http2_role::client);
    resource.reject_allocations_of_size(
        sizeof(ruvia::detail::http2_output_buffer::segment_type) * std::size_t{2});

    bool allocation_failed = false;
    try {
        conn.begin_connection();
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.feed({}) == http2_feed_result::connection_not_started);

    resource.clear_allocation_size_rejection();
    conn.begin_connection();
    const auto expected_bytes = ruvia::http2_client_preface.size() +
                                ruvia::detail::http2_local_settings::frame_bytes +
                                ruvia::detail::http2_window_update_frame_bytes;
    RUVIA_CHECK_EQ(conn.pending_output().size(), expected_bytes);
    const auto first_attempt = std::string(conn.pending_output());
    conn.begin_connection();
    RUVIA_CHECK_EQ(conn.pending_output(), std::string_view(first_attempt));
}
#endif  // !_MSC_VER

#if !defined(_MSC_VER)
// These fault-injection paths include PMR string growth. MSVC's debug
// pmr::string does not complete when its resource throws during growth.
RUVIA_TEST(http2_connection_lifecycle_output_transitions_are_retryable_on_allocation_failure) {
    {
        toggle_rejecting_memory_resource resource;
        http2_connection conn(&resource);
        resource.reject_allocations();

        bool allocation_failed = false;
        try {
            conn.begin_connection();
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }

        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK(conn.pending_output().empty());
        RUVIA_CHECK(conn.feed({}) == http2_feed_result::connection_not_started);

        resource.reject_allocations(false);
        conn.begin_connection();
        RUVIA_CHECK(!conn.pending_output().empty());
    }

    {
        toggle_rejecting_memory_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        std::pmr::string discarded(&resource);
        conn.take_output(discarded);

        resource.reject_allocations();
        bool allocation_failed = false;
        try {
            conn.begin_drain();
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }

        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK(!conn.draining());
        RUVIA_CHECK(conn.pending_output().empty());

        resource.reject_allocations(false);
        conn.begin_drain();
        RUVIA_CHECK(conn.draining());
        RUVIA_CHECK(!conn.pending_output().empty());
    }

    {
        toggle_rejecting_memory_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        std::pmr::string discarded(&resource);
        conn.take_output(discarded);

        char update[ruvia::detail::http2_window_update_frame_bytes];
        ruvia::detail::http2_write_window_update(update, 0, 0);
        resource.reject_allocations();
        bool allocation_failed = false;
        try {
            (void)conn.feed(std::string_view(update, sizeof(update)));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }

        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK(!conn.connection_error().has_value());
        RUVIA_CHECK(conn.pending_output().empty());

        resource.reject_allocations(false);
        RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
                    http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);
    }

    {
        toggle_rejecting_memory_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        std::pmr::string discarded(&resource);
        conn.take_output(discarded);

        char ping[9 + 8];
        ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
        std::memset(ping + 9, 0, 8);
        resource.reject_allocations();
        bool allocation_failed = false;
        std::string output_before_failure;
        // A 17-byte PING ACK exceeds libstdc++'s string SSO but fits in libc++'s.
        // Keep submitting complete fast-path frames until the actual output
        // capacity is reached, then verify that the first allocating transition
        // leaves the failed frame wholly retryable on either implementation.
        for (std::size_t attempt_value = 0; attempt_value < 64 && !allocation_failed; ++attempt_value) {
            output_before_failure.assign(conn.pending_output());
            try {
                RUVIA_CHECK(
                    conn.feed(std::string_view(ping, sizeof(ping))) == http2_feed_result::accepted);
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }
        }

        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK_EQ(conn.pending_output(), std::string_view(output_before_failure));
        RUVIA_CHECK(!conn.connection_error().has_value());

        resource.reject_allocations(false);
        const auto pending_before_retry = conn.pending_output().size();
        RUVIA_CHECK(conn.feed(std::string_view(ping, sizeof(ping))) == http2_feed_result::accepted);
        RUVIA_CHECK_EQ(conn.pending_output().size(), pending_before_retry + std::size_t{9 + 8});
    }
}

RUVIA_TEST(http2_connection_feed_batch_is_retryable_when_a_later_frame_allocates) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 0);
    drive_get_request(conn, &resource);

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(stream->send_window(), std::int32_t{0});

    // Move the handshake/request output away so the next PING ACK must reserve
    // fresh PMR storage. The preceding WINDOW_UPDATE itself needs no storage.
    std::pmr::string discarded(&resource);
    conn.take_output(discarded);

    std::array<char, 13 + 9 + 8> batch{};
    ruvia::detail::http2_write_window_update(batch.data(), 1, 100);
    ruvia::detail::http2_encode_frame_header(batch.data() + 13, 8, http2_frame_type::ping, 0, 0);
    std::memset(batch.data() + 13 + 9, 0, 8);

    // Fill the implementation-defined small-string output capacity first. The
    // failed probe is a complete fast-path PING and therefore leaves no retained
    // input; the following batch then deterministically fails on its later PING
    // output rather than depending on whether 17 bytes fit libc++ or libstdc++ SSO.
    resource.reject_allocations();
    bool primed_output_failure = false;
    const auto ping = std::string_view(batch.data() + 13, 9 + 8);
    for (std::size_t attempt_value = 0; attempt_value < 64 && !primed_output_failure; ++attempt_value) {
        try {
            RUVIA_CHECK(conn.feed(ping) == http2_feed_result::accepted);
        } catch (const std::bad_alloc&) {
            primed_output_failure = true;
        }
    }
    RUVIA_CHECK(primed_output_failure);

    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(batch.data(), batch.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK_EQ(stream->send_window(), std::int32_t{100});

    resource.reject_allocations(false);
    RUVIA_CHECK(
        conn.feed(std::string_view(batch.data(), batch.size())) == http2_feed_result::accepted);
    RUVIA_CHECK_EQ(stream->send_window(), std::int32_t{100});
}

RUVIA_TEST(http2_connection_header_callback_allocation_failure_retries_final_continuation) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::string large_path(4096, 'p');
    large_path.front() = '/';
    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "GET");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", large_path);
    hpack_encoder::encode_header(block, ":authority", "example.com");

    // Put the complete compressed block in the first HEADERS frame, then use an
    // empty final CONTINUATION. This isolates callback allocation from block
    // buffering and exercises the continuation latch's rollback as well.
    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, block);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, {});
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.header_block_in_progress());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(!stream->has_method());
        RUVIA_CHECK(!stream->has_path());
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.header_block_in_progress());
    const auto head_event = conn.next_event();
    RUVIA_CHECK(head_event.has_value());
    if (head_event.has_value()) {
        RUVIA_CHECK(head_event->message_head() != nullptr);
    }
    const auto end_event = conn.next_event();
    RUVIA_CHECK(end_event.has_value());
    if (end_event.has_value()) {
        RUVIA_CHECK(end_event->message_end() != nullptr);
    }
}

RUVIA_TEST(http2_connection_header_error_output_allocation_failure_retains_block) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // This block is syntactically valid HPACK but omits :scheme, so the request
    // semantic check returns a stream error only after the whole block is decoded.
    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "GET");
    hpack_encoder::encode_header(block, ":path", "/");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, block);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, {});

    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());

    // Fail while constructing the RST_STREAM response, after decode has already
    // classified the request. The compressed block must remain available for retry.
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.header_block_in_progress());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(!stream->remote_header_block().empty());
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.header_block_in_progress());
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK_EQ(conn.pending_output().size(), std::size_t{13});
}

RUVIA_TEST(http2_connection_header_event_publication_allocation_failure_retries_block) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", "example.com");
    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, block);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, {});

    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());

    // Decoding succeeds, but publishing message_head/message_end must grow the
    // event queue. The stream transaction must keep both decoded state and block
    // bytes uncommitted until that publication succeeds.
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.header_block_in_progress());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(!stream->has_method());
        RUVIA_CHECK(!stream->has_path());
        RUVIA_CHECK(!stream->remote_header_block().empty());
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.header_block_in_progress());
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_single_frame_new_peer_headers_event_failure_is_retryable) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", "example.com");
    const auto frame = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    resource.reject_allocations_of_size(2 * sizeof(ruvia::detail::http2_event));
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(frame.data(), frame.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(!conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());

    resource.clear_allocation_size_rejection();
    RUVIA_CHECK(
        conn.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.stream(1) != nullptr);
}

RUVIA_TEST(
    http2_connection_rejected_connect_response_output_is_retryable_on_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_connect_request_head("example.test:443");
    RUVIA_CHECK(request.submitted() != nullptr);
    client.consume_output(client.pending_output().size());

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":status", "403");
    const auto response = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    resource.reject_allocations_of_size(2 * sizeof(ruvia::detail::http2_event));
    bool allocation_failed = false;
    try {
        (void)client.feed(std::string_view(response.data(), response.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.pending_output().empty());
    auto* stream = client.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->tunnel().pending() != nullptr);
    }
    RUVIA_CHECK(!client.next_event().has_value());

    resource.clear_allocation_size_rejection();
    RUVIA_CHECK(client.feed(std::string_view(response.data(), response.size())) ==
                http2_feed_result::accepted);
    const auto out = client.pending_output();
    RUVIA_CHECK_EQ(out.size(), std::size_t{ruvia::detail::http2_frame_header_bytes});
    if (out.size() >= ruvia::detail::http2_frame_header_bytes) {
        const auto local_end = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(local_end.type_, static_cast<std::uint8_t>(http2_frame_type::data));
        RUVIA_CHECK_EQ(local_end.stream_id_, std::uint32_t{1});
        RUVIA_CHECK((local_end.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    }
    const auto head = client.next_event();
    RUVIA_CHECK(head.has_value());
    if (head.has_value()) {
        RUVIA_CHECK(head->kind() == http2_event_kind::message_head);
    }
    const auto end = client.next_event();
    RUVIA_CHECK(end.has_value());
    if (end.has_value()) {
        RUVIA_CHECK(end->kind() == http2_event_kind::message_end);
    }
    RUVIA_CHECK(!client.next_event().has_value());
}

RUVIA_TEST(http2_connection_connect_response_tunnel_end_allocation_failure_is_retryable) {
    toggle_rejecting_memory_resource resource;
    http2_connection server(&resource);
    handshake(server);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":authority", "example.test:443");
    const auto request = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);

    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->tunnel().pending() != nullptr);
        RUVIA_CHECK(stream->remote_receive().connect_pending_end_stream() != nullptr);
    }

    ruvia::http_response accepted({.resource_ = &resource});
    accepted.status(ruvia::http_status::ok);

    resource.reject_allocations_of_size(2 * sizeof(ruvia::detail::http2_event));
    bool allocation_failed = false;
    try {
        (void)server.submit_connect_response_head(1, accepted);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(server.pending_output().empty());
    stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->tunnel().pending() != nullptr);
        RUVIA_CHECK(stream->remote_receive().connect_pending_end_stream() != nullptr);
    }

    const auto head = server.next_event();
    RUVIA_CHECK(head.has_value());
    if (head.has_value()) {
        RUVIA_CHECK(head->kind() == http2_event_kind::message_head);
    }
    RUVIA_CHECK(!server.next_event().has_value());

    resource.clear_allocation_size_rejection();
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == http2_submit_status::accepted);
    RUVIA_CHECK(!server.pending_output().empty());
    const auto tunnel_end = server.next_event();
    RUVIA_CHECK(tunnel_end.has_value());
    if (tunnel_end.has_value()) {
        RUVIA_CHECK(tunnel_end->kind() == http2_event_kind::tunnel_end);
    }
    RUVIA_CHECK(!server.next_event().has_value());
}

RUVIA_TEST(http2_connection_final_continuation_retry_rolls_back_appended_fragment) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string first(&resource);
    hpack_encoder::encode_header(first, ":method", "GET");
    hpack_encoder::encode_header(first, ":scheme", "https");
    hpack_encoder::encode_header(first, ":path", "/");

    std::pmr::string final(&resource);
    hpack_encoder::encode_header(final, ":authority", "example.com");

    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, first);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, final);
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        stream->remote_header_block().reserve(first.size() + final.size());
    }

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.header_block_in_progress());
    stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->remote_header_block().size(), first.size());
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.header_block_in_progress());
    const auto head_event = conn.next_event();
    RUVIA_CHECK(head_event.has_value());
    if (head_event.has_value()) {
        RUVIA_CHECK(head_event->kind() == http2_event_kind::message_head);
    }
    const auto end_event = conn.next_event();
    RUVIA_CHECK(end_event.has_value());
    if (end_event.has_value()) {
        RUVIA_CHECK(end_event->kind() == http2_event_kind::message_end);
    }
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.stream(1) != nullptr);
}

RUVIA_TEST(http2_connection_client_response_event_publication_allocation_failure_retries_block) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    client.consume_output(client.pending_output().size());

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":status", "200");
    const auto head = headers_frame(&resource, 1, 0, block);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, {});
    RUVIA_CHECK(
        client.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(client.header_block_in_progress());

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)client.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.header_block_in_progress());
    auto* stream = client.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->response_status() == nullptr);
        RUVIA_CHECK(!stream->remote_header_block().empty());
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(client.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!client.header_block_in_progress());
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());
}

RUVIA_TEST(http2_connection_header_event_retry_rolls_back_hpack_dynamic_table) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", "example.com");
    const auto append_indexed_header = [&block](std::string_view name, std::string_view value) {
        const auto offset = block.size();
        hpack_encoder::encode_header(block, name, value);
        block[offset] = static_cast<char>(static_cast<unsigned char>(block[offset]) | 0x40U);
    };
    append_indexed_header("x-first", "one");
    append_indexed_header("x-second", "two");

    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, block);
    const auto final_continuation =
        continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, {});
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);

    resource.reject_allocations_of_size(2 * sizeof(ruvia::detail::http2_event));
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.header_block_in_progress());

    resource.clear_allocation_size_rejection();
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().has_value());
    RUVIA_CHECK(conn.next_event().has_value());
    RUVIA_CHECK(!conn.next_event().has_value());

    // With one successful decode, dynamic indexes 62 and 63 are occupied and
    // index 64 is invalid. A duplicate commit from the failed publication would
    // make index 64 spuriously resolve to one of the retried fields.
    std::pmr::string invalid(&resource);
    encode_request(invalid, "GET", "https", "/", "example.com");
    hpack_encoder::encode_indexed(invalid, 64);
    const auto next_value = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream, invalid);
    RUVIA_CHECK(
        conn.feed(std::string_view(next_value.data(), next_value.size())) == http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::compression_error);
}

RUVIA_TEST(http2_connection_settings_drain_allocation_failure_is_fully_retryable) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake_with_window(client, 0);

    const auto request = client.submit_regular_request_head(
        "POST", "https", "example.test", "/upload", {}, http2_request_content::streaming());
    RUVIA_CHECK(request.submitted() != nullptr);
    client.consume_output(client.pending_output().size());

    const std::string body(20, 'x');
    RUVIA_CHECK(
        client.submit_data(1, body, http2_end_stream::keep_open) == http2_data_submit_status::queued);
    auto* stream = client.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(client.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->send_window(), 0);

    char settings[9 + 6];
    auto* out = ruvia::detail::http2_write_frame_header(settings, 6, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::initial_window_size, 20);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)client.feed(std::string_view(settings, sizeof(settings)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->send_window(), 0);

    resource.reject_allocations(false);
    RUVIA_CHECK(
        client.feed(std::string_view(settings, sizeof(settings))) == http2_feed_result::accepted);
    RUVIA_CHECK(!client.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->send_window(), 0);
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{20});
}

RUVIA_TEST(http2_connection_data_publication_is_retryable_on_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto data = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "hello");
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(data.data(), data.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK_EQ(stream->receive_window(),
        static_cast<std::int32_t>(ruvia::detail::http2_local_settings::initial_window_size));
    RUVIA_CHECK_EQ(stream->remote_content().allowed_without_length()->received_bytes(), std::size_t{0});

    resource.reject_allocations(false);
    RUVIA_CHECK(
        conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(
    http2_connection_short_terminal_data_reset_is_retryable_on_close_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "10");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto data = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "short");
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream == nullptr) {
        return;
    }

    resource.reject_allocations_of_size(2 * sizeof(ruvia::detail::http2_event));
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(data.data(), data.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK_EQ(stream->receive_window(),
        static_cast<std::int32_t>(ruvia::detail::http2_local_settings::initial_window_size));
    RUVIA_CHECK_EQ(stream->window_debt(), std::uint32_t{0});
    RUVIA_CHECK_EQ(stream->remote_content().allowed_known_length()->received_bytes(), std::size_t{0});

    resource.clear_allocation_size_rejection();
    RUVIA_CHECK(
        conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK_EQ(conn.stream(1), nullptr);
}

RUVIA_TEST(http2_connection_new_peer_headers_buffering_is_retryable_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // Force the first compressed fragment out of std::pmr::string's small-buffer
    // path. The frame is prepared before rejection; only the core's first HEADERS
    // buffering attempt is allowed to fail.
    const std::string authority(256, 'a');
    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", std::string_view(authority));
    const auto frame = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(frame.data(), frame.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(!conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(
        conn.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);
    const auto head = conn.next_event();
    RUVIA_CHECK(head.has_value());
    if (head.has_value()) {
        RUVIA_CHECK(head->kind() == http2_event_kind::message_head);
    }
    const auto end = conn.next_event();
    RUVIA_CHECK(end.has_value());
    if (end.has_value()) {
        RUVIA_CHECK(end->kind() == http2_event_kind::message_end);
    }
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_continuation_buffering_does_not_consume_retry_budget) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const std::string authority(256, 'a');
    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", std::string_view(authority));
    const auto first = headers_frame(
        &resource, 1, ruvia::detail::http2_flag_end_stream, std::string_view(block.data(), 1));
    const auto empty_continuation = continuation_frame(&resource, 1, 0, {});
    const auto final_continuation = continuation_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers, std::string_view(block.data() + 1, block.size() - 1));

    RUVIA_CHECK(
        conn.feed(std::string_view(first.data(), first.size())) == http2_feed_result::accepted);
    // Leave one slot before the limit. The final continuation will first fail while
    // growing the compressed block; retrying it must still be the 1024th frame, not
    // an over-budget 1025th frame.
    for (std::uint32_t i = 0; i < ruvia::detail::http2_max_continuation_frames - 1; ++i) {
        RUVIA_CHECK(conn.feed(std::string_view(empty_continuation.data(),
                        empty_continuation.size())) == http2_feed_result::accepted);
    }
    RUVIA_CHECK(conn.header_block_in_progress());

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(final_continuation.data(), final_continuation.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.header_block_in_progress());
    RUVIA_CHECK(!conn.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(final_continuation.data(), final_continuation.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().has_value());
    RUVIA_CHECK(conn.next_event().has_value());
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_stream_close_publication_is_retryable_on_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource, ruvia::detail::http2_role::client);
    handshake(conn);
    const auto request = conn.submit_regular_request_head(
        "POST", "https", "example.test", "/", {}, http2_request_content::streaming());
    RUVIA_CHECK(request.submitted() != nullptr);
    conn.consume_output(conn.pending_output().size());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);

    char reset[9 + 4];
    ruvia::detail::http2_encode_frame_header(reset, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(reset + 9, static_cast<std::uint32_t>(http2_error_code::cancel));

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(reset, sizeof(reset)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(stream != nullptr && !stream->is_aborted());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(reset, sizeof(reset))) == http2_feed_result::accepted);
    const auto event = conn.next_event();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        const auto* closed = event->stream_closed();
        RUVIA_CHECK(closed != nullptr);
        if (closed != nullptr) {
            RUVIA_CHECK_EQ(closed->stream_id(), std::uint32_t{1});
            RUVIA_CHECK(closed->source() == http2_stream_close_source::peer);
            RUVIA_CHECK(closed->error() == http2_error_code::cancel);
        }
    }
}

RUVIA_TEST(http2_connection_malformed_priority_reset_is_retryable_on_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    client.consume_output(client.pending_output().size());

    char priority[9 + 4]{};
    ruvia::detail::http2_encode_frame_header(priority, 4, http2_frame_type::priority, 0, 1);
    ruvia::detail::http2_write32(priority + 9, 0);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)client.feed(std::string_view(priority, sizeof(priority)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) != nullptr);
    RUVIA_CHECK(!client.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(
        client.feed(std::string_view(priority, sizeof(priority))) == http2_feed_result::accepted);
    const auto out = client.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, std::uint32_t{1});
    RUVIA_CHECK(client.stream(1) == nullptr);
    const auto event = client.next_event();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        RUVIA_CHECK(event->stream_closed() != nullptr);
    }
}

RUVIA_TEST(http2_connection_zero_window_update_reset_is_retryable_on_event_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    client.consume_output(client.pending_output().size());

    char window_update[ruvia::detail::http2_window_update_frame_bytes]{};
    ruvia::detail::http2_write_window_update(window_update, 1, 0);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)client.feed(std::string_view(window_update, sizeof(window_update)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) != nullptr);
    RUVIA_CHECK(!client.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(client.feed(std::string_view(window_update, sizeof(window_update))) ==
                http2_feed_result::accepted);
    const auto out = client.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, std::uint32_t{1});
    RUVIA_CHECK(client.stream(1) == nullptr);
    const auto event = client.next_event();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        RUVIA_CHECK(event->stream_closed() != nullptr);
    }
}

RUVIA_TEST(http2_connection_submit_reset_rolls_back_rst_when_window_credit_allocation_fails) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    const auto head_event = conn.next_event();
    RUVIA_CHECK(head_event.has_value());
    if (head_event.has_value()) {
        RUVIA_CHECK(head_event->kind() == http2_event_kind::message_head);
    }
    RUVIA_CHECK(!conn.next_event().has_value());

    std::uint32_t remaining_debt = ruvia::detail::http2_receive_window_update_threshold;
    const std::string body(ruvia::detail::http2_local_settings::max_frame_size, 'x');
    while (remaining_debt != 0) {
        const auto chunk_bytes = static_cast<std::size_t>(std::min<std::uint32_t>(
            remaining_debt, ruvia::detail::http2_local_settings::max_frame_size));
        const auto data = data_frame(&resource, 1, 0, std::string_view(body.data(), chunk_bytes));
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
        const auto data_event = conn.next_event();
        RUVIA_CHECK(data_event.has_value());
        if (data_event.has_value()) {
            RUVIA_CHECK(data_event->kind() == http2_event_kind::message_body_chunk);
        }
        RUVIA_CHECK(!conn.next_event().has_value());
        remaining_debt -= static_cast<std::uint32_t>(chunk_bytes);
    }

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->window_debt(), ruvia::detail::http2_receive_window_update_threshold);
    }

    std::pmr::string drained_output(&resource);
    conn.take_output(
        drained_output);  // leave a small output buffer where RST fits but WINDOW_UPDATE does not

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.submit_reset(1, http2_error_code::cancel);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(!stream->is_aborted());
        RUVIA_CHECK_EQ(stream->window_debt(), ruvia::detail::http2_receive_window_update_threshold);
    }

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(out.size(), std::size_t{2 * ruvia::detail::http2_window_update_frame_bytes});
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    const auto update = ruvia::detail::http2_parse_frame_header(
        out.substr(ruvia::detail::http2_window_update_frame_bytes, 9));
    RUVIA_CHECK_EQ(update.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(update.stream_id_, std::uint32_t{0});
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_rapid_reset_goaway_allocation_failure_is_retryable) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);

    char rst[9 + 4]{};
    for (std::uint32_t sid = 1; sid < 1U + 2U * 1000U; sid += 2) {
        const auto head = headers_frame(&resource, sid,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());

        ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, sid);
        ruvia::detail::http2_write32(rst + 9, 0);
        RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) == http2_feed_result::accepted);
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());
    }

    const std::uint32_t tripping_stream_id = 1U + 2U * 1000U;
    const auto head = headers_frame(&resource, tripping_stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(conn.stream(tripping_stream_id) != nullptr);

    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, tripping_stream_id);
    ruvia::detail::http2_write32(rst + 9, 0);

    std::pmr::string drained_output(&resource);
    conn.take_output(drained_output);  // swap a small empty buffer into output_

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(rst, sizeof(rst)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
    auto* stream = conn.stream(tripping_stream_id);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(!stream->is_aborted());
    }
    RUVIA_CHECK(!conn.next_event().has_value());

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) == http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::enhance_your_calm);
    RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()), enhance_your_calm);
}

RUVIA_TEST(http2_connection_peer_goaway_publication_is_retryable_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    std::pmr::string discarded(&resource);
    conn.take_output(discarded);

    const auto frame = goaway_frame(&resource, 0, http2_error_code::no_error);
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(frame.data(), frame.size()));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(!conn.peer_goaway().has_value());
    RUVIA_CHECK(!conn.draining());
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.pending_output().empty());

    resource.reject_allocations(false);
    RUVIA_CHECK(
        conn.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.peer_goaway().has_value());
    RUVIA_CHECK(conn.draining());
    RUVIA_CHECK(conn.next_event().has_value());
}
#endif  // !_MSC_VER

// A zero-increment connection WINDOW_UPDATE is a protocol error (GOAWAY).

RUVIA_TEST(http2_connection_feed_zero_window_update_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 0, 0);
    const auto result_value = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error().has_value());
    const auto goaway = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
}

// Opening stream 3 transitions skipped stream 1 from idle to closed (RFC 9113
// §5.1.1). A WINDOW_UPDATE is permitted there, but §6.9 still requires a stream
// PROTOCOL_ERROR when its increment is zero. Since RST_STREAM is forbidden on
// the already-closed stream, the implementation promotes that error to GOAWAY.

RUVIA_TEST(http2_connection_zero_window_update_on_skipped_stream_is_connection_error) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);
    const auto request = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(request) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());

    char update[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(update, 1, 0);
    RUVIA_CHECK(
        conn.feed(std::string_view(update, sizeof(update))) == http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);

    const auto out = conn.pending_output();
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(goaway.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

// RFC 9113 §6.8 distinguishes the two GOAWAY malformations: a nonzero stream id is a
// PROTOCOL_ERROR, a payload shorter than the 8 fixed octets is a FRAME_SIZE_ERROR.

RUVIA_TEST(http2_connection_malformed_goaway_error_codes) {
    const auto goaway_error_for = [&](std::uint32_t stream_id, std::size_t payload_bytes) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        conn.consume_output(conn.pending_output().size());

        std::pmr::string frame(&resource);
        char hdr[9];
        ruvia::detail::http2_encode_frame_header(
            hdr, static_cast<std::uint32_t>(payload_bytes), http2_frame_type::goaway, 0, stream_id);
        frame.append(hdr, 9);
        frame.append(payload_bytes, '\0');
        (void)conn.feed(std::string_view(frame.data(), frame.size()));

        const auto out = conn.pending_output();
        const auto header_value = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(header_value.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
        return ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13));
    };

    RUVIA_CHECK_EQ(
        goaway_error_for(1, 8), static_cast<std::uint32_t>(http2_error_code::protocol_error));
    RUVIA_CHECK_EQ(
        goaway_error_for(0, 7), static_cast<std::uint32_t>(http2_error_code::frame_size_error));
}

// RST_STREAM referencing an idle (never-opened) stream is a protocol error (GOAWAY).

RUVIA_TEST(http2_connection_feed_rst_on_idle_stream_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char frame[9 + 4];
    ruvia::detail::http2_encode_frame_header(frame, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(frame + 9, 0);
    const auto result_value = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error().has_value());
    const auto g = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(g.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
}

RUVIA_TEST(http2_connection_consumed_data_batches_window_updates_at_half_window) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }

    constexpr std::uint32_t chunk_bytes = 16 * 1024;
    constexpr std::uint32_t threshold = ruvia::detail::http2_receive_window_update_threshold;

    std::pmr::string body(chunk_bytes, 'x', &resource);
    const auto data = data_frame(&resource, 1, 0, std::string_view(body.data(), body.size()));

    for (std::uint32_t consumed = chunk_bytes; consumed <= threshold; consumed += chunk_bytes) {
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
        RUVIA_CHECK(!conn.next_event().has_value());
        conn.release_all_received_data(1);
        if (consumed < threshold) {
            RUVIA_CHECK(conn.pending_output().empty());
        }
    }

    const auto updates = conn.pending_output();
    RUVIA_CHECK_EQ(updates.size(), std::size_t{2 * ruvia::detail::http2_window_update_frame_bytes});
    const auto connection_update = ruvia::detail::http2_parse_frame_header(updates.substr(0, 9));
    const auto stream_update = ruvia::detail::http2_parse_frame_header(
        updates.substr(ruvia::detail::http2_window_update_frame_bytes, 9));
    RUVIA_CHECK_EQ(connection_update.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(connection_update.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(stream_update.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(stream_update.stream_id_, std::uint32_t{1});
    RUVIA_CHECK_EQ(ruvia::detail::http2_window_update_increment(updates.substr(9, 4)), threshold);
    RUVIA_CHECK_EQ(ruvia::detail::http2_window_update_increment(
                       updates.substr(ruvia::detail::http2_window_update_frame_bytes + 9, 4)),
        threshold);
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_connection_receive_window_update_is_transactional_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // Drop the handshake bytes and their capacity so the first threshold-crossing
    // WINDOW_UPDATE must obtain fresh output storage.
    std::pmr::string discarded(&resource);
    conn.take_output(discarded);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    constexpr std::size_t chunk_bytes = 16 * 1024;
    constexpr std::uint32_t threshold = ruvia::detail::http2_receive_window_update_threshold;

    std::pmr::string body(chunk_bytes, 'x', &resource);
    const auto data = data_frame(&resource, 1, 0, std::string_view(body.data(), body.size()));

    for (std::uint32_t received_value = chunk_bytes; received_value <= threshold; received_value += chunk_bytes) {
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
        if (received_value < threshold) {
            conn.release_all_received_data(1);
            RUVIA_CHECK(conn.pending_output().empty());
        }
    }

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        conn.release_all_received_data(1);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());

    resource.reject_allocations(false);
    conn.release_all_received_data(1);
    RUVIA_CHECK_EQ(
        conn.pending_output().size(), std::size_t{2 * ruvia::detail::http2_window_update_frame_bytes});
}
#endif  // !_MSC_VER

// A body larger than the send window is partially sent and the remainder queued. The
// core owns that remainder; a second submission is rejected without growing output.
// WINDOW_UPDATE drains it with END_STREAM and reports the stream ready for new input.

RUVIA_TEST(http2_connection_submit_data_blocks_then_drains_on_window) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 3);  // stream 1 starts with a 3-byte send window
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "10");
    const auto head_result = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(head_result));
    conn.consume_output(conn.pending_output().size());

    const char body[5] = {'a', 'b', 'c', 'd', 'e'};
    const auto r1 = conn.submit_data(1, std::string_view(body, 5), http2_end_stream::keep_open);
    RUVIA_CHECK(r1 == http2_data_submit_status::queued);
    RUVIA_CHECK(conn.has_queued_data(1));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(require_local_known_length(*stream).declared_length(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{3});

    const auto out1 = conn.pending_output();
    const auto d1 = ruvia::detail::http2_parse_frame_header(out1.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(d1.length_, static_cast<std::uint32_t>(3));             // only 3 fit
    RUVIA_CHECK((d1.flags_ & ruvia::detail::http2_flag_end_stream) == 0);  // not terminal
    const auto queued_bytes = out1.size();
    RUVIA_CHECK(conn.submit_data(1, "later", http2_end_stream::end_stream) ==
                http2_data_submit_status::backpressured);
    RUVIA_CHECK_EQ(conn.pending_output().size(), queued_bytes);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{3});
    conn.consume_output(out1.size());

    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 1, 10);  // reopen stream 1's window
    (void)conn.feed(std::string_view(wu, sizeof(wu)));

    const auto out2 = conn.pending_output();
    const auto d2 = ruvia::detail::http2_parse_frame_header(out2.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(d2.length_, static_cast<std::uint32_t>(2));  // remaining 2
    RUVIA_CHECK((d2.flags_ & ruvia::detail::http2_flag_end_stream) == 0);

    const auto drained = conn.take_drained_data_streams();
    RUVIA_CHECK_EQ(drained.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK_EQ(drained[0], static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5});
    RUVIA_CHECK(conn.submit_data(1, "later", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{10});
}

RUVIA_TEST(http2_connection_queued_terminal_data_closes_stream_after_drain) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 3);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "5");
    const auto head_result = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(head_result));
    conn.consume_output(conn.pending_output().size());

    const char body[5] = {'h', 'e', 'l', 'l', 'o'};
    RUVIA_CHECK(conn.submit_data(1, std::string_view(body, 5), http2_end_stream::end_stream) ==
                http2_data_submit_status::queued);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->local_send().end_stream_queued() != nullptr);
    RUVIA_CHECK(stream->local_send().end_stream_committed() == nullptr);
    RUVIA_CHECK(conn.submit_data(1, "again", http2_end_stream::keep_open) ==
                http2_data_submit_status::invalid_state);

    const auto first_output = conn.pending_output();
    const auto first_data = ruvia::detail::http2_parse_frame_header(first_output.substr(0, 9));
    RUVIA_CHECK_EQ(first_data.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(first_data.length_, static_cast<std::uint32_t>(3));
    RUVIA_CHECK((first_data.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(first_output.size());

    char window_update[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(window_update, 1, 10);
    RUVIA_CHECK(conn.feed(std::string_view(window_update, sizeof(window_update))) ==
                ruvia::detail::http2_feed_result::accepted);

    const auto second_output = conn.pending_output();
    const auto second_data = ruvia::detail::http2_parse_frame_header(second_output.substr(0, 9));
    RUVIA_CHECK_EQ(second_data.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(second_data.length_, static_cast<std::uint32_t>(2));
    RUVIA_CHECK((second_data.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    RUVIA_CHECK(stream->local_send().end_stream_queued() == nullptr);
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
    RUVIA_CHECK(conn.submit_data(1, "again", http2_end_stream::keep_open) ==
                http2_data_submit_status::invalid_state);
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_connection_submit_data_reserves_output_before_accepting_state) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 1024);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "1024");
    const auto head = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(head));
    conn.consume_output(conn.pending_output().size());

    resource.reject_allocations();
    const std::string body(1024, 'x');
    bool allocation_failed = false;
    try {
        (void)conn.submit_data(1, body, http2_end_stream::keep_open);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    auto* stream = conn.stream(1);
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{0});
}

RUVIA_TEST(http2_connection_window_update_drain_is_transactional_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 3);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "5000");
    const auto head = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(head));
    conn.consume_output(conn.pending_output().size());

    const std::string body(5000, 'x');
    RUVIA_CHECK(
        conn.submit_data(1, body, http2_end_stream::keep_open) == http2_data_submit_status::queued);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->send_window(), 0);
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{3});
    conn.consume_output(conn.pending_output().size());

    char update[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(update, 1, 5000);
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.feed(std::string_view(update, sizeof(update)));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->send_window(), 0);
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{3});

    resource.reject_allocations(false);
    RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.has_queued_data(1));
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5000});
    RUVIA_CHECK_EQ(conn.take_drained_data_streams().size(), std::size_t{1});
}
#endif  // !_MSC_VER

RUVIA_TEST(http2_connection_goaway_rejects_unprocessed_requests_in_core) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake_with_window(client, 0);

    const auto first = client.submit_regular_request_head(
        "GET", "https", "example.test", "/first", {}, http2_request_content::none());
    const auto second = client.submit_regular_request_head(
        "POST", "https", "example.test", "/upload", {}, http2_request_content::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    RUVIA_CHECK(second.submitted() != nullptr);
    const auto first_stream_id = submitted_request_stream_id(first);
    const auto second_stream_id = submitted_request_stream_id(second);
    RUVIA_CHECK_EQ(first_stream_id, std::uint32_t{1});
    RUVIA_CHECK_EQ(second_stream_id, std::uint32_t{3});
    client.pin_stream(second_stream_id);
    client.consume_output(client.pending_output().size());
    RUVIA_CHECK(client.submit_data(second_stream_id, "queued", http2_end_stream::keep_open) ==
                http2_data_submit_status::queued);
    RUVIA_CHECK(client.has_queued_data(second_stream_id));

    const auto goaway = goaway_frame(&resource, first_stream_id, http2_error_code::no_error);
    const auto result_value = client.feed(std::string_view(goaway.data(), goaway.size()));
    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!client.connection_error().has_value());
    RUVIA_CHECK(client.draining());
    const auto reciprocal = client.pending_output();
    const auto reciprocal_head = ruvia::detail::http2_parse_frame_header(reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(reciprocal_head.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read31(reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reciprocal.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::no_error));
    client.consume_output(reciprocal.size());

    const auto info = client.peer_goaway();
    RUVIA_CHECK(info.has_value());
    RUVIA_CHECK_EQ(info->last_stream_id(), first_stream_id);
    RUVIA_CHECK(info->error() == http2_error_code::no_error);
    auto event = client.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::goaway);
    RUVIA_CHECK_EQ(event.goaway()->last_stream_id(), first_stream_id);
    RUVIA_CHECK(event.goaway()->error() == http2_error_code::no_error);
    auto unprocessed_event = client.next_event().value();
    RUVIA_CHECK(unprocessed_event.kind() == http2_event_kind::request_unprocessed);
    RUVIA_CHECK_EQ(unprocessed_event.request_unprocessed()->stream_id(), second_stream_id);
    RUVIA_CHECK(!client.next_event().has_value());

    auto* first_stream = client.stream(first_stream_id);
    auto* second_stream = client.stream(second_stream_id);
    RUVIA_CHECK(first_stream != nullptr && !first_stream->is_aborted());
    RUVIA_CHECK(second_stream != nullptr && second_stream->is_aborted());
    RUVIA_CHECK(second_stream->local_send().aborted() != nullptr);
    RUVIA_CHECK(second_stream->local_send().aborted()->source() ==
                ruvia::detail::http2_stream_close_source::peer_goaway);
    RUVIA_CHECK(!second_stream->release_peer_concurrency_slot());
    RUVIA_CHECK(!client.has_queued_data(second_stream_id));
    RUVIA_CHECK(request_head_submit_error(client.submit_regular_request_head(
                    "GET", "https", "example.test", "/new", {}, http2_request_content::none())) ==
                http2_request_head_submit_error::connection_unavailable);

    client.unpin_stream(second_stream_id);
    RUVIA_CHECK(client.stream(second_stream_id) == nullptr);

    // GOAWAY does not abort a request at or below the peer boundary. Its response can
    // still complete after the reciprocal local drain has started.
    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    const auto response_head = headers_frame(&resource, first_stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(!client.connection_error().has_value());
}

RUVIA_TEST(http2_connection_goaway_last_stream_id_is_monotonic) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    for (const std::string_view path : {"/one", "/two", "/three"}) {
        const auto submitted = client.submit_regular_request_head(
            "GET", "https", "example.test", path, {}, http2_request_content::none());
        RUVIA_CHECK(submitted.submitted() != nullptr);
    }
    client.consume_output(client.pending_output().size());

    const auto notice = goaway_frame(&resource, 0x7fffffffU, http2_error_code::no_error);
    RUVIA_CHECK(client.feed(std::string_view(notice.data(), notice.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    auto event = client.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::goaway);
    RUVIA_CHECK_EQ(event.goaway()->last_stream_id(), std::uint32_t{0x7fffffffU});
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.stream(5) != nullptr);
    RUVIA_CHECK(client.draining());
    RUVIA_CHECK(!client.pending_output().empty());  // reciprocal GOAWAY(NO_ERROR)
    client.consume_output(client.pending_output().size());

    const auto narrowed = goaway_frame(&resource, 3, http2_error_code::internal_error);
    RUVIA_CHECK(client.feed(std::string_view(narrowed.data(), narrowed.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    auto narrowed_event = client.next_event().value();
    RUVIA_CHECK(narrowed_event.kind() == http2_event_kind::goaway);
    RUVIA_CHECK_EQ(narrowed_event.goaway()->last_stream_id(), std::uint32_t{3});
    RUVIA_CHECK(narrowed_event.goaway()->error() == http2_error_code::internal_error);
    auto unprocessed_event = client.next_event().value();
    RUVIA_CHECK(unprocessed_event.kind() == http2_event_kind::request_unprocessed);
    RUVIA_CHECK_EQ(unprocessed_event.request_unprocessed()->stream_id(), std::uint32_t{5});
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.stream(5) == nullptr);
    const auto narrowed_info = client.peer_goaway();
    RUVIA_CHECK(narrowed_info.has_value());
    RUVIA_CHECK_EQ(narrowed_info->last_stream_id(), std::uint32_t{3});
    RUVIA_CHECK(narrowed_info->error() == http2_error_code::internal_error);
    RUVIA_CHECK(client.pending_output().empty());  // local drain is idempotent

    const auto invalid_increase = goaway_frame(&resource, 5, http2_error_code::no_error);
    RUVIA_CHECK(client.feed(std::string_view(invalid_increase.data(), invalid_increase.size())) ==
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(client.connection_error() == http2_error_code::protocol_error);
    RUVIA_CHECK_EQ(client.peer_goaway()->last_stream_id(), std::uint32_t{3});
    const auto local_goaway = client.pending_output();
    const auto head = ruvia::detail::http2_parse_frame_header(local_goaway.substr(0, 9));
    RUVIA_CHECK_EQ(head.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(local_goaway.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connection_goaway_cannot_exclude_a_started_response) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());

    const auto contradictory = goaway_frame(&resource, 0, http2_error_code::no_error);
    RUVIA_CHECK(client.feed(std::string_view(contradictory.data(), contradictory.size())) ==
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(client.connection_error() == http2_error_code::protocol_error);
    RUVIA_CHECK(!client.peer_goaway().has_value());
    RUVIA_CHECK(client.stream(stream_id) != nullptr);
    RUVIA_CHECK(!client.stream(stream_id)->is_aborted());
    RUVIA_CHECK(!client.next_event().has_value());
}

RUVIA_TEST(http2_connection_peer_goaway_drains_without_truncating_server_request) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource, http2_role::server);
    handshake(server);

    const auto request_head = post_head_frame(&resource, "4");
    RUVIA_CHECK(server.feed(std::string_view(request_head.data(), request_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    auto event = server.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::message_head);
    RUVIA_CHECK_EQ(event.message_head()->stream_id(), std::uint32_t{1});
    RUVIA_CHECK(!server.next_event().has_value());

    // The client GOAWAY and the rest of an already established request can share one
    // transport read. The core must consume both frames and preserve event order.
    const auto peer_goaway = goaway_frame(&resource, 0, http2_error_code::no_error);
    const auto request_body = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "body");
    std::pmr::string batch(&resource);
    batch.append(peer_goaway.data(), peer_goaway.size());
    batch.append(request_body.data(), request_body.size());
    const auto result_value = server.feed(std::string_view(batch.data(), batch.size()));
    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(server.draining());
    RUVIA_CHECK(server.peer_goaway().has_value());

    auto goaway_event = server.next_event().value();
    RUVIA_CHECK(goaway_event.kind() == http2_event_kind::goaway);
    RUVIA_CHECK_EQ(goaway_event.goaway()->last_stream_id(), std::uint32_t{0});
    auto body_event = server.next_event().value();
    RUVIA_CHECK(body_event.kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK(body_event.message_body_chunk()->bytes() == "body");
    auto end_event = server.next_event().value();
    RUVIA_CHECK(end_event.kind() == http2_event_kind::message_end);
    RUVIA_CHECK_EQ(end_event.message_end()->stream_id(), std::uint32_t{1});
    RUVIA_CHECK(!server.next_event().has_value());

    // Reciprocal GOAWAY uses the highest accepted client stream (1), not the peer's
    // directional last-stream-id (which refers to server-initiated streams).
    const auto reciprocal = server.pending_output();
    const auto reciprocal_head = ruvia::detail::http2_parse_frame_header(reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(reciprocal_head.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read31(reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{1});
    server.consume_output(reciprocal.size());

    // A stream opened after our advertised boundary is safely refused, while stream 1
    // remains response-capable.
    std::pmr::string late_block(&resource);
    encode_get_request(late_block);
    const auto late = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(late_block.data(), late_block.size()));
    RUVIA_CHECK(server.feed(std::string_view(late.data(), late.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto reset = server.pending_output();
    const auto reset_head = ruvia::detail::http2_parse_frame_header(reset.substr(0, 9));
    RUVIA_CHECK_EQ(reset_head.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset_head.stream_id_, std::uint32_t{3});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::refused_stream));
    server.consume_output(reset.size());

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("ok");
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(server, 1, response)));
    RUVIA_CHECK(
        server.submit_data(1, "ok", http2_end_stream::end_stream) == http2_data_submit_status::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
}

// Graceful drain (RFC 9113 §6.8): begin_drain emits GOAWAY(NO_ERROR) at the last
// accepted stream id; streams already open keep working, HEADERS for a higher id are
// refused with RST_STREAM(REFUSED_STREAM), and begin_drain is idempotent.

RUVIA_TEST(http2_connection_begin_drain_refuses_new_streams) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);  // stream 1 open (half-closed remote)
    conn.consume_output(conn.pending_output().size());

    conn.begin_drain();
    // GOAWAY(NO_ERROR, last_stream_id=1) emitted without a connection error.
    const auto goaway = conn.pending_output();
    RUVIA_CHECK(goaway.size() >= 9);
    const auto gh = ruvia::detail::http2_parse_frame_header(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(gh.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.draining());
    conn.consume_output(goaway.size());

    // A new stream ABOVE the advertised id (3) is refused only after its complete
    // multi-frame field block is decoded. The inserted dynamic entry must survive.
    std::pmr::string block(&resource);
    encode_get_request(block);
    encode_short_dynamic_header(block, "x-refused", "indexed");
    const auto split = block.size() / 2;
    const auto h = headers_frame(
        &resource, 3, ruvia::detail::http2_flag_end_stream, std::string_view(block.data(), split));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());
    const auto continuation = continuation_frame(&resource, 3, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data() + split, block.size() - split));
    (void)conn.feed(std::string_view(continuation.data(), continuation.size()));
    while (conn.next_event().has_value()) {
        // stream 3 must NOT surface as a request (it was refused)
    }
    const auto rst = conn.pending_output();
    RUVIA_CHECK(rst.size() >= 9);
    const auto rh = ruvia::detail::http2_parse_frame_header(rst.substr(0, 9));
    RUVIA_CHECK_EQ(rh.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(rh.stream_id_, static_cast<std::uint32_t>(3));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(rst.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::refused_stream));
    RUVIA_CHECK(!conn.connection_error().has_value());  // refusal is not a connection error
    conn.consume_output(rst.size());

    // A later refused stream can reference the dynamic entry created by stream 3;
    // successful decode yields another REFUSED_STREAM, never COMPRESSION_ERROR.
    std::pmr::string dependent(&resource);
    encode_get_request(dependent);
    hpack_encoder::encode_indexed(dependent, 62);
    const auto h5 = headers_frame(&resource, 5,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(dependent.data(), dependent.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h5.data(), h5.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    const auto rst5 = conn.pending_output();
    const auto rh5 = ruvia::detail::http2_parse_frame_header(rst5.substr(0, 9));
    RUVIA_CHECK_EQ(rh5.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(rh5.stream_id_, static_cast<std::uint32_t>(5));
    RUVIA_CHECK(!conn.connection_error().has_value());
    conn.consume_output(rst5.size());

    // Stream 1 (opened before the drain) can still be answered.
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("ok");
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(conn, 1, response)));
    RUVIA_CHECK(
        conn.submit_data(1, "ok", http2_end_stream::end_stream) == http2_data_submit_status::accepted);
    RUVIA_CHECK(conn.pending_output().size() > 9);  // response frames produced
    conn.consume_output(conn.pending_output().size());

    conn.begin_drain();  // idempotent: no further GOAWAY
    RUVIA_CHECK(conn.pending_output().empty());
}

// RFC 9113 §6.8: "Endpoints MUST NOT increase the value they send in the last stream
// identifier." A drain advertises id 1; a later HEADERS(3) is refused but still raises
// the internal idle-stream high-water mark. A fatal GOAWAY after that must still
// advertise 1 -- widening to 3 would tell the peer that a request it already retried
// elsewhere (on REFUSED_STREAM) might have been processed after all.

RUVIA_TEST(http2_connection_fatal_goaway_never_widens_drained_last_stream_id) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);  // stream 1 open
    conn.consume_output(conn.pending_output().size());

    conn.begin_drain();  // GOAWAY(NO_ERROR, last_stream_id=1)
    const auto drain_goaway = conn.pending_output();
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read31(reinterpret_cast<const unsigned char*>(drain_goaway.data() + 9)),
        static_cast<std::uint32_t>(1));
    conn.consume_output(drain_goaway.size());

    // Stream 3 arrives after the drain: refused, but it bumps last_stream_id_ to 3.
    std::pmr::string block(&resource);
    encode_get_request(block);
    const auto h3 = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h3.data(), h3.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    conn.consume_output(conn.pending_output().size());  // RST_STREAM(3, REFUSED_STREAM)

    // Now trip a connection error: the fatal GOAWAY must not advertise past 1.
    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 0, 0);
    RUVIA_CHECK(conn.feed(std::string_view(wu, sizeof(wu))) ==
                ruvia::detail::http2_feed_result::protocol_failure);

    const auto fatal = conn.pending_output();
    const auto fh = ruvia::detail::http2_parse_frame_header(fatal.substr(0, 9));
    RUVIA_CHECK_EQ(fh.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read31(reinterpret_cast<const unsigned char*>(fatal.data() + 9)),
        static_cast<std::uint32_t>(1));
}

RUVIA_TEST(http2_connection_fatal_failure_atomically_supersedes_local_drain) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    client.consume_output(client.pending_output().size());

    client.begin_drain();
    RUVIA_CHECK(client.draining());
    RUVIA_CHECK(!client.connection_error().has_value());
    client.consume_output(client.pending_output().size());

    // SETTINGS is connection-scoped. A non-zero stream id is fatal and must
    // replace, rather than coexist with, the earlier graceful drain state.
    char invalid_settings[9];
    ruvia::detail::http2_encode_frame_header(invalid_settings, 0, http2_frame_type::settings, 0, 1);
    RUVIA_CHECK(client.feed(std::string_view(invalid_settings, sizeof(invalid_settings))) ==
                http2_feed_result::protocol_failure);
    RUVIA_CHECK(!client.draining());
    RUVIA_CHECK(client.connection_error() == http2_error_code::protocol_error);

    const auto failed_output_size = client.pending_output().size();
    client.begin_drain();
    RUVIA_CHECK(!client.draining());
    RUVIA_CHECK_EQ(client.pending_output().size(), failed_output_size);

    const auto rejected = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(rejected.submitted() == nullptr);
    RUVIA_CHECK(
        request_head_submit_error(rejected) == http2_request_head_submit_error::connection_unavailable);
}

// Every non-empty DATA event retains receive-window debt. Removing the stream must
// transfer that debt into the connection's batched credit, even if the owner never
// calls release_received_data(), or the shared window would shrink permanently.

RUVIA_TEST(http2_connection_window_debt_batches_on_removal) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());

    constexpr std::uint32_t chunk_bytes = http2_local_settings::max_frame_size;
    constexpr std::uint32_t threshold = ruvia::detail::http2_receive_window_update_threshold;

    std::pmr::string body(chunk_bytes, 'x', &resource);
    const auto data = data_frame(&resource, 1, 0, std::string_view(body.data(), body.size()));
    for (std::uint32_t received_value = chunk_bytes; received_value <= threshold; received_value += chunk_bytes) {
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
        RUVIA_CHECK(!conn.next_event().has_value());
        RUVIA_CHECK(conn.pending_output().empty());
    }

    // Peer RST_STREAM removes the unpinned stream. Exactly one threshold of banked
    // debt must now restore the connection scope, never the dead stream scope.
    char rst[9 + 4];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, 8 /* CANCEL */);
    RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }

    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(out.size(), std::size_t{ruvia::detail::http2_window_update_frame_bytes});
    const auto update = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(update.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(update.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::http2_window_update_increment(out.substr(9, 4)), threshold);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_discarded_data_still_enforces_connection_window) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // Stream 1 banks valid DATA until owner acknowledgement, allowing the test to
    // exhaust the shared connection receive window without exceeding its stream
    // window.
    const auto streaming_head = post_head_frame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(streaming_head.data(), streaming_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());
    // Open then locally reset stream 3 so later DATA targets a known closed stream.
    std::pmr::string closed_block(&resource);
    encode_get_request(closed_block);
    const auto closed_head = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(closed_block.data(), closed_block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(closed_head.data(), closed_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.submit_reset(3, http2_error_code::cancel) == http2_submit_status::accepted);
    RUVIA_CHECK(conn.stream(3) == nullptr);
    conn.consume_output(conn.pending_output().size());

    std::string chunk(http2_local_settings::max_frame_size, 'x');
    std::uint32_t remaining = http2_local_settings::initial_window_size;
    while (remaining != 0) {
        const auto chunk_bytes =
            static_cast<std::size_t>(remaining < chunk.size() ? remaining : chunk.size());
        const auto data = data_frame(&resource, 1, 0, std::string_view(chunk.data(), chunk_bytes));
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::http2_feed_result::accepted);
        const auto event = conn.next_event().value();
        RUVIA_CHECK(event.kind() == http2_event_kind::message_body_chunk);
        RUVIA_CHECK_EQ(event.message_body_chunk()->bytes().size(), chunk_bytes);
        RUVIA_CHECK(!conn.next_event().has_value());
        RUVIA_CHECK(conn.pending_output().empty());
        remaining -= static_cast<std::uint32_t>(chunk_bytes);
    }

    // Even though stream 3 is closed and its DATA will be discarded, the DATA must
    // first fit the connection window. At zero remaining credit this is a connection
    // FLOW_CONTROL_ERROR, not RST_STREAM plus an unearned WINDOW_UPDATE.
    const auto overflow = data_frame(&resource, 3, 0, "x");
    const auto result_value = conn.feed(std::string_view(overflow.data(), overflow.size()));
    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::flow_control_error);
    const auto goaway = conn.pending_output();
    const auto goaway_head = ruvia::detail::http2_parse_frame_header(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(goaway_head.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(goaway.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::flow_control_error));
}

// Healthy keepalive PINGs (ACKs drained each round) never trip the budget, however many.

RUVIA_TEST(http2_connection_drained_pings_never_trip) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
    std::memset(ping + 9, 0, 8);

    for (int i = 0; i < 5000; ++i) {
        const auto r = conn.feed(std::string_view(ping, sizeof(ping)));
        RUVIA_CHECK(r == ruvia::detail::http2_feed_result::accepted);
        conn.consume_output(conn.pending_output().size());  // flush ACK -> resets budget
        RUVIA_CHECK(!conn.connection_error().has_value());
    }
}

// Healthy SETTINGS re-tuning (ACKs drained each round) never trips, however many.

RUVIA_TEST(http2_connection_drained_settings_never_trip) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char settings[9];
    ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);

    for (int i = 0; i < 5000; ++i) {
        const auto r = conn.feed(std::string_view(settings, sizeof(settings)));
        RUVIA_CHECK(r == ruvia::detail::http2_feed_result::accepted);
        conn.consume_output(conn.pending_output().size());  // flush ACK -> resets budget
        RUVIA_CHECK(!conn.connection_error().has_value());
    }
}

RUVIA_TEST(http2_connection_window_update_after_peer_reset_never_reopens_stream) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_peer_reset(conn, &resource, pinned);

        // RFC 9113 section 6.9 permits WINDOW_UPDATE on a closed stream. A
        // valid increment is ignored, including while pinned storage remains.
        char update[ruvia::detail::http2_window_update_frame_bytes];
        ruvia::detail::http2_write_window_update(update, 1, 1);
        RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
                    ruvia::detail::http2_feed_result::accepted);
        RUVIA_CHECK(!conn.connection_error().has_value());
        RUVIA_CHECK(conn.pending_output().empty());

        // Increment zero is still a PROTOCOL_ERROR. Because the peer reset
        // already closed this ordered stream, the core cannot legally send a
        // second RST_STREAM and must promote the stream error to the connection.
        ruvia::detail::http2_write_window_update(update, 1, 0);
        RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
                    ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);
        RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}

RUVIA_TEST(http2_connection_zero_window_update_on_closed_stream_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_local_reset(conn, &resource, pinned);

        char update[ruvia::detail::http2_window_update_frame_bytes];
        ruvia::detail::http2_write_window_update(update, 1, 1);
        RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
                    ruvia::detail::http2_feed_result::accepted);
        RUVIA_CHECK(!conn.connection_error().has_value());
        RUVIA_CHECK(conn.pending_output().empty());

        ruvia::detail::http2_write_window_update(update, 1, 0);
        RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
                    ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);
        RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}
