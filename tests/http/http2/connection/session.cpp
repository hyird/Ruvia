#include "ruvia/http/http_response_stream.h"

#include "http2_connection_fixture.h"
#include "response/http_response_headers_access.h"

// http2_connection: the connection preface, SETTINGS, PING and the frame loop.

// The sans-I/O core produces a SETTINGS frame (stream 0) into its outbound buffer,
// and consume_output drains it. Exercises the core with zero asio / zero I/O.

namespace {

void add_unchecked_header(
    ruvia::http_response& response, std::string_view name, std::string_view value) {
    auto& headers = const_cast<ruvia::http_response_headers&>(response.headers());
    (void)ruvia::detail::http_response_headers_access::add(headers, name, value, 0);
}

}  // namespace

RUVIA_TEST(http2_connection_begin_server_connection_emits_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);

    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.wants_write());

    conn.begin_connection();

    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(
        out.size(), http2_local_settings::frame_bytes + ruvia::detail::http2_window_update_frame_bytes);
    RUVIA_CHECK(conn.wants_write());

    const auto header_value = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(header_value.type_, static_cast<std::uint8_t>(http2_frame_type::settings));
    RUVIA_CHECK_EQ(header_value.stream_id_, static_cast<std::uint32_t>(0));
    RUVIA_CHECK_EQ(header_value.length_, http2_local_settings::payload_bytes);

    const auto window =
        ruvia::detail::http2_parse_frame_header(out.substr(http2_local_settings::frame_bytes, 9));
    RUVIA_CHECK_EQ(window.type_, static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(window.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::http2_window_update_increment(
                       out.substr(http2_local_settings::frame_bytes + 9, 4)),
        http2_local_settings::initial_window_size -
            static_cast<std::uint32_t>(ruvia::detail::http2_default_initial_window_size));

    conn.begin_connection();
    RUVIA_CHECK_EQ(conn.pending_output().size(), out.size());

    conn.consume_output(out.size());
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.wants_write());
}

RUVIA_TEST(http2_connection_output_consumption_is_transactional) {
    http2_connection connection(std::pmr::get_default_resource(), ruvia::detail::http2_role::server);
    connection.begin_connection();
    const std::string original(connection.pending_output());
    RUVIA_CHECK(!original.empty());

    RUVIA_CHECK(connection.consume_output(original.size() + 1) ==
                ruvia::detail::http2_output_consume_status::out_of_range);
    RUVIA_CHECK_EQ(connection.pending_output(), std::string_view(original));

    RUVIA_CHECK(connection.consume_output(1) == ruvia::detail::http2_output_consume_status::pending);
    RUVIA_CHECK_EQ(connection.pending_output(), std::string_view(original).substr(1));
    RUVIA_CHECK(connection.consume_output(original.size() - 1) ==
                ruvia::detail::http2_output_consume_status::drained);
    RUVIA_CHECK(connection.pending_output().empty());
    RUVIA_CHECK(!connection.wants_write());
}

RUVIA_TEST(http2_connection_begin_client_connection_prefixes_same_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);

    client.begin_connection();
    const auto out = client.pending_output();
    RUVIA_CHECK_EQ(out.size(), ruvia::detail::http2_client_preface.size() +
                                   http2_local_settings::frame_bytes +
                                   ruvia::detail::http2_window_update_frame_bytes);
    RUVIA_CHECK_EQ(out.substr(0, ruvia::detail::http2_client_preface.size()),
        ruvia::detail::http2_client_preface);

    const auto settings_offset = ruvia::detail::http2_client_preface.size();
    const auto settings = ruvia::detail::http2_parse_frame_header(out.substr(settings_offset, 9));
    RUVIA_CHECK_EQ(settings.type_, static_cast<std::uint8_t>(http2_frame_type::settings));
    RUVIA_CHECK_EQ(settings.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(settings.length_, http2_local_settings::payload_bytes);

    const auto first_size = out.size();
    client.begin_connection();
    RUVIA_CHECK_EQ(client.pending_output().size(), first_size);
}

RUVIA_TEST(http2_connection_outbound_extension_method_is_valid_wire_token) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);

    const auto extension = client.submit_regular_request_head(
        "PROPFIND", "https", "example.test", "/dav", {}, http2_request_content::none());
    RUVIA_CHECK(extension.submitted() != nullptr);
    const auto extension_stream_id = submitted_request_stream_id(extension);
    RUVIA_CHECK_EQ(extension_stream_id, std::uint32_t{1});
    const auto* stream = client.stream(extension_stream_id);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->request_method(), std::string_view("PROPFIND"));
        RUVIA_CHECK(stream->request_known_method() == ruvia::http_known_method::unknown);
    }

    client.consume_output(client.pending_output().size());
    const auto malformed = client.submit_regular_request_head(
        "BAD METHOD", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(malformed.submitted() == nullptr);
    RUVIA_CHECK(request_head_submit_error(malformed) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);
}

RUVIA_TEST(http2_connection_feed_before_begin_retains_input_and_is_retryable) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);

    char settings[9];
    ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
    const auto before_begin = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(before_begin == ruvia::detail::http2_feed_result::connection_not_started);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(!server.received_peer_settings());
    RUVIA_CHECK(!server.next_event().has_value());

    begin_peer_input(server);
    const auto retried = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(retried == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(server.received_peer_settings());
}

// feed() drives the SETTINGS handshake with zero I/O: feed the peer's empty
// SETTINGS frame and the core must emit a SETTINGS ACK.

RUVIA_TEST(http2_connection_feed_settings_emits_ack) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    begin_peer_input(conn);

    char frame[9];
    ruvia::detail::http2_encode_frame_header(frame, 0, http2_frame_type::settings, 0, 0);
    const auto result_value = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);

    const auto out = conn.pending_output();
    RUVIA_CHECK(out.size() >= 9);
    const auto ack = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type_, static_cast<std::uint8_t>(http2_frame_type::settings));
    RUVIA_CHECK((ack.flags_ & ruvia::detail::http2_flag_ack) != 0);
    RUVIA_CHECK_EQ(ack.length_, static_cast<std::uint32_t>(0));
}

