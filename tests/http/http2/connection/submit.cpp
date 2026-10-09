#include <array>
#include <new>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response_stream.h"

#include "http2/http2_websocket_handshake.h"
#include "http2_connection_fixture.h"

// http2_connection: submitting request and response heads.

namespace {

using ruvia::http2_data_queue_state;

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

RUVIA_TEST(http2_connection_request_head_requires_started_preface_without_consuming_id) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);

    const auto before_start = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(before_start.submitted() == nullptr);
    RUVIA_CHECK(
        request_head_submit_error(before_start) == http2_request_head_submit_error::connection_not_started);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    begin_client(client);
    const auto accepted = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
}

#if !defined(_MSC_VER)
// Both probes inject failure while outbound PMR strings can grow; see the
// MSVC debug-library limitation documented by the response-head spill test.
RUVIA_TEST(http2_connection_request_head_rolls_back_stream_admission_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    std::pmr::string discarded(&resource);
    client.take_output(discarded);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)client.submit_regular_request_head(
            "GET", "https", "example.test", "/", {}, http2_request_content::none());
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(client.stream(1) == nullptr);
    RUVIA_CHECK(client.pending_output().empty());

    resource.reject_allocations(false);
    const auto retried = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(retried.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(retried), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_response_head_does_not_publish_local_phase_before_output_commit) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    conn.consume_output(conn.pending_output().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream == nullptr) {
        return;
    }

    // Keep HPACK staging allocation-free so the injected failure lands at the
    // outbound reservation, after semantic header preparation but before any
    // response lifecycle state is allowed to publish.
    stream->local_header_block().reserve(64 * 1024);
    const std::string large_value(64'000, 'x');
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.header("X-Large", large_value);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.submit_streaming_response_head(1, std::move(response),
            ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(stream->local_header_block().empty());
    RUVIA_CHECK(stream->local_content().unset() != nullptr);
    RUVIA_CHECK(stream->local_send().head_pending() != nullptr);

    // The same stream remains a valid retry target once the resource recovers.
    resource.reject_allocations(false);
    ruvia::http_response retry({.resource_ = &resource});
    retry.status(ruvia::http_status::ok);
    retry.header("X-Large", large_value);
    const auto retried = conn.submit_streaming_response_head(1, std::move(retry),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(retried.submitted() != nullptr);
    RUVIA_CHECK(stream->local_content().unbounded() != nullptr);
    RUVIA_CHECK(stream->local_send().response_content_open() != nullptr);
}

RUVIA_TEST(http2_connection_websocket_handshake_clears_staged_block_on_encoding_failure) {
    toggle_rejecting_memory_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "websocket");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", "/ws");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    const auto headers = headers_frame(&resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(headers.data(), headers.size())) == http2_feed_result::accepted);
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream == nullptr) {
        return;
    }
    RUVIA_CHECK(stream->tunnel().pending() != nullptr);

    // Fail while encoding the response head. The connection must not retain a
    // partial HPACK block.
    ruvia::detail::http1_server_request_parser negotiation_parser;
    const auto negotiation_request = negotiation_parser.parse_message(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");
    const auto validation = ruvia::detail::validate_http2_websocket_handshake(
        *stream, negotiation_request.request_);

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)conn.submit_websocket_handshake(1, negotiation_request.request_, validation);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK(stream->local_header_block().empty());
    RUVIA_CHECK(stream->tunnel().pending() != nullptr);
    RUVIA_CHECK(stream->local_send().head_pending() != nullptr);

    resource.reject_allocations(false);
    const auto retried =
        conn.submit_websocket_handshake(1, negotiation_request.request_, validation);
    RUVIA_CHECK(retried.submitted() != nullptr);
    RUVIA_CHECK(stream->local_header_block().empty());
    RUVIA_CHECK(stream->tunnel().open() != nullptr);
    RUVIA_CHECK(!conn.pending_output().empty());
}
#endif  // !_MSC_VER

RUVIA_TEST(http2_connection_feed_extension_method_emits_request_event) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "PROPFIND");
    const auto frame = headers_frame(&resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    const auto result_value = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!conn.connection_error().has_value());
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(conn.next_event().value().kind() == http2_event_kind::message_end);
    RUVIA_CHECK(!conn.next_event().has_value());

    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->request_method(), std::string_view("PROPFIND"));
        RUVIA_CHECK(stream->request_known_method() == ruvia::http_known_method::unknown);
    }
}

RUVIA_TEST(http2_connection_feed_accepts_non_http_request_scheme) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encode_request(block, "GET", "gemini", "/", std::nullopt);
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
        RUVIA_CHECK_EQ(stream->request_scheme(), std::string_view("gemini"));
        RUVIA_CHECK_EQ(stream->scheme_default_port(), std::uint16_t{0});
        RUVIA_CHECK(!stream->has_authority());
    }
}

RUVIA_TEST(http2_connection_accepts_http_request_with_host_instead_of_authority) {
    // RFC 9113 §8.3.1: translating HTTP/1.1 origin-form MUST omit :authority
    // and keep Host. Host is the target-URI authority in that case.
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::string_view schemes[] = {"http", "HTTPS"};
    for (const auto scheme : schemes) {
        http2_connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encode_request(block, "GET", scheme, "/resource", std::nullopt);
        hpack_encoder::encode_header(block, "host", "example.com");
        const auto request = headers_frame(&resource, 1,
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                    http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_head);
        auto* stream = server.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK(stream->has_host());
            RUVIA_CHECK(!stream->has_authority());
        }
    }
}

