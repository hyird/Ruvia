#include <new>

#include "http2_connection_fixture.h"

// http2_connection: the stream table: admission, PRIORITY, RST_STREAM and close.

namespace {

#if !defined(_MSC_VER)
class toggle_rejecting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
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
};
#endif  // !_MSC_VER

}  // namespace

RUVIA_TEST(http2_connection_peer_stream_limit_waits_for_both_half_closes) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    apply_peer_max_concurrent_streams(client, 1);

    const auto first = client.submit_regular_request_head(
        "POST", "https", "example.test", "/upload", {}, http2_request_content::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto first_stream_id = submitted_request_stream_id(first);
    RUVIA_CHECK_EQ(first_stream_id, std::uint32_t{1});
    client.consume_output(client.pending_output().size());

    const auto while_open = client.submit_regular_request_head(
        "GET", "https", "example.test", "/second", {}, http2_request_content::none());
    RUVIA_CHECK(while_open.submitted() == nullptr);
    RUVIA_CHECK(
        request_head_submit_error(while_open) == http2_request_head_submit_error::peer_stream_limit_reached);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    const auto response_head = headers_frame(&resource, first_stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    while (client.next_event().has_value()) {
    }

    const auto peer_half_only = client.submit_regular_request_head(
        "GET", "https", "example.test", "/second", {}, http2_request_content::none());
    RUVIA_CHECK(peer_half_only.submitted() == nullptr);
    RUVIA_CHECK(request_head_submit_error(peer_half_only) ==
                http2_request_head_submit_error::peer_stream_limit_reached);
    RUVIA_CHECK(client.submit_data(first_stream_id, {}, http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    client.consume_output(client.pending_output().size());

    const auto after_both_halves = client.submit_regular_request_head(
        "GET", "https", "example.test", "/second", {}, http2_request_content::none());
    RUVIA_CHECK(after_both_halves.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(after_both_halves), std::uint32_t{3});
}

RUVIA_TEST(http2_connection_peer_reset_releases_peer_stream_limit_slot) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    apply_peer_max_concurrent_streams(client, 1);

    const auto first = client.submit_regular_request_head(
        "GET", "https", "example.test", "/first", {}, http2_request_content::none());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto first_stream_id = submitted_request_stream_id(first);
    client.consume_output(client.pending_output().size());
    RUVIA_CHECK(request_head_submit_error(client.submit_regular_request_head(
                    "GET", "https", "example.test", "/second", {}, http2_request_content::none())) ==
                http2_request_head_submit_error::peer_stream_limit_reached);

    char reset[13];
    ruvia::detail::http2_encode_frame_header(reset, 4, http2_frame_type::rst_stream, 0, first_stream_id);
    ruvia::detail::http2_write32(reset + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
    RUVIA_CHECK(client.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::detail::http2_feed_result::accepted);

    const auto after_reset = client.submit_regular_request_head(
        "GET", "https", "example.test", "/second", {}, http2_request_content::none());
    RUVIA_CHECK(after_reset.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(after_reset), std::uint32_t{3});
}

// RFC 9113 deprecated the RFC 7540 priority tree. Dependency and weight are ignored
// after validating frame shape, including the old self-dependency case.

RUVIA_TEST(http2_connection_feed_priority_payload_is_ignored) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // A self-dependent advisory payload on a live stream has no stream-state effect.
    drive_get_request(conn, &resource);  // stream 1 open
    conn.consume_output(conn.pending_output().size());
    char live[9 + 5];
    ruvia::detail::http2_encode_frame_header(live, 5, http2_frame_type::priority, 0, 1);
    ruvia::detail::http2_write32(live + 9, 1);  // depends on stream 1 (itself)
    live[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(live, sizeof(live))) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && !conn.stream(1)->is_aborted());

    // The same is true on an idle stream; PRIORITY never opens it.
    char idle[9 + 5];
    ruvia::detail::http2_encode_frame_header(idle, 5, http2_frame_type::priority, 0, 7);
    ruvia::detail::http2_write32(idle + 9, 7);  // idle stream depends on itself
    idle[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(idle, sizeof(idle))) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());  // ignored: no RST, no GOAWAY
}