RUVIA_TEST(http2_connection_feed_handles_deterministic_random_frames_after_settings) {
    std::uint64_t state_value = 0x4832'4652'414D'4553ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::pmr::monotonic_buffer_resource resource;
        std::array<char, ruvia::detail::http2_frame_header_bytes + 64> frame{};
        http2_connection connection(&resource);
        begin_peer_input(connection);

        ruvia::detail::http2_encode_frame_header(frame.data(), 0, http2_frame_type::settings, 0, 0);
        RUVIA_CHECK(connection.feed(std::string_view(frame.data(), 9)) ==
                    http2_feed_result::accepted);
        while (connection.next_event().has_value()) {
        }
        connection.consume_output(connection.pending_output().size());

        const auto payload_size = static_cast<std::uint32_t>(next_value() % 65U);
        ruvia::detail::http2_encode_frame_header(frame.data(), payload_size,
            static_cast<http2_frame_type>(static_cast<std::uint8_t>(next_value())),
            static_cast<std::uint8_t>(next_value()), static_cast<std::uint32_t>(next_value()));
        for (std::size_t index = 0; index < payload_size; ++index) {
            frame[ruvia::detail::http2_frame_header_bytes + index] = static_cast<char>(next_value());
        }

        const auto result_value = connection.feed(
            std::string_view(frame.data(), ruvia::detail::http2_frame_header_bytes + payload_size));
        RUVIA_CHECK(result_value == http2_feed_result::accepted || result_value == http2_feed_result::need_input ||
                    result_value == http2_feed_result::protocol_failure);
        if (result_value == http2_feed_result::protocol_failure) {
            RUVIA_CHECK(connection.connection_error().has_value());
            RUVIA_CHECK(connection.pending_output().size() >= ruvia::detail::http2_frame_header_bytes);
        } else {
            RUVIA_CHECK(!connection.connection_error().has_value());
        }
    }
}

RUVIA_TEST(http2_connection_enable_push_validation_uses_peer_direction) {
    char frame[15];
    auto* out = ruvia::detail::http2_write_frame_header(frame, 6, http2_frame_type::settings, 0, 0);
    out =
        ruvia::detail::http2_write_settings_entry(out, ruvia::detail::http2_setting_id::enable_push, 1);
    RUVIA_CHECK_EQ(out, frame + sizeof(frame));

    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_peer_input(client);
    const auto client_result = client.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(client_result == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(client.connection_error().has_value());
    const auto goaway = client.pending_output();
    const auto goaway_header = ruvia::detail::http2_parse_frame_header(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(goaway_header.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(goaway.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));

    http2_connection server(&resource);
    begin_peer_input(server);
    const auto server_result = server.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(server_result == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    const auto ack = ruvia::detail::http2_parse_frame_header(server.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type_, static_cast<std::uint8_t>(http2_frame_type::settings));
    RUVIA_CHECK((ack.flags_ & ruvia::detail::http2_flag_ack) != 0);
}

// A non-SETTINGS first frame is a protocol error (GOAWAY emitted, feed reports error).

RUVIA_TEST(http2_connection_feed_rejects_non_settings_first_frame) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    begin_peer_input(conn);

    char frame[9];
    ruvia::detail::http2_encode_frame_header(frame, 0, http2_frame_type::ping, 0, 0);
    const auto result_value = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error().has_value());
    const auto out = conn.pending_output();
    RUVIA_CHECK(out.size() >= 9);
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
}

RUVIA_TEST(http2_connection_server_requires_client_magic_before_frames) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    server.begin_connection();
    server.consume_output(server.pending_output().size());

    char bytes_value[ruvia::detail::http2_client_preface.size()]{};
    ruvia::detail::http2_encode_frame_header(bytes_value, 0, http2_frame_type::settings, 0, 0);
    const auto result_value = server.feed(std::string_view(bytes_value, sizeof(bytes_value)));
    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error().has_value());
    RUVIA_CHECK(!server.received_peer_settings());
}

RUVIA_TEST(http2_connection_first_peer_settings_must_not_be_ack_for_either_role) {
    char ack[9];
    ruvia::detail::http2_encode_frame_header(
        ack, 0, http2_frame_type::settings, ruvia::detail::http2_flag_ack, 0);

    std::pmr::monotonic_buffer_resource resource;
    {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_peer_input(client);
        const auto result_value = client.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(client.connection_error().has_value());
        RUVIA_CHECK(!client.received_peer_settings());
    }
    {
        http2_connection server(&resource);
        begin_peer_input(server);
        const auto result_value = server.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
        RUVIA_CHECK(server.connection_error().has_value());
        RUVIA_CHECK(!server.received_peer_settings());
    }
}

RUVIA_TEST(http2_connection_settings_validation_is_atomic_across_entries) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    handshake(client);

    char frame[9 + 12];
    auto* out = ruvia::detail::http2_write_frame_header(frame, 12, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::enable_connect_protocol, 1);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::max_frame_size, 0);
    RUVIA_CHECK_EQ(out, frame + sizeof(frame));

    RUVIA_CHECK(
        client.feed(std::string_view(frame, sizeof(frame))) == http2_feed_result::protocol_failure);
    RUVIA_CHECK(!client.peer_extended_connect_enabled());
}

// After the handshake, a PING is echoed back with the ACK flag and the same payload.

RUVIA_TEST(http2_connection_feed_ping_echoes_ack) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
    const char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(ping + 9, data, 8);
    (void)conn.feed(std::string_view(ping, sizeof(ping)));

    const auto out = conn.pending_output();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(9 + 8));
    const auto ack = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type_, static_cast<std::uint8_t>(http2_frame_type::ping));
    RUVIA_CHECK((ack.flags_ & ruvia::detail::http2_flag_ack) != 0);
    RUVIA_CHECK(out.substr(9, 8) == std::string_view(data, 8));
}

