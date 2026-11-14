#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_decoding_fixture.h"
#include "http2_connection.h"
#include "http_response.h"
#include "http_response_server.h"

namespace {

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload_value) {
    std::array<char, ruvia::http2_frame_header_bytes> header_value{};
    if (!ruvia::encode_http2_frame_header(header_value, static_cast<std::uint32_t>(payload_value.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header_value.data(), header_value.size());
    wire.append(payload_value);
}

void append_header_section(std::string& wire, std::uint32_t stream_id, std::string_view block, bool end_stream, bool fragmented) {
    const auto flags = static_cast<std::uint8_t>(end_stream ? 0x1 : 0);
    if (!fragmented) {
        append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(flags | 0x4), stream_id, block);
        return;
    }
    const auto split = block.size() / 2;
    append_frame(wire, ruvia::http2_frame_type::headers, flags, stream_id, block.substr(0, split));
    append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, stream_id, block.substr(split));
}

void append_peer_settings(std::string& wire) {
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
}

std::string client_response_wire(std::string_view body, bool include_header = false, bool end_stream = true) {
    std::string block;
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    if (include_header) {
        ruvia::hpack_encoder::encode_header(block, "x-test", "value");
    }

    std::string wire;
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0, 1, body);
    return wire;
}

std::string server_request_wire(std::string_view body, std::string_view method = "POST", std::string_view expectation = {}, bool end_stream = true) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", body.empty() ? "0" : "1");
    if (!expectation.empty()) {
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
    }

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    const auto head_flags = static_cast<std::uint8_t>(0x4 | (end_stream && body.empty() ? 0x1 : 0));
    append_frame(wire, ruvia::http2_frame_type::headers, head_flags, 1, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0, 1, body);
    }
    return wire;
}

struct data_output_observations final {
    struct entry_type final {
        std::uint32_t stream_id_{};
        std::size_t payload_bytes_{};
    };
    std::array<entry_type, 4> entries_{};
    std::size_t count_{};
    bool overflow_{};

    static void observe(void* context, std::uint32_t stream_id, std::size_t payload_bytes) noexcept {
        auto& self = *static_cast<data_output_observations*>(context);
        if (self.count_ == self.entries_.size()) {
            self.overflow_ = true;
            return;
        }
        self.entries_[self.count_++] = {stream_id, payload_bytes};
    }
};

ruvia::http2_submit_status submit_buffered_response(ruvia::http2_connection& server, std::uint32_t stream_id, const ruvia::http_response& response) {
    std::variant<ruvia::http_buffered_response_write_plan, ruvia::http2_response_head_submit_error> head = ruvia::http2_response_head_submit_error::response_plan_mismatch;
    for (const auto method : {ruvia::http_known_method::get, ruvia::http_known_method::post, ruvia::http_known_method::options, ruvia::http_known_method::head, ruvia::http_known_method::put, ruvia::http_known_method::delete_value, ruvia::http_known_method::patch, ruvia::http_known_method::connect, ruvia::http_known_method::unknown}) {
        head = server.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(method, response));
        const auto* error = std::get_if<ruvia::http2_response_head_submit_error>(&head);
        if (error == nullptr || *error != ruvia::http2_response_head_submit_error::response_plan_mismatch) {
            break;
        }
    }
    const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&head);
    if (plan == nullptr) {
        const auto error = std::get<ruvia::http2_response_head_submit_error>(head);
        if (error == ruvia::http2_response_head_submit_error::closed) {
            return ruvia::http2_submit_status::closed;
        }
        if (error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
            return ruvia::http2_submit_status::peer_capability_unavailable;
        }
        return ruvia::http2_submit_status::invalid_state;
    }
    if (plan->send_body()) {
        const auto data = server.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, response.body_bytes().size());
        if (data.status_ == ruvia::http2_data_submit_status::closed) {
            return ruvia::http2_submit_status::closed;
        }
        if (data.status_ != ruvia::http2_data_submit_status::complete || data.consumed_bytes_ != response.body_bytes().size()) {
            return ruvia::http2_submit_status::invalid_state;
        }
    }
    return ruvia::http2_submit_status::accepted;
}
}  // namespace

TEST(Ruvia, http2_public_server_release_before_message_end_keeps_terminal_event) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(server.release(std::move(*request->request_head())) == ruvia::http2_server_request_release_status::released);

    auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(end->message_end()->stream_id() == 1);
    EXPECT_TRUE(end->message_end()->trailers().empty());
}

TEST(Ruvia, http2_public_server_request_snapshot_exposes_http_decisions) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event = server.next_event();
    auto* request = event ? event->request_head() : nullptr;
    EXPECT_TRUE(request != nullptr);
    if (request == nullptr) {
        return;
    }
    EXPECT_TRUE(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
    EXPECT_TRUE(request->snapshot().body_open_);
    EXPECT_TRUE(!request->snapshot().connect_pending_);
    const auto plan = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(plan == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, http2_public_semantically_invalid_expectations_preserve_upload_boundaries) {
    for (const std::string_view expectation : {"bad field", "100-continue;foo=bar", "custom=\"unterminated", "100-continue, bad field"}) {
        auto server = ruvia::http2_connection::server({});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        ruvia::hpack_encoder::encode_header(block, "content-length", "3");
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_header_section(wire, 1, block, false, true);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (!request) {
            continue;
        }
        EXPECT_EQ(request->stream_id(), std::uint32_t{1});
        EXPECT_TRUE(request->build_request().header("Expect") == expectation);
        EXPECT_TRUE(request->snapshot().body_open_);
        EXPECT_TRUE(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
        const auto rejected = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
        EXPECT_EQ(ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
        const auto ignored = request->expectation_plan(ruvia::http_unsupported_expectation_policy::ignore);
        const bool known_continue = expectation == "100-continue, bad field";
        EXPECT_TRUE(ignored == (known_continue ? ruvia::http_server_expectation_plan::send_continue : ruvia::http_server_expectation_plan::no_action));
        EXPECT_TRUE(!server.next_event());

        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "a");
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "bc");
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::string body;
        unsigned ends = 0;
        while (auto content = server.next_event()) {
            if (auto* chunk = content->message_body_chunk()) {
                EXPECT_EQ(chunk->stream_id(), std::uint32_t{1});
                body.append(chunk->bytes());
                (void)chunk->take_credit();
            } else {
                const auto* end = content->message_end();
                EXPECT_TRUE(end != nullptr);
                if (end) {
                    EXPECT_EQ(end->stream_id(), std::uint32_t{1});
                    EXPECT_TRUE(end->trailers().empty());
                    ++ends;
                }
            }
        }
        EXPECT_EQ(body, "abc");
        EXPECT_EQ(ends, 1u);
        ruvia::http_response reply;
        EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);

        block.clear();
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        wire.clear();
        append_header_section(wire, 3, block, true, true);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto next = server.next_event();
        EXPECT_TRUE(next && next->request_head());
        if (next && next->request_head()) {
            EXPECT_EQ(next->request_head()->stream_id(), std::uint32_t{3});
            EXPECT_EQ(next->request_head()->request_view().path_, "/next");
            EXPECT_TRUE(submit_buffered_response(server, 3, reply) == ruvia::http2_submit_status::accepted);
            EXPECT_TRUE(server.release(std::move(*next->request_head())) == ruvia::http2_server_request_release_status::released);
        }
        auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end());
        if (end && end->message_end()) {
            EXPECT_EQ(end->message_end()->stream_id(), std::uint32_t{3});
        }
        EXPECT_TRUE(!server.next_event());
        EXPECT_TRUE(!server.connection_error());
    }
}

// body_open_ describes the head as published: a HEADERS frame without
// END_STREAM promises content events even when the DATA that completes the
// request arrived in the same feed() and the core already consumed it.
TEST(Ruvia, http2_public_server_request_snapshot_body_open_survives_batched_content) {
    for (const bool end_stream_on_headers : {false, true}) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(end_stream_on_headers ? 0x5 : 0x4), 1, block);
        if (!end_stream_on_headers) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
        }

        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (request == nullptr) {
            return;
        }
        EXPECT_TRUE(request->snapshot().body_open_ == !end_stream_on_headers);
        EXPECT_EQ(request->snapshot().initial_end_stream_, end_stream_on_headers);
        EXPECT_TRUE(!request->snapshot().connect_pending_);
        if (!end_stream_on_headers) {
            const auto chunk = server.next_event();
            EXPECT_TRUE(chunk && chunk->message_body_chunk() != nullptr && chunk->message_body_chunk()->bytes() == "x");
        }
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() != nullptr);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_dropped_request_before_message_end_keeps_terminal_event) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    request.reset();

    bool saw_end = false;
    while (const auto event = server.next_event()) {
        if (event->message_end() != nullptr) {
            saw_end = true;
            EXPECT_TRUE(event->message_end()->stream_id() == 1);
            EXPECT_TRUE(event->message_end()->trailers().empty());
        }
    }
    EXPECT_TRUE(saw_end);
}

TEST(Ruvia, http2_public_client_reset_before_terminal_events_drain_keeps_events_readable) {
    auto client = ruvia::http2_connection::client({});
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    auto wire = client_response_wire({}, false, true);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto reset = client.submit_reset(1, ruvia::http2_error_code::cancel);
    EXPECT_TRUE(reset == ruvia::http2_submit_status::accepted);

    bool saw_head = false;
    bool saw_end = false;
    while (const auto event = client.next_event()) {
        if (event->response_head() != nullptr) {
            saw_head = true;
            EXPECT_TRUE(event->response_head()->head().status() == ruvia::http_status::ok);
        }
        if (event->message_end() != nullptr) {
            saw_end = true;
            EXPECT_TRUE(event->message_end()->trailers().empty());
        }
    }
    EXPECT_TRUE(saw_head);
    EXPECT_TRUE(saw_end);
}

TEST(Ruvia, http2_public_streaming_known_length_keeps_upload_open_for_trailers) {
    for (const std::uint64_t length : {0U, 3U}) {
        auto client = ruvia::http2_connection::client({});
        auto server = ruvia::http2_connection::server({});
        ruvia::test::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto head = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming(length)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(head));
        exchange_value(client, server);
        bool saw_end = false;
        std::optional<ruvia::http2_request_head_event> lease;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                EXPECT_EQ(request->build_request().header("content-length").value_or(""), length == 0 ? "0" : "3");
                lease.emplace(std::move(*request));
            }
            saw_end |= event->message_end() != nullptr;
        }
        EXPECT_TRUE(!saw_end);
        const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
        if (length != 0) {
            // Content short of the declared length is a value; nothing changes.
            EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::invalid_message);
            EXPECT_TRUE(client.submit_data(1, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
        // So is a trailer field that may not appear in request trailers.
        const std::array<ruvia::http_header_view, 1> forbidden{{{"content-length", "3"}}};
        EXPECT_TRUE(client.finish_request(1, forbidden) == ruvia::http2_finish_status::invalid_message);
        EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
        exchange_value(client, server);
        while (auto event = server.next_event()) {
            if (const auto* end = event->message_end()) {
                saw_end = true;
                EXPECT_EQ(end->trailers().size(), std::size_t{1});
                if (!end->trailers().empty()) {
                    EXPECT_EQ(end->trailers()[0].value(), "retained");
                }
            }
        }
        EXPECT_TRUE(saw_end);
    }
}

