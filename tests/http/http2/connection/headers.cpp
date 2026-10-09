#include <string_view>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response_stream.h"

#include "http2/http2_request_builder.h"
#include "http2_connection_fixture.h"
#include "request/http_request_access.h"

// http2_connection: HEADERS, CONTINUATION, HPACK and trailers.

RUVIA_TEST(http2_connection_header_table_reduction_prefixes_next_field_block) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_peer_input(client);

    // RFC 9113 §4.3.1: after acknowledging a reduction of the peer's HPACK
    // dynamic-table maximum, the next field block must begin with a conformant
    // Dynamic Table Size Update. The stateless Ruvia encoder uses no dynamic
    // entries, so it can truthfully select zero (encoded as the single byte 0x20).
    char settings[15];
    auto* out = ruvia::detail::http2_write_frame_header(settings, 6, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::header_table_size, 0);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));
    RUVIA_CHECK(
        client.feed(std::string_view(settings, sizeof(settings))) == http2_feed_result::accepted);

    const auto ack = client.pending_output();
    RUVIA_CHECK_EQ(ack.size(), std::size_t{9});
    RUVIA_CHECK(
        (ruvia::detail::http2_parse_frame_header(ack).flags_ & ruvia::detail::http2_flag_ack) != 0);
    client.consume_output(ack.size());

    const auto submitted = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto bytes_value = client.pending_output();
    const auto headers = ruvia::detail::http2_parse_frame_header(bytes_value.substr(0, 9));
    RUVIA_CHECK_EQ(headers.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK(headers.length_ > 0);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(bytes_value[9]), static_cast<unsigned char>(0x20));
}

RUVIA_TEST(http2_connection_rejects_invalid_hpack_size_update_sequences) {
    // RFC 7541 section 4.2 permits at most two table-size updates at the start
    // of a field block and requires the smallest value before the final value.
    // Either violation makes the field block undecodable, which RFC 9113
    // section 4.3 maps to a connection-level COMPRESSION_ERROR.
    constexpr std::string_view invalid_prefixes[] = {
        std::string_view("\x20\x21\x22", 3),  // three updates
        std::string_view("\x2a\x25", 2),      // 10 then 5: smallest is last
    };

    for (const auto prefix : invalid_prefixes) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(prefix, &resource);
        encode_get_request(block);
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error() == http2_error_code::compression_error);

        const auto output = conn.pending_output();
        RUVIA_CHECK(output.size() >= 17);
        if (output.size() >= 17) {
            const auto goaway = ruvia::detail::http2_parse_frame_header(output.substr(0, 9));
            RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
            RUVIA_CHECK_EQ(ruvia::detail::http2_read32(
                               reinterpret_cast<const unsigned char*>(output.data() + 13)),
                static_cast<std::uint32_t>(http2_error_code::compression_error));
        }
    }
}

// RFC 9113 §4.2/§6.2 gives malformed HEADERS payloads two distinct connection
// errors: missing mandatory PRIORITY fields are FRAME_SIZE_ERROR, while an invalid
// Pad Length remains PROTOCOL_ERROR.

RUVIA_TEST(http2_connection_malformed_headers_payload_error_codes) {
    const auto goaway_error_for = [&](std::uint8_t flags, std::string_view payload_value) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        const auto frame = headers_frame(&resource, 1, flags, payload_value);
        RUVIA_CHECK(conn.feed(frame) == http2_feed_result::protocol_failure);
        RUVIA_CHECK(conn.connection_error().has_value());

        const auto out = conn.pending_output();
        const auto header_value = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(header_value.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
        return ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13));
    };

    RUVIA_CHECK_EQ(
        goaway_error_for(ruvia::detail::http2_flag_priority, std::string_view("\0\0\0\0", 4)),
        static_cast<std::uint32_t>(http2_error_code::frame_size_error));
    RUVIA_CHECK_EQ(goaway_error_for(ruvia::detail::http2_flag_padded, std::string_view()),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connection_rejects_regular_header_values_with_edge_whitespace) {
    for (const auto value : {" value", "value ", "\tvalue", "value\t"}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_get_request(block);
        hpack_encoder::encode_header(block, "x-test", value);
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!conn.connection_error().has_value());
        const auto event = conn.next_event();
        RUVIA_CHECK(event.has_value());
        if (event.has_value()) {
            RUVIA_CHECK(event->kind() == http2_event_kind::stream_closed);
        }
        RUVIA_CHECK(!conn.next_event().has_value());

        const auto out = conn.pending_output();
        RUVIA_CHECK(out.size() >= ruvia::detail::http2_frame_header_bytes + 4);
        if (out.size() >= ruvia::detail::http2_frame_header_bytes + 4) {
            const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
            RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
            RUVIA_CHECK_EQ(reset.stream_id_, std::uint32_t{1});
            RUVIA_CHECK_EQ(
                ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
                static_cast<std::uint32_t>(http2_error_code::protocol_error));
        }
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }
}