RUVIA_TEST(http2_connection_partial_frame_reports_need_more_until_complete) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
    std::memcpy(ping + 9, "12345678", 8);

    constexpr std::size_t first_bytes = 12;  // full header + partial payload
    const auto partial = conn.feed(std::string_view(ping, first_bytes));
    RUVIA_CHECK(partial == ruvia::detail::http2_feed_result::need_input);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto complete_value =
        conn.feed(std::string_view(ping + first_bytes, sizeof(ping) - first_bytes));
    RUVIA_CHECK(complete_value == ruvia::detail::http2_feed_result::accepted);
    const auto ack = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type_, static_cast<std::uint8_t>(http2_frame_type::ping));
    RUVIA_CHECK((ack.flags_ & ruvia::detail::http2_flag_ack) != 0);
}

// A complete HEADERS frame (END_HEADERS + END_STREAM) decodes the request head and the
// sans-I/O core emits message_head then message_end; the head is exposed via stream().

RUVIA_TEST(http2_connection_event_queue_is_optional_and_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_get_request(block);
    const auto frame = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    const auto result_value = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());

    const auto e1 = conn.next_event().value();
    RUVIA_CHECK(e1.kind() == http2_event_kind::message_head);
    RUVIA_CHECK(e1.message_body_chunk() == nullptr);
    RUVIA_CHECK(e1.goaway() == nullptr);
    RUVIA_CHECK_EQ(e1.message_head()->stream_id(), static_cast<std::uint32_t>(1));
    const auto e2 = conn.next_event().value();
    RUVIA_CHECK(e2.kind() == http2_event_kind::message_end);
    RUVIA_CHECK(e2.message_head() == nullptr);
    RUVIA_CHECK_EQ(e2.message_end()->stream_id(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.next_event().has_value());

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);
    RUVIA_CHECK_EQ(s->request_method(), std::string_view("GET"));
    RUVIA_CHECK(s->request_known_method() == ruvia::http_known_method::get);
}

RUVIA_TEST(http2_connection_feed_preserves_empty_non_http_path) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "git+ssh", "", std::nullopt);
    const auto frame = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    RUVIA_CHECK(
        conn.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->has_path());
        RUVIA_CHECK(stream->request_path().empty());
    }
}

RUVIA_TEST(http2_connection_feed_rejects_empty_http_path) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::string_view schemes[] = {"http", "HTTPS"};
    for (const auto scheme : schemes) {
        http2_connection conn(&resource);
        handshake(conn);
        std::pmr::string block(&resource);
        encode_request(block, "GET", scheme, "", "example.test");
        const auto frame = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(
            conn.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(!conn.connection_error().has_value());
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::stream_closed);
        RUVIA_CHECK(!conn.next_event().has_value());
        const auto out = conn.pending_output();
        const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
}

RUVIA_TEST(http2_connection_accepts_non_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    std::pmr::string block(&resource);
    encode_request(block, "GET", "git+ssh", "/repository", "deploy:secret@example.test:9418");
    const auto request = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_end);
    const auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(
            stream->request_authority(), std::string_view("deploy:secret@example.test:9418"));
    }
}

RUVIA_TEST(http2_connection_rejects_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    std::pmr::string block(&resource);
    encode_request(block, "GET", "https", "/", "user@example.test");
    const auto request = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto out = server.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connection_asterisk_path_requires_options_and_accepts_authority) {
    std::pmr::monotonic_buffer_resource resource;
    {
        http2_connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encode_request(block, "GET", "https", "*");
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::stream_closed);
        RUVIA_CHECK(!server.next_event().has_value());
        const auto out = server.pending_output();
        const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
    {
        http2_connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encode_request(block, "OPTIONS", "https", "*");
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_end);
        const auto* stream = server.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK_EQ(stream->request_authority(), std::string_view("example.com"));
            RUVIA_CHECK_EQ(stream->request_path(), std::string_view("*"));
        }
    }
    {
        http2_connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encode_request(block, "OPTIONS", "https", "*", std::nullopt);
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_end);
        const auto* stream = server.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK_EQ(stream->request_path(), std::string_view("*"));
        }
    }
}

RUVIA_TEST(http2_connection_accepts_huffman_block_larger_than_decoded_field_section) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    // obs-text is legal field-value data. HPACK byte 0xdc has a 28-bit Huffman
    // code, so this 20 KiB decoded value expands to 70 KiB on the wire while
    // remaining well below the advertised 64 KiB decoded field-section limit.
    constexpr unsigned char expanded_byte = 0xdc;
    constexpr std::size_t decoded_value_bytes = 20 * 1024;
    std::pmr::string block(&resource);
    encode_get_request(block);
    encode_repeated_huffman_header(block, "x-huffman-expanded", expanded_byte, decoded_value_bytes);
    RUVIA_CHECK(block.size() > ruvia::max_http_header_bytes);

    std::size_t offset = 0;
    const auto first_bytes =
        std::min(block.size(), static_cast<std::size_t>(http2_local_settings::max_frame_size));
    const auto first = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), first_bytes));
    RUVIA_CHECK(
        conn.feed(std::string_view(first.data(), first.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(!conn.next_event().has_value());
    offset += first_bytes;

    while (offset < block.size()) {
        const auto fragment_bytes = std::min(
            block.size() - offset, static_cast<std::size_t>(http2_local_settings::max_frame_size));
        const auto flags = offset + fragment_bytes == block.size()
                               ? ruvia::detail::http2_flag_end_headers
                               : std::uint8_t{0};
        const auto continuation = continuation_frame(
            &resource, 1, flags, std::string_view(block.data() + offset, fragment_bytes));
        RUVIA_CHECK(conn.feed(std::string_view(continuation.data(), continuation.size())) ==
                    http2_feed_result::accepted);
        offset += fragment_bytes;
    }

    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->remote_header_count(), std::size_t{1});
    const auto expanded = stream->remote_header_at(0);
    RUVIA_CHECK_EQ(expanded.name_, std::string_view("x-huffman-expanded"));
    RUVIA_CHECK_EQ(expanded.value_.size(), decoded_value_bytes);
    RUVIA_CHECK(std::all_of(expanded.value_.begin(), expanded.value_.end(),
        [](char byte) { return static_cast<unsigned char>(byte) == expanded_byte; }));
}

// If a discarded field block exceeds the buffering budget, the core cannot satisfy
// RFC 9113's mandatory decompression step. The connection error is COMPRESSION_ERROR,
// not an application/load-shedding code that would imply HPACK remains usable.

RUVIA_TEST(http2_connection_undecodable_discarded_block_is_compression_error) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.pin_stream(1);
    RUVIA_CHECK(conn.submit_reset(1, http2_error_code::cancel) == http2_submit_status::accepted);
    conn.consume_output(conn.pending_output().size());

    const std::string full_frame(ruvia::detail::http2_default_max_frame_size, '\0');
    const auto first = headers_frame(&resource, 1, 0, full_frame);
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    constexpr auto full_frame_count =
        ruvia::detail::max_http2_encoded_header_block_bytes / ruvia::detail::http2_default_max_frame_size;

    for (std::size_t i = 1; i < full_frame_count; ++i) {
        const auto continuation = continuation_frame(&resource, 1, 0, full_frame);
        RUVIA_CHECK(conn.feed(std::string_view(continuation.data(), continuation.size())) ==
                    ruvia::detail::http2_feed_result::accepted);
    }
    const auto overflow = continuation_frame(&resource, 1, ruvia::detail::http2_flag_end_headers, "x");
    RUVIA_CHECK(conn.feed(std::string_view(overflow.data(), overflow.size())) ==
                ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(conn.connection_error().has_value());
    const auto out = conn.pending_output();
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::compression_error));
    conn.unpin_stream(1);
}