TEST(Ruvia, http2_public_push_request_lease_preserves_fields_and_releases_repeated_streams) {
    {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server({});
        ruvia::test::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        exchange_value(client, server);
        std::optional<ruvia::http2_request_head_event> parent;
        while (auto event = server.next_event()) {
            if (auto* head = event->request_head()) {
                parent.emplace(std::move(*head));
            }
        }
        EXPECT_TRUE(parent.has_value());
        const std::array<ruvia::http_header_view, 3> headers{{{"host", "EXAMPLE.test:443"}, {"cookie", "a=1"}, {"x-push", "request"}}};
        for (unsigned repeat = 0; repeat != 100; ++repeat) {
            auto pushed = server.submit_push_request(std::get<std::uint32_t>(submitted), {.authority_ = "example.test", .path_ = "/asset?version=1", .headers_ = headers});
            EXPECT_TRUE((pushed.index() == 0));
            if ((pushed.index() != 0)) {
                break;
            }
            const auto request = std::get<0>(pushed).build_request();
            EXPECT_TRUE(request.scheme() == "https" && request.authority() == "example.test");
            EXPECT_TRUE(request.path() == "/asset" && request.query_string() == "version=1");
            EXPECT_TRUE(request.header("cookie") == "a=1");
            EXPECT_TRUE(request.header("host") == "EXAMPLE.test:443");
            EXPECT_TRUE(request.header("x-push") == "request");
            ruvia::http_response response;
            EXPECT_TRUE(submit_buffered_response(server, std::get<0>(pushed).stream_id(), response) == ruvia::http2_submit_status::accepted);
            exchange_value(server, client);
            unsigned promised = 0;
            unsigned ended = 0;
            while (auto event = client.next_event()) {
                if (const auto* promise = event->push_promise()) {
                    ++promised;
                    EXPECT_TRUE(promise->request_.path_ == "/asset?version=1");
                }
                if (event->message_end()) {
                    ++ended;
                }
            }
            EXPECT_TRUE(promised == 1 && ended == 1);
            EXPECT_TRUE(server.release(std::move(std::get<0>(pushed))) == ruvia::http2_server_request_release_status::released);
            exchange_value(client, server);
            while (server.next_event()) {
            }
        }
        EXPECT_TRUE(server.release(std::move(*parent)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_request_reset_keeps_events_consumable) {
    for (const bool split_input : {false, true}) {
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        auto wire = server_request_wire({});
        if (split_input) {
            const auto prefix_size = wire.size() - 1;
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(0, prefix_size)) == ruvia::http2_feed_result::accepted);
            wire.erase(0, prefix_size);
        }
        const char cancel[] = {0, 0, 0, 8};
        append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view(cancel, sizeof(cancel)));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!server.connection_error().has_value());
        bool saw_close = false;
        std::size_t request_heads = 0;
        while (auto event = server.next_event()) {
            saw_close = saw_close || event->stream_closed() != nullptr;
            if (auto* request = event->request_head()) {
                ++request_heads;
                EXPECT_EQ(request->request_view().path_, std::string_view("/upload"));
                EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
            }
        }
        EXPECT_TRUE(saw_close);
        EXPECT_EQ(request_heads, std::size_t{1});
    }
}

TEST(Ruvia, http2_public_closed_request_lease_does_not_overflow_settings) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto request_wire = server_request_wire({});
    EXPECT_TRUE(server.feed(request_wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    while (server.next_event()) {
    }
    std::string wire;
    const char increment[] = {0x7f, static_cast<char>(0xff), 0, 0};
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view(increment, sizeof(increment)));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    while (server.next_event()) {
    }
    (void)ruvia::test::drain_output(server);
    const char settings[] = {0, 4, 0, 1, 0, 0};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(settings, sizeof(settings)));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    if (request_head != nullptr) {
        EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_server_refuses_stream_beyond_advertised_concurrency) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");

    // The server advertises SETTINGS_MAX_CONCURRENT_STREAMS = 128. One HEADERS
    // beyond that limit is a stream error (RFC 9113 5.1.2), not a connection error.
    constexpr std::uint32_t advertised_limit = 128;
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    for (std::uint32_t index = 0; index <= advertised_limit; ++index) {
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, index * 2 + 1, block);
    }
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());

    std::string refused;
    constexpr std::array<char, 4> refused_stream_payload{0, 0, 0, 7};
    append_frame(refused, ruvia::http2_frame_type::rst_stream, 0, advertised_limit * 2 + 1, std::string_view(refused_stream_payload.data(), refused_stream_payload.size()));
    EXPECT_TRUE(ruvia::test::drain_output(server).ends_with(std::string_view(refused)));

    std::size_t request_heads = 0;
    while (auto event = server.next_event()) {
        if (event->request_head() != nullptr) {
            ++request_heads;
        }
    }
    EXPECT_EQ(request_heads, std::size_t{advertised_limit});
}

TEST(Ruvia, http2_public_reports_available_client_request_streams_without_reserving) {
    auto server = ruvia::http2_connection::server({});
    EXPECT_EQ(server.available_request_streams(4), std::size_t{0});

    auto client = ruvia::http2_connection::client({});
    EXPECT_EQ(client.available_request_streams(1000), std::size_t{128});
    EXPECT_EQ(client.available_request_streams(0), std::size_t{0});
    std::string settings;
    constexpr std::array<char, 6> peer_limit{0, 3, 0, 0, 0, 2};
    append_frame(settings, ruvia::http2_frame_type::settings, 0, 0, std::string_view(peer_limit.data(), peer_limit.size()));
    EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    EXPECT_EQ(client.available_request_streams(4), std::size_t{2});
    EXPECT_EQ(client.available_request_streams(1), std::size_t{1});

    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto* stream_id = std::get_if<std::uint32_t>(&submitted);
    EXPECT_TRUE(stream_id != nullptr);
    EXPECT_EQ(client.available_request_streams(4), std::size_t{1});
    if (stream_id != nullptr) {
        EXPECT_TRUE(client.submit_reset(*stream_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
    }
    EXPECT_EQ(client.available_request_streams(4), std::size_t{2});

    client.begin_drain();
    EXPECT_EQ(client.available_request_streams(4), std::size_t{0});
}

namespace {

bool http2_client_submits_field(std::string_view name, std::string_view value) {
    auto client = ruvia::http2_connection::client({});
    const std::array headers{ruvia::http_header_view{name, value}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/", .headers_ = headers, .content_ = ruvia::http_request_content_plan::streaming()});
    return std::holds_alternative<std::uint32_t>(submitted);
}

bool http2_server_accepts_field(std::string_view name, std::string_view value) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    ruvia::hpack_encoder::encode_header(block, name, value);
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    if (server.feed(wire) != ruvia::http2_feed_result::accepted) {
        return false;
    }
    while (auto event = server.next_event()) {
        if (event->request_head() != nullptr) {
            return true;
        }
    }
    return false;
}

auto submit_field_request(ruvia::http2_connection& client, unsigned entry, std::span<const ruvia::http_header_view> fields, ruvia::http_request_field_source source) {
    switch (entry) {
        case 0:
            return client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test", .headers_ = fields, .field_source_ = source});
        case 1:
            return client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming(), .field_source_ = source});
        case 2:
            return client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443", .headers_ = fields, .field_source_ = source});
        default:
            return client.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "example", .authority_ = "example.test:443", .target_ = "/tunnel", .headers_ = fields, .field_source_ = source});
    }
}

}  // namespace

TEST(Ruvia, http2_public_client_generates_framing_and_expectation_fields_itself) {
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"expect", "100-continue"}, {"Expect", "foo=bar"}, {"content-length", "0"}, {"Content-Length", "3"}}) {
        EXPECT_TRUE(!http2_client_submits_field(name, value));
    }
}

TEST(Ruvia, http2_public_te_allows_only_trailers_with_list_ows) {
    for (const std::string_view value : {"trailers", "TRAILERS", "trailers , trailers"}) {
        EXPECT_TRUE(http2_client_submits_field("te", value));
        EXPECT_TRUE(http2_server_accepts_field("te", value));
    }
    for (const std::string_view value : {"gzip", "trailers, gzip", "trailers;q=1"}) {
        EXPECT_TRUE(!http2_client_submits_field("te", value));
        EXPECT_TRUE(!http2_server_accepts_field("te", value));
    }
}

TEST(Ruvia, http2_public_received_te_accepts_empty_lists_without_dropping_fields) {
    for (const std::string_view value : {"", ",,", ", trailers,,"}) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, "te", value);
        ruvia::hpack_encoder::encode_header(block, "x-after", "kept");
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (request == nullptr) {
            continue;
        }
        const auto built_req = request->build_request();
        EXPECT_TRUE(built_req.header("te") == std::optional<std::string_view>{value});
        EXPECT_TRUE(built_req.header("x-after") == std::string_view("kept"));
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
        EXPECT_TRUE(!server.connection_error());
        ruvia::http_response response;
        EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_normalized_request_capacity_accepts_many_fields_within_byte_budget) {
    for (const std::size_t fields : {std::size_t{63}, std::size_t{80}}) {
        for (const bool explicit_host : {false, true}) {
            for (const unsigned cookies : {0U, 1U, 2U}) {
                std::string wire(ruvia::http2_client_preface);
                append_peer_settings(wire);
                std::string block;
                const auto encode_target = [&](std::string_view path) {
                    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
                    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
                    ruvia::hpack_encoder::encode_header(block, ":path", path);
                };
                encode_target("/capacity?x=kept");
                if (explicit_host) {
                    ruvia::hpack_encoder::encode_header(block, "host", "EXAMPLE.test:443");
                }
                for (unsigned i = 0; i < cookies; ++i) {
                    ruvia::hpack_encoder::encode_header(block, "cookie", i == 0 ? "a=1" : "b=2");
                }
                for (std::size_t i = static_cast<std::size_t>(explicit_host) + cookies; i < fields; ++i) {
                    ruvia::hpack_encoder::encode_header(block, "x-pad", "value");
                }
                append_header_section(wire, 1, block, true, true);
                block.clear();
                encode_target("/next");
                append_header_section(wire, 3, block, true, false);
                auto server = ruvia::http2_connection::server({});
                EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
                const auto normalized = fields - cookies + static_cast<std::size_t>(cookies != 0) + static_cast<std::size_t>(!explicit_host);
                unsigned first_heads = 0;
                unsigned first_ends = 0;
                unsigned resets = 0;
                unsigned following_heads = 0;
                unsigned following_ends = 0;
                while (auto event = server.next_event()) {
                    if (auto* head = event->request_head()) {
                        const auto id = head->stream_id();
                        if (id == 1) {
                            ++first_heads;
                            const auto built_req = head->build_request();
                            EXPECT_EQ(built_req.headers().size(), normalized);
                            EXPECT_EQ(built_req.header("Host").value_or(""), explicit_host ? "EXAMPLE.test:443" : "example.test");
                            EXPECT_EQ(built_req.query_string(), "x=kept");
                            if (cookies != 0) {
                                EXPECT_EQ(built_req.header("Cookie").value_or(""), cookies == 1 ? "a=1" : "a=1; b=2");
                            }
                        } else {
                            ++following_heads;
                            EXPECT_TRUE(id == 3 && head->request_view().path_ == "/next");
                        }
                        ruvia::http_response reply;
                        EXPECT_TRUE(submit_buffered_response(server, id, reply) == ruvia::http2_submit_status::accepted);
                        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
                    } else if (const auto* end = event->message_end()) {
                        if (end->stream_id() == 1) {
                            ++first_ends;
                        } else if (end->stream_id() == 3) {
                            ++following_ends;
                        }
                    } else if (const auto* closed = event->stream_closed()) {
                        if (closed->stream_id() == 1 && closed->source() == ruvia::http2_stream_close_source::local && closed->error() == ruvia::http2_error_code::protocol_error) {
                            ++resets;
                        }
                    }
                }
                EXPECT_TRUE(first_heads == 1U);
                EXPECT_TRUE(first_ends == 1U);
                EXPECT_TRUE(resets == 0U);
                EXPECT_TRUE(following_heads == 1 && following_ends == 1 && !server.connection_error());
            }
        }
    }
}