// A HEADERS frame WITHOUT END_HEADERS leaves the block open (awaiting CONTINUATION); a
// CONTINUATION carrying the rest with END_HEADERS completes the head and emits the event.

RUVIA_TEST(http2_connection_feed_headers_continuation_completes_head) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string first(&resource);
    hpack_encoder::encode_header(first, ":method", "GET");
    hpack_encoder::encode_header(first, ":scheme", "https");
    std::pmr::string second(&resource);
    hpack_encoder::encode_header(second, ":path", "/");
    hpack_encoder::encode_header(second, ":authority", "example.com");

    // HEADERS with END_STREAM but no END_HEADERS -> no event yet.
    const auto h = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream,
        std::string_view(first.data(), first.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.next_event().has_value());

    // CONTINUATION with END_HEADERS -> head completes.
    char chdr[9];
    ruvia::detail::http2_encode_frame_header(chdr, static_cast<std::uint32_t>(second.size()),
        http2_frame_type::continuation, ruvia::detail::http2_flag_end_headers, 1);
    std::pmr::string cont(&resource);
    cont.append(chdr, 9);
    cont.append(second.data(), second.size());
    RUVIA_CHECK(conn.feed(std::string_view(cont.data(), cont.size())) ==
                ruvia::detail::http2_feed_result::accepted);

    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr && s->request_method() == "GET");
    RUVIA_CHECK(s != nullptr && s->request_known_method() == ruvia::http_known_method::get);
}

// RFC 9113 §8.3.1: an intermediary translating HTTP/1.1 origin-form MUST omit
// :authority and keep Host. That request is not malformed.

RUVIA_TEST(http2_connection_accepts_host_without_authority_pseudo_header) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", std::nullopt);
    hpack_encoder::encode_header(block, "host", "example.com");
    const auto request = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->has_host());
        RUVIA_CHECK(!stream->has_authority());
        auto built = ruvia::detail::http_request_access::make();
        const auto build = ruvia::detail::http2_request_builder::build(
            *stream, built, &resource, {});
        RUVIA_CHECK(build.built() != nullptr);
        RUVIA_CHECK_EQ(built.authority(), std::string_view("example.com"));
        RUVIA_CHECK_EQ(
            built.header("host").value_or(""), std::string_view("example.com"));
    }
}

RUVIA_TEST(http2_connection_rejects_https_request_without_authority_or_host) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", std::nullopt);
    const auto request = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    const auto event = conn.next_event();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        RUVIA_CHECK(event->kind() == http2_event_kind::stream_closed);
    }
    const auto out = conn.pending_output();
    RUVIA_CHECK(out.size() >= ruvia::detail::http2_frame_header_bytes + 4);
    if (out.size() >= ruvia::detail::http2_frame_header_bytes + 4) {
        const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
}

// RFC 9113 requires field blocks received after our RST_STREAM to be minimally
// processed. A pinned reset stream must not capture the fragments, and the dynamic
// entry created by the discarded block must remain usable by the next stream.