RUVIA_TEST(http2_connection_trace_rejects_declared_or_transferred_content) {
    {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "TRACE", "https", "/diagnostic");
        hpack_encoder::encode_header(block, "content-length", "0");
        const auto head = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);

        bool saw_protocol_error = false;
        while (const auto event = conn.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            RUVIA_CHECK(event->message_end() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(saw_protocol_error);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "TRACE", "https", "/diagnostic");
        const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(!conn.next_event().has_value());

        const auto content = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "body");
        RUVIA_CHECK(conn.feed(std::string_view(content.data(), content.size())) ==
                    http2_feed_result::accepted);

        bool saw_body = false;
        bool saw_protocol_error = false;
        while (const auto event = conn.next_event()) {
            saw_body = saw_body || event->message_body_chunk() != nullptr;
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(!saw_body);
        RUVIA_CHECK(saw_protocol_error);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }
}

RUVIA_TEST(http2_connection_trace_allows_empty_terminal_framing) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "TRACE", "https", "/diagnostic");
    hpack_encoder::encode_header(block, "expect", "100-continue");
    const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        const auto expectation =
            stream->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(expectation.no_action() != nullptr);
        RUVIA_CHECK(expectation.send_continue() == nullptr);
    }

    const auto end = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, {});
    RUVIA_CHECK(conn.feed(std::string_view(end.data(), end.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());
    RUVIA_CHECK(!conn.connection_error().has_value());
}

RUVIA_TEST(http2_connection_options_content_requires_valid_content_type) {
    const auto check_rejected = [&ruvia_ctx](std::string_view method,
                                    std::optional<std::string_view> content_type_value,
                                    std::optional<std::string_view> content_length, bool end_stream) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, method, "https", "/diagnostics");
        if (content_type_value.has_value()) {
            hpack_encoder::encode_header(block, "content-type", *content_type_value);
        }
        if (content_length.has_value()) {
            hpack_encoder::encode_header(block, "content-length", *content_length);
        }
        auto flags = ruvia::detail::http2_flag_end_headers;
        if (end_stream) {
            flags |= ruvia::detail::http2_flag_end_stream;
        }
        const auto head =
            headers_frame(&resource, 1, flags, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);

        bool saw_protocol_error = false;
        while (const auto event = conn.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            RUVIA_CHECK(event->message_end() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(saw_protocol_error);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    };

    check_rejected("OPTIONS", std::nullopt, "0", true);
    check_rejected("OPTIONS", std::nullopt, std::nullopt, false);
    check_rejected("OPTIONS", "not a media type", "0", true);

    {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "OPTIONS", "https", "/diagnostics");
        hpack_encoder::encode_header(block, "content-type", "application/json; charset=utf-8");
        hpack_encoder::encode_header(block, "content-length", "0");
        const auto head = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
        RUVIA_CHECK(!conn.next_event().has_value());
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "OPTIONS", "https", "/diagnostics");
        hpack_encoder::encode_header(block, "content-type", "application/octet-stream");
        const auto head = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);

        const auto body = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "x");
        RUVIA_CHECK(
            conn.feed(std::string_view(body.data(), body.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_body_chunk);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
        RUVIA_CHECK(!conn.next_event().has_value());
        conn.release_all_received_data(1);
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "options", "https", "/diagnostics");
        hpack_encoder::encode_header(block, "content-length", "0");
        const auto head = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
        RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
        RUVIA_CHECK(!conn.next_event().has_value());
    }
}

RUVIA_TEST(http2_connection_rejects_invalid_content_type_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "POST", "https", "/items");
    hpack_encoder::encode_header(block, "content-type", "not a media type");
    hpack_encoder::encode_header(block, "content-length", "0");
    const auto head = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_rejects_invalid_content_encoding_syntax) {
    const auto check = [&ruvia_ctx](std::string_view value, bool rejected) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encode_request(block, "POST", "https", "/items");
        hpack_encoder::encode_header(block, "content-encoding", value);
        hpack_encoder::encode_header(block, "content-length", "0");
        const auto head = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(
            conn.feed(std::string_view(head.data(), head.size())) == http2_feed_result::accepted);

        bool saw_head = false;
        bool saw_protocol_error = false;
        while (const auto event = conn.next_event()) {
            saw_head = saw_head || event->message_head() != nullptr;
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(saw_protocol_error == rejected);
        RUVIA_CHECK(saw_head != rejected);
    };

    check("gzip;level=9", true);
    check("bad coding", true);
    check("gzip/deflate", true);
    check(", gzip,,", false);
    check("deflate", false);
    check("gzip, br", false);
}