TEST(Ruvia, http2_public_forwards_received_application_values_through_hpack) {
    const std::array cases{ruvia::http_header_view{"content-type", "not-a-media-type"}, ruvia::http_header_view{"origin", "https://example.test/path"}, ruvia::http_header_view{"access-control-request-method", "GET, POST"}, ruvia::http_header_view{"access-control-request-headers", "X-Good, Bad Header"}, ruvia::http_header_view{"content-encoding", ""}, ruvia::http_header_view{"content-encoding", ",,"}, ruvia::http_header_view{"content-encoding", ", gzip,, br,"}, ruvia::http_header_view{"content-encoding", ", unknown-coding,,"}};
    for (const auto& field : cases) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, field.name(), field.value());
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        auto incoming = ruvia::http2_connection::server({});
        EXPECT_TRUE(incoming.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = incoming.next_event();
        auto* received_head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(received_head != nullptr);
        if (!received_head) {
            continue;
        }
        const auto received_request = received_head->build_request();
        const auto received = received_request.header(field.name());
        EXPECT_TRUE(received == std::optional<std::string_view>{field.value()});
        if (!received) {
            continue;
        }
        const std::array fields{ruvia::http_header_view{field.name(), *received}};
        for (unsigned entry = 0; entry != 4; ++entry) {
            ruvia::test::output_relay relay;
            auto client = ruvia::http2_connection::client({});
            auto upstream = ruvia::http2_connection::server({});
            relay.transfer(client, upstream);
            relay.transfer(upstream, client);
            relay.transfer(client, upstream);
            const auto generated = submit_field_request(client, entry, fields, ruvia::http_request_field_source::generated);
            EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
            EXPECT_TRUE(!client.wants_write());
            const auto forwarded = submit_field_request(client, entry, fields, ruvia::http_request_field_source::forwarded);
            const auto* id = std::get_if<std::uint32_t>(&forwarded);
            EXPECT_TRUE(id != nullptr);
            if (!id) {
                continue;
            }
            EXPECT_EQ(*id, std::uint32_t{1});
            relay.transfer(client, upstream);
            auto forwarded_event = upstream.next_event();
            auto* forwarded_head = forwarded_event ? forwarded_event->request_head() : nullptr;
            EXPECT_TRUE(forwarded_head != nullptr);
            if (forwarded_head) {
                EXPECT_EQ(forwarded_head->stream_id(), *id);
                const auto request = forwarded_head->build_request();
                EXPECT_TRUE(request.header(field.name()) == received);
            }
            EXPECT_TRUE(!upstream.connection_error());
        }
    }
}

TEST(Ruvia, http2_public_forwarding_never_relaxes_owned_hop_by_hop_or_singleton_fields) {
    const std::array invalid{ruvia::http_header_view{"x-test", "ok\r\ninjected: yes"}, ruvia::http_header_view{"bad name", "value"}, ruvia::http_header_view{"host", "other.test"}, ruvia::http_header_view{"content-length", "0"}, ruvia::http_header_view{"expect", "100-continue"}, ruvia::http_header_view{"transfer-encoding", "chunked"}, ruvia::http_header_view{"connection", "upgrade"}, ruvia::http_header_view{"upgrade", ",websocket,"}, ruvia::http_header_view{"keep-alive", "timeout=5"}, ruvia::http_header_view{"proxy-connection", "keep-alive"}, ruvia::http_header_view{"te", ",trailers,"}, ruvia::http_header_view{"content-encoding", ",gzip;level=9,"}, ruvia::http_header_view{"content-encoding", ",bad coding,"}};
    for (unsigned entry = 0; entry != 4; ++entry) {
        ruvia::test::output_relay relay;
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto reject = [&](std::span<const ruvia::http_header_view> fields, ruvia::http_request_field_source source) {
            const auto result = submit_field_request(client, entry, fields, source);
            const auto* error = std::get_if<ruvia::http2_request_head_submit_error>(&result);
            EXPECT_TRUE(error != nullptr);
            if (error) {
                EXPECT_TRUE(*error == ruvia::http2_request_head_submit_error::invalid_message);
            }
            EXPECT_TRUE(!client.wants_write());
        };
        for (const auto& field : invalid) {
            const std::array fields{field};
            reject(fields, ruvia::http_request_field_source::forwarded);
        }
        for (const auto& field : std::array{ruvia::http_header_view{"content-type", "invalid"}, ruvia::http_header_view{"origin", "invalid"}, ruvia::http_header_view{"access-control-request-method", "GET, POST"}, ruvia::http_header_view{"authorization", "Bearer token"}, ruvia::http_header_view{"user-agent", "agent"}, ruvia::http_header_view{"if-modified-since", "invalid"}, ruvia::http_header_view{"if-unmodified-since", "invalid"}}) {
            const std::array fields{field, field};
            reject(fields, ruvia::http_request_field_source::forwarded);
        }
        reject({}, static_cast<ruvia::http_request_field_source>(0xff));
        const std::array valid{ruvia::http_header_view{"content-encoding", ",gzip,,"}};
        const auto accepted = submit_field_request(client, entry, valid, ruvia::http_request_field_source::forwarded);
        const auto* id = std::get_if<std::uint32_t>(&accepted);
        EXPECT_TRUE(id && *id == 1);
        relay.transfer(client, server);
        auto event = server.next_event();
        EXPECT_TRUE(event && event->request_head());
    }
}

TEST(Ruvia, http2_public_forwarded_options_content_keeps_missing_media_type) {
    for (const auto content : {ruvia::http_request_content_plan::known_length(1), ruvia::http_request_content_plan::streaming(1), ruvia::http_request_content_plan::streaming()}) {
        const auto incoming_wire = server_request_wire("x", "OPTIONS");
        auto incoming = ruvia::http2_connection::server({});
        EXPECT_TRUE(incoming.feed(incoming_wire) == ruvia::http2_feed_result::accepted);
        auto received = incoming.next_event();
        EXPECT_TRUE(received && received->request_head());
        std::string body;
        while (auto next = incoming.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                body.append(chunk->bytes());
            }
        }
        EXPECT_EQ(body, "x");
        ruvia::test::output_relay relay;
        auto client = ruvia::http2_connection::client({});
        auto upstream = ruvia::http2_connection::server({});
        relay.transfer(client, upstream);
        relay.transfer(upstream, client);
        relay.transfer(client, upstream);
        ruvia::http2_regular_request_head_view request{.method_ = "OPTIONS", .authority_ = "example.test", .content_ = content};
        const auto generated = client.submit_request_head(request);
        EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
        request.method_ = "TRACE";
        request.field_source_ = ruvia::http_request_field_source::forwarded;
        const auto trace = client.submit_request_head(request);
        EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&trace) != nullptr);
        EXPECT_TRUE(!client.wants_write());
        request.method_ = "OPTIONS";
        const auto forwarded = client.submit_request_head(request);
        const auto* id = std::get_if<std::uint32_t>(&forwarded);
        EXPECT_TRUE(id && *id == 1);
        if (!id) {
            continue;
        }
        EXPECT_TRUE(client.submit_data(*id, body, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        relay.transfer(client, upstream);
        auto event = upstream.next_event();
        auto* head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head) {
            const auto forwarded_request = head->build_request();
            EXPECT_TRUE(!forwarded_request.header("content-type"));
        }
        bool ended = false;
        std::string forwarded_body;
        while (auto next = upstream.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                forwarded_body.append(chunk->bytes());
            }
            ended |= next->message_end() != nullptr;
        }
        EXPECT_TRUE(ended);
        EXPECT_EQ(forwarded_body, body);
        EXPECT_TRUE(!upstream.connection_error());
    }
}

TEST(Ruvia, http2_public_trace_zero_length_accepts_header_or_empty_data_endings) {
    for (const bool head_end : {false, true}) {
        auto wire = server_request_wire({}, "TRACE", {}, head_end);
        if (!head_end) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, {});
        }
        auto server = ruvia::http2_connection::server({});
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = server.next_event();
        auto* request = head ? head->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (!request) {
            continue;
        }
        const auto built_req = request->build_request();
        EXPECT_EQ(built_req.method(), "TRACE");
        EXPECT_EQ(built_req.header("Content-Length").value_or(""), "0");
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
        EXPECT_TRUE(!server.next_event());
        ruvia::http_response reply;
        EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_known_empty_requests_never_wait_for_continue_or_send_content) {
    for (const std::string_view method : {"GET", "OPTIONS", "TRACE"}) {
        for (const bool streaming : {false, true}) {
            auto client = ruvia::http2_connection::client({});
            std::string wire(ruvia::test::drain_output(client));
            ruvia::http2_regular_request_head_view head{.method_ = method, .authority_ = "example.test", .content_ = streaming ? ruvia::http_request_content_plan::streaming(0) : ruvia::http_request_content_plan::known_length(0), .expectation_ = ruvia::http_client_request_expectation::continue_value};
            const auto rejected = client.submit_request_head(head);
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) && std::get<ruvia::http2_request_head_submit_error>(rejected) == ruvia::http2_request_head_submit_error::invalid_message);
            EXPECT_TRUE(!client.wants_write());
            head.expectation_ = ruvia::http_client_request_expectation::none;
            const auto submitted = client.submit_request_head(head);
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted) && std::get<std::uint32_t>(submitted) == 1);
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            // A known empty head ended the stream: DATA is an ordering error.
            // A streaming zero stays open, and content beyond it is a value.
            if (streaming) {
                const auto overrun = client.submit_data(1, "x", ruvia::http2_end_stream::keep_open, 65535);
                EXPECT_TRUE(overrun.status_ == ruvia::http2_data_submit_status::invalid_message && overrun.consumed_bytes_ == 0);
            } else {
                EXPECT_TRUE(ruvia::test::throws_on([&] {
                    (void)client.submit_data(1, "x", ruvia::http2_end_stream::keep_open, 65535);
                }));
            }
            if (streaming) {
                EXPECT_TRUE(client.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
            }
            wire.append(ruvia::test::drain_output(client));
            auto server = ruvia::http2_connection::server({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request_event = server.next_event();
            auto* request = request_event ? request_event->request_head() : nullptr;
            EXPECT_TRUE(request != nullptr);
            if (!request) {
                continue;
            }
            const auto built_req = request->build_request();
            EXPECT_EQ(built_req.method(), method);
            EXPECT_TRUE(!built_req.header("Content-Type"));
            EXPECT_TRUE(!built_req.header("Expect"));
            EXPECT_TRUE(built_req.header("Content-Length") == (method == "TRACE" ? std::nullopt : std::optional<std::string_view>{"0"}));
            const auto end = server.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            EXPECT_TRUE(!server.next_event());
            ruvia::http_response reply;
            EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
            EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
        }
    }
}

