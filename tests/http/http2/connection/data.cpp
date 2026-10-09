#include <array>

#include "ruvia/http/http_response_stream.h"

#include "http2/http2_websocket_handshake.h"
#include "http2_connection_fixture.h"

// http2_connection: inbound and outbound DATA.

// A DATA frame after the head yields a message_body_chunk carrying the bytes, then
// (on END_STREAM) message_end. WINDOW_UPDATE is withheld until the event owner
// explicitly confirms that the borrowed bytes were consumed.

RUVIA_TEST(http2_connection_feed_data_emits_body_chunk_and_end) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto h = post_head_frame(&resource, "");
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    const char body[5] = {'h', 'e', 'l', 'l', 'o'};
    const auto d =
        data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    const auto chunk = conn.next_event().value();
    RUVIA_CHECK(chunk.kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK_EQ(chunk.message_body_chunk()->stream_id(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(chunk.message_body_chunk()->bytes() == std::string_view(body, 5));
    const auto end = conn.next_event().value();
    RUVIA_CHECK(end.kind() == http2_event_kind::message_end);
    RUVIA_CHECK_EQ(end.message_end()->stream_id(), static_cast<std::uint32_t>(1));

    RUVIA_CHECK(conn.pending_output().empty());
    conn.release_all_received_data(1);
    // END_STREAM leaves no live stream scope. The consumed connection credit is
    // retained below the batching threshold without manufacturing output.
    RUVIA_CHECK(conn.pending_output().empty());
}

// A DATA frame is protocol progress even when its Data field is empty, but it is not
// an application body chunk. Padding still consumes both flow-control windows and
// queues batched credit, while END_STREAM still emits the terminal message event.

RUVIA_TEST(http2_connection_empty_data_never_emits_body_chunk) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto empty = data_frame(&resource, 1, 0, {});
    RUVIA_CHECK(
        conn.feed(std::string_view(empty.data(), empty.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.pending_output().empty());

    const std::string_view padding_only("\x02\0\0", 3);
    const auto padded = data_frame(&resource, 1, ruvia::detail::http2_flag_padded, padding_only);
    RUVIA_CHECK(
        conn.feed(std::string_view(padded.data(), padded.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.next_event().has_value());

    RUVIA_CHECK(conn.pending_output().empty());

    const auto terminal = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, {});
    RUVIA_CHECK(conn.feed(std::string_view(terminal.data(), terminal.size())) ==
                http2_feed_result::accepted);
    const auto end = conn.next_event();
    RUVIA_CHECK(end.has_value());
    if (end.has_value()) {
        RUVIA_CHECK(end->kind() == http2_event_kind::message_end);
    }
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_padding_only_data_does_not_amplify_output) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }

    const std::string_view padding_only("\x02\0\0", 3);
    const auto padded = data_frame(&resource, 1, ruvia::detail::http2_flag_padded, padding_only);
    for (int i = 0; i < 1200; ++i) {
        RUVIA_CHECK(conn.feed(std::string_view(padded.data(), padded.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!conn.next_event().has_value());
    }
    // 3,600 consumed octets are far below half the advertised 1 MiB window.
    // Per-frame updates would turn 15 KiB of input framing into 31.2 KiB of
    // unread output; credit must remain pending instead.
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.connection_error().has_value());
}

RUVIA_TEST(http2_connection_same_feed_data_credit_queues_owner_batch) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    const auto first = data_frame(&resource, 1, 0, "one");
    const auto second = data_frame(&resource, 1, 0, "two");
    std::pmr::string batch(&resource);
    batch.append(head.data(), head.size());
    batch.append(first.data(), first.size());
    batch.append(second.data(), second.size());

    RUVIA_CHECK(conn.feed(std::string_view(batch.data(), batch.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    const auto first_chunk = conn.next_event().value();
    RUVIA_CHECK_EQ(first_chunk.message_body_chunk()->bytes(), std::string_view("one"));
    const auto second_chunk = conn.next_event().value();
    RUVIA_CHECK_EQ(second_chunk.message_body_chunk()->bytes(), std::string_view("two"));
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.pending_output().empty());

    // One owner acknowledgement releases the complete copied event batch and
    // queues its six flow-controlled octets once in each live receive scope.
    conn.release_all_received_data(1);
    RUVIA_CHECK(conn.pending_output().empty());
}

// A DATA/END_STREAM that falls short of a declared content-length is a protocol error:
// the core RST_STREAMs the stream and does NOT emit message_end.

RUVIA_TEST(http2_connection_feed_data_short_of_content_length_resets) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto h = post_head_frame(&resource, "10");  // promises 10 bytes
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);

    const char body[5] = {'s', 'h', 'o', 'r', 't'};  // only 5, with END_STREAM
    const auto d =
        data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
    // The length mismatch aborts the stream: stream_closed (never message_end), and
    // the (unpinned) stream is removed from the table.
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// submit_response_head emits a HEADERS block (END_HEADERS, no END_STREAM when a body
// follows); submit_data then sends the buffered body as a terminal DATA frame.

RUVIA_TEST(http2_connection_submit_response_head_and_body) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response resp({.resource_ = &resource});
    resp.status(ruvia::http_status::ok);
    resp.body("hello");
    const auto head_result = submit_buffered_response_head(conn, 1, resp);
    RUVIA_CHECK(response_head_submitted(head_result));

    const auto head = conn.pending_output();
    const auto hd = ruvia::detail::http2_parse_frame_header(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((hd.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
    RUVIA_CHECK((hd.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(head.size());

    const auto r = conn.submit_data(1, "hello", http2_end_stream::end_stream);
    RUVIA_CHECK(r == http2_data_submit_status::accepted);
    const auto body = conn.pending_output();
    const auto dd = ruvia::detail::http2_parse_frame_header(body.substr(0, 9));
    RUVIA_CHECK_EQ(dd.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(dd.length_, static_cast<std::uint32_t>(5));
    RUVIA_CHECK((dd.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
}

RUVIA_TEST(http2_connection_rejects_every_data_submission_after_local_end_stream) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());

    std::uint64_t state_value = 0x4ae7'196d'25f0'83bcULL;
    const auto next_value = [&state_value] {
        state_value ^= state_value << 13U;
        state_value ^= state_value >> 7U;
        state_value ^= state_value << 17U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 64; ++sample) {
        const auto body_bytes = static_cast<std::size_t>(next_value() % 9U);
        std::string body;
        for (std::size_t i = 0; i < body_bytes; ++i) {
            body.push_back(static_cast<char>('a' + (next_value() % 26U)));
        }
        const auto end_stream = (next_value() & 1U) != 0 || sample == 63
                                    ? http2_end_stream::end_stream
                                    : http2_end_stream::keep_open;
        const auto status = conn.submit_data(1, body, end_stream);
        RUVIA_CHECK(status == http2_data_submit_status::accepted);
        if (end_stream == http2_end_stream::end_stream) {
            break;
        }
        conn.consume_output(conn.pending_output().size());
    }

    const auto after_end_output = conn.pending_output();
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
    }
    for (const std::string_view body : {std::string_view{}, std::string_view("x"),
             std::string_view("second body")}) {
        RUVIA_CHECK(conn.submit_data(1, body, http2_end_stream::keep_open) ==
                    http2_data_submit_status::invalid_state);
        RUVIA_CHECK_EQ(conn.pending_output(), after_end_output);
        RUVIA_CHECK(conn.submit_data(1, body, http2_end_stream::end_stream) ==
                    http2_data_submit_status::invalid_state);
        RUVIA_CHECK_EQ(conn.pending_output(), after_end_output);
    }
}

RUVIA_TEST(http2_connection_rejects_data_before_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    const auto before = conn.pending_output().size();
    RUVIA_CHECK(conn.submit_data(1, "body", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK_EQ(conn.pending_output().size(), before);
}

// HEAD carries the representation metadata (including Content-Length) but the
// protocol core terminates the stream on HEADERS and tells the runtime not to
// submit DATA, even when the application response contains bytes.

RUVIA_TEST(http2_connection_head_buffered_response_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_request(conn, &resource, "HEAD");

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("hello");
    const auto head_result = submit_buffered_response_head(conn, 1, response);

    RUVIA_CHECK(response_head_submitted(head_result));
    RUVIA_CHECK(submitted_response_plan(head_result).body_suppressed());
    RUVIA_CHECK(!submitted_response_plan(head_result).send_body());
    RUVIA_CHECK_EQ(
        submitted_response_plan(head_result).content_length(), static_cast<std::uint64_t>(5));
    const auto head = conn.pending_output();
    const auto frame = ruvia::detail::http2_parse_frame_header(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    const auto before_rejected_data = conn.pending_output().size();
    RUVIA_CHECK(conn.submit_data(1, "forbidden", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK_EQ(conn.pending_output().size(), before_rejected_data);
}

// 205 is not a normal zero-byte response whose body phase may remain open: RFC
// 9110 §15.3.6 forbids content. The shared response plan therefore terminates the
// HTTP/2 stream on HEADERS even when the application attached buffered bytes.

RUVIA_TEST(http2_connection_reset_content_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::reset_content);
    response.body("must-not-be-sent");
    response.header("Content-Length", "16");
    const auto head_result = submit_buffered_response_head(conn, 1, response);

    RUVIA_CHECK(response_head_submitted(head_result));
    RUVIA_CHECK(!submitted_response_plan(head_result).status_allows_body());
    RUVIA_CHECK(submitted_response_plan(head_result).body_suppressed());
    RUVIA_CHECK(!submitted_response_plan(head_result).send_body());
    RUVIA_CHECK_EQ(
        submitted_response_plan(head_result).content_length(), static_cast<std::uint64_t>(0));
    const auto head = conn.pending_output();
    const auto frame = ruvia::detail::http2_parse_frame_header(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    const auto before_rejected_data = conn.pending_output().size();
    RUVIA_CHECK(conn.submit_data(1, "forbidden", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK_EQ(conn.pending_output().size(), before_rejected_data);
}

RUVIA_TEST(http2_connection_short_finish_does_not_mutate_queued_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 3);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "8");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(
        conn.submit_data(1, "12345", http2_end_stream::keep_open) == http2_data_submit_status::queued);
    conn.consume_output(conn.pending_output().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{3});
    RUVIA_CHECK(conn.finish_response(1, validated_trailers({})) ==
                http2_finish_submit_status::content_length_incomplete);
    RUVIA_CHECK(stream->local_send().response_content_open() != nullptr);
    RUVIA_CHECK(conn.has_queued_data(1));
    RUVIA_CHECK(conn.data_queue_state(1) == ruvia::http2_data_queue_state::queued);
    RUVIA_CHECK(conn.pending_output().empty());

    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 1, 10);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    const auto drained_output = conn.pending_output();
    const auto drained_frame = ruvia::detail::http2_parse_frame_header(drained_output.substr(0, 9));
    RUVIA_CHECK_EQ(drained_frame.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK((drained_frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(drained_output.size());
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5});
    RUVIA_CHECK(conn.data_queue_state(1) == ruvia::http2_data_queue_state::drained);

    RUVIA_CHECK(
        conn.submit_data(1, "678", http2_end_stream::end_stream) == http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{8});
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
}

RUVIA_TEST(http2_connection_reset_aborts_queued_data_state) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 0);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "5");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(std::uint32_t{1},
        std::move(response), ruvia::http_response_stream_kind::generic,
        http_response_trailer_intent::none)));
    RUVIA_CHECK(conn.submit_data(1, "reset", http2_end_stream::keep_open) ==
                http2_data_submit_status::queued);
    RUVIA_CHECK(conn.data_queue_state(1) == ruvia::http2_data_queue_state::queued);
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    RUVIA_CHECK(conn.data_queue_state(1) == ruvia::http2_data_queue_state::aborted);
    RUVIA_CHECK(!conn.has_queued_data(1));
}

// RFC 8441 Extended CONNECT: a CONNECT + :protocol=websocket head emits message_head
// with NO message_end (the tunnel stays open), and the stream carries the
// extended_connect_websocket mark for the owner's route policy. The handshake atomically
// opens the tunnel, tunnel DATA flows as tunnel_data events (no content-length
// required), and submit_data carries frames
// back on the still-open stream.

RUVIA_TEST(http2_connection_websocket_tunnel_handshake_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "websocket");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", "/ws");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    const auto h = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));

    bool saw_headers = false;
    bool saw_end = false;
    while (const auto event = conn.next_event()) {
        if (const auto* head = event->message_head(); head != nullptr && head->stream_id() == 1) {
            saw_headers = true;
        }
        if (event->message_end() != nullptr) {
            saw_end = true;
        }
    }
    RUVIA_CHECK(saw_headers);
    RUVIA_CHECK(!saw_end);  // the tunnel must stay open

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == http2_connect_form::extended);
    RUVIA_CHECK(stream->protocol_is_websocket());

    // Owner route policy admitted a websocket route: answer 200 and open the tunnel.
    conn.consume_output(conn.pending_output().size());
    ruvia::detail::http1_server_request_parser negotiation_parser;
    const auto negotiation_request = negotiation_parser.parse_message(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "\r\n");
    const auto validation = ruvia::detail::validate_http2_websocket_handshake(
        *stream, negotiation_request.request_);
    const auto handshake_result =
        conn.submit_websocket_handshake(1, negotiation_request.request_, validation);
    RUVIA_CHECK(handshake_result.submitted() != nullptr);
    RUVIA_CHECK(handshake_result.failure() == nullptr);
    RUVIA_CHECK(handshake_result.submitted()->subprotocol().empty());

    const auto duplicate_handshake_result =
        conn.submit_websocket_handshake(1, negotiation_request.request_, validation);
    RUVIA_CHECK(duplicate_handshake_result.submitted() == nullptr);
    RUVIA_CHECK(duplicate_handshake_result.failure() != nullptr);
    RUVIA_CHECK(duplicate_handshake_result.failure()->error() ==
                ruvia::detail::http2_websocket_handshake_submit_error::invalid_state);

    const auto out = conn.pending_output();
    RUVIA_CHECK(out.size() > 9);
    const auto head = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(head.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK_EQ(head.stream_id_, static_cast<std::uint32_t>(1));
    RUVIA_CHECK((head.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
    RUVIA_CHECK((head.flags_ & ruvia::detail::http2_flag_end_stream) == 0);  // stream open
    conn.consume_output(out.size());

    // Inbound tunnel bytes (a would-be masked frame) surface as tunnel DATA even with
    // no content-length: the tunnel is exempt from body accounting.
    char data[9 + 4];
    ruvia::detail::http2_encode_frame_header(data, 4, http2_frame_type::data, 0, 1);
    std::memcpy(data + 9, "\x81\x80\x01\x02", 4);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    bool saw_chunk = false;
    while (const auto event = conn.next_event()) {
        if (const auto* tunnel_data = event->tunnel_data(); tunnel_data != nullptr &&
                                                            tunnel_data->stream_id() == 1 &&
                                                            tunnel_data->bytes().size() == 4) {
            saw_chunk = true;
        }
    }
    RUVIA_CHECK(saw_chunk);

    // Outbound tunnel frames ride submit_data on the still-open stream.
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(conn.submit_data(1, "\x81\x02hi", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    const auto frame_out = conn.pending_output();
    const auto data_head = ruvia::detail::http2_parse_frame_header(frame_out.substr(0, 9));
    RUVIA_CHECK_EQ(data_head.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK((data_head.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    RUVIA_CHECK(frame_out.substr(9) == std::string_view("\x81\x02hi"));
}

RUVIA_TEST(http2_connection_rejects_half_closed_websocket_opening_handshake) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "websocket");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", "/ws");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    const auto h = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())) == http2_feed_result::accepted);

    bool saw_headers = false;
    while (const auto event = conn.next_event()) {
        if (const auto* head = event->message_head(); head != nullptr && head->stream_id() == 1) {
            saw_headers = true;
        }
        RUVIA_CHECK(event->tunnel_end() == nullptr);
    }
    RUVIA_CHECK(saw_headers);

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remote_receive().connect_pending_end_stream() != nullptr);

    ruvia::detail::http1_server_request_parser negotiation_parser;
    const auto negotiation_request = negotiation_parser.parse_message(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");
    const auto validation = ruvia::detail::validate_http2_websocket_handshake(
        *stream, negotiation_request.request_);
    const auto handshake_result =
        conn.submit_websocket_handshake(1, negotiation_request.request_, validation);
    RUVIA_CHECK(handshake_result.submitted() == nullptr);
    RUVIA_CHECK(handshake_result.failure() != nullptr);
    if (const auto* failure = handshake_result.failure()) {
        RUVIA_CHECK(
            failure->error() == ruvia::detail::http2_websocket_handshake_submit_error::invalid_state);
    }
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
}

// RFC 9110 Section 6.4.1 defines HEAD/204/304 responses as having no message
// content. RFC 9113 Section 8.1.1 therefore makes a non-empty DATA payload a
// malformed response and requires PROTOCOL_ERROR on that stream. This is distinct
// from Content-Length representation metadata, which remains legal for HEAD/204/304.

RUVIA_TEST(http2_connection_client_rejects_data_for_responses_without_content) {
    struct case_value final {
        std::string_view method_;
        std::string_view status_;
    };
    constexpr std::array cases{case_value{"HEAD", "200"}, case_value{"GET", "204"}, case_value{"GET", "304"}};

    for (const auto& test_case : cases) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        handshake(client);

        const auto request = client.submit_regular_request_head(
            test_case.method_, "https", "example.test", "/", {}, http2_request_content::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto stream_id = submitted_request_stream_id(request);
        client.pin_stream(stream_id);
        client.consume_output(client.pending_output().size());

        std::pmr::string response(&resource);
        hpack_encoder::encode_header(response, ":status", test_case.status_);
        const auto response_head =
            headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(!client.next_event().has_value());
        const auto* live = client.stream(stream_id);
        RUVIA_CHECK(live != nullptr);
        RUVIA_CHECK(live->remote_content().metadata_only_without_length() != nullptr);

        const auto forbidden =
            data_frame(&resource, stream_id, ruvia::detail::http2_flag_end_stream, "x");
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
        const auto reset_head = ruvia::detail::http2_parse_frame_header(reset.substr(0, 9));
        RUVIA_CHECK_EQ(reset_head.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
        client.unpin_stream(stream_id);
    }
}

RUVIA_TEST(http2_connection_client_allows_empty_terminal_data_without_content_event) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "HEAD", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    hpack_encoder::encode_header(response, "content-length", "12");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    const auto* metadata = client.stream(stream_id)->remote_content().metadata_only_known_length();
    RUVIA_CHECK(metadata != nullptr);
    RUVIA_CHECK_EQ(metadata->declared_length(), std::size_t{12});

    const auto terminal = data_frame(&resource, stream_id, ruvia::detail::http2_flag_end_stream, {});
    RUVIA_CHECK(client.feed(std::string_view(terminal.data(), terminal.size())) ==
                http2_feed_result::accepted);
    const auto end = client.next_event();
    RUVIA_CHECK(end.has_value());
    RUVIA_CHECK(end->kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(stream_id)->remote_receive().end_stream() != nullptr);

    client.unpin_stream(stream_id);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
}

RUVIA_TEST(http2_connection_discarded_data_batches_full_payload_credit_once) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    conn.consume_output(conn.pending_output().size());

    // The flow-controlled length is 6: one Pad Length byte, two data bytes, and
    // three padding bytes. A closed stream has no stream window to restore, but all
    // six octets must join the connection's batch exactly once.
    constexpr char padded_payload[] = {3, 'o', 'k', 0, 0, 0};
    const auto discarded = data_frame(&resource, 1, ruvia::detail::http2_flag_padded,
        std::string_view(padded_payload, sizeof(padded_payload)));
    RUVIA_CHECK(conn.feed(std::string_view(discarded.data(), discarded.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());

    constexpr std::uint32_t threshold = ruvia::detail::http2_receive_window_update_threshold;
    std::string chunk(http2_local_settings::max_frame_size, 'x');
    std::uint32_t remaining = threshold - static_cast<std::uint32_t>(sizeof(padded_payload));
    while (remaining != 0) {
        const auto chunk_bytes =
            static_cast<std::size_t>(remaining < chunk.size() ? remaining : chunk.size());
        const auto data = data_frame(&resource, 1, 0, std::string_view(chunk.data(), chunk_bytes));
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == http2_feed_result::accepted);
        remaining -= static_cast<std::uint32_t>(chunk_bytes);
        if (remaining != 0) {
            RUVIA_CHECK(conn.pending_output().empty());
        }
    }

    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(out.size(), std::size_t{ruvia::detail::http2_window_update_frame_bytes});
    const auto update = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(update.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(update.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::http2_window_update_increment(out.substr(9, 4)), threshold);
}

RUVIA_TEST(http2_connection_data_after_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_peer_reset(conn, &resource, pinned);

        // The peer's RST_STREAM precedes this DATA on the same ordered byte
        // stream. It cannot be an in-flight race with a reset sent by us, and
        // replying with another stream frame would itself violate RFC 9113 6.4.
        const auto data = data_frame(&resource, 1, 0, {});
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::stream_closed);
        RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()),
            static_cast<std::uint32_t>(http2_error_code::stream_closed));

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}

RUVIA_TEST(http2_connection_data_after_local_reset_is_discarded_without_stream_output) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        open_then_local_reset(conn, &resource, pinned);

        // DATA might have been in flight when this endpoint sent RST_STREAM.
        // Minimal processing consumes connection flow control but cannot emit a
        // second stream frame after the stream entered the closed state.
        const auto data = data_frame(&resource, 1, 0, {});
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::http2_feed_result::accepted);
        RUVIA_CHECK(!conn.connection_error().has_value());
        RUVIA_CHECK(conn.pending_output().empty());

        if (pinned) {
            conn.unpin_stream(1);
        }
    }
}

RUVIA_TEST(http2_connection_closed_stream_data_flood_never_amplifies_output) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    open_then_local_reset(conn, &resource);

    // Empty DATA isolates the closed-state decision from flow-control credit.
    // Even without draining output, discarded frames must not manufacture an
    // unbounded queue of illegal second RST_STREAM frames.
    const auto data = data_frame(&resource, 1, 0, {});
    bool accepted_all = true;
    for (int i = 0; i < 1200; ++i) {
        if (conn.feed(std::string_view(data.data(), data.size())) !=
            ruvia::detail::http2_feed_result::accepted) {
            accepted_all = false;
            break;
        }
    }
    RUVIA_CHECK(accepted_all);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
}

RUVIA_TEST(http2_connection_closed_data_credit_does_not_amplify_output) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    open_then_local_reset(conn, &resource);

    const auto data = data_frame(&resource, 1, 0, "x");
    for (int i = 0; i < 1200; ++i) {
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::http2_feed_result::accepted);
    }
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.pending_output().empty());
}