RUVIA_TEST(http2_connection_local_reset_discards_multiframe_headers_and_keeps_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.pin_stream(1);
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    conn.consume_output(conn.pending_output().size());

    std::pmr::string dynamic(&resource);
    encode_short_dynamic_header(dynamic, "x-discarded", "indexed");
    const auto split = dynamic.size() / 2;
    const auto first = headers_frame(&resource, 1, 0, std::string_view(dynamic.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());  // no second RST

    const auto last = continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(dynamic.data() + split, dynamic.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(!conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && conn.stream(1)->is_aborted());

    std::pmr::string next_block(&resource);
    encode_get_request(next_block);
    hpack_encoder::encode_indexed(next_block, 62);  // x-discarded: indexed
    const auto next_value = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(next_block.data(), next_block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next_value.data(), next_value.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.connection_error().has_value());
    conn.unpin_stream(1);
}

// A trailer section without END_STREAM is a stream error, but its complete field
// block still has to update HPACK before the reset is emitted. Splitting it proves the
// core neither sends an early RST nor mistakes the required CONTINUATION for a new frame.

RUVIA_TEST(http2_connection_invalid_multiframe_trailer_resets_after_hpack_decode) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto request = post_head_frame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());
    conn.consume_output(conn.pending_output().size());

    std::pmr::string trailer(&resource);
    encode_short_dynamic_header(trailer, "x-invalid-trailer", "indexed");
    const auto split = trailer.size() / 2;
    const auto first = headers_frame(&resource, 1, 0, std::string_view(trailer.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.header_block_in_progress());
    RUVIA_CHECK(conn.pending_output().empty());

    const auto last = continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(trailer.data() + split, trailer.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    const auto reset_bytes = conn.pending_output();
    RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset_bytes.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(reset_bytes.size());

    std::pmr::string next_block(&resource);
    encode_get_request(next_block);
    hpack_encoder::encode_indexed(next_block, 62);  // x-invalid-trailer: indexed
    const auto next_value = headers_frame(&resource, 3,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(next_block.data(), next_block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next_value.data(), next_value.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.connection_error().has_value());
}

RUVIA_TEST(http2_connection_rejects_trailer_field_flood) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection connection(&resource);
    handshake(connection);

    const auto request = post_head_frame(&resource, "");
    RUVIA_CHECK(connection.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(connection.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!connection.next_event().has_value());

    std::pmr::string trailers(&resource);
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        hpack_encoder::encode_header(trailers, "x-trace", "value");
    }
    const auto trailer_frame = headers_frame(&resource, 1,
        static_cast<std::uint8_t>(
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
        std::string_view(trailers.data(), trailers.size()));
    RUVIA_CHECK(connection.feed(std::string_view(trailer_frame.data(), trailer_frame.size())) ==
                http2_feed_result::accepted);

    bool saw_closed = false;
    while (const auto event = connection.next_event()) {
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
        }
    }
    RUVIA_CHECK(saw_closed);
    RUVIA_CHECK(connection.stream(1) == nullptr);
    RUVIA_CHECK(!connection.connection_error().has_value());

    const auto reset_bytes = connection.pending_output();
    RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, std::uint32_t{1});
}

RUVIA_TEST(http2_connection_rejects_invalid_request_head_before_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const auto reject = [&](std::string_view method, std::string_view scheme,
                            std::optional<std::string_view> authority, std::string_view path,
                            std::span<const ruvia::http_header_view> headers) {
        const auto result_value = client.submit_regular_request_head(
            method, scheme, authority, path, headers, http2_request_content::none());
        RUVIA_CHECK(result_value.submitted() == nullptr);
        RUVIA_CHECK(request_head_submit_error(result_value) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const ruvia::http_header_view uppercase[] = {{"X-Test", "value"}};
    const ruvia::http_header_view connection[] = {{"connection", "keep-alive"}};
    const ruvia::http_header_view matching_host[] = {{"host", "example.test"}};
    const ruvia::http_header_view mismatched_host[] = {{"host", "other.test"}};
    const ruvia::http_header_view invalid_expect[] = {{"expect", "bad value"}};
    const ruvia::http_header_view invalid_trailer_list[] = {{"trailer", "x-checksum, bad field"}};
    const ruvia::http_header_view empty_trailer_element[] = {{"trailer", ","}};
    const ruvia::http_header_view forbidden_trailer_name[] = {{"trailer", "content-length"}};
    reject("CONNECT", "https", "example.test:443", "/", {});
    reject("GET bad", "https", "example.test", "/", {});
    reject("GET", "1ftp", "example.test", "/", {});
    reject("GET", "https", "example.test", "*", {});
    reject("GET", "https", std::nullopt, "/", {});
    reject("GET", "HTTP", std::nullopt, "/", matching_host);
    reject("GET", "https", "user@example.test", "/", {});
    reject("GET", "https", "example.test", "relative", {});
    reject("GET", "https", "example.test", "", {});
    reject("GET", "https", "example.test", "/", uppercase);
    reject("GET", "https", "example.test", "/", connection);
    reject("GET", "https", "example.test", "/", mismatched_host);
    reject("GET", "https", "example.test", "/", invalid_expect);
    reject("GET", "https", "example.test", "/", invalid_trailer_list);
    reject("GET", "https", "example.test", "/", empty_trailer_element);
    reject("GET", "https", "example.test", "/", forbidden_trailer_name);

    const ruvia::http_header_view empty_trailer_list[] = {{"trailer", ""}};
    const auto accepted_empty_trailer_list = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", empty_trailer_list, http2_request_content::none());
    RUVIA_CHECK(accepted_empty_trailer_list.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted_empty_trailer_list), std::uint32_t{1});
    client.consume_output(client.pending_output().size());

    const auto accepted = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{3});
}

RUVIA_TEST(http2_connection_submits_options_asterisk_with_authority) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);

    const auto accepted = client.submit_regular_request_head(
        "OPTIONS", "https", "example.test", "*", {}, http2_request_content::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
    RUVIA_CHECK(!client.pending_output().empty());
}