RUVIA_TEST(http2_connection_feed_preserves_pending_events_and_retries_input) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    const auto head = post_head_frame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto data = data_frame(&resource, 1, ruvia::detail::http2_flag_end_stream, "body");
    RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    const auto output_before_retry = conn.pending_output().size();

    char ping[9 + 8];
    ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
    std::memcpy(ping + 9, "retry-me", 8);
    const auto blocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(blocked == ruvia::detail::http2_feed_result::events_pending);
    RUVIA_CHECK_EQ(conn.pending_output().size(), output_before_retry);

    const auto chunk = conn.next_event().value();
    RUVIA_CHECK(chunk.kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK_EQ(chunk.message_body_chunk()->bytes(), std::string_view("body"));
    const auto still_blocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(still_blocked == ruvia::detail::http2_feed_result::events_pending);
    RUVIA_CHECK_EQ(chunk.message_body_chunk()->bytes(), std::string_view("body"));

    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    const auto retried = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(retried == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(conn.pending_output().size(), output_before_retry + sizeof(ping));
    const auto ack =
        ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(output_before_retry, 9));
    RUVIA_CHECK_EQ(ack.type_, static_cast<std::uint8_t>(http2_frame_type::ping));
    RUVIA_CHECK((ack.flags_ & ruvia::detail::http2_flag_ack) != 0);
    RUVIA_CHECK(!conn.next_event().has_value());
}

RUVIA_TEST(http2_connection_rejects_invalid_outbound_content_encoding_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check = [&resource, &ruvia_ctx](std::string_view value, bool rejected) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const ruvia::http_header_view content_encoding[] = {{"content-encoding", value}};
        const auto result_value = client.submit_regular_request_head("POST", "https", "example.test",
            "/upload", content_encoding, http2_request_content::known_length(1));
        const bool failed = result_value.submitted() == nullptr;
        RUVIA_CHECK(failed == rejected);
        if (rejected && failed) {
            RUVIA_CHECK(
                request_head_submit_error(result_value) == http2_request_head_submit_error::invalid_message);
            RUVIA_CHECK(client.pending_output().empty());
            RUVIA_CHECK(client.stream(1) == nullptr);
        }
    };

    check("gzip;level=9", true);
    check("bad coding", true);
    check("", true);
    check(",gzip", true);
    check("gzip,", true);
    check("deflate", false);
    check("gzip, br", false);
}

RUVIA_TEST(http2_connection_validates_outbound_cors_fields_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check_rejected = [&resource, &ruvia_ctx](
                                    std::string_view name, std::string_view value) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const ruvia::http_header_view header_value[] = {{name, value}};
        const auto rejected = client.submit_regular_request_head(
            "OPTIONS", "https", "example.test", "/resource", header_value, http2_request_content::none());
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    check_rejected("origin", "https://example.test/path");
    check_rejected("access-control-request-method", "GET, POST");
    check_rejected("access-control-request-headers", "x-good, bad header");

    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const ruvia::http_header_view valid_headers[] = {
        {"origin", "https://first.test https://second.test"},
        {"access-control-request-method", "GET"},
        {"access-control-request-headers", "x-first"},
        {"access-control-request-headers", "x-second, x-third"},
    };
    const auto accepted = client.submit_regular_request_head(
        "OPTIONS", "https", "example.test", "/resource", valid_headers, http2_request_content::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_owns_expect_header_and_gates_typed_continue_content) {
    std::pmr::monotonic_buffer_resource resource;
    const ruvia::http_header_view expect_continue[] = {{"expect", "100-Continue"}};
    const ruvia::http_header_view combined_expectation[] = {{"expect", "extension, 100-Continue"}};

    const auto check_regular_rejected = [&resource, &ruvia_ctx](
                                            std::span<const ruvia::http_header_view> headers,
                                            http2_request_content content) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const auto rejected = client.submit_regular_request_head(
            "POST", "https", "example.test", "/upload", headers, content);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submit_regular_request_head(
            "GET", "https", "example.test", "/", {}, http2_request_content::none());
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
    };

    check_regular_rejected(expect_continue, http2_request_content::none());
    check_regular_rejected(expect_continue, http2_request_content::known_length(0));
    check_regular_rejected(expect_continue, http2_request_content::known_length(1));
    check_regular_rejected(expect_continue, http2_request_content::streaming());
    check_regular_rejected(combined_expectation, http2_request_content::known_length(0));
    const ruvia::http_header_view extension_expectation[] = {{"expect", "extension"}};
    check_regular_rejected(extension_expectation, http2_request_content::none());

    http2_connection connect_client(&resource, ruvia::detail::http2_role::client);
    begin_client(connect_client);
    const auto rejected_connect =
        connect_client.submit_connect_request_head("example.test:443", expect_continue);
    RUVIA_CHECK(rejected_connect.submitted() == nullptr);
    RUVIA_CHECK(
        request_head_submit_error(rejected_connect) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(connect_client.pending_output().empty());
    RUVIA_CHECK(connect_client.stream(1) == nullptr);

    const auto check_typed_rejected = [&resource, &ruvia_ctx](http2_request_content content) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const auto rejected = client.submit_regular_request_head("POST", "https", "example.test",
            "/upload", {}, content, ruvia::http_client_request_expectation::continue_value);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    check_typed_rejected(http2_request_content::none());
    check_typed_rejected(http2_request_content::known_length(0));

    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const auto accepted =
        client.submit_regular_request_head("POST", "https", "example.test", "/upload", {},
            http2_request_content::known_length(1), ruvia::http_client_request_expectation::continue_value);
    RUVIA_CHECK(accepted.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(accepted);
    RUVIA_CHECK_EQ(stream_id, std::uint32_t{1});
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::expectation_pending);
    RUVIA_CHECK(client.release_request_content(stream_id) ==
                ruvia::detail::http2_request_content_release_status::released);
    RUVIA_CHECK(client.release_request_content(stream_id) ==
                ruvia::detail::http2_request_content_release_status::not_pending);
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
}

RUVIA_TEST(http2_connection_rejects_upgrade_required_final_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response buffered({.resource_ = &resource});
    buffered.status(ruvia::http_status::upgrade_required);
    buffered.header("Upgrade", "websocket");
    const auto buffered_result = submit_buffered_response_head(conn, 1, buffered);
    RUVIA_CHECK(
        response_head_submit_failure_message(buffered_result) == "invalid HTTP/2 response head message");
    RUVIA_CHECK(conn.pending_output().empty());

    ruvia::http_response streaming({.resource_ = &resource});
    streaming.status(ruvia::http_status::upgrade_required);
    streaming.header("Upgrade", "websocket");
    const auto streaming_result = conn.submit_streaming_response_head(1, std::move(streaming),
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submit_failure_message(streaming_result) ==
                "invalid HTTP/2 response head message");
    RUVIA_CHECK(conn.pending_output().empty());

    // Both failures occur before HPACK/stream mutation, so a conformant final
    // response can still be submitted on the same stream.
    ruvia::http_response fallback({.resource_ = &resource});
    fallback.status(ruvia::http_status::bad_request);
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(conn, 1, fallback)));
    RUVIA_CHECK(!conn.pending_output().empty());
}