TEST(Ruvia, http2_public_options_without_content_type_preserves_body_and_following_stream) {
    for (const std::string_view body : {"", "abc"}) {
        for (const bool declared_length : {false, true}) {
            std::string wire(ruvia::http2_client_preface);
            append_peer_settings(wire);
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "OPTIONS");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "*");
            if (declared_length) {
                char length_buffer[20];
                const auto length_end = std::to_chars(length_buffer, length_buffer + sizeof(length_buffer), body.size()).ptr;
                const std::string_view length(length_buffer, length_end);
                ruvia::hpack_encoder::encode_header(block, "content-length", length);
            }
            append_header_section(wire, 1, block, false, true);
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
            block.clear();
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "/next");
            append_header_section(wire, 3, block, true, false);
            auto server = ruvia::http2_connection::server({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto first = server.next_event();
            auto* options = first ? first->request_head() : nullptr;
            EXPECT_TRUE(options != nullptr);
            if (options == nullptr) {
                continue;
            }
            const auto built_options = options->build_request();
            EXPECT_EQ(built_options.method(), "OPTIONS");
            EXPECT_EQ(built_options.path(), "*");
            EXPECT_TRUE(!built_options.header("Content-Type"));
            if (!body.empty()) {
                auto data = server.next_event();
                auto* chunk = data ? data->message_body_chunk() : nullptr;
                EXPECT_TRUE(chunk && chunk->stream_id() == 1 && chunk->bytes() == body);
                if (chunk) {
                    (void)chunk->take_credit();
                }
            }
            const auto end = server.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            auto next = server.next_event();
            auto* following = next ? next->request_head() : nullptr;
            EXPECT_TRUE(following && following->stream_id() == 3 && following->request_view().path_ == "/next");
            const auto next_end = server.next_event();
            EXPECT_TRUE(next_end && next_end->message_end() && next_end->message_end()->stream_id() == 3);
            EXPECT_TRUE(!server.connection_error());
            for (auto* request : {options, following}) {
                if (request) {
                    ruvia::http_response reply;
                    EXPECT_TRUE(submit_buffered_response(server, request->stream_id(), reply) == ruvia::http2_submit_status::accepted);
                    EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

TEST(Ruvia, http2_public_opaque_metadata_preserves_request_and_response_bodies) {
    for (const bool server_role : {true, false}) {
        for (const std::string_view field_name : {"content-type", "content-encoding", "origin", "access-control-request-method", "access-control-request-headers", "trailer"}) {
            for (const std::string_view value : {"", "not a media type", "text/plain; charset=\"unfinished"}) {
                std::string block;
                if (server_role) {
                    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
                    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
                    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
                    ruvia::hpack_encoder::encode_header(block, ":path", "/");
                } else {
                    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
                }
                ruvia::hpack_encoder::encode_header(block, field_name, value);
                ruvia::hpack_encoder::encode_header(block, "content-length", "3");
                ruvia::hpack_encoder::encode_header(block, "x-after", "kept");
                std::string wire;
                if (server_role) {
                    wire.append(ruvia::http2_client_preface);
                }
                append_peer_settings(wire);
                append_header_section(wire, 1, block, false, true);
                append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "abc");
                auto connection = server_role ? ruvia::http2_connection::server({}) : ruvia::http2_connection::client({});
                if (!server_role) {
                    const auto submitted = connection.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
                    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
                }
                EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
                auto head = connection.next_event();
                auto* request = head ? head->request_head() : nullptr;
                const auto* response = head ? head->response_head() : nullptr;
                EXPECT_TRUE(server_role ? request != nullptr : response != nullptr);
                if (server_role ? request == nullptr : response == nullptr) {
                    continue;
                }
                if (request != nullptr) {
                    const auto built_req = request->build_request();
                    EXPECT_TRUE(built_req.header(field_name) == std::optional<std::string_view>{value});
                    EXPECT_TRUE(built_req.header("x-after") == std::string_view("kept"));
                } else {
                    const auto fields = response->head().headers();
                    const auto metadata = std::find_if(fields.begin(), fields.end(), [field_name](const auto& field) {
                        return field.name() == field_name;
                    });
                    EXPECT_TRUE(metadata != fields.end() && metadata->value() == value);
                    const auto after = std::find_if(fields.begin(), fields.end(), [](const auto& field) {
                        return field.name() == "x-after";
                    });
                    EXPECT_TRUE(after != fields.end() && after->value() == "kept");
                }
                auto body = connection.next_event();
                auto* chunk = body ? body->message_body_chunk() : nullptr;
                EXPECT_TRUE(chunk != nullptr);
                if (chunk != nullptr) {
                    EXPECT_EQ(chunk->bytes(), "abc");
                    (void)chunk->take_credit();
                }
                const auto end = connection.next_event();
                EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
                EXPECT_TRUE(!connection.connection_error());
                if (request != nullptr) {
                    ruvia::http_response reply;
                    EXPECT_TRUE(submit_buffered_response(connection, 1, reply) == ruvia::http2_submit_status::accepted);
                    EXPECT_TRUE(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

TEST(Ruvia, http2_public_sender_rejects_invalid_request_metadata) {
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"origin", "https://app.example/path"}, {"access-control-request-method", "POST GET"}, {"access-control-request-headers", "x bad"}, {"trailer", "Content-Length"}, {"trailer", "bad field"}, {"trailer", "\"x-name\""}}) {
        EXPECT_TRUE(!http2_client_submits_field(name, value));
    }
    EXPECT_TRUE(http2_client_submits_field("origin", "https://app.example"));
    EXPECT_TRUE(http2_server_accepts_field("origin", "https://app.example"));
}

TEST(Ruvia, http2_public_completed_response_preserves_request_send_half_and_retires_credits) {
    for (const bool reset : {false, true}) {
        auto client = ruvia::http2_connection::client({});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming(3)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        auto wire = client_response_wire("ok");
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_received_data_credit> credit;
        bool ended = false;
        std::string received;
        while (auto event = client.next_event()) {
            if (auto* chunk = event->message_body_chunk()) {
                received.append(chunk->bytes());
                credit.emplace(chunk->take_credit());
            }
            ended |= event->message_end() != nullptr;
        }
        EXPECT_EQ(received, "ok");
        EXPECT_TRUE(ended);
        if (!reset) {
            credit.reset();
        }
        EXPECT_TRUE(client.submit_data(id, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        if (reset) {
            EXPECT_TRUE(client.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        } else {
            EXPECT_TRUE(client.finish_request(id) == ruvia::http2_finish_status::accepted);
        }
        credit.reset();
        while (client.next_event()) {
        }
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_completed_response_keeps_unconsumed_request_suffix_retryable) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::known_length(65536)});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    const std::string payload(65536, 'b');
    const auto submitted = client.submit_data(id, payload, ruvia::http2_end_stream::end_stream, payload.size());
    EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{65535});
    std::string output;
    ruvia::test::drain_output(client, output);
    auto wire = client_response_wire({});
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    bool ended = false;
    while (auto event = client.next_event()) {
        ended |= event->message_end() != nullptr;
    }
    EXPECT_TRUE(ended);
    EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
    (void)ruvia::test::drain_output(client);
    wire.clear();
    const std::string_view increment("\0\0\0\1", 4);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, increment);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.wants_write());
    const auto retried = client.submit_data(id, std::string_view(payload).substr(submitted.consumed_bytes_), ruvia::http2_end_stream::end_stream, 1);
    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_EQ(retried.consumed_bytes_, std::size_t{1});
    EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{1});
    const auto final_output = ruvia::test::drain_output(client);
    const auto frame = ruvia::parse_http2_frame_header(final_output);
    EXPECT_TRUE(frame && frame->length_ == 1 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
    EXPECT_EQ(final_output.size(), ruvia::http2_frame_header_bytes + 1);
}

TEST(Ruvia, http2_public_data_consumption_is_bounded_and_does_not_retain_input_suffixes) {
    for (const std::uint32_t window : {0U, 2U, 65535U}) {
        for (const std::size_t allowance : {std::size_t{0}, std::size_t{1}, std::size_t{6}}) {
            auto client = ruvia::http2_connection::client({});
            std::string wire;
            const std::array<char, 6> setting{0, 4, static_cast<char>(window >> 24), static_cast<char>(window >> 16), static_cast<char>(window >> 8), static_cast<char>(window)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(setting.data(), setting.size()));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(6)});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            if (!std::holds_alternative<std::uint32_t>(request)) {
                continue;
            }
            const auto id = std::get<std::uint32_t>(request);
            (void)ruvia::test::drain_output(client);
            const auto expected_count = std::min({std::size_t{6}, static_cast<std::size_t>(window), allowance});
            ruvia::http2_data_submit_result submitted{};
            {
                std::string input = "abcdef";
                submitted = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, allowance);
                input.assign(input.size(), 'x');
            }
            EXPECT_EQ(submitted.consumed_bytes_, expected_count);
            EXPECT_TRUE(submitted.status_ == (expected_count == 6 ? ruvia::http2_data_submit_status::complete : ruvia::http2_data_submit_status::backpressured));
            EXPECT_EQ(client.pending_data_output_bytes(id), expected_count);
            auto output = ruvia::test::drain_output(client);
            std::string expected(std::string_view("abcdef").substr(0, expected_count));
            if (expected_count != 6) {
                wire.clear();
                append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, std::string_view("\0\0\0\6", 4));
                EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
                EXPECT_TRUE(!client.wants_write());
                EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
                const std::string replacement(6 - expected_count, 'z');
                const auto retried = client.submit_data(id, replacement, ruvia::http2_end_stream::end_stream, replacement.size());
                EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
                EXPECT_EQ(retried.consumed_bytes_, replacement.size());
                expected.append(replacement);
                output.append(ruvia::test::drain_output(client));
            }
            std::string received;
            unsigned terminal_frames = 0;
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(remaining);
                EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                received.append(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_));
                terminal_frames += (frame->flags_ & 1U) != 0;
                if ((frame->flags_ & 1U) != 0) {
                    EXPECT_EQ(received.size(), std::size_t{6});
                }
                remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
            }
            EXPECT_TRUE(remaining.empty());
            EXPECT_EQ(received, expected);
            EXPECT_EQ(terminal_frames, 1U);
            EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        }
    }
}