RUVIA_TEST(http2_connection_terminal_large_head_sets_end_stream_only_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::no_content);
    const std::string large_value(20 * 1024, 'a');
    response.header("X-Large", large_value);
    const auto result_value = submit_buffered_response_head(conn, 1, response);
    RUVIA_CHECK(response_head_submitted(result_value));

    auto out = conn.pending_output();
    bool first = true;
    bool saw_continuation = false;
    bool saw_end_headers = false;
    while (out.size() >= 9) {
        const auto frame = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK(out.size() >= 9 + frame.length_);
        if (first) {
            RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
            RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
            first = false;
        } else {
            RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::continuation));
            RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
            saw_continuation = true;
        }
        if ((frame.flags_ & ruvia::detail::http2_flag_end_headers) != 0) {
            saw_end_headers = true;
        }
        out.remove_prefix(9 + frame.length_);
    }
    RUVIA_CHECK(!first);
    RUVIA_CHECK(saw_continuation);
    RUVIA_CHECK(saw_end_headers);
    RUVIA_CHECK(out.empty());
}

RUVIA_TEST(http2_connection_streaming_content_length_finish_and_trailers_are_exact) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "5");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);

    RUVIA_CHECK(conn.submit_data(1, "hey", http2_end_stream::end_stream) ==
                http2_data_submit_status::content_length_incomplete);
    RUVIA_CHECK(conn.submit_data(1, "toolong", http2_end_stream::keep_open) ==
                http2_data_submit_status::content_length_exceeded);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{0});

    RUVIA_CHECK(
        conn.submit_data(1, "hel", http2_end_stream::keep_open) == http2_data_submit_status::accepted);
    conn.consume_output(conn.pending_output().size());
    const std::array<ruvia::http_header_view, 1> trailers{ruvia::http_header_view{"X-Checksum", "ok"}};
    RUVIA_CHECK(conn.finish_response(1, validated_trailers(trailers)) ==
                http2_finish_submit_status::content_length_incomplete);
    RUVIA_CHECK(stream->local_send().response_content_open() != nullptr);
    RUVIA_CHECK(conn.pending_output().empty());

    RUVIA_CHECK(
        conn.submit_data(1, "lo", http2_end_stream::keep_open) == http2_data_submit_status::accepted);
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers(trailers)) == http2_finish_submit_status::accepted);
    const auto trailer = conn.pending_output();
    const auto trailer_bytes = trailer.size();
    RUVIA_CHECK(trailer_bytes != 0);
    // The terminal call committed the complete section and closed the local half;
    // no separately staged metadata can be stranded by a later DATA submission.
    RUVIA_CHECK(
        conn.submit_data(1, {}, http2_end_stream::end_stream) == http2_data_submit_status::invalid_state);
    RUVIA_CHECK_EQ(conn.pending_output().size(), trailer_bytes);
    const auto frame = ruvia::detail::http2_parse_frame_header(trailer.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5});
}

// An explicitly registered streaming HEAD route still cannot emit a payload.
// The method/status decision belongs to the HTTP/2 core, not the Web sink.

RUVIA_TEST(http2_connection_head_streaming_response_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_request(conn, &resource, "HEAD");

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "10");
    const auto head_result = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);

    RUVIA_CHECK(response_head_submitted(head_result));
    RUVIA_CHECK(submitted_response_plan(head_result).body_plan().status_allows_body());
    RUVIA_CHECK(submitted_response_plan(head_result).body_plan().body_suppressed());
    RUVIA_CHECK(submitted_response_plan(head_result).head_disposition() ==
                http_response_stream_head_disposition::message_ended);
    RUVIA_CHECK(conn.stream(1)->local_content().forbidden() != nullptr);
    RUVIA_CHECK(conn.stream(1)->local_content().known_length() == nullptr);
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