RUVIA_TEST(http2_connection_rejects_connection_specific_final_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    constexpr std::pair<std::string_view, std::string_view> fields_value[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields_value) {
        ruvia::http_response buffered({.resource_ = &resource});
        if (name == "TE") {
            add_unchecked_header(buffered, name, value);
        } else {
            buffered.header(name, value);
        }
        const auto buffered_result = submit_buffered_response_head(conn, 1, buffered);
        RUVIA_CHECK(response_head_submit_failure_message(buffered_result) ==
                    "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pending_output().empty());

        ruvia::http_response streaming({.resource_ = &resource});
        if (name == "TE") {
            add_unchecked_header(streaming, name, value);
        } else {
            streaming.header(name, value);
        }
        const auto streaming_result = conn.submit_streaming_response_head(1, std::move(streaming),
            ruvia::http_response_stream_kind::generic,
            ruvia::http_response_trailer_intent::none);
        RUVIA_CHECK(response_head_submit_failure_message(streaming_result) ==
                    "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pending_output().empty());
    }

    // Every rejection happened before HPACK and stream mutation, so the same
    // stream can still accept one conformant final response.
    ruvia::http_response fallback({.resource_ = &resource});
    fallback.status(ruvia::http_status::internal_server_error);
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(conn, 1, fallback)));
    RUVIA_CHECK(!conn.pending_output().empty());
}