RUVIA_TEST(http2_connection_malformed_priority_is_stream_frame_size_error) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);  // stream 1 open

    char malformed[9 + 4];
    ruvia::detail::http2_encode_frame_header(malformed, 4, http2_frame_type::priority, 0, 1);
    ruvia::detail::http2_write32(malformed + 9, 0);
    RUVIA_CHECK(
        conn.feed(std::string_view(malformed, sizeof(malformed))) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());

    const auto out = conn.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, std::uint32_t{1});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::frame_size_error));
    RUVIA_CHECK(conn.stream(1) == nullptr);

    const auto event = conn.next_event();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        const auto* closed = event->stream_closed();
        RUVIA_CHECK(closed != nullptr);
        if (closed != nullptr) {
            RUVIA_CHECK_EQ(closed->stream_id(), std::uint32_t{1});
            RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::frame_size_error);
        }
    }
}

RUVIA_TEST(http2_connection_malformed_idle_priority_is_connection_error) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char malformed[9 + 1];
    ruvia::detail::http2_encode_frame_header(malformed, 1, http2_frame_type::priority, 0, 7);
    malformed[9] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(malformed, sizeof(malformed))) ==
                http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::frame_size_error);
    const auto out = conn.pending_output();
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(goaway.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::frame_size_error));
}

RUVIA_TEST(http2_connection_priority_stream_zero_is_connection_protocol_error) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char malformed[9 + 4];
    ruvia::detail::http2_encode_frame_header(malformed, 4, http2_frame_type::priority, 0, 0);
    ruvia::detail::http2_write32(malformed + 9, 0);
    RUVIA_CHECK(conn.feed(std::string_view(malformed, sizeof(malformed))) ==
                http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);
    const auto out = conn.pending_output();
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connection_rejects_non_increasing_new_peer_stream_id) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string first_block(&resource);
    encode_get_request(first_block);
    const auto first = headers_frame(&resource, 5,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(first_block.data(), first_block.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(first.data(), first.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());

    std::pmr::string lower_block(&resource);
    encode_get_request(lower_block);
    const auto lower = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(lower_block.data(), lower_block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(lower.data(), lower.size())) ==
                http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error() == http2_error_code::protocol_error);
}

// A client can cancel while a multi-frame response head is incomplete. The partial
// compressed block must move out of the stream before removal, then CONTINUATION
// completes silently and updates HPACK for later responses.