RUVIA_TEST(http2_connection_head_response_can_end_with_trailers_only) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_request(conn, &resource, "HEAD");

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "10");
    const auto head_result = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::present);

    RUVIA_CHECK(response_head_submitted(head_result));
    RUVIA_CHECK(submitted_response_plan(head_result).body_plan().body_suppressed());
    RUVIA_CHECK(submitted_response_plan(head_result).head_disposition() ==
                http_response_stream_head_disposition::trailers_only);
    RUVIA_CHECK(submitted_response_plan(head_result).trailer_framing() ==
                http_response_stream_trailer_framing::http2_trailing_headers);
    const auto initial_head = conn.pending_output();
    const auto initial_frame = ruvia::detail::http2_parse_frame_header(initial_head.substr(0, 9));
    RUVIA_CHECK_EQ(initial_frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((initial_frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(initial_head.size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->local_send().response_content_open() == nullptr);
    RUVIA_CHECK(stream->local_send().response_trailers_only() != nullptr);
    RUVIA_CHECK(stream->local_content().forbidden() != nullptr);
    RUVIA_CHECK(conn.submit_data(1, "forbidden", http2_end_stream::keep_open) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK(conn.pending_output().empty());

    // Declaring trailer intent cannot fall back to an empty DATA terminator.
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers({})) == http2_finish_submit_status::invalid_state);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(stream->local_send().response_trailers_only() != nullptr);

    const std::array<ruvia::http_header_view, 1> trailers{
        ruvia::http_header_view{"Server-Timing", "db;dur=4"}};
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers(trailers)) == http2_finish_submit_status::accepted);
    const auto terminal = conn.pending_output();
    const auto terminal_frame = ruvia::detail::http2_parse_frame_header(terminal.substr(0, 9));
    RUVIA_CHECK_EQ(terminal_frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((terminal_frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
}

RUVIA_TEST(http2_response_finish_owns_trailer_section_atomically) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    const std::array<ruvia::http_header_view, 1> valid_trailers{
        ruvia::http_header_view{"X-Checksum", "ok"}};
    RUVIA_CHECK(conn.finish_response(1, validated_trailers(valid_trailers)) ==
                http2_finish_submit_status::invalid_state);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());

    const std::array<ruvia::http_header_view, 2> mixed_trailers{
        ruvia::http_header_view{"X-Checksum", "ok"}, ruvia::http_header_view{"Content-Length", "2"}};
    const auto mixed_result = ruvia::detail::check_http_response_trailer_section(mixed_trailers);
    RUVIA_CHECK(mixed_result.section() == nullptr);
    RUVIA_CHECK(mixed_result.failure() != nullptr);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.stream(1)->local_send().response_content_open() != nullptr);

    RUVIA_CHECK(conn.finish_response(1, validated_trailers(valid_trailers)) ==
                http2_finish_submit_status::accepted);
    const auto accepted_bytes = conn.pending_output().size();
    RUVIA_CHECK(accepted_bytes != 0);
    RUVIA_CHECK(conn.finish_response(1, validated_trailers(valid_trailers)) ==
                http2_finish_submit_status::invalid_state);
    RUVIA_CHECK_EQ(conn.pending_output().size(), accepted_bytes);
}

RUVIA_TEST(http2_connection_rejects_trailers_for_contentless_statuses_before_headers) {
    for (const auto status : {ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);
        drive_get_request(conn, &resource);

        ruvia::http_response response({.resource_ = &resource});
        response.status(status);
        const auto result_value = conn.submit_streaming_response_head(1, std::move(response),
            ruvia::http_response_stream_kind::generic, http_response_trailer_intent::present);
        RUVIA_CHECK(!response_head_submitted(result_value));
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            RUVIA_CHECK(result_value.failure()->error() ==
                        ruvia::http2_response_head_submit_error::invalid_message);
        }
        RUVIA_CHECK(conn.pending_output().empty());
        RUVIA_CHECK(conn.stream(1)->local_send().head_pending() != nullptr);
    }
}

