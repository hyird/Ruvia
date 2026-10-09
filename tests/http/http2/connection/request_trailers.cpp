#include "http2_connection_fixture.h"

RUVIA_TEST(http2_request_trailers_finish_content_and_reject_framing_fields) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    const auto request = client.submit_regular_request_head("POST", "https", "example.test", "/upload",
        {}, http2_request_content::known_length(2));
    const auto id = submitted_request_stream_id(request);
    using finish = ruvia::http2_finish_request_status;
    RUVIA_CHECK(client.finish_request(id, {}) == finish::content_length_incomplete);
    RUVIA_CHECK(client.submit_data(id, "ok", http2_end_stream::keep_open) == http2_data_submit_status::accepted);
    const std::array forbidden{ruvia::http_header_view{"content-length", "2"}};
    RUVIA_CHECK(client.finish_request(id, forbidden) == finish::invalid_trailer);
    client.consume_output(client.pending_output().size());
    const std::array trailers{ruvia::http_header_view{"x-checksum", "123"}};
    RUVIA_CHECK(client.finish_request(id, trailers) == finish::accepted);
    const auto frame = ruvia::detail::http2_parse_frame_header(client.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    RUVIA_CHECK(client.submit_data(id, "", http2_end_stream::keep_open) == http2_data_submit_status::invalid_state);
    RUVIA_CHECK(client.finish_request(id, trailers) == finish::invalid_state);
}

RUVIA_TEST(http2_request_trailers_follow_window_blocked_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    const auto request = client.submit_regular_request_head("POST", "https", "example.test", "/upload",
        {}, http2_request_content::known_length(70'000));
    const auto id = submitted_request_stream_id(request);
    const std::string body(70'000, 'x');
    RUVIA_CHECK(client.submit_data(id, body, http2_end_stream::keep_open) == http2_data_submit_status::queued);
    const std::array trailers{ruvia::http_header_view{"x-checksum", "done"}};
    RUVIA_CHECK(client.finish_request(id, trailers) == ruvia::http2_finish_request_status::queued);
    client.consume_output(client.pending_output().size());
    std::array<char, 26> updates{};
    ruvia::detail::http2_write_window_update(updates.data(), 0, 70'000);
    ruvia::detail::http2_write_window_update(updates.data() + 13, id, 70'000);
    RUVIA_CHECK(client.feed(std::string_view(updates.data(), updates.size())) == http2_feed_result::accepted);
    auto bytes_value = client.pending_output();
    std::size_t payload_value = 0;
    bool saw_trailer = false;
    while (!bytes_value.empty()) {
        const auto frame = ruvia::detail::http2_parse_frame_header(bytes_value.substr(0, 9));
        if (frame.type_ == static_cast<std::uint8_t>(http2_frame_type::data)) {
            RUVIA_CHECK(!saw_trailer);
            RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
            payload_value += frame.length_;
        } else if (frame.type_ == static_cast<std::uint8_t>(http2_frame_type::headers)) {
            saw_trailer = true;
            RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
        }
        bytes_value.remove_prefix(9 + frame.length_);
    }
    RUVIA_CHECK(payload_value > 0);
    RUVIA_CHECK(saw_trailer);
}