RUVIA_TEST(http2_connection_rejects_http_request_without_authority_or_host) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::string_view schemes[] = {"http", "HTTPS"};
    for (const auto scheme : schemes) {
        http2_connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encode_request(block, "GET", scheme, "/resource", std::nullopt);
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
}

RUVIA_TEST(http2_connection_buffered_response_length_is_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("hello");
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(conn, 1, response)));
    conn.consume_output(conn.pending_output().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(require_local_known_length(*stream).declared_length(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{0});

    // All mismatches are rejected before output, flow-window, counters, or phase
    // change. The caller can correct the submission and continue the same stream.
    RUVIA_CHECK(conn.finish_response(1, validated_trailers({})) ==
                http2_finish_submit_status::content_length_incomplete);
    RUVIA_CHECK(conn.submit_data(1, "four", http2_end_stream::end_stream) ==
                http2_data_submit_status::content_length_incomplete);
    RUVIA_CHECK(conn.submit_data(1, "sixsix", http2_end_stream::keep_open) ==
                http2_data_submit_status::content_length_exceeded);
    RUVIA_CHECK(conn.pending_output().empty());
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->local_send().response_content_open() != nullptr);

    RUVIA_CHECK(
        conn.submit_data(1, "he", http2_end_stream::keep_open) == http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{2});
    RUVIA_CHECK(
        conn.submit_data(1, "llo", http2_end_stream::end_stream) == http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->local_content().length_complete());
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
}