RUVIA_TEST(http2_connection_reset_content_streaming_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::reset_content);
    response.header("Content-Length", "9");
    const auto head_result = conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);

    RUVIA_CHECK(response_head_submitted(head_result));
    RUVIA_CHECK(!submitted_response_plan(head_result).body_plan().status_allows_body());
    RUVIA_CHECK(submitted_response_plan(head_result).body_plan().body_suppressed());
    RUVIA_CHECK(submitted_response_plan(head_result).head_disposition() ==
                http_response_stream_head_disposition::message_ended);
    RUVIA_CHECK(conn.stream(1)->local_content().forbidden() != nullptr);
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

RUVIA_TEST(http2_connection_peer_reset_discards_queued_data_and_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 0);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(conn.submit_data(1, "deferred", http2_end_stream::keep_open) ==
                http2_data_submit_status::queued);
    RUVIA_CHECK(conn.has_queued_data(1));
    RUVIA_CHECK(conn.pending_output().empty());

    const std::array<ruvia::http_header_view, 1> trailers{ruvia::http_header_view{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers(trailers)) == http2_finish_submit_status::queued);

    char rst[13];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.next_event().has_value()) {
    }
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(!conn.has_queued_data(1));
    RUVIA_CHECK(conn.take_drained_data_streams().empty());
    RUVIA_CHECK(conn.pending_output().empty());

    char wu[ruvia::detail::http2_window_update_frame_bytes];
    ruvia::detail::http2_write_window_update(wu, 1, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(conn.take_drained_data_streams().empty());
}

// A HEAD response's Content-Length is representation metadata, not a DATA
// contract. The same exemption must apply when trailing HEADERS, rather than the
// initial response HEADERS, carries END_STREAM.

RUVIA_TEST(http2_connection_client_head_representation_length_survives_trailer_terminal) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "HEAD", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    hpack_encoder::encode_header(response, "content-length", "10");
    const auto response_head = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());
    const auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    const auto* known = stream->remote_content().metadata_only_known_length();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declared_length(), std::size_t{10});
    RUVIA_CHECK(stream->remote_content().metadata_only_known_length() != nullptr);
    RUVIA_CHECK_EQ(stream->remote_header_count(), std::size_t{1});

    std::pmr::string trailers(&resource);
    hpack_encoder::encode_header(trailers, "server-timing", "db;dur=4");
    const auto trailer_head = headers_frame(&resource, stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(trailers.data(), trailers.size()));
    RUVIA_CHECK(client.feed(std::string_view(trailer_head.data(), trailer_head.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(!client.connection_error().has_value());
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(stream_id)->remote_receive().end_stream() != nullptr);
    RUVIA_CHECK_EQ(client.stream(stream_id)->remote_header_count(), std::size_t{1});
    RUVIA_CHECK_EQ(client.stream(stream_id)->remote_trailers().size(), std::size_t{1});
    const auto& stored_trailer = client.stream(stream_id)->remote_trailers().front();
    RUVIA_CHECK_EQ(stored_trailer.name(), std::string_view("server-timing"));
    RUVIA_CHECK_EQ(stored_trailer.value(), std::string_view("db;dur=4"));

    client.unpin_stream(stream_id);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
}

RUVIA_TEST(http2_connection_client_accepts_204_content_length_as_metadata) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "204");
    hpack_encoder::encode_header(response, "content-length", "10");
    const auto response_head = headers_frame(&resource, stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);

    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!client.next_event().has_value());
    const auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    const auto* known = stream->remote_content().metadata_only_known_length();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declared_length(), std::size_t{10});
    RUVIA_CHECK(!client.connection_error().has_value());
    RUVIA_CHECK(client.pending_output().empty());

    client.unpin_stream(stream_id);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
}

RUVIA_TEST(http2_connection_client_accepts_combined_equal_response_content_length) {
    {
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
        hpack_encoder::encode_header(response, "content-length", "5, 5");
        const auto response_head =
            headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(!client.next_event().has_value());
        auto* stream = client.stream(stream_id);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            const auto* known = stream->remote_content().allowed_known_length();
            RUVIA_CHECK(known != nullptr);
            if (known != nullptr) {
                RUVIA_CHECK_EQ(known->declared_length(), std::size_t{5});
            }
        }
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(!client.connection_error().has_value());
    }

    {
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
        hpack_encoder::encode_header(response, "content-length", "5, 6");
        const auto response_head =
            headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);

        bool saw_closed = false;
        while (const auto event = client.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_closed = true;
                RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
                RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
            }
        }
        RUVIA_CHECK(saw_closed);
        RUVIA_CHECK(client.stream(stream_id) == nullptr);
        RUVIA_CHECK(!client.pending_output().empty());
        RUVIA_CHECK(!client.connection_error().has_value());
    }
}