// RFC 9113 §6.9: even an empty DATA frame waits for positive connection and
// stream windows. An empty terminal submission or finish without trailers is
// backpressured without changing state until both windows open.
TEST(Ruvia, http2_public_empty_end_stream_waits_for_both_send_windows) {
    for (const bool finish : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto filled_request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(65535)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(filled_request));
        if (!std::holds_alternative<std::uint32_t>(filled_request)) {
            return;
        }
        const std::string filling(65535, 'f');
        const auto filled = client.submit_data(std::get<std::uint32_t>(filled_request), filling, ruvia::http2_end_stream::end_stream, filling.size());
        EXPECT_TRUE(filled.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(filled.consumed_bytes_, filling.size());
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming(0)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto end_stream = [&](bool sent) {
            if (finish) {
                EXPECT_TRUE(client.finish_request(id) == (sent ? ruvia::http2_finish_status::accepted : ruvia::http2_finish_status::backpressured));
                return;
            }
            const auto result = client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
            EXPECT_EQ(result.consumed_bytes_, std::size_t{0});
            EXPECT_TRUE(result.status_ == (sent ? ruvia::http2_data_submit_status::complete : ruvia::http2_data_submit_status::backpressured));
        };
        const auto credit = [&](std::uint32_t stream_id) {
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::window_update, 0, stream_id, std::string_view("\0\0\0\1", 4));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        for (const std::uint32_t opened : {0U, id}) {
            const auto window = client.send_window_state(id);
            EXPECT_TRUE(window && (window->connection_window_ == 0 || window->stream_window_ == 0));
            end_stream(false);
            EXPECT_TRUE(!client.wants_write());
            EXPECT_TRUE(client.send_window_state(id).has_value());
            credit(opened);
        }
        end_stream(true);
        const auto output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && frame->length_ == 0 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
        EXPECT_EQ(output.size(), ruvia::http2_frame_header_bytes);
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
        }));
        EXPECT_TRUE(!client.wants_write());
    }
}

TEST(Ruvia, http2_public_data_validates_terminal_length_before_partial_consumption) {
    for (const std::size_t allowance : {std::size_t{0}, std::size_t{2}, std::size_t{8}}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto before = client.send_window_state(id);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            const auto rejected = client.submit_data(id, invalid, ruvia::http2_end_stream::end_stream, allowance);
            EXPECT_TRUE(rejected.status_ == ruvia::http2_data_submit_status::invalid_message && rejected.consumed_bytes_ == 0);
            EXPECT_TRUE(!client.wants_write());
            EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
            const auto after = client.send_window_state(id);
            EXPECT_TRUE(before && after && before->connection_window_ == after->connection_window_ && before->stream_window_ == after->stream_window_);
        }
        const auto first = client.submit_data(id, "abcd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(first.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{2});
        auto output = ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0).status_ == ruvia::http2_data_submit_status::invalid_message);
        EXPECT_TRUE(client.submit_data(id, "cde", ruvia::http2_end_stream::end_stream, 0).status_ == ruvia::http2_data_submit_status::invalid_message);
        EXPECT_TRUE(!client.wants_write());
        const auto second = client.submit_data(id, "cd", ruvia::http2_end_stream::end_stream, 1);
        EXPECT_TRUE(second.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(second.consumed_bytes_, std::size_t{1});
        const auto last = client.submit_data(id, "d", ruvia::http2_end_stream::end_stream, 1);
        EXPECT_TRUE(last.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(last.consumed_bytes_, std::size_t{1});
        output.append(ruvia::test::drain_output(client));
        std::string_view remaining(output);
        for (const auto payload : {std::string_view("ab"), std::string_view("c"), std::string_view("d")}) {
            const auto frame = ruvia::parse_http2_frame_header(remaining);
            EXPECT_TRUE(frame && frame->length_ == payload.size() && ((frame->flags_ & 1U) != 0) == (payload == "d"));
            if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                break;
            }
            EXPECT_TRUE(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_) == payload);
            remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
        }
        EXPECT_TRUE(remaining.empty());
    }
}

TEST(Ruvia, http2_public_response_data_counts_only_consumed_bytes_toward_content_length) {
    for (const bool buffered : {false, true}) {
        auto server = ruvia::http2_connection::server();
        const auto wire = server_request_wire({}, "GET");
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            continue;
        }
        EXPECT_TRUE(server.next_event()->message_end() != nullptr);
        ruvia::http_response response;
        if (buffered) {
            response.body("abcd");
            EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(server.submit_response_head(1, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
        } else {
            response.header("content-length", "4");
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(1, std::move(response))));
        }
        (void)ruvia::test::drain_output(server);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            EXPECT_TRUE(server.submit_data(1, invalid, ruvia::http2_end_stream::end_stream, 2).status_ == ruvia::http2_data_submit_status::invalid_message);
            EXPECT_TRUE(!server.wants_write());
            EXPECT_TRUE(server.send_window_state(1)->available_ == 65535);
        }
        const auto first = server.submit_data(1, "abcd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(first.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto prefix = ruvia::test::drain_output(server);
        const auto prefix_frame = ruvia::parse_http2_frame_header(prefix);
        EXPECT_TRUE(prefix_frame && prefix_frame->length_ == 2 && (prefix_frame->flags_ & 1U) == 0);
        EXPECT_TRUE(std::string_view(prefix).substr(ruvia::http2_frame_header_bytes) == "ab");
        const auto trailers = ruvia::validate_http_response_trailers(std::span<const ruvia::http_header_view>{});
        EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::invalid_message);
        EXPECT_TRUE(!server.wants_write());
        const auto final = server.submit_data(1, "cd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(final.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(final.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto suffix = ruvia::test::drain_output(server);
        const auto suffix_frame = ruvia::parse_http2_frame_header(suffix);
        EXPECT_TRUE(suffix_frame && suffix_frame->length_ == 2 && (suffix_frame->flags_ & 1U) != 0);
        EXPECT_TRUE(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == "cd");
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_expectation_gate_does_not_consume_or_retain_data) {
    for (const bool timeout_release : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4), .expectation_ = ruvia::http_client_request_expectation::continue_value});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        {
            const std::string input = "body";
            const auto pending = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, input.size());
            EXPECT_TRUE(pending.status_ == ruvia::http2_data_submit_status::expectation_pending);
            EXPECT_EQ(pending.consumed_bytes_, std::size_t{0});
        }
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_TRUE(!client.wants_write());
        if (timeout_release) {
            (void)client.release_request_content(id);
        } else {
            std::string wire;
            append_peer_settings(wire);
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":status", "100");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, id, block);
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            while (client.next_event()) {
            }
            (void)ruvia::test::drain_output(client);
        }
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        const auto retried = client.submit_data(id, "next", ruvia::http2_end_stream::end_stream, 4);
        EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(retried.consumed_bytes_, std::size_t{4});
        const auto output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        EXPECT_TRUE(frame && frame->length_ == 4 && (frame->flags_ & 1U) != 0);
        EXPECT_TRUE(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == "next");
    }
}

TEST(Ruvia, http2_public_reset_and_goaway_discard_no_caller_owned_suffix) {
    for (const bool goaway : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto submitted = client.submit_data(id, "body", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(submitted.consumed_bytes_, std::size_t{2});
        (void)ruvia::test::drain_output(client);
        std::string wire;
        append_peer_settings(wire);
        if (goaway) {
            append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
        } else {
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, id, std::string_view("\0\0\0\10", 4));
        }
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        bool closed = false;
        while (auto event = client.next_event()) {
            closed |= goaway ? event->request_unprocessed() != nullptr : event->stream_closed() != nullptr;
        }
        EXPECT_TRUE(closed);
        EXPECT_TRUE(client.stream_aborted(id));
        (void)ruvia::test::drain_output(client);
        const auto retry = client.submit_data(id, "dy", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(retry.status_ == ruvia::http2_data_submit_status::closed);
        EXPECT_EQ(retry.consumed_bytes_, std::size_t{0});
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_TRUE(!client.wants_write());
    }
}

// Window-clamped DATA is serialized completely within each submission, so
// streams keep their submission order and both send windows stay exact.
TEST(Ruvia, http2_public_window_clamped_data_preserves_stream_order_and_windows) {
    {
        auto client = ruvia::http2_connection::client({});
        std::string wire;
        const auto set_window = [&](std::uint32_t size) {
            wire.clear();
            const std::array<char, 6> setting{0, 4, static_cast<char>(size >> 24), static_cast<char>(size >> 16), static_cast<char>(size >> 8), static_cast<char>(size)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(setting.data(), setting.size()));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        set_window(0);
        const std::array bodies{std::string(4097, 'a'), std::string(1024, 'b'), std::string(8192, 'd')};
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(bodies[i].size())});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            const auto blocked = client.submit_data(id, bodies[i], ruvia::http2_end_stream::end_stream, bodies[i].size());
            EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
            EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
        }
        {
            std::string discarded;
            ruvia::test::drain_output(client, discarded);
        }
        std::vector<std::uint32_t> frame_order;
        std::array<std::string, 3> received;
        std::array<unsigned, 3> terminal_frames{};
        const auto collect = [&] {
            const auto output = ruvia::test::drain_output(client);
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(std::span(remaining.data(), remaining.size()));
                EXPECT_TRUE(frame && remaining.size() >= ruvia::http2_frame_header_bytes + frame->length_);
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    const auto index = (frame->stream_id_ - 1) / 2;
                    EXPECT_TRUE(index < received.size());
                    if (index < received.size()) {
                        frame_order.push_back(frame->stream_id_);
                        received[index].append(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_));
                        terminal_frames[index] += (frame->flags_ & 1U) != 0;
                    }
                }
                remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
            }
        };
        set_window(4096);
        collect();
        EXPECT_TRUE(frame_order.empty());
        std::array<std::size_t, 3> consumed{};
        const auto first = client.submit_data(1, bodies[0], ruvia::http2_end_stream::end_stream, 4096);
        consumed[0] = first.consumed_bytes_;
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(consumed[0], std::size_t{4096});
        for (std::size_t i = 1; i < bodies.size(); ++i) {
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1), bodies[i], ruvia::http2_end_stream::end_stream, 4096);
            consumed[i] = result.consumed_bytes_;
            EXPECT_TRUE(result.status_ == (i == 1 ? ruvia::http2_data_submit_status::complete : ruvia::http2_data_submit_status::backpressured));
        }
        collect();
        EXPECT_EQ(consumed[1], std::size_t{1024});
        EXPECT_EQ(consumed[2], std::size_t{4096});
        set_window(8192);
        collect();
        EXPECT_TRUE((frame_order == std::vector<std::uint32_t>{1, 3, 5}));
        for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
            const auto suffix = std::string_view(bodies[i]).substr(consumed[i]);
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1), suffix, ruvia::http2_end_stream::end_stream, suffix.size());
            EXPECT_TRUE(result.status_ == ruvia::http2_data_submit_status::complete);
            EXPECT_EQ(result.consumed_bytes_, suffix.size());
            consumed[i] += result.consumed_bytes_;
        }
        collect();
        EXPECT_TRUE((frame_order == std::vector<std::uint32_t>{1, 3, 5, 1, 5}));
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            EXPECT_TRUE(received[i] == bodies[i]);
            EXPECT_EQ(terminal_frames[i], 1U);
            EXPECT_EQ(consumed[i], bodies[i].size());
            const auto window = client.send_window_state(id);
            EXPECT_TRUE(window && window->connection_window_ == 52222 && window->stream_window_ == static_cast<std::int32_t>(8192 - bodies[i].size()));
        }
    }
}