RUVIA_TEST(http2_connection_response_head_submit_result_is_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("ok");

    http2_connection missing_stream(&resource);
    const auto closed = submit_buffered_response_head(missing_stream, 1, response);
    RUVIA_CHECK(closed.submitted() == nullptr);
    RUVIA_CHECK(closed.failure() != nullptr);
    RUVIA_CHECK_EQ(closed.failure()->error(), ruvia::http2_response_head_submit_error::closed);
    RUVIA_CHECK_EQ(ruvia::detail::http2_response_head_submit_error_message(
                       ruvia::http2_response_head_submit_error::closed),
        std::string_view("HTTP/2 response stream is closed"));
    RUVIA_CHECK(missing_stream.pending_output().empty());

    http2_connection buffered(&resource);
    handshake(buffered);
    drive_get_request(buffered, &resource);
    const auto submitted = submit_buffered_response_head(buffered, 1, response);
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK(submitted.failure() == nullptr);
    RUVIA_CHECK_EQ(submitted.submitted()->content_length(), std::uint64_t{2});
    RUVIA_CHECK(buffered.submit_data(1, "ok", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(submitted.submitted()->content_length(), std::uint64_t{2});

    http2_connection streaming(&resource);
    handshake(streaming);
    drive_get_request(streaming, &resource);
    ruvia::http_response streaming_head({.resource_ = &resource});
    streaming_head.status(ruvia::http_status::ok);
    const auto streaming_submitted =
        streaming.submit_streaming_response_head(1, std::move(streaming_head),
            ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(streaming_submitted.submitted() != nullptr);
    RUVIA_CHECK(streaming_submitted.failure() == nullptr);
    RUVIA_CHECK(streaming.submit_data(1, "body", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    RUVIA_CHECK(streaming.finish_response(1, validated_trailers({})) ==
                http2_finish_submit_status::accepted);
    RUVIA_CHECK(streaming_submitted.submitted() != nullptr);
    RUVIA_CHECK(streaming_submitted.failure() == nullptr);
}

RUVIA_TEST(http2_connection_buffered_response_requires_matching_prepared_plan) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection connection(&resource);
    handshake(connection);
    drive_get_request(connection, &resource);

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::multi_status);
    response.body("old");

    const auto wrong_method_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    const auto wrong_method = connection.submit_response_head(1, response, wrong_method_plan);
    RUVIA_CHECK(response_head_submit_failure_message(wrong_method) ==
                "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pending_output().empty());

    const auto stale_representation_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    response.body("longer");
    const auto stale_representation =
        connection.submit_response_head(1, response, stale_representation_plan);
    RUVIA_CHECK(response_head_submit_failure_message(stale_representation) ==
                "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pending_output().empty());

    const auto stale_status_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    response.status(ruvia::http_status::already_reported);
    const auto stale_status = connection.submit_response_head(1, response, stale_status_plan);
    RUVIA_CHECK(response_head_submit_failure_message(stale_status) ==
                "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pending_output().empty());

    const auto submitted = connection.submit_response_head(1, response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
    RUVIA_CHECK(response_head_submitted(submitted));
    const auto& committed_plan = submitted_response_plan(submitted);
    RUVIA_CHECK(committed_plan.request_method() == ruvia::http_known_method::get);
    RUVIA_CHECK_EQ(committed_plan.response_status(), ruvia::http_status::already_reported);
    RUVIA_CHECK_EQ(committed_plan.content_length(), std::uint64_t{6});
}

RUVIA_TEST(http2_connection_rejects_duplicate_response_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response first({.resource_ = &resource});
    first.status(ruvia::http_status::ok);
    const auto first_result = conn.submit_streaming_response_head(1, std::move(first),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(first_result));
    conn.consume_output(conn.pending_output().size());

    ruvia::http_response duplicate({.resource_ = &resource});
    duplicate.status(ruvia::http_status::ok);
    const auto duplicate_result = submit_buffered_response_head(conn, 1, duplicate);
    RUVIA_CHECK(response_head_submit_failure_message(duplicate_result) ==
                "invalid HTTP/2 response head submission state");
    RUVIA_CHECK(conn.pending_output().empty());
}

RUVIA_TEST(http2_connection_rejects_head_api_for_wrong_role) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    drive_get_request(server, &resource);
    RUVIA_CHECK(
        request_head_submit_error(server.submit_regular_request_head("GET", "https", "example.test", "/",
            {}, http2_request_content::none())) == http2_request_head_submit_error::invalid_state);
    RUVIA_CHECK(server.pending_output().empty());

    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const auto request = client.submit_regular_request_head(
        "GET", "https", "example.test", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    const auto result_value = submit_buffered_response_head(client, stream_id, response);
    RUVIA_CHECK(response_head_submit_failure_message(result_value) ==
                "invalid HTTP/2 response head submission state");
    RUVIA_CHECK(client.pending_output().empty());
}

RUVIA_TEST(http2_connection_request_content_alternatives_own_wire_framing) {
    std::pmr::monotonic_buffer_resource resource;

    const auto without_content = http2_request_content::none();
    RUVIA_CHECK(without_content.without_content() != nullptr);
    RUVIA_CHECK(without_content.known_length_content() == nullptr);
    RUVIA_CHECK(without_content.streaming_content() == nullptr);

    const auto zero_length = http2_request_content::known_length(0);
    RUVIA_CHECK(zero_length.without_content() == nullptr);
    RUVIA_CHECK(zero_length.known_length_content() != nullptr);
    RUVIA_CHECK(zero_length.streaming_content() == nullptr);
    if (const auto* known_length = zero_length.known_length_content()) {
        RUVIA_CHECK_EQ(known_length->length(), std::uint64_t{0});
    }

    const auto streaming = http2_request_content::streaming();
    RUVIA_CHECK(streaming.without_content() == nullptr);
    RUVIA_CHECK(streaming.known_length_content() == nullptr);
    RUVIA_CHECK(streaming.streaming_content() != nullptr);

    const auto check = [&resource, &ruvia_ctx](std::string_view method, http2_request_content content,
                           bool expect_end_stream, std::string_view expected_content_length,
                           auto&& verify_local_content) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const auto submit = client.submit_regular_request_head(
            method, "https", "example.test", "/upload", {}, content);
        RUVIA_CHECK(submit.submitted() != nullptr);
        const auto stream_id = submitted_request_stream_id(submit);
        RUVIA_CHECK_EQ(stream_id, static_cast<std::uint32_t>(1));

        const auto out = client.pending_output();
        RUVIA_CHECK(out.size() >= 9);
        const auto frame = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
        RUVIA_CHECK_EQ(frame.stream_id_, stream_id);
        RUVIA_CHECK((frame.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
        RUVIA_CHECK(((frame.flags_ & ruvia::detail::http2_flag_end_stream) != 0) == expect_end_stream);
        RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(9 + frame.length_));

        request_content_length_observation observation;
        hpack_decoder decoder({.resource_ = &resource});
        const auto decode_result =
            decoder.decode(out.substr(9, frame.length_), &observation, &observe_request_content_length);
        RUVIA_CHECK(decode_result.decoded());
        if (expected_content_length.empty()) {
            RUVIA_CHECK_EQ(observation.count_, static_cast<std::size_t>(0));
        } else {
            RUVIA_CHECK_EQ(observation.count_, static_cast<std::size_t>(1));
            RUVIA_CHECK_EQ(observation.value_, std::string(expected_content_length));
        }

        const auto* stream = client.stream(stream_id);
        RUVIA_CHECK(stream != nullptr);
        verify_local_content(stream->local_content());
        RUVIA_CHECK_EQ(stream->local_send().end_stream_committed() != nullptr, expect_end_stream);
    };

    check("GET", http2_request_content::none(), true, {},
        [&ruvia_ctx](const http2_local_content_state& local_content) {
            RUVIA_CHECK(local_content.forbidden() != nullptr);
            RUVIA_CHECK(local_content.known_length() == nullptr);
        });
    check("POST", http2_request_content::known_length(0), true, "0",
        [&ruvia_ctx](const http2_local_content_state& local_content) {
            const auto* known_length = local_content.known_length();
            RUVIA_CHECK(known_length != nullptr);
            if (known_length != nullptr) {
                RUVIA_CHECK_EQ(known_length->declared_length(), std::uint64_t{0});
            }
        });
    check("POST", http2_request_content::known_length(5), false, "5",
        [&ruvia_ctx](const http2_local_content_state& local_content) {
            const auto* known_length = local_content.known_length();
            RUVIA_CHECK(known_length != nullptr);
            if (known_length != nullptr) {
                RUVIA_CHECK_EQ(known_length->declared_length(), std::uint64_t{5});
            }
        });
    check("POST", http2_request_content::streaming(), false, {},
        [&ruvia_ctx](const http2_local_content_state& local_content) {
            RUVIA_CHECK(local_content.unbounded() != nullptr);
            RUVIA_CHECK(local_content.known_length() == nullptr);
        });
}

RUVIA_TEST(http2_connection_enforces_request_method_content_semantics_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check_rejected = [&resource, &ruvia_ctx](std::string_view method,
                                    std::span<const ruvia::http_header_view> headers,
                                    http2_request_content content) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);

        const auto rejected = client.submit_regular_request_head(
            method, "https", "example.test", "/diagnostics", headers, content);
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

    check_rejected("TRACE", {}, http2_request_content::known_length(0));
    check_rejected("TRACE", {}, http2_request_content::known_length(1));
    check_rejected("TRACE", {}, http2_request_content::streaming());
    check_rejected("OPTIONS", {}, http2_request_content::known_length(0));
    check_rejected("OPTIONS", {}, http2_request_content::known_length(1));
    check_rejected("OPTIONS", {}, http2_request_content::streaming());

    const ruvia::http_header_view invalid_content_type[] = {{"content-type", "not a media type"}};
    check_rejected("OPTIONS", invalid_content_type, http2_request_content::known_length(1));

    http2_connection trace_client(&resource, ruvia::detail::http2_role::client);
    begin_client(trace_client);
    const auto trace = trace_client.submit_regular_request_head(
        "TRACE", "https", "example.test", "/diagnostics", {}, http2_request_content::none());
    RUVIA_CHECK(trace.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(trace), std::uint32_t{1});

    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const ruvia::http_header_view content_type_value[] = {{"content-type", "application/json"}};
    const auto accepted = client.submit_regular_request_head("OPTIONS", "https", "example.test",
        "/diagnostics", content_type_value, http2_request_content::known_length(1));
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
}

RUVIA_TEST(
    http2_connection_rejects_repeated_websocket_identity_and_user_agent_fields_transactionally) {
    struct case_value final {
        std::string_view name_;
        std::string_view first_;
        std::string_view second_;
    };
    const case_value cases[] = {
        {"sec-websocket-key", "first", "second"},
        {"sec-websocket-version", "13", "12"},
        {"user-agent", "first/1", "second/2"},
    };
    std::pmr::monotonic_buffer_resource resource;
    for (const auto& test : cases) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const ruvia::http_header_view headers[] = {
            {test.name_, test.first_},
            {test.name_, test.second_},
        };
        const auto rejected = client.submit_regular_request_head(
            "GET", "https", "example.test", "/resource", headers, http2_request_content::none());
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    }
}

RUVIA_TEST(http2_connection_rejects_oversized_outbound_request_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check_rejected = [&resource, &ruvia_ctx](std::string_view path,
                                    std::span<const ruvia::http_header_view> headers,
                                    http2_request_content content) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const auto rejected =
            client.submit_regular_request_head("GET", "https", "example.test", path, headers, content);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const ruvia::http_header_view oversized[] = {
        {"x-oversized", oversized_value},
    };
    check_rejected("/resource", oversized, http2_request_content::none());

    const std::string oversized_path(ruvia::max_http_header_bytes, 'p');
    check_rejected(oversized_path, {}, http2_request_content::none());

    std::array<ruvia::http_header_view, ruvia::max_http_header_fields + 1> too_many{};
    for (auto& header : too_many) {
        header = {"x-many", "value"};
    }
    check_rejected("/resource", too_many, http2_request_content::none());

    std::array<ruvia::http_header_view, ruvia::max_http_header_fields> generated_overflow{};
    for (auto& header : generated_overflow) {
        header = {"x-generated", "value"};
    }
    check_rejected("/resource", generated_overflow, http2_request_content::known_length(0));

    http2_connection connect_client(&resource, ruvia::detail::http2_role::client);
    begin_client(connect_client);
    const auto rejected_connect =
        connect_client.submit_connect_request_head("example.test:443", oversized);
    RUVIA_CHECK(rejected_connect.submitted() == nullptr);
    RUVIA_CHECK(
        request_head_submit_error(rejected_connect) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(connect_client.pending_output().empty());
    RUVIA_CHECK(connect_client.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_rejects_oversized_response_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check_rejected = [&resource, &ruvia_ctx](ruvia::http_response response) {
        http2_connection connection(&resource);
        handshake(connection);
        drive_get_request(connection, &resource);

        const auto rejected = submit_buffered_response_head(connection, 1, response);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            response_head_submit_failure_message(rejected) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(connection.pending_output().empty());
        RUVIA_CHECK(connection.stream(1) != nullptr);
        RUVIA_CHECK(connection.stream(1)->local_send().head_pending() != nullptr);
    };

    ruvia::http_response oversized({.resource_ = &resource});
    oversized.header("X-Oversized", std::string(ruvia::max_http_header_bytes, 'x'));
    check_rejected(std::move(oversized));

    ruvia::http_response too_many({.resource_ = &resource});
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        too_many.header("X-Field-" + std::to_string(i), "value");
    }
    check_rejected(std::move(too_many));

    ruvia::http_response generated_overflow({.resource_ = &resource});
    for (std::size_t i = 0; i < ruvia::max_http_header_fields - 1; ++i) {
        generated_overflow.header("X-Generated-" + std::to_string(i), "value");
    }
    check_rejected(std::move(generated_overflow));
}

RUVIA_TEST(http2_connection_rejects_response_connection_fields_transactionally) {
    constexpr std::array forbidden_names{std::string_view("Connection"),
        std::string_view("Keep-Alive"), std::string_view("Proxy-Connection"),
        std::string_view("Transfer-Encoding"), std::string_view("Upgrade")};
    std::uint64_t state_value = 0x0cb3'9172'57df'a608ULL;
    const auto next_value = [&state_value] {
        state_value ^= state_value << 13U;
        state_value ^= state_value >> 7U;
        state_value ^= state_value << 17U;
        return state_value;
    };
    const auto random_case = [&next_value](std::string_view name) {
        std::string out;
        out.reserve(name.size());
        for (const char c : name) {
            if (c >= 'A' && c <= 'Z') {
                out.push_back((next_value() & 1U) != 0 ? static_cast<char>(c - 'A' + 'a') : c);
            } else if (c >= 'a' && c <= 'z') {
                out.push_back((next_value() & 1U) != 0 ? static_cast<char>(c - 'a' + 'A') : c);
            } else {
                out.push_back(c);
            }
        }
        return out;
    };

    for (std::size_t sample = 0; sample < 512; ++sample) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection connection(&resource);
        handshake(connection);
        drive_get_request(connection, &resource);

        ruvia::http_response response({.resource_ = &resource});
        response.status(ruvia::http_status::ok);
        const auto name = random_case(forbidden_names[sample % forbidden_names.size()]);
        response.header_stable_view(name, "close");

        const auto rejected = submit_buffered_response_head(connection, 1, response);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            response_head_submit_failure_message(rejected) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(connection.pending_output().empty());
        auto* stream = connection.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK(stream->local_send().head_pending() != nullptr);
            RUVIA_CHECK(stream->local_header_block().empty());
        }

        ruvia::http_response retry({.resource_ = &resource});
        retry.status(ruvia::http_status::ok);
        retry.header("X-Retry", "accepted");
        RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(connection, 1, retry)));
        RUVIA_CHECK(!connection.pending_output().empty());
    }
}

RUVIA_TEST(http2_connection_rejects_raw_request_content_length_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    const auto check_rejected = [&resource, &ruvia_ctx](
                                    std::span<const ruvia::http_header_view> headers) {
        http2_connection client(&resource, ruvia::detail::http2_role::client);
        begin_client(client);
        const auto rejected = client.submit_regular_request_head("POST", "https", "example.test",
            "/upload", headers, http2_request_content::known_length(5));
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            request_head_submit_error(rejected) == http2_request_head_submit_error::invalid_message);
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submit_regular_request_head(
            "POST", "https", "example.test", "/upload", {}, http2_request_content::known_length(5));
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submitted_request_stream_id(accepted), std::uint32_t{1});
        RUVIA_CHECK(!client.pending_output().empty());
    };

    const ruvia::http_header_view matching[] = {{"content-length", "5"}};
    const ruvia::http_header_view conflicting[] = {{"content-length", "4"}};
    const ruvia::http_header_view duplicate[] = {{"content-length", "5"}, {"content-length", "5"}};
    const ruvia::http_header_view invalid[] = {{"content-length", "invalid"}};
    check_rejected(matching);
    check_rejected(conflicting);
    check_rejected(duplicate);
    check_rejected(invalid);
}