RUVIA_TEST(http2_connection_applies_response_specific_trailer_rules) {
    struct case_value final {
        std::string_view name_;
        std::string_view value_;
        bool accepted_;
    };
    constexpr case_value cases[] = {
        // RFC 9110 Section 14.3 explicitly permits Accept-Ranges in a trailer.
        {"accept-ranges", "bytes", true},
        // Date is response control data that has to be known before content.
        {"date", "Sun, 06 Nov 1994 08:49:37 GMT", false},
    };

    for (const auto& test : cases) {
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
        const auto response_head =
            headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(!client.next_event().has_value());

        std::pmr::string trailers(&resource);
        hpack_encoder::encode_header(trailers, test.name_, test.value_);
        const auto trailer_head = headers_frame(&resource, stream_id,
            static_cast<std::uint8_t>(
                ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
            std::string_view(trailers.data(), trailers.size()));
        RUVIA_CHECK(client.feed(std::string_view(trailer_head.data(), trailer_head.size())) ==
                    http2_feed_result::accepted);

        bool saw_message_end = false;
        bool saw_protocol_close = false;
        while (const auto event = client.next_event()) {
            saw_message_end = saw_message_end || event->message_end() != nullptr;
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_close = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK_EQ(saw_message_end, test.accepted_);
        RUVIA_CHECK_EQ(saw_protocol_close, !test.accepted_);
        RUVIA_CHECK_EQ(client.pending_output().empty(), test.accepted_);
        RUVIA_CHECK(!client.connection_error().has_value());
    }
}

// Server-role trailers: a trailing HEADERS block WITHOUT END_STREAM is a protocol
// error on that stream (RFC 9113 §8.1) -- the core RSTs and closes it, no message_end.

RUVIA_TEST(http2_connection_server_trailers_without_end_stream_rejected) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, no END_STREAM on HEADERS).
    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "POST");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", "/");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    const auto h = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());

    // A trailer HEADERS block with END_HEADERS but NO END_STREAM -> stream error.
    std::pmr::string trailer(&resource);
    hpack_encoder::encode_header(trailer, "x-checksum", "abc");
    const auto t = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers,  // deliberately no END_STREAM
        std::string_view(trailer.data(), trailer.size()));
    (void)conn.feed(std::string_view(t.data(), t.size()));

    bool saw_closed = false;
    bool saw_end = false;
    while (const auto event = conn.next_event()) {
        if (const auto* closed = event->stream_closed()) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
        }
        if (event->message_end() != nullptr) {
            saw_end = true;
        }
    }
    RUVIA_CHECK(saw_closed);
    RUVIA_CHECK(!saw_end);                              // never completes the request
    RUVIA_CHECK(!conn.connection_error().has_value());  // stream error, not connection error
    const auto rst = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK(conn.stream(1) == nullptr);  // removed, not leaked
}

// Semantic response trailers queued behind a window-blocked body: the HEADERS must be
// emitted AFTER the deferred DATA drains (RFC 9113 §8.1), carrying END_STREAM in place
// of it -- never ahead of the body bytes.