RUVIA_TEST(http2_connection_client_reset_detaches_partial_response_head) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    client.begin_connection();
    client.consume_output(client.pending_output().size());
    char settings[9];
    ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::detail::http2_feed_result::accepted);
    client.consume_output(client.pending_output().size());

    const auto first_head = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(first_head.submitted() != nullptr);
    const auto first_stream = submitted_request_stream_id(first_head);
    RUVIA_CHECK_EQ(first_stream, static_cast<std::uint32_t>(1));
    client.consume_output(client.pending_output().size());

    std::pmr::string response_block(&resource);
    hpack_encoder::encode_header(response_block, ":status", "200");
    const auto status_bytes = response_block.size();
    encode_short_dynamic_header(response_block, "x-response", "indexed");
    const auto first = headers_frame(
        &resource, first_stream, 0, std::string_view(response_block.data(), status_bytes));
    RUVIA_CHECK(client.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(client.header_block_in_progress());

    RUVIA_CHECK(
        client.submit_reset(first_stream, http2_error_code::cancel) == http2_submit_status::accepted);
    RUVIA_CHECK(client.stream(first_stream) == nullptr);
    client.consume_output(client.pending_output().size());

    const auto continuation = continuation_frame(&resource, first_stream,
        ruvia::detail::http2_flag_end_headers,
        std::string_view(response_block.data() + status_bytes, response_block.size() - status_bytes));
    RUVIA_CHECK(client.feed(std::string_view(continuation.data(), continuation.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!client.connection_error().has_value());
    RUVIA_CHECK(client.pending_output().empty());  // no second RST
    RUVIA_CHECK(!client.next_event().has_value());

    const auto next_head = client.submit_regular_request_head(
        "GET", "https", "example.test", "/next", {}, http2_request_content::none());
    RUVIA_CHECK(next_head.submitted() != nullptr);
    const auto next_stream = submitted_request_stream_id(next_head);
    RUVIA_CHECK_EQ(next_stream, static_cast<std::uint32_t>(3));
    client.consume_output(client.pending_output().size());

    std::pmr::string next_response(&resource);
    hpack_encoder::encode_header(next_response, ":status", "200");
    hpack_encoder::encode_indexed(next_response, 62);  // x-response: indexed
    const auto final = headers_frame(&resource, next_stream,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(next_response.data(), next_response.size()));
    RUVIA_CHECK(client.feed(std::string_view(final.data(), final.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.connection_error().has_value());
}

// submit_reset emits a RST_STREAM and marks the stream reset so no further response
// bytes are produced for it.

RUVIA_TEST(http2_connection_submit_reset_emits_rst) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    const auto out = conn.pending_output();
    const auto r = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(r.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(r.stream_id_, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_local_reset_unknown_and_repeat_emit_no_illegal_frame) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    RUVIA_CHECK(conn.submit_reset(99, http2_error_code::cancel) == http2_submit_status::invalid_state);
    RUVIA_CHECK(conn.pending_output().empty());

    drive_get_request(conn, &resource);
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    const auto first_reset_bytes = conn.pending_output().size();
    RUVIA_CHECK_EQ(first_reset_bytes, static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));

    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::closed);
    RUVIA_CHECK_EQ(conn.pending_output().size(), first_reset_bytes);

    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    RUVIA_CHECK(client.submit_reset(1, http2_error_code::cancel) == http2_submit_status::invalid_state);
    RUVIA_CHECK(client.pending_output().empty());
}

// A pinned stream (handler in flight) is NOT freed by a peer RST_STREAM: it stays in
// the table (so the handler's request views survive) but is marked reset, and
// stream_closed is emitted so the owner can drop the response. unpin then frees it.

RUVIA_TEST(http2_connection_pinned_stream_survives_peer_reset) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);  // stream 1 created + decoded
    RUVIA_CHECK(conn.stream(1) != nullptr);

    conn.pin_stream(1);

    char rst[9 + 4];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);     // kept alive because pinned
    RUVIA_CHECK(s->is_aborted());  // retained storage, but protocol ownership ended
    bool saw_closed = false;
    while (const auto event = conn.next_event()) {
        if (const auto* closed = event->stream_closed();
            closed != nullptr && closed->stream_id() == 1) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::http2_stream_close_source::peer);
            RUVIA_CHECK(closed->error() == http2_error_code::cancel);
        }
    }
    RUVIA_CHECK(saw_closed);

    conn.unpin_stream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);  // freed once the handler finished
}

// Unpinning a stream that completed normally on both halves frees it without a reset.

RUVIA_TEST(http2_connection_unpin_frees_completed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.pin_stream(1);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::no_content);
    const auto head_result = submit_buffered_response_head(conn, 1, response);
    RUVIA_CHECK(response_head_submitted(head_result));
    conn.consume_output(conn.pending_output().size());
    conn.unpin_stream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(conn.pending_output().empty());
}

// Dropping the last owner while the local response is still open must produce an
// explicit terminal transition, rather than silently erasing a peer-visible stream.

RUVIA_TEST(http2_connection_unpin_incomplete_stream_emits_cancel_reset) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.pin_stream(1);

    conn.unpin_stream(1);

    RUVIA_CHECK(conn.stream(1) == nullptr);
    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, static_cast<std::uint32_t>(1));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::cancel));
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_connection_unpin_keeps_pin_when_owner_reset_allocation_fails) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.pin_stream(1);

    const std::string large_hint(4096, 'x');
    const std::array<ruvia::http_header_view, 1> headers{ruvia::http_header_view{"Link", large_hint}};
    const ruvia::http_interim_response_head early_hints(ruvia::http_status::early_hints, headers);
    RUVIA_CHECK(conn.submit_interim_response_head(1, early_hints) == http2_submit_status::accepted);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        conn.unpin_stream(1);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    RUVIA_CHECK(!conn.stream(1)->is_aborted());

    resource.reject_allocations(false);
    char rst[9 + 4];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
    RUVIA_CHECK(
        conn.feed(std::string_view(rst, sizeof(rst))) == ruvia::detail::http2_feed_result::accepted);

    auto* retained = conn.stream(1);
    RUVIA_CHECK(retained != nullptr);
    if (retained != nullptr) {
        RUVIA_CHECK(retained->is_aborted());
    }
    conn.unpin_stream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}