TEST(Ruvia, http2_output_observer_can_submit_new_data_without_losing_it) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::test::drain_output(client);
    EXPECT_TRUE(client.submit_data(id, "first", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    struct observation final {
        ruvia::http2_connection* client_;
        std::optional<ruvia::http2_data_submit_result> submitted_;
        bool failed_{};
    } observed{&client};
    std::string first;
    const auto batch = client.take_output_batch(16384, first, [](void* context, std::uint32_t stream, std::size_t) noexcept {
        auto& state = *static_cast<observation*>(context);
        try {
            state.submitted_ = state.client_->submit_data(stream, "second", ruvia::http2_end_stream::end_stream, 65535);
        } catch (...) {
            state.failed_ = true;
        }
    }, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_TRUE(!observed.failed_);
    EXPECT_TRUE(observed.submitted_ && observed.submitted_->status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_TRUE(observed.submitted_ && observed.submitted_->consumed_bytes_ == 6);
    EXPECT_TRUE(client.wants_write());
    std::string second;
    ruvia::test::drain_output(client, second);
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(second.data(), second.size()));
    EXPECT_TRUE(frame && frame->stream_id_ == id && frame->length_ == 6);
    if (frame) {
        EXPECT_TRUE(std::string_view(second).substr(ruvia::http2_frame_header_bytes) == "second");
    }
}

TEST(Ruvia, http2_output_observer_can_recursively_drain_and_append_frames) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::test::drain_output(client);
    for (const auto payload : {"a", "b", "c", "d"}) {
        EXPECT_TRUE(client.submit_data(id, payload, ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    }
    struct observation final {
        ruvia::http2_connection* client_;
        std::string* nested_;
        std::size_t calls_{};
        bool failed_{};
        static void on_data(void* context, std::uint32_t stream, std::size_t bytes) noexcept {
            auto& state = *static_cast<observation*>(context);
            ++state.calls_;
            if (bytes != 1) {
                state.failed_ = true;
            }
            if (state.calls_ != 1) {
                return;
            }
            try {
                const auto batch = state.client_->take_output_batch(16384, *state.nested_, &on_data, context);
                state.failed_ |= batch.status_ != ruvia::http2_output_batch_status::taken;
                for (int i = 0; i < 40; ++i) {
                    state.failed_ |= state.client_->submit_data(stream, "x", ruvia::http2_end_stream::keep_open, 65535).status_ != ruvia::http2_data_submit_status::complete;
                }
                state.failed_ |= state.client_->submit_data(stream, "z", ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete;
            } catch (...) {
                state.failed_ = true;
            }
        }
    };
    std::string nested;
    observation observed{&client, &nested};
    std::string first;
    const auto batch = client.take_output_batch(20, first, &observation::on_data, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_TRUE(!observed.failed_);
    EXPECT_EQ(observed.calls_, std::size_t{4});
    EXPECT_EQ(first.size(), std::size_t{20});
    EXPECT_EQ(nested.size(), std::size_t{20});
    std::string remaining;
    ruvia::test::drain_output(client, remaining);
    std::string payload;
    std::size_t offset = 0;
    while (offset < remaining.size()) {
        const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(remaining.data() + offset, remaining.size() - offset));
        if (!frame || frame->stream_id_ != id || frame->length_ != 1 || remaining.size() - offset < ruvia::http2_frame_header_bytes + frame->length_) {
            break;
        }
        payload.push_back(remaining[offset + ruvia::http2_frame_header_bytes]);
        offset += ruvia::http2_frame_header_bytes + frame->length_;
    }
    EXPECT_EQ(offset, remaining.size());
    EXPECT_TRUE(payload == std::string(40, 'x') + "z");
}

TEST(Ruvia, http2_public_full_output_batch_transfers_storage_and_accounts_data) {
    {
        auto client = ruvia::http2_connection::client({});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const std::string body(8192, 'b');
        EXPECT_TRUE(client.submit_data(id, body, ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        std::string output;
        data_output_observations observed;
        ruvia::http2_output_batch_result batch;
        bool threw = false;
        try {
            batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(!threw);
        if (threw) {
            return;
        }
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, body.size() + ruvia::http2_frame_header_bytes);
        const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && frame->stream_id_ == id && frame->length_ == body.size() && frame->flags_ == 0);
        if (frame) {
            EXPECT_TRUE(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == body);
        }
        EXPECT_TRUE(!client.wants_write());
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_EQ(observed.count_, std::size_t{1});
        EXPECT_TRUE(observed.entries_[0].stream_id_ == id && observed.entries_[0].payload_bytes_ == body.size());

        output.clear();
        EXPECT_TRUE(client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(0, output, &data_output_observations::observe, &observed);
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes);
        const auto empty_end = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        EXPECT_TRUE(empty_end && empty_end->stream_id_ == id && empty_end->length_ == 0 && empty_end->flags_ == 1);
        EXPECT_EQ(observed.count_, std::size_t{2});
        EXPECT_TRUE(observed.entries_[1].stream_id_ == id && observed.entries_[1].payload_bytes_ == 0);

        const auto next = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/next", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(next));
        if (!std::holds_alternative<std::uint32_t>(next)) {
            return;
        }
        const auto next_id = std::get<std::uint32_t>(next);
        (void)ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(next_id, "tail", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + 4);
        EXPECT_EQ(output.size(), 2 * ruvia::http2_frame_header_bytes + 4);
        if (output.size() >= ruvia::http2_frame_header_bytes) {
            const auto tail = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()).subspan(ruvia::http2_frame_header_bytes));
            EXPECT_TRUE(tail && tail->stream_id_ == next_id && tail->length_ == 4 && tail->flags_ == 1);
            if (tail) {
                EXPECT_TRUE(std::string_view(output).substr(2 * ruvia::http2_frame_header_bytes) == "tail");
            }
        }
        EXPECT_EQ(observed.count_, std::size_t{3});
        EXPECT_TRUE(observed.entries_[2].stream_id_ == next_id && observed.entries_[2].payload_bytes_ == 4);
        EXPECT_TRUE(!observed.overflow_);
        EXPECT_TRUE(client.take_output_batch(16384, output, &data_output_observations::observe, &observed).status_ == ruvia::http2_output_batch_status::empty);
        EXPECT_EQ(observed.count_, std::size_t{3});
    }
}

TEST(Ruvia, http2_public_output_tail_preserves_storage_transfer_and_data_observations) {
    {
        auto client = ruvia::http2_connection::client({});
        (void)ruvia::test::drain_output(client);
        const std::string padding(4096, 'p');
        const std::array fields{ruvia::http_header_view{"x-padding", padding}};
        std::array<std::uint32_t, 2> ids{};
        for (std::size_t index = 0; index < 64; ++index) {
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming()});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            if (!std::holds_alternative<std::uint32_t>(request)) {
                return;
            }
            if (index < ids.size()) {
                ids[index] = std::get<std::uint32_t>(request);
            }
        }
        const std::array<std::string_view, 2> bodies{"left", "right"};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            EXPECT_TRUE(client.submit_data(ids[index], bodies[index], ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
        std::string prefix;
        data_output_observations observed;
        const auto first = client.take_output_batch(160 * 1024, prefix, &data_output_observations::observe, &observed);
        EXPECT_TRUE(first.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_TRUE(client.wants_write());
        EXPECT_EQ(observed.count_, std::size_t{0});

        std::string tail;
        bool threw = false;
        try {
            const auto last = client.take_output_batch((std::numeric_limits<std::size_t>::max)(), tail, &data_output_observations::observe, &observed);
            EXPECT_TRUE(last.status_ == ruvia::http2_output_batch_status::taken);
            EXPECT_EQ(last.bytes_, tail.size());
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(!threw);
        EXPECT_TRUE(!client.wants_write());
        EXPECT_EQ(observed.count_, ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index) {
            EXPECT_EQ(client.pending_data_output_bytes(ids[index]), std::size_t{0});
            EXPECT_TRUE(observed.entries_[index].stream_id_ == ids[index] && observed.entries_[index].payload_bytes_ == bodies[index].size());
        }
        std::string_view pending(tail);
        std::size_t data_index = 0;
        while (!pending.empty()) {
            const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(pending.data(), pending.size()));
            EXPECT_TRUE(frame && frame->length_ <= pending.size() - ruvia::http2_frame_header_bytes);
            if (!frame || frame->length_ > pending.size() - ruvia::http2_frame_header_bytes) {
                break;
            }
            const auto payload = pending.substr(ruvia::http2_frame_header_bytes, frame->length_);
            if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                EXPECT_TRUE(data_index < ids.size());
                if (data_index < ids.size()) {
                    EXPECT_EQ(frame->stream_id_, ids[data_index]);
                    EXPECT_EQ(payload, bodies[data_index]);
                }
                ++data_index;
            }
            pending.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
        }
        EXPECT_EQ(data_index, ids.size());
    }
}

TEST(Ruvia, http2_public_output_batch_copy_failure_preserves_frame_boundaries) {
    auto client = ruvia::http2_connection::client({});
    std::array<std::uint32_t, 3> ids{};
    const std::array<std::string, 3> bodies{std::string(32, 'a'), std::string(16, 'b'), std::string(8, 'c')};
    for (auto& id : ids) {
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        id = std::get<std::uint32_t>(request);
    }
    (void)ruvia::test::drain_output(client);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        EXPECT_TRUE(client.submit_data(ids[index], bodies[index], ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    }
    data_output_observations observed;
    std::string copied = "prefix";
    const auto first_bytes = ruvia::http2_frame_header_bytes + bodies[0].size();
    auto batch = client.take_output_batch(first_bytes, copied, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, first_bytes);
    EXPECT_TRUE(std::string_view(copied).starts_with("prefix"));
    if (copied.size() >= 6) {
        const auto first = ruvia::parse_http2_frame_header(std::span<const char>(copied.data(), copied.size()).subspan(6));
        EXPECT_TRUE(first && first->stream_id_ == ids[0] && first->length_ == bodies[0].size());
        if (first) {
            EXPECT_TRUE(std::string_view(copied).substr(6 + ruvia::http2_frame_header_bytes) == bodies[0]);
        }
    }
    EXPECT_EQ(client.pending_data_output_bytes(ids[0]), std::size_t{0});
    EXPECT_EQ(client.pending_data_output_bytes(ids[1]), bodies[1].size());
    EXPECT_EQ(observed.count_, std::size_t{1});
    EXPECT_TRUE(observed.entries_[0].stream_id_ == ids[0] && observed.entries_[0].payload_bytes_ == bodies[0].size());

    // A unit budget still moves exactly one whole frame per batch.
    std::string suffix;
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[1].size());
    EXPECT_EQ(client.pending_data_output_bytes(ids[1]), std::size_t{0});
    EXPECT_EQ(observed.count_, std::size_t{2});
    suffix.clear();
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[2].size());
    const auto last = ruvia::parse_http2_frame_header(std::span<const char>(suffix.data(), suffix.size()));
    EXPECT_TRUE(last && last->stream_id_ == ids[2] && last->length_ == bodies[2].size());
    if (last) {
        EXPECT_TRUE(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == bodies[2]);
    }
    EXPECT_TRUE(!client.wants_write());
    EXPECT_EQ(observed.count_, std::size_t{3});
    EXPECT_TRUE(observed.entries_[1].stream_id_ == ids[1] && observed.entries_[1].payload_bytes_ == bodies[1].size());
    EXPECT_TRUE(observed.entries_[2].stream_id_ == ids[2] && observed.entries_[2].payload_bytes_ == bodies[2].size());
    EXPECT_TRUE(!observed.overflow_);
}

TEST(Ruvia, http2_public_websocket_handshake_validates_required_subprotocols_and_configures_deflate) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);

    std::string block1;
    ruvia::hpack_encoder::encode_header(block1, ":method", "CONNECT");
    ruvia::hpack_encoder::encode_header(block1, ":protocol", "websocket");
    ruvia::hpack_encoder::encode_header(block1, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block1, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block1, ":path", "/ws");
    ruvia::hpack_encoder::encode_header(block1, "sec-websocket-version", "13");
    ruvia::hpack_encoder::encode_header(block1, "sec-websocket-protocol", "other, chat.v1");
    ruvia::hpack_encoder::encode_header(block1, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block1);

    std::string block3;
    ruvia::hpack_encoder::encode_header(block3, ":method", "CONNECT");
    ruvia::hpack_encoder::encode_header(block3, ":protocol", "websocket");
    ruvia::hpack_encoder::encode_header(block3, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block3, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block3, ":path", "/ws");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-version", "13");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-extensions", "permessage-deflate");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 3, block3);

    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event1 = server.next_event();
    auto* head1 = event1 ? event1->request_head() : nullptr;
    EXPECT_TRUE(head1 != nullptr);
    if (head1 == nullptr) {
        return;
    }

    const auto built_req1 = head1->build_request();
    constexpr std::array<std::string_view, 1> unmatched{"chat.v2"};
    const auto rejected = ruvia::validate_http2_websocket_handshake(server, 1, built_req1, {.supported_subprotocols_ = unmatched, .require_subprotocol_ = true});
    EXPECT_TRUE(rejected.accepted() == nullptr);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::bad_request);
    }

    const auto optional_subprotocol = ruvia::validate_http2_websocket_handshake(server, 1, built_req1, {.supported_subprotocols_ = unmatched, .require_subprotocol_ = false});
    EXPECT_TRUE(optional_subprotocol.accepted() != nullptr);

    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto accepted1 = ruvia::validate_http2_websocket_handshake(server, 1, built_req1, {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    EXPECT_TRUE(accepted1.accepted() != nullptr);

    const auto submitted1 = server.submit_websocket_handshake(1, built_req1, accepted1,
        {
            .supported_subprotocols_ = supported,
            .deflate_ = {.enabled_ = true, .context_takeover_ = true},
        });
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted1));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted1)) {
        EXPECT_EQ(negotiation->subprotocol(), std::string_view("chat.v1"));
        EXPECT_TRUE(negotiation->compression().enabled_);
        EXPECT_TRUE(!negotiation->compression().server_no_context_takeover_);
        EXPECT_TRUE(negotiation->compression().client_no_context_takeover_);
    }
    EXPECT_TRUE(server.release(std::move(*head1)) == ruvia::http2_server_request_release_status::released);

    auto event3 = server.next_event();
    auto* head3 = event3 ? event3->request_head() : nullptr;
    EXPECT_TRUE(head3 != nullptr);
    if (head3 == nullptr) {
        return;
    }

    const auto built_req3 = head3->build_request();
    const auto rejected3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3, {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    EXPECT_TRUE(rejected3.accepted() == nullptr);
    EXPECT_TRUE(rejected3.failure() != nullptr);

    const auto accepted3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3);
    EXPECT_TRUE(accepted3.accepted() != nullptr);
    const auto submitted3 = server.submit_websocket_handshake(3, built_req3, accepted3,
        {
            .deflate_ = {.enabled_ = false},
        });
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted3));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted3)) {
        EXPECT_TRUE(negotiation->subprotocol().empty());
        EXPECT_TRUE(!negotiation->compression().enabled_);
    }
    EXPECT_TRUE(server.release(std::move(*head3)) == ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_websocket_handshake_ignores_rfc6455_key_exchange_fields) {
    // RFC 8441 section 5: :protocol supersedes Sec-WebSocket-Key/-Accept, which
    // a recipient does not process; it neither validates nor rejects them.
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> extra{{
        {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
        {"sec-websocket-accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="},
        {"sec-websocket-protocol", "chat, chat"},
    }};
    std::uint32_t stream_id = 1;
    for (const auto& [name, value] : extra) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/ws");
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        ruvia::hpack_encoder::encode_header(block, name, value);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
        stream_id += 2;
    }
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    constexpr std::array<std::string_view, 1> supported{"chat"};
    for (std::size_t i = 0; i < extra.size(); ++i) {
        auto event = server.next_event();
        auto* head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            return;
        }
        const auto id = head->stream_id();
        const auto request = head->build_request();
        const auto validation = ruvia::validate_http2_websocket_handshake(server, id, request, {.supported_subprotocols_ = supported, .require_subprotocol_ = i == 2});
        EXPECT_TRUE(validation.accepted() != nullptr);
        const auto submitted = server.submit_websocket_handshake(id, request, validation, {.supported_subprotocols_ = supported});
        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
        if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted)) {
            EXPECT_EQ(negotiation->subprotocol(), std::string_view(i == 2 ? "chat" : ""));
        }
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_outbound_request_headers_and_trailers_lowercase_standard_tokens) {
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    ruvia::test::output_relay relay;
    auto exchange_value = [&](auto& from, auto& to) {
        EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
    };
    exchange_value(client, server);
    exchange_value(server, client);

    const std::string long_mixed_name = "X-Mixed-" + std::string(80, 'N');
    const std::string long_lower_name = "x-mixed-" + std::string(80, 'n');
    const std::string last_upper_name = std::string(62, 'a') + "Z";
    const std::string first_upper_name = "Z" + std::string(63, 'b');
    const std::string middle_upper_name = std::string(32, 'c') + "Z" + std::string(32, 'c');
    const std::string last_lower_name = std::string(62, 'a') + "z";
    const std::string first_lower_name = "z" + std::string(63, 'b');
    const std::string middle_lower_name = std::string(32, 'c') + "z" + std::string(32, 'c');
    const std::array headers{ruvia::http_header_view{long_mixed_name, "head-value"}, ruvia::http_header_view{"X-Custom-Head", "short"}, ruvia::http_header_view{last_upper_name, "last-upper"}, ruvia::http_header_view{first_upper_name, "first-upper"}, ruvia::http_header_view{middle_upper_name, "middle-upper"}};
    const std::array normalized{ruvia::http_header_view{long_lower_name, "head-value"}, ruvia::http_header_view{"x-custom-head", "short"}, ruvia::http_header_view{last_lower_name, "last-upper"}, ruvia::http_header_view{first_lower_name, "first-upper"}, ruvia::http_header_view{middle_lower_name, "middle-upper"}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/mixed", .headers_ = headers, .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    const std::array<ruvia::http_header_view, 2> trailers{{{"X-Mixed-Trailer", "tail-value"}, {"X-Second-Trailer", "tail-two"}}};
    EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
    exchange_value(client, server);

    std::optional<ruvia::http2_request_head_event> parent;
    while (auto event = server.next_event()) {
        if (auto* head = event->request_head()) {
            const auto built = head->build_request();
            for (const auto& field : normalized) {
                EXPECT_EQ(built.header(field.name()).value_or(""), field.value());
            }
            parent.emplace(std::move(*head));
        } else if (const auto* end = event->message_end()) {
            EXPECT_EQ(end->trailers().size(), std::size_t{2});
            if (end->trailers().size() == 2) {
                EXPECT_EQ(end->trailers()[0].name(), "x-mixed-trailer");
                EXPECT_EQ(end->trailers()[0].value(), "tail-value");
                EXPECT_EQ(end->trailers()[1].name(), "x-second-trailer");
                EXPECT_EQ(end->trailers()[1].value(), "tail-two");
            }
        }
    }
    EXPECT_TRUE(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed", .headers_ = headers});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() == 0) {
        const auto built_push = std::get<0>(pushed).build_request();
        for (const auto& field : normalized) {
            EXPECT_EQ(built_push.header(field.name()).value_or(""), field.value());
        }
        (void)server.release(std::move(std::get<0>(pushed)));
    }
    exchange_value(server, client);
    auto promise = client.next_event();
    EXPECT_TRUE(promise && promise->push_promise());
    if (promise && promise->push_promise()) {
        const auto& fields = promise->push_promise()->request_.headers_;
        EXPECT_EQ(fields.size(), normalized.size());
        if (fields.size() == normalized.size()) {
            for (std::size_t i = 0; i < fields.size(); ++i) {
                EXPECT_EQ(fields[i].name(), normalized[i].name());
                EXPECT_EQ(fields[i].value(), normalized[i].value());
            }
        }
    }
    // Consume the abandoned push's terminal event before the next input batch.
    while (client.next_event()) {
    }
    if (parent) {
        ruvia::http_response response;
        response.status(ruvia::http_status::no_content);
        for (const auto& field : headers) {
            response.header(field.name(), field.value());
        }
        const auto result = server.submit_response_head(parent->stream_id(), response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response));
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(result));
        (void)server.release(std::move(*parent));
        exchange_value(server, client);
        bool received_response = false;
        while (auto event = client.next_event()) {
            if (const auto* head = event->response_head()) {
                EXPECT_TRUE(head->head().status() == ruvia::http_status::no_content);
                for (const auto& field : normalized) {
                    EXPECT_EQ(head->head().header(field.name()).value_or(""), field.value());
                }
                received_response = true;
            }
        }
        EXPECT_TRUE(received_response);
    }
}