RUVIA_TEST(http2_connection_trailers_wait_for_blocked_body) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake_with_window(conn, 4);  // tiny 4-byte stream send window
    drive_get_request(conn, &resource);
    conn.consume_output(conn.pending_output().size());

    // Streaming response head declares an exact 8-byte content length. Only 4 DATA
    // bytes fit the window, so the other 4 are core-owned and deferred -> queued.
    ruvia::http_response head({.resource_ = &resource});
    head.status(ruvia::http_status::ok);
    head.header("Content-Length", "8");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(head),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    conn.consume_output(conn.pending_output().size());
    RUVIA_CHECK(conn.submit_data(1, "AAAABBBB", http2_end_stream::keep_open) ==
                http2_data_submit_status::queued);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{4});

    // First 4 bytes went out as DATA (no END_STREAM).
    auto out = conn.pending_output();
    const auto d1 = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(d1.length_, static_cast<std::uint32_t>(4));
    RUVIA_CHECK((d1.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(out.size());

    // Queue trailers while the remaining 4 bytes are still window-blocked.
    const std::array<ruvia::http_header_view, 1> invalid_trailers{
        ruvia::http_header_view{"Content-Length", "8"}};
    const auto invalid_result = ruvia::detail::check_http_response_trailer_section(invalid_trailers);
    RUVIA_CHECK(invalid_result.section() == nullptr);
    RUVIA_CHECK(invalid_result.failure() != nullptr);
    RUVIA_CHECK(conn.has_queued_data(1));
    RUVIA_CHECK(stream->local_send().response_content_open() != nullptr);
    RUVIA_CHECK(conn.pending_output().empty());
    const std::array<ruvia::http_header_view, 1> trailers{ruvia::http_header_view{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers(trailers)) == http2_finish_submit_status::queued);
    RUVIA_CHECK(conn.pending_output().empty());  // nothing emitted yet (still blocked)

    // Peer WINDOW_UPDATE reopens the window: the deferred DATA drains, THEN the trailer
    // HEADERS(END_STREAM) follows -- in that order.
    char wu[9 + 4];
    ruvia::detail::http2_encode_frame_header(wu, 4, http2_frame_type::window_update, 0, 1);
    ruvia::detail::http2_write32(wu + 9, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    while (conn.next_event().has_value()) {
    }

    out = conn.pending_output();
    const auto d2 = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(d2.length_, static_cast<std::uint32_t>(4));             // the remaining body
    RUVIA_CHECK((d2.flags_ & ruvia::detail::http2_flag_end_stream) == 0);  // NOT on the DATA
    out = out.substr(9 + d2.length_);
    const auto th = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(th.type_, static_cast<std::uint8_t>(http2_frame_type::headers));  // trailer
    RUVIA_CHECK((th.flags_ & ruvia::detail::http2_flag_end_stream) != 0);            // END_STREAM here
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{8});
    const auto trailer_payload = out.substr(9, th.length_);
    RUVIA_CHECK((trailer_payload.find("x-checksum") != std::string_view::npos));
    RUVIA_CHECK(!(trailer_payload.find("X-Checksum") != std::string_view::npos));
}

// A HEADERS without END_HEADERS followed by an endless stream of EMPTY CONTINUATION
// frames keeps the field block "in progress" forever: empty frames add no bytes,
// so the accumulated-block size cap never trips. The CONTINUATION frame-count
// budget cuts the peer off with GOAWAY(ENHANCE_YOUR_CALM) (RFC 9113 §6.10,
// CVE-2024-27316).

RUVIA_TEST(http2_connection_continuation_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);
    // Open the block WITHOUT END_HEADERS so CONTINUATION frames are expected.
    const auto head = headers_frame(&resource, 1, 0, std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) !=
                ruvia::detail::http2_feed_result::protocol_failure);

    const auto empty = continuation_frame(&resource, 1, 0, {});
    bool tripped = false;
    for (std::uint32_t i = 0; i < ruvia::detail::http2_max_continuation_frames + 2 && !tripped; ++i) {
        tripped = conn.feed(std::string_view(empty.data(), empty.size())) ==
                  ruvia::detail::http2_feed_result::protocol_failure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connection_error().has_value());
    RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()), enhance_your_calm);
}

// A well-formed head split across a few CONTINUATION frames completes normally: the
// budget is generous enough that legitimate fragmentation never trips it.

RUVIA_TEST(http2_connection_fragmented_headers_within_budget_complete) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);
    const std::string_view whole(block.data(), block.size());
    const auto q = whole.size() / 4;
    const auto head = headers_frame(&resource, 1, 0, whole.substr(0, q));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) !=
                ruvia::detail::http2_feed_result::protocol_failure);
    const auto c1 = continuation_frame(&resource, 1, 0, whole.substr(q, q));
    const auto c2 = continuation_frame(&resource, 1, 0, whole.substr(2 * q, q));
    const auto c3 = continuation_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        whole.substr(3 * q));
    RUVIA_CHECK(conn.feed(std::string_view(c1.data(), c1.size())) !=
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.feed(std::string_view(c2.data(), c2.size())) !=
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.feed(std::string_view(c3.data(), c3.size())) !=
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(!conn.connection_error().has_value());

    bool saw_head = false;
    while (const auto event = conn.next_event()) {
        if (event->message_head() != nullptr) {
            saw_head = true;
        }
    }
    RUVIA_CHECK(saw_head);
}