#endif  // !_MSC_VER

// RFC 9110 Section 15.3.6 requires every 205 response to have zero-length
// content. Unlike HEAD/204/304, 205 still has an ordinary content phase, but a
// peer cannot use that phase to transfer any non-empty DATA. HTTP/2 can reject
// this without losing connection synchronization because the failure is scoped
// to the response stream.

RUVIA_TEST(http2_connection_client_rejects_reset_content_payload) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "205");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());

    const auto forbidden = data_frame(&resource, stream_id, ruvia::detail::http2_flag_end_stream, "x");
    RUVIA_CHECK(client.feed(std::string_view(forbidden.data(), forbidden.size())) ==
                http2_feed_result::accepted);

    bool saw_closed = false;
    while (const auto event = client.next_event()) {
        RUVIA_CHECK(event->message_body_chunk() == nullptr);
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
        }
    }
    RUVIA_CHECK(saw_closed);
    RUVIA_CHECK(!client.connection_error().has_value());
    const auto reset = client.pending_output();
    RUVIA_CHECK_EQ(reset.size(), static_cast<std::size_t>(13));
    const auto reset_head = ruvia::detail::http2_parse_frame_header(reset.substr(0, 9));
    RUVIA_CHECK_EQ(reset_head.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
    client.unpin_stream(stream_id);
}

RUVIA_TEST(http2_connection_client_rejects_nonzero_reset_content_length) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "205");
    hpack_encoder::encode_header(response, "content-length", "1");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);

    bool saw_closed = false;
    while (const auto event = client.next_event()) {
        RUVIA_CHECK(event->message_head() == nullptr);
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
        }
    }
    RUVIA_CHECK(saw_closed);
    RUVIA_CHECK(!client.connection_error().has_value());
    const auto reset = client.pending_output();
    RUVIA_CHECK_EQ(reset.size(), static_cast<std::size_t>(13));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
    client.unpin_stream(stream_id);
}

RUVIA_TEST(http2_connection_client_accepts_empty_reset_content_terminal) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "205");
    hpack_encoder::encode_header(response, "content-length", "0");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());
    const auto* known = client.stream(stream_id)->remote_content().allowed_known_length();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declared_length(), std::size_t{0});

    const auto terminal = data_frame(&resource, stream_id, ruvia::detail::http2_flag_end_stream, {});
    RUVIA_CHECK(client.feed(std::string_view(terminal.data(), terminal.size())) ==
                http2_feed_result::accepted);
    const auto end = client.next_event();
    RUVIA_CHECK(end.has_value());
    RUVIA_CHECK(end->kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(!client.connection_error().has_value());
    client.unpin_stream(stream_id);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
}

// Client role protocol errors: HEADERS on an odd stream never opened is a connection
// error, and HEADERS on an even (server-initiated) stream is one too (push disabled).

RUVIA_TEST(http2_connection_client_role_rejects_unexpected_streams) {
    std::pmr::monotonic_buffer_resource resource;
    {
        http2_connection client(&resource, http2_role::client);
        client.begin_connection();
        client.consume_output(client.pending_output().size());
        char settings[9];
        ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        hpack_encoder::encode_header(head, ":status", "200");
        const auto idle = headers_frame(&resource, 5, ruvia::detail::http2_flag_end_headers,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(idle.data(), idle.size()));
        RUVIA_CHECK(client.connection_error().has_value());  // HEADERS on idle stream -> GOAWAY
    }
    {
        http2_connection client(&resource, http2_role::client);
        client.begin_connection();
        client.consume_output(client.pending_output().size());
        char settings[9];
        ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        hpack_encoder::encode_header(head, ":status", "200");
        const auto even = headers_frame(&resource, 2, ruvia::detail::http2_flag_end_headers,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(even.data(), even.size()));
        RUVIA_CHECK(client.connection_error().has_value());  // no push: even ids are never valid
    }
}

RUVIA_TEST(http2_connection_repeated_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_peer_reset(conn, &resource, pinned);

        // The second reset is ordered after the reset that this peer already
        // sent. Unlike a reset racing with one sent by us, it cannot predate
        // the peer's knowledge that the stream is closed.
        char rst[9 + 4];
        ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
        ruvia::detail::http2_write32(rst + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
        RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) ==
                    ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::stream_closed);
        RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
            static_cast<std::uint32_t>(http2_error_code::stream_closed));

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}