RUVIA_TEST(http2_connection_encodes_non_http_request_without_authority) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);

    const auto submitted = client.submit_regular_request_head(
        "GET", "git+ssh", std::nullopt, "", {}, http2_request_content::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(submitted), std::uint32_t{1});

    const auto out = client.pending_output();
    const auto frame = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    request_content_length_observation observation;
    hpack_decoder decoder({.resource_ = &resource});
    const auto decoded =
        decoder.decode(out.substr(9, frame.length_), &observation, &observe_request_content_length);
    RUVIA_CHECK(decoded.decoded());
    RUVIA_CHECK_EQ(observation.scheme_, std::string("git+ssh"));
    RUVIA_CHECK_EQ(observation.authority_count_, std::size_t{0});
    RUVIA_CHECK_EQ(observation.path_count_, std::size_t{1});
    RUVIA_CHECK(observation.path_.empty());
    const auto* stream = client.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->request_scheme(), std::string_view("git+ssh"));
    }
}

RUVIA_TEST(http2_connection_encodes_non_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);

    const auto submitted = client.submit_regular_request_head("GET", "git+ssh",
        "deploy:secret@example.test:9418", "/repository", {}, http2_request_content::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(submitted), std::uint32_t{1});

    const auto out = client.pending_output();
    const auto frame = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    request_content_length_observation observation;
    hpack_decoder decoder({.resource_ = &resource});
    const auto decoded =
        decoder.decode(out.substr(9, frame.length_), &observation, &observe_request_content_length);
    RUVIA_CHECK(decoded.decoded());
    RUVIA_CHECK_EQ(observation.authority_count_, std::size_t{1});
    RUVIA_CHECK_EQ(observation.authority_, std::string("deploy:secret@example.test:9418"));
}