TEST(Ruvia, http2_public_rejected_connect_with_invalid_terminal_length_resets_without_data_frame) {
    auto client = ruvia::http2_connection::client({});
    const auto submitted = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        return;
    }
    const auto stream_id = std::get<std::uint32_t>(submitted);
    (void)ruvia::test::drain_output(client);

    std::string wire;
    append_peer_settings(wire);
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", "403");
    ruvia::hpack_encoder::encode_header(block, "content-length", "5");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream_id, block);

    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto closed = client.next_event();
    EXPECT_TRUE(closed && closed->stream_closed());
    if (closed && closed->stream_closed()) {
        EXPECT_EQ(closed->stream_closed()->stream_id(), stream_id);
        EXPECT_TRUE(closed->stream_closed()->error() == ruvia::http2_error_code::protocol_error);
    }

    const auto out = ruvia::test::drain_output(client);
    // Peer SETTINGS generates a 9-byte SETTINGS ACK, followed only by the 13-byte RST_STREAM
    // (no spurious 9-byte DATA(END_STREAM) frame may be emitted before RST_STREAM).
    EXPECT_EQ(out.size(), ruvia::http2_frame_header_bytes + ruvia::http2_frame_header_bytes + 4);
    if (out.size() >= ruvia::http2_frame_header_bytes * 2 + 4) {
        const auto rst = ruvia::parse_http2_frame_header(std::span(out.data() + ruvia::http2_frame_header_bytes, ruvia::http2_frame_header_bytes));
        EXPECT_TRUE(rst.has_value());
        if (rst) {
            EXPECT_EQ(rst->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
            EXPECT_EQ(rst->stream_id_, stream_id);
            EXPECT_EQ(rst->length_, std::uint32_t{4});
        }
    }
}