RUVIA_TEST(http2_connection_racing_reset_does_not_spend_rapid_reset_budget) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    open_then_local_reset(conn, &resource);

    // A reset that raced with the reset sent by this endpoint closes no live
    // stream and spawned no new handler. It is required minimal processing,
    // not one unit of the rapid-reset lifecycle budget.
    char rst[9 + 4];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
    RUVIA_CHECK(
        conn.feed(std::string_view(rst, sizeof(rst))) == ruvia::detail::http2_feed_result::accepted);

    std::pmr::string block(&resource);
    encode_get_request(block);
    for (std::uint32_t sid = 3; sid < 3U + 2U * 1000U; sid += 2) {
        const auto head = headers_frame(&resource, sid,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
                    ruvia::detail::http2_feed_result::accepted);
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());

        ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, sid);
        RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) ==
                    ruvia::detail::http2_feed_result::accepted);
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());
    }
    RUVIA_CHECK(!conn.connection_error().has_value());
}

RUVIA_TEST(http2_connection_malformed_priority_after_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_peer_reset(conn, &resource, pinned);

        // PRIORITY itself is legal in every stream state, but its payload must
        // contain exactly five bytes. The resulting FRAME_SIZE_ERROR cannot be
        // reported with RST_STREAM after this peer-originated reset.
        char priority[9 + 4]{};
        ruvia::detail::http2_encode_frame_header(priority, 4, http2_frame_type::priority, 0, 1);
        RUVIA_CHECK(conn.feed(std::string_view(priority, sizeof(priority))) ==
                    ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::frame_size_error);
        RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
            static_cast<std::uint32_t>(http2_error_code::frame_size_error));

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}

RUVIA_TEST(http2_connection_malformed_priority_without_active_stream_is_connection_error) {
    for (const bool closed : {false, true}) {
        for (const bool pinned : {false, true}) {
            if (!closed && pinned) {
                continue;
            }
            std::pmr::monotonic_buffer_resource resource;
            http2_connection conn(&resource);
            handshake(conn);
            if (closed) {
                open_then_local_reset(conn, &resource, pinned);
            }

            // A malformed PRIORITY requires a stream FRAME_SIZE_ERROR, but
            // emitting RST_STREAM is itself forbidden on idle and closed streams.
            // Promoting the error to the connection is the only legal report.
            char priority[9 + 4]{};
            ruvia::detail::http2_encode_frame_header(priority, 4, http2_frame_type::priority, 0, 1);
            RUVIA_CHECK(conn.feed(std::string_view(priority, sizeof(priority))) ==
                        ruvia::detail::http2_feed_result::protocol_failure);
            RUVIA_CHECK(conn.connection_error() == http2_error_code::frame_size_error);
            RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
                static_cast<std::uint32_t>(http2_error_code::frame_size_error));

            if (pinned) {
                conn.unpin_stream(1);
            }
        }
    }
}

// A rapid-reset flood (open a stream, RST it, repeat -- never letting a response finish)
// is cut off with GOAWAY(ENHANCE_YOUR_CALM); the 128-stream cap alone never trips because
// each RST immediately frees the slot (CVE-2023-44487).

RUVIA_TEST(http2_connection_rapid_reset_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);

    bool tripped = false;
    for (std::uint32_t sid = 1; sid < 1U + 2U * 1200U; sid += 2) {
        const auto h = headers_frame(&resource, sid,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        if (conn.feed(std::string_view(h.data(), h.size())) ==
            ruvia::detail::http2_feed_result::protocol_failure) {
            tripped = true;
            break;
        }
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());

        char rst[9 + 4];
        ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, sid);
        ruvia::detail::http2_write32(rst + 9, 0);
        if (conn.feed(std::string_view(rst, sizeof(rst))) ==
            ruvia::detail::http2_feed_result::protocol_failure) {
            tripped = true;
            break;  // leave the GOAWAY in the outbound buffer for inspection
        }
        while (conn.next_event().has_value()) {
        }
        conn.consume_output(conn.pending_output().size());
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connection_error().has_value());
    RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()), enhance_your_calm);
}