RUVIA_TEST(http2_connection_streaming_zero_content_length_stays_open_for_finish) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "0");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    const auto head = conn.pending_output();
    const auto head_frame = ruvia::detail::http2_parse_frame_header(head.substr(0, 9));
    RUVIA_CHECK((head_frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(head.size());

    RUVIA_CHECK(conn.submit_data(1, "x", http2_end_stream::keep_open) ==
                http2_data_submit_status::content_length_exceeded);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(
        conn.finish_response(1, validated_trailers({})) == http2_finish_submit_status::accepted);
    const auto terminal = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(terminal.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(terminal.length_, static_cast<std::uint32_t>(0));
    RUVIA_CHECK((terminal.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
}

// Client role end-to-end against the server core with ZERO I/O: the client core opens
// stream 1, sends a GET, the server core dispatches a 200 "pong", and the client core
// surfaces the response head (status via the stream state), body chunk, and end.

RUVIA_TEST(http2_connection_client_role_get_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    server.begin_connection();
    http2_connection client(&resource, http2_role::client);
    client.begin_connection();

    std::string client_body;
    std::uint16_t status = 0;
    bool client_saw_head = false;
    bool client_saw_end = false;
    const auto on_server_event = [&](const http2_event& event) {
        if (const auto* message_end = event.message_end()) {
            const auto stream_id = message_end->stream_id();
            ruvia::http_response response({.resource_ = &resource});
            response.status(ruvia::http_status::ok);
            response.body("pong");
            RUVIA_CHECK(
                response_head_submitted(submit_buffered_response_head(server, stream_id, response)));
            RUVIA_CHECK(server.submit_data(stream_id, "pong", http2_end_stream::end_stream) ==
                        http2_data_submit_status::accepted);
        }
    };
    const auto on_client_event = [&](const http2_event& event) {
        if (const auto* message_head = event.message_head()) {
            client_saw_head = true;
            if (auto* stream = client.stream(message_head->stream_id())) {
                const auto* response_status = stream->response_status();
                RUVIA_CHECK(response_status != nullptr);
                if (response_status != nullptr) {
                    status = response_status->value();
                }
            }
        } else if (const auto* body_chunk = event.message_body_chunk()) {
            client_body.append(body_chunk->bytes().data(), body_chunk->bytes().size());
        } else if (event.message_end() != nullptr) {
            client_saw_end = true;
        }
    };

    const auto request = client.submit_regular_request_head(
        "GET", "http", "example.com", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    RUVIA_CHECK_EQ(stream_id, static_cast<std::uint32_t>(1));
    client.pin_stream(stream_id);
    const auto request_bytes = client.pending_output().size();
    RUVIA_CHECK(client.submit_data(stream_id, "forbidden", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK_EQ(client.pending_output().size(), request_bytes);

    for (int round = 0; round < 4; ++round) {
        shuttle_once(client, server, on_server_event);
        shuttle_once(server, client, on_client_event);
    }

    RUVIA_CHECK(client_saw_head);
    RUVIA_CHECK_EQ(status, static_cast<std::uint16_t>(200));
    RUVIA_CHECK(client_body == "pong");
    RUVIA_CHECK(client_saw_end);
    auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    // content-length was decoded into the stream (auto CL from the server head).
    const auto* remote_known_length = stream->remote_content().allowed_known_length();
    RUVIA_CHECK(remote_known_length != nullptr);
    RUVIA_CHECK_EQ(remote_known_length->declared_length(), std::size_t{4});
    client.unpin_stream(stream_id);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
}

// Client role POST: the request body flows through submit_data with END_STREAM, the
// server core buffers it (owner-side append) and answers; both directions complete.

RUVIA_TEST(http2_connection_client_role_post_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    server.begin_connection();
    http2_connection client(&resource, http2_role::client);
    client.begin_connection();

    std::string server_body;
    std::string client_body;
    bool client_saw_end = false;
    const auto on_server_event = [&](const http2_event& event) {
        if (const auto* body_chunk = event.message_body_chunk()) {
            server_body.append(body_chunk->bytes().data(), body_chunk->bytes().size());
        } else if (const auto* message_end = event.message_end()) {
            const auto stream_id = message_end->stream_id();
            ruvia::http_response response({.resource_ = &resource});
            response.status(ruvia::http_status::ok);
            response.body(server_body);
            RUVIA_CHECK(
                response_head_submitted(submit_buffered_response_head(server, stream_id, response)));
            RUVIA_CHECK(
                server.submit_data(stream_id, std::string_view(server_body.data(), server_body.size()),
                    http2_end_stream::end_stream) == http2_data_submit_status::accepted);
        }
    };
    const auto on_client_event = [&](const http2_event& event) {
        if (const auto* body_chunk = event.message_body_chunk()) {
            client_body.append(body_chunk->bytes().data(), body_chunk->bytes().size());
        } else if (event.message_end() != nullptr) {
            client_saw_end = true;
        }
    };

    const auto request = client.submit_regular_request_head(
        "POST", "http", "example.com", "/echo", {}, http2_request_content::known_length(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    RUVIA_CHECK(client.submit_data(stream_id, "hello", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);

    for (int round = 0; round < 4; ++round) {
        shuttle_once(client, server, on_server_event);
        shuttle_once(server, client, on_client_event);
    }

    RUVIA_CHECK(server_body == "hello");
    RUVIA_CHECK(client_body == "hello");
    RUVIA_CHECK(client_saw_end);
}

RUVIA_TEST(http2_connection_client_rejects_interim_field_flood) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    std::pmr::string interim(&resource);
    hpack_encoder::encode_header(interim, ":status", "103");
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        hpack_encoder::encode_header(interim, "x-hint", "warm");
    }
    const auto frame = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
        std::string_view(interim.data(), interim.size()));
    RUVIA_CHECK(
        client.feed(std::string_view(frame.data(), frame.size())) == http2_feed_result::accepted);

    bool saw_closed = false;
    while (const auto event = client.next_event()) {
        RUVIA_CHECK(event->message_head() == nullptr);
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_closed = true;
            RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
        }
    }
    RUVIA_CHECK(saw_closed);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
    RUVIA_CHECK(!client.connection_error().has_value());

    const auto reset_bytes = client.pending_output();
    RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, stream_id);
}

RUVIA_TEST(http2_connection_client_rejects_forbidden_interim_fields) {
    constexpr std::array forbidden_fields{
        std::pair{std::string_view("content-length"), std::string_view("0")},
        std::pair{std::string_view("trailer"), std::string_view("x-check")},
    };

    for (const auto& [name, value] : forbidden_fields) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection client(&resource, http2_role::client);
        handshake(client);

        const auto request = client.submit_regular_request_head(
            "GET", "https", "example.test", "/", {}, http2_request_content::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto stream_id = submitted_request_stream_id(request);
        client.consume_output(client.pending_output().size());

        std::pmr::string response(&resource);
        hpack_encoder::encode_header(response, ":status", "103");
        hpack_encoder::encode_header(response, name, value);
        const auto response_head =
            headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);

        bool saw_closed = false;
        while (const auto event = client.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            RUVIA_CHECK(event->message_end() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_closed = true;
                RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
                RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
            }
        }
        RUVIA_CHECK(saw_closed);
        RUVIA_CHECK(client.stream(stream_id) == nullptr);
        RUVIA_CHECK(!client.connection_error().has_value());

        const auto reset_bytes = client.pending_output();
        RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(reset.stream_id_, stream_id);
        RUVIA_CHECK_EQ(ruvia::detail::http2_read32(
                           reinterpret_cast<const unsigned char*>(reset_bytes.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
}

RUVIA_TEST(http2_connection_client_validates_interim_representation_field_syntax) {
    const auto check = [&ruvia_ctx](std::string_view name, std::string_view value, bool rejected) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection client(&resource, http2_role::client);
        handshake(client);

        const auto request = client.submit_regular_request_head(
            "GET", "https", "example.test", "/", {}, http2_request_content::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto stream_id = submitted_request_stream_id(request);
        client.consume_output(client.pending_output().size());

        std::pmr::string interim(&resource);
        hpack_encoder::encode_header(interim, ":status", "103");
        hpack_encoder::encode_header(interim, name, value);
        const auto frame = headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers,
            std::string_view(interim.data(), interim.size()));
        RUVIA_CHECK(client.feed(std::string_view(frame.data(), frame.size())) ==
                    http2_feed_result::accepted);

        bool saw_protocol_error = false;
        bool saw_informational_head = false;
        while (const auto event = client.next_event()) {
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            } else if (const auto* informational = event->informational_head()) {
                saw_informational_head = true;
                RUVIA_CHECK(informational->head().status() == ruvia::http_status::early_hints);
                RUVIA_CHECK_EQ(informational->head().headers().size(), std::size_t{1});
            } else {
                RUVIA_CHECK(false);
            }
        }
        RUVIA_CHECK(saw_protocol_error == rejected);
        RUVIA_CHECK(saw_informational_head != rejected);
        RUVIA_CHECK((client.stream(stream_id) == nullptr) == rejected);
    };

    check("content-encoding", "gzip;level=9", true);
    check("content-encoding", ", gzip,,", false);
    check("content-type", "not a media type", true);
    check("content-type", "text/html; charset=utf-8", false);
}

RUVIA_TEST(http2_connection_client_rejects_status_outside_http_range) {
    {
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
        hpack_encoder::encode_header(response, ":status", "599");
        const auto response_head = headers_frame(&resource, stream_id,
            static_cast<std::uint8_t>(
                ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
            std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);

        int message_heads = 0;
        int message_ends = 0;
        while (const auto event = client.next_event()) {
            message_heads += event->message_head() != nullptr ? 1 : 0;
            message_ends += event->message_end() != nullptr ? 1 : 0;
            RUVIA_CHECK(event->stream_closed() == nullptr);
        }
        RUVIA_CHECK_EQ(message_heads, 1);
        RUVIA_CHECK_EQ(message_ends, 1);
        const auto* stream = client.stream(stream_id);
        RUVIA_CHECK(stream != nullptr);
        const auto* response_status = stream->response_status();
        RUVIA_CHECK(response_status != nullptr);
        if (response_status != nullptr) {
            RUVIA_CHECK_EQ(*response_status, ruvia::http_status_code::from_value(599));
        }
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(!client.connection_error().has_value());
        client.unpin_stream(stream_id);
    }

    for (const std::string_view status : {"600", "999"}) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection client(&resource, http2_role::client);
        handshake(client);

        const auto request = client.submit_regular_request_head(
            "GET", "https", "example.test", "/", {}, http2_request_content::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto stream_id = submitted_request_stream_id(request);
        client.consume_output(client.pending_output().size());

        std::pmr::string response(&resource);
        hpack_encoder::encode_header(response, ":status", status);
        const auto response_head = headers_frame(&resource, stream_id,
            static_cast<std::uint8_t>(
                ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
            std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);

        bool saw_closed = false;
        while (const auto event = client.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            RUVIA_CHECK(event->message_end() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_closed = true;
                RUVIA_CHECK(closed->source() == http2_stream_close_source::local);
                RUVIA_CHECK(closed->error() == http2_error_code::protocol_error);
            }
        }
        RUVIA_CHECK(saw_closed);
        RUVIA_CHECK(client.stream(stream_id) == nullptr);
        RUVIA_CHECK(!client.connection_error().has_value());

        const auto reset_bytes = client.pending_output();
        RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(reset.stream_id_, stream_id);
        RUVIA_CHECK_EQ(ruvia::detail::http2_read32(
                           reinterpret_cast<const unsigned char*>(reset_bytes.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
}

RUVIA_TEST(http2_connection_client_rejects_invalid_content_type_syntax) {
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
    hpack_encoder::encode_header(response, "content-type", "not a media type");
    hpack_encoder::encode_header(response, "content-length", "0");
    const auto response_head = headers_frame(&resource, stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);

    bool saw_protocol_error = false;
    while (const auto event = client.next_event()) {
        RUVIA_CHECK(event->message_head() == nullptr);
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                 closed->error() == http2_error_code::protocol_error;
        }
    }
    RUVIA_CHECK(saw_protocol_error);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
    RUVIA_CHECK(!client.connection_error().has_value());

    const auto reset_bytes = client.pending_output();
    RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(reset.stream_id_, stream_id);
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(reset_bytes.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connection_client_rejects_invalid_content_encoding_syntax) {
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
    hpack_encoder::encode_header(response, "content-encoding", "gzip;level=9");
    hpack_encoder::encode_header(response, "content-length", "0");
    const auto response_head = headers_frame(&resource, stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);

    bool saw_protocol_error = false;
    while (const auto event = client.next_event()) {
        RUVIA_CHECK(event->message_head() == nullptr);
        RUVIA_CHECK(event->message_end() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                 closed->error() == http2_error_code::protocol_error;
        }
    }
    RUVIA_CHECK(saw_protocol_error);
    RUVIA_CHECK(client.stream(stream_id) == nullptr);
    RUVIA_CHECK(!client.connection_error().has_value());
}

RUVIA_TEST(http2_connection_client_rejects_invalid_trailer_field_names_in_response_head) {
    for (const std::string_view value : {"Content-Length", "X-Checksum, bad field"}) {
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
        hpack_encoder::encode_header(response, "trailer", value);
        hpack_encoder::encode_header(response, "content-length", "0");
        const auto response_head = headers_frame(&resource, stream_id,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                    http2_feed_result::accepted);

        bool saw_protocol_error = false;
        while (const auto event = client.next_event()) {
            RUVIA_CHECK(event->message_head() == nullptr);
            RUVIA_CHECK(event->message_end() == nullptr);
            if (const auto* closed = event->stream_closed()) {
                saw_protocol_error = closed->source() == http2_stream_close_source::local &&
                                     closed->error() == http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(saw_protocol_error);
        RUVIA_CHECK(client.stream(stream_id) == nullptr);
        RUVIA_CHECK(!client.connection_error().has_value());
    }
}

// --- flood defense-in-depth budgets (GOAWAY ENHANCE_YOUR_CALM) -----------------

// A peer that floods PINGs without ever letting us flush the echoed ACKs is cut off with
// GOAWAY(ENHANCE_YOUR_CALM) instead of accumulating unbounded ACK bytes.

RUVIA_TEST(http2_connection_ping_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2_encode_frame_header(ping, 8, http2_frame_type::ping, 0, 0);
    std::memset(ping + 9, 0, 8);

    bool tripped = false;
    for (int i = 0; i < 1200 && !tripped; ++i) {
        // Deliberately do NOT drain output between pings, so the un-drained PING budget
        // accumulates (consume_output would reset it -- see the keepalive test below).
        tripped = conn.feed(std::string_view(ping, sizeof(ping))) ==
                  ruvia::detail::http2_feed_result::protocol_failure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connection_error().has_value());
    RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()), enhance_your_calm);
}

// A peer that floods non-ACK SETTINGS without ever letting us flush the echoed ACKs is
// cut off with GOAWAY(ENHANCE_YOUR_CALM) instead of accumulating unbounded ACK bytes
// (CVE-2019-9515 SETTINGS flood -- the sibling of the PING flood above).

RUVIA_TEST(http2_connection_settings_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    char settings[9];
    ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);

    bool tripped = false;
    for (int i = 0; i < 1200 && !tripped; ++i) {
        // Deliberately do NOT drain output between frames, so the un-drained SETTINGS
        // budget accumulates (consume_output would reset it -- see the drained test below).
        tripped = conn.feed(std::string_view(settings, sizeof(settings))) ==
                  ruvia::detail::http2_feed_result::protocol_failure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connection_error().has_value());
    RUVIA_CHECK_EQ(first_goaway_error(conn.pending_output()), enhance_your_calm);
}