RUVIA_TEST(http2_connection_encodes_options_asterisk_path) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);

    const auto submitted = client.submit_regular_request_head(
        "OPTIONS", "https", std::nullopt, "*", {}, http2_request_content::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submitted_request_stream_id(submitted), std::uint32_t{1});
    const auto out = client.pending_output();
    const auto frame = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    request_content_length_observation observation;
    hpack_decoder decoder({.resource_ = &resource});
    const auto decoded =
        decoder.decode(out.substr(9, frame.length_), &observation, &observe_request_content_length);
    RUVIA_CHECK(decoded.decoded());
    RUVIA_CHECK_EQ(observation.path_, std::string("*"));
    RUVIA_CHECK_EQ(observation.authority_count_, std::size_t{0});
}

RUVIA_TEST(http2_connection_exposes_negotiated_extended_connect_capability) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    RUVIA_CHECK(!client.peer_extended_connect_enabled());
    begin_peer_input(client);

    char settings[15];
    auto* out = ruvia::detail::http2_write_frame_header(settings, 6, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::enable_connect_protocol, 1);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(client.peer_extended_connect_enabled());

    http2_connection server(&resource);
    begin_peer_input(server);
    RUVIA_CHECK(server.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.peer_extended_connect_enabled());
}

RUVIA_TEST(http2_connection_request_known_length_is_exact_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const auto request = client.submit_regular_request_head(
        "POST", "https", "example.test", "/upload", {}, http2_request_content::known_length(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(client.submit_data(stream_id, "four", http2_end_stream::end_stream) ==
                http2_data_submit_status::content_length_incomplete);
    RUVIA_CHECK(client.submit_data(stream_id, "sixsix", http2_end_stream::keep_open) ==
                http2_data_submit_status::content_length_exceeded);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->local_send().request_content_open() != nullptr);

    RUVIA_CHECK(client.data_queue_state(stream_id) == http2_data_queue_state::drained);
    RUVIA_CHECK(client.submit_data(stream_id, "he", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    RUVIA_CHECK(client.data_queue_state(stream_id) == http2_data_queue_state::drained);
    auto out = client.pending_output();
    auto data = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(data.length_, static_cast<std::uint32_t>(2));
    RUVIA_CHECK((data.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{2});
    client.consume_output(out.size());

    RUVIA_CHECK(client.submit_data(stream_id, "llo", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    out = client.pending_output();
    data = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length_, static_cast<std::uint32_t>(3));
    RUVIA_CHECK((data.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    RUVIA_CHECK_EQ(stream->local_content().accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->local_content().committed_bytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->local_content().length_complete());
    RUVIA_CHECK(stream->local_send().request_content_open() == nullptr);
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
    client.consume_output(out.size());

    RUVIA_CHECK(client.submit_data(stream_id, "again", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
    RUVIA_CHECK(client.pending_output().empty());
}

RUVIA_TEST(http2_connection_request_streaming_content_has_no_length_contract) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, ruvia::detail::http2_role::client);
    begin_client(client);
    const auto request = client.submit_regular_request_head(
        "POST", "https", "example.test", "/upload", {}, http2_request_content::streaming());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    RUVIA_CHECK(client.submit_data(stream_id, "chunk-a", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    client.consume_output(client.pending_output().size());
    RUVIA_CHECK(client.submit_data(stream_id, "chunk-b", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    const auto out = client.pending_output();
    const auto data = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length_, static_cast<std::uint32_t>(7));
    RUVIA_CHECK((data.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
}

RUVIA_TEST(http2_connection_interim_head_preserves_final_head_phase) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    const ruvia::http_header_view invalid_headers[] = {
        {"Content-Length", "0"},
    };
    const ruvia::http_interim_response_head invalid_early_hints(
        ruvia::http_status::early_hints, invalid_headers);
    RUVIA_CHECK(
        conn.submit_interim_response_head(1, invalid_early_hints) == http2_submit_status::invalid_message);
    RUVIA_CHECK(conn.pending_output().empty());

    const ruvia::http_header_view early_hint_headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::http_interim_response_head early_hints(
        ruvia::http_status::early_hints, early_hint_headers);
    RUVIA_CHECK(conn.submit_interim_response_head(1, early_hints) == http2_submit_status::accepted);
    const auto informational = conn.pending_output();
    const auto info_frame = ruvia::detail::http2_parse_frame_header(informational.substr(0, 9));
    RUVIA_CHECK_EQ(info_frame.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((info_frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    conn.consume_output(informational.size());

    ruvia::http_response final_response({.resource_ = &resource});
    final_response.status(ruvia::http_status::ok);
    const auto final_result = submit_buffered_response_head(conn, 1, final_response);
    RUVIA_CHECK(response_head_submitted(final_result));
    const auto final_head = ruvia::detail::http2_parse_frame_header(conn.pending_output().substr(0, 9));
    RUVIA_CHECK_EQ(final_head.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((final_head.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
}

// submit_streaming_response_head emits HEADERS with NO Content-Length and leaves the
// stream open; subsequent submit_data chunks stream the body, the last with END_STREAM.

RUVIA_TEST(http2_connection_submit_streaming_response_head_and_chunks) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    ruvia::http_response resp({.resource_ = &resource});
    resp.status(ruvia::http_status::ok);
    const auto head_result = conn.submit_streaming_response_head(1, std::move(resp),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
    RUVIA_CHECK(response_head_submitted(head_result));

    const auto head = conn.pending_output();
    const auto hd = ruvia::detail::http2_parse_frame_header(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK((hd.flags_ & ruvia::detail::http2_flag_end_headers) != 0);
    RUVIA_CHECK((hd.flags_ & ruvia::detail::http2_flag_end_stream) == 0);  // stays open
    conn.consume_output(head.size());

    RUVIA_CHECK(conn.submit_data(1, "chunk1", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    RUVIA_CHECK(conn.submit_data(1, "chunk2", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
    const auto body = conn.pending_output();
    const auto d1 = ruvia::detail::http2_parse_frame_header(body.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(d1.length_, static_cast<std::uint32_t>(6));
    RUVIA_CHECK((d1.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    const auto d2 = ruvia::detail::http2_parse_frame_header(body.substr(9 + 6, 9));
    RUVIA_CHECK_EQ(d2.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK((d2.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
}

RUVIA_TEST(http2_connection_streaming_rejects_invalid_content_length_before_head) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);

    for (const std::string_view invalid : {std::string_view{"x"}, std::string_view{"-1"},
             std::string_view{"5,5"}, std::string_view{"18446744073709551616"}}) {
        ruvia::http_response response({.resource_ = &resource});
        response.status(ruvia::http_status::ok);
        response.header("Content-Length", invalid);
        const auto result_value = conn.submit_streaming_response_head(1, std::move(response),
            ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none);
        RUVIA_CHECK(
            response_head_submit_failure_message(result_value) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pending_output().empty());
        auto* stream = conn.stream(1);
        RUVIA_CHECK(stream != nullptr);
        RUVIA_CHECK(stream->local_send().head_pending() != nullptr);
        RUVIA_CHECK(stream->local_content().unset() != nullptr);
    }

    // A valid retry still owns the initial-head transition.
    ruvia::http_response valid({.resource_ = &resource});
    valid.status(ruvia::http_status::ok);
    valid.header("Content-Length", "5");
    RUVIA_CHECK(response_head_submitted(conn.submit_streaming_response_head(1, std::move(valid),
        ruvia::http_response_stream_kind::generic, http_response_trailer_intent::none)));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(require_local_known_length(*stream).declared_length(), std::uint64_t{5});
}

RUVIA_TEST(http2_connection_rejects_invalid_response_trailer_field_names_before_hpack) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check_rejected = [&resource, &ruvia_ctx](std::string_view value) {
        http2_connection conn(&resource);
        handshake(conn);
        drive_get_request(conn, &resource);

        ruvia::http_response response({.resource_ = &resource});
        response.status(ruvia::http_status::ok);
        response.header_stable_view("Trailer", value);
        const auto result_value = submit_buffered_response_head(conn, 1, response);
        RUVIA_CHECK(
            response_head_submit_failure_message(result_value) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pending_output().empty());
        auto* stream = conn.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK(stream->local_send().head_pending() != nullptr);
            RUVIA_CHECK(stream->local_header_block().empty());
        }
    };

    check_rejected("Content-Length");
    check_rejected("X-Checksum, bad field");
    check_rejected(",");

    http2_connection conn(&resource);
    handshake(conn);
    drive_get_request(conn, &resource);
    ruvia::http_response valid({.resource_ = &resource});
    valid.status(ruvia::http_status::ok);
    valid.header("Trailer", "ETag, X-Checksum");
    RUVIA_CHECK(response_head_submitted(submit_buffered_response_head(conn, 1, valid)));
    RUVIA_CHECK(!conn.pending_output().empty());
}

// Every 1xx head is observable, but only the final head completes the request-body
// decision. Hand-encoded server bytes drive the client.

RUVIA_TEST(http2_connection_client_role_surfaces_early_hints_separately) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    client.begin_connection();
    client.consume_output(client.pending_output().size());

    const auto request = client.submit_regular_request_head(
        "GET", "http", "example.com", "/", {}, http2_request_content::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.pin_stream(stream_id);
    client.consume_output(client.pending_output().size());

    // Server bytes: SETTINGS, then HEADERS(103), then HEADERS(200) + DATA END_STREAM.
    std::pmr::string bytes_value(&resource);
    {
        char settings[9];
        ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
        bytes_value.append(settings, sizeof(settings));
        std::pmr::string interim(&resource);
        hpack_encoder::encode_header(interim, ":status", "103");
        hpack_encoder::encode_header(interim, "link", "</style.css>; rel=preload");
        const auto interim_frame = headers_frame(&resource, stream_id,
            ruvia::detail::http2_flag_end_headers, std::string_view(interim.data(), interim.size()));
        bytes_value.append(interim_frame.data(), interim_frame.size());
        std::pmr::string final_(&resource);
        hpack_encoder::encode_header(final_, ":status", "200");
        hpack_encoder::encode_header(final_, "content-length", "2");
        const auto final_frame = headers_frame(&resource, stream_id,
            ruvia::detail::http2_flag_end_headers, std::string_view(final_.data(), final_.size()));
        bytes_value.append(final_frame.data(), final_frame.size());
        char data[9 + 2];
        ruvia::detail::http2_encode_frame_header(
            data, 2, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, stream_id);
        std::memcpy(data + 9, "ok", 2);
        bytes_value.append(data, sizeof(data));
    }
    (void)client.feed(std::string_view(bytes_value.data(), bytes_value.size()));

    int informational_heads = 0;
    int final_heads = 0;
    std::string body;
    bool end = false;
    while (const auto event = client.next_event()) {
        if (const auto* informational = event->informational_head()) {
            ++informational_heads;
            RUVIA_CHECK(informational->head().status() == ruvia::http_status::early_hints);
            RUVIA_CHECK(
                informational->head().protocol_version() == ruvia::http_protocol_version::http2);
            RUVIA_CHECK_EQ(informational->head().headers().size(), std::size_t{1});
            if (!informational->head().headers().empty()) {
                RUVIA_CHECK(informational->head().headers()[0].name() == "link");
                RUVIA_CHECK(
                    informational->head().headers()[0].value() == "</style.css>; rel=preload");
            }
            RUVIA_CHECK(!informational->request_content_signal().has_value());
        }
        if (const auto* head = event->message_head()) {
            ++final_heads;
            RUVIA_CHECK(!head->request_content_signal().has_value());
        }
        if (const auto* body_chunk = event->message_body_chunk()) {
            body.append(body_chunk->bytes().data(), body_chunk->bytes().size());
        }
        if (event->message_end() != nullptr) {
            end = true;
        }
    }
    RUVIA_CHECK_EQ(informational_heads, 1);
    RUVIA_CHECK_EQ(final_heads, 1);
    RUVIA_CHECK(body == "ok");
    RUVIA_CHECK(end);
    auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    const auto* response_status = stream->response_status();
    RUVIA_CHECK(response_status != nullptr);
    if (response_status != nullptr) {
        RUVIA_CHECK_EQ(*response_status, ruvia::http_status::ok);
    }
    RUVIA_CHECK_EQ(static_cast<int>(stream->interim_response_count()), 1);
    RUVIA_CHECK(!client.connection_error().has_value());
}

RUVIA_TEST(http2_connection_continue_releases_only_the_pending_request_body_gate) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head("POST", "https", "example.test", "/upload",
        {}, http2_request_content::known_length(1), ruvia::http_client_request_expectation::continue_value);
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::expectation_pending);

    std::pmr::string hints(&resource);
    hpack_encoder::encode_status(hints, ruvia::http_status::early_hints);
    const auto hints_frame =
        headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers, hints);
    RUVIA_CHECK(client.feed(hints_frame) == http2_feed_result::accepted);
    const auto hints_event = client.next_event();
    RUVIA_CHECK(hints_event.has_value());
    if (hints_event) {
        RUVIA_CHECK(hints_event->informational_head() != nullptr);
        RUVIA_CHECK(!hints_event->informational_head()->request_content_signal().has_value());
    }
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::expectation_pending);

    std::pmr::string continue_block(&resource);
    hpack_encoder::encode_status(continue_block, ruvia::http_status::continue_value);
    const auto continue_frame =
        headers_frame(&resource, stream_id, ruvia::detail::http2_flag_end_headers, continue_block);
    RUVIA_CHECK(client.feed(continue_frame) == http2_feed_result::accepted);
    const auto continue_event = client.next_event();
    RUVIA_CHECK(continue_event.has_value());
    if (continue_event) {
        RUVIA_CHECK(continue_event->informational_head() != nullptr);
        RUVIA_CHECK(continue_event->informational_head()->request_content_signal() ==
                    ruvia::http_client_request_content_signal::continue_value);
    }
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
}

RUVIA_TEST(http2_connection_final_response_cancels_a_pending_request_body_gate) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);

    const auto request = client.submit_regular_request_head("POST", "https", "example.test", "/upload",
        {}, http2_request_content::known_length(1), ruvia::http_client_request_expectation::continue_value);
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    std::pmr::string final_block(&resource);
    hpack_encoder::encode_status(final_block, ruvia::http_status::expectation_failed);
    const auto final_frame = headers_frame(&resource, stream_id,
        static_cast<std::uint8_t>(
            ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
        final_block);
    RUVIA_CHECK(client.feed(final_frame) == http2_feed_result::accepted);
    const auto final_head = client.next_event();
    RUVIA_CHECK(final_head.has_value());
    if (final_head) {
        RUVIA_CHECK(final_head->message_head() != nullptr);
        RUVIA_CHECK(final_head->message_head()->request_content_signal() ==
                    ruvia::http_client_request_content_signal::exchange_complete);
    }
    RUVIA_CHECK(client.next_event()->message_end() != nullptr);
    RUVIA_CHECK(client.submit_data(stream_id, "x", http2_end_stream::end_stream) ==
                http2_data_submit_status::invalid_state);
}

RUVIA_TEST(http2_connection_client_rejects_te_in_every_response_head) {
    constexpr std::array statuses{std::string_view("103"), std::string_view("200")};

    for (const auto status : statuses) {
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
        hpack_encoder::encode_header(response, "te", "trailers");
        const auto response_head = headers_frame(&resource, stream_id,
            static_cast<std::uint8_t>(ruvia::detail::http2_flag_end_headers |
                                      (status == "200" ? ruvia::detail::http2_flag_end_stream : 0)),
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
        RUVIA_CHECK(client.stream(stream_id) == nullptr);
        RUVIA_CHECK(!client.connection_error().has_value());

        const auto reset_bytes = client.pending_output();
        RUVIA_CHECK_EQ(reset_bytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2_parse_frame_header(reset_bytes.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(ruvia::detail::http2_read32(
                           reinterpret_cast<const unsigned char*>(reset_bytes.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
    }
}

RUVIA_TEST(http2_connection_client_accepts_repeated_websocket_version_response_fields) {
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
    hpack_encoder::encode_header(response, ":status", "400");
    hpack_encoder::encode_header(response, "sec-websocket-version", "13");
    hpack_encoder::encode_header(response, "sec-websocket-version", "8, 7");
    const auto response_head = headers_frame(&resource, stream_id,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(response_head.data(), response_head.size())) ==
                http2_feed_result::accepted);

    bool saw_head = false;
    bool saw_end = false;
    bool saw_closed = false;
    while (const auto event = client.next_event()) {
        saw_head = saw_head || event->message_head() != nullptr;
        saw_end = saw_end || event->message_end() != nullptr;
        saw_closed = saw_closed || event->stream_closed() != nullptr;
    }
    RUVIA_CHECK(saw_head);
    RUVIA_CHECK(saw_end);
    RUVIA_CHECK(!saw_closed);
    RUVIA_CHECK(!client.connection_error().has_value());
    client.unpin_stream(stream_id);
}