// H2-02: Client receiving RST_STREAM(REFUSED_STREAM) after a 1xx informational
// response treats it as a stream reset rather than an unprocessed request.
TEST(Ruvia, http2_public_client_refused_stream_after_informational_is_not_unprocessed) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub));
    const auto sid = std::get<std::uint32_t>(sub);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> sreq;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            sreq.emplace(std::move(*rh));
        }
    }
    EXPECT_TRUE(server.submit_interim_response_head(sid, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    bool saw_1xx = false;
    while (auto ev = client.next_event()) {
        if (ev->informational_head()) {
            saw_1xx = true;
        }
    }
    EXPECT_TRUE(saw_1xx);

    std::string rst;
    constexpr std::array<char, 4> refused_payload{0, 0, 0, 7};
    append_frame(rst, ruvia::http2_frame_type::rst_stream, 0, sid, std::string_view(refused_payload.data(), refused_payload.size()));
    EXPECT_TRUE(client.feed(rst) == ruvia::http2_feed_result::accepted);
    bool saw_unprocessed = false;
    bool saw_closed = false;
    while (auto ev = client.next_event()) {
        if (ev->request_unprocessed()) {
            saw_unprocessed = true;
        }
        if (const auto* closed = ev->stream_closed()) {
            saw_closed = closed->error() == ruvia::http2_error_code::refused_stream;
        }
    }
    EXPECT_TRUE(!saw_unprocessed);
    EXPECT_TRUE(saw_closed);
    if (sreq) {
        (void)server.release(std::move(*sreq));
    }
}

// CONNECT 成功时保留但忽略长度；拒绝响应仍按普通 HTTP 正文分帧处理。
TEST(Ruvia, http2_public_client_connect_response_ignores_success_length_and_validates_rejection) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid1 = std::get<std::uint32_t>(sub1);
    const auto sub2 = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid2 = std::get<std::uint32_t>(sub2);
    (void)ruvia::test::drain_output(client);

    // Valid Content-Length on 200 CONNECT is retained in response_head.headers()
    std::string rblock1;
    ruvia::hpack_encoder::encode_header(rblock1, ":status", "200");
    ruvia::hpack_encoder::encode_header(rblock1, "content-length", "5");
    std::string rwire1;
    append_frame(rwire1, ruvia::http2_frame_type::headers, 0x4, sid1, rblock1);
    EXPECT_TRUE(client.feed(rwire1) == ruvia::http2_feed_result::accepted);
    bool has_cl = false;
    while (auto ev = client.next_event()) {
        if (auto* rh = ev->response_head()) {
            for (const auto& h : rh->head().headers()) {
                if (h.name() == "content-length" && h.value() == "5") {
                    has_cl = true;
                }
            }
        }
    }
    EXPECT_TRUE(has_cl);
    EXPECT_TRUE(!client.stream_aborted(sid1));

    // 非 2xx CONNECT 的非法 Content-Length 仍触发 PROTOCOL_ERROR。
    std::string rblock2;
    ruvia::hpack_encoder::encode_header(rblock2, ":status", "400");
    ruvia::hpack_encoder::encode_header(rblock2, "content-length", "not-a-number");
    std::string rwire2;
    append_frame(rwire2, ruvia::http2_frame_type::headers, 0x4, sid2, rblock2);
    EXPECT_TRUE(client.feed(rwire2) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    EXPECT_TRUE(client.stream_aborted(sid2));
}

TEST(Ruvia, http2_public_asterisk_form_request_never_generates_authority) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    const auto rejected = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "OPTIONS", .authority_ = "example.test", .target_ = "*"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) && std::get<ruvia::http2_request_head_submit_error>(rejected) == ruvia::http2_request_head_submit_error::invalid_message);
    EXPECT_TRUE(!client.wants_write());
    const auto accepted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "OPTIONS", .target_ = "*"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(accepted));
}

TEST(Ruvia, http2_public_built_request_exposes_extended_protocol) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const std::array websocket_headers{ruvia::http_header_view{"sec-websocket-version", "13"}};
    for (const std::string_view protocol : {"websocket", "connect-udp"}) {
        const ruvia::http2_extended_connect_request_head_view head{.protocol_ = protocol, .authority_ = "example.test", .target_ = "/proxy", .headers_ = protocol == "websocket" ? std::span<const ruvia::http_header_view>(websocket_headers) : std::span<const ruvia::http_header_view>{}};
        const auto submitted = client.submit_request_head(head);
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        relay.transfer(client, server);
        std::optional<ruvia::http2_request_head_event> request_head_event;
        while (auto event_value = server.next_event()) {
            if (auto* request_head = event_value->request_head()) {
                request_head_event.emplace(std::move(*request_head));
            }
        }
        EXPECT_TRUE(request_head_event.has_value());
        if (!request_head_event) {
            return;
        }
        const auto request = request_head_event->build_request();
        EXPECT_EQ(request.protocol(), protocol);
        // RFC 8441/9298: :protocol selects the tunnel protocol, so a WebSocket
        // CONNECT must not validate as CONNECT-UDP.
        EXPECT_EQ(ruvia::validate_http_connect_udp_request(request).has_value(), protocol == "websocket");
        (void)server.release(std::move(*request_head_event));
    }
}

TEST(Ruvia, http2_public_late_responses_after_many_client_cancellations_keep_connection) {
    auto client = ruvia::http2_connection::client({});
    std::string settings;
    append_peer_settings(settings);
    EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
    (void)ruvia::test::drain_output(client);

    std::uint32_t first_canceled_stream = 0;
    for (std::size_t index = 0; index != 2100; ++index) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/canceled"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (index == 0) {
            first_canceled_stream = stream_id;
        }
        EXPECT_TRUE(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        (void)ruvia::test::drain_output(client);
    }

    const auto active = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/active"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(active));
    if (!std::holds_alternative<std::uint32_t>(active)) {
        return;
    }
    const auto active_stream = std::get<std::uint32_t>(active);
    (void)ruvia::test::drain_output(client);

    std::string response_block;
    ruvia::hpack_encoder::encode_header(response_block, ":status", "200");
    std::string late_head;
    append_frame(late_head, ruvia::http2_frame_type::headers, 0x4, first_canceled_stream, response_block);
    const auto late_head_result = client.feed(late_head);
    EXPECT_TRUE(late_head_result == ruvia::http2_feed_result::accepted);
    if (late_head_result != ruvia::http2_feed_result::accepted) {
        return;
    }
    EXPECT_TRUE(!client.next_event());

    std::string late_data;
    append_frame(late_data, ruvia::http2_frame_type::data, 0x1, first_canceled_stream, "late");
    EXPECT_TRUE(client.feed(late_data) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event());
    EXPECT_TRUE(!client.connection_error());

    std::string active_response;
    append_frame(active_response, ruvia::http2_frame_type::headers, 0x5, active_stream, response_block);
    EXPECT_TRUE(client.feed(active_response) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head() && head->response_head()->stream_id() == active_stream);
    const auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == active_stream);
    EXPECT_TRUE(!client.connection_error());
}

// A promised stream refused while decoding its PUSH_PROMISE is already reset, so
// submit_reset(PROTOCOL_ERROR) does not emit a duplicate RST_STREAM.
TEST(Ruvia, http2_public_rejected_push_promise_does_not_emit_duplicate_late_rst_stream) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto push_server = ruvia::http2_connection::server({});
    relay.transfer(client, push_server);
    relay.transfer(push_server, client);
    relay.transfer(client, push_server);
    const auto sub = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub));
    (void)ruvia::test::drain_output(client);

    // An unsafe method (POST) cannot be pushed (RFC 9113 §8.4).
    std::string post_push;
    ruvia::hpack_encoder::encode_header(post_push, ":method", "POST");
    ruvia::hpack_encoder::encode_header(post_push, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(post_push, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(post_push, ":path", "/upload");
    std::string post_payload("\x00\x00\x00\x02", 4);
    post_payload.append(post_push);
    std::string post_wire;
    append_frame(post_wire, ruvia::http2_frame_type::push_promise, 0x4, 1, post_payload);
    EXPECT_TRUE(client.feed(post_wire) == ruvia::http2_feed_result::accepted);
    EXPECT_EQ(ruvia::test::drain_output(client).size(), ruvia::http2_frame_header_bytes + 4);
    EXPECT_TRUE(client.submit_reset(2, ruvia::http2_error_code::protocol_error) == ruvia::http2_submit_status::closed);
    EXPECT_TRUE(ruvia::test::drain_output(client).empty());
}

// RFC 9110 §9.3.6：成功 CONNECT 的长度字段仅保留为元数据，不能阻止进入隧道。
TEST(Ruvia, http2_public_successful_connect_ignores_content_length_semantics) {
    for (const bool extended : {false, true}) {
        for (const std::string_view length : {"", "0", "42", "not-a-number", "18446744073709551616", "1, 2"}) {
            auto client = ruvia::http2_connection::client({});
            (void)ruvia::test::drain_output(client);
            constexpr std::array<char, 6> enable_connect{0, 8, 0, 0, 0, 1};
            std::string settings;
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0, std::string_view(enable_connect.data(), enable_connect.size()));
            EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);

            const auto submitted = extended ? client.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "example", .authority_ = "example.test", .target_ = "/tunnel"}) : client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            const auto stream_id = std::get<std::uint32_t>(submitted);
            (void)ruvia::test::drain_output(client);

            std::string block;
            ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            std::string wire;
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, "tunnel bytes");
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            bool received_head = false;
            bool received_data = false;
            bool received_end = false;
            while (auto event = client.next_event()) {
                EXPECT_TRUE(event->stream_closed() == nullptr);
                if (const auto* head = event->response_head()) {
                    received_head = true;
                    const auto headers = head->head().headers();
                    EXPECT_EQ(headers.size(), std::size_t{1});
                    if (!headers.empty()) {
                        EXPECT_EQ(headers.front().name(), std::string_view("content-length"));
                        EXPECT_EQ(headers.front().value(), length);
                    }
                } else if (const auto* data = event->tunnel_data()) {
                    received_data = true;
                    EXPECT_EQ(data->bytes(), std::string_view("tunnel bytes"));
                } else if (event->tunnel_end() != nullptr) {
                    received_end = true;
                }
            }
            EXPECT_TRUE(received_head);
            EXPECT_TRUE(received_data);
            EXPECT_TRUE(received_end);
            EXPECT_TRUE(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        }
    }
}

namespace {

template <typename headers_type>
concept push_request_view_accepts_headers = requires(headers_type&& headers) { ruvia::http_push_request_view{.headers_ = std::forward<headers_type>(headers)}; };

template <typename headers_type>
concept regular_request_view_accepts_headers = requires(headers_type&& headers) { ruvia::http2_regular_request_head_view{.headers_ = std::forward<headers_type>(headers)}; };

static_assert(push_request_view_accepts_headers<const std::vector<ruvia::http_header_view>&>);
static_assert(!push_request_view_accepts_headers<std::vector<ruvia::http_header_view>>);
static_assert(!push_request_view_accepts_headers<std::array<ruvia::http_header_view, 1>>);
static_assert(regular_request_view_accepts_headers<const std::vector<ruvia::http_header_view>&>);
static_assert(!regular_request_view_accepts_headers<std::vector<ruvia::http_header_view>>);
static_assert(!regular_request_view_accepts_headers<std::array<ruvia::http_header_view, 1>>);

}  // namespace
