#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"

#include "failing_memory_resource.h"
#include "http3_relay.h"
#include "stl_allocation_fixture.h"

namespace {

constexpr ruvia::http3_regular_request_head_view get_request_head{
    .authority_ = "example.test", .target_ = "/resource"};

constexpr ruvia::http3_regular_request_head_view head_request_head{
    .method_ = "HEAD", .authority_ = "example.test", .target_ = "/resource"};

template <typename error_type>
[[nodiscard]] bool throws_as(auto&& operation) {
    try {
        operation();
    } catch (const error_type&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

[[nodiscard]] bool rejected_with(const auto& result, ruvia::http3_response_head_error error) {
    const auto* actual = std::get_if<ruvia::http3_response_head_error>(&result);
    return actual != nullptr && *actual == error;
}

// RFC 9204 §4.5.3: literal names and values, without dynamic table references
// or Huffman coding, exercise the real decoder with arbitrary HTTP semantics.
void append_qpack_integer(std::string& bytes, std::size_t value, unsigned prefix_bits, unsigned tag) {
    const auto mask = (1u << prefix_bits) - 1;
    if (value < mask) {
        bytes.push_back(static_cast<char>(tag | static_cast<unsigned>(value)));
        return;
    }
    bytes.push_back(static_cast<char>(tag | mask));
    value -= mask;
    while (value >= 128) {
        bytes.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    bytes.push_back(static_cast<char>(value));
}

void append_http3_wire_frame(std::string& bytes, unsigned type, std::string_view payload) {
    bytes.push_back(static_cast<char>(type));
    if (payload.size() < 64) {
        bytes.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() < 16384) {
        bytes.push_back(static_cast<char>(0x40 | (payload.size() >> 8)));
        bytes.push_back(static_cast<char>(payload.size() & 255));
    } else {
        throw std::length_error("HTTP/3 test frame exceeds its two-byte length");
    }
    bytes.append(payload);
}

void append_http3_wire_headers(std::string& bytes, std::span<const ruvia::http3_field_view> fields) {
    std::string section(2, '\0');  // Required Insert Count = 0; Delta Base = 0.
    for (const auto& field : fields) {
        append_qpack_integer(section, field.name_.size(), 3, 0x20);
        section.append(field.name_);
        append_qpack_integer(section, field.value_.size(), 7, 0);
        section.append(field.value_);
    }
    append_http3_wire_frame(bytes, 1, section);
}

void append_http3_wire_headers(std::string& bytes, std::initializer_list<ruvia::http3_field_view> fields) {
    append_http3_wire_headers(bytes, std::span(fields.begin(), fields.size()));
}

[[nodiscard]] ruvia::http3_result receive_http3_wire_response(ruvia::test::http3_peers& peers,
    std::int64_t stream_id, std::string_view bytes, bool fin, ruvia::test::http3_event_log& log) {
    const auto result = peers.client_.read_stream(stream_id, ruvia::test::http3_bytes(bytes), fin, peers.timestamp_++);
    while (auto event = peers.client_.next_event()) {
        log(*event);
    }
    return result;
}

[[nodiscard]] bool has_message_error(const ruvia::test::http3_event_log& log, std::int64_t stream_id) {
    for (const auto& action : log.transport_actions_) {
        if (action.stream_id_ == stream_id &&
            action.application_error_code_ == static_cast<std::uint64_t>(ruvia::http3_error_code::message_error)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool rejected_http3_message(const ruvia::http3_result& result,
    const ruvia::http3_connection& connection, const ruvia::test::http3_event_log& log,
    std::int64_t stream_id) {
    const auto message_error = static_cast<std::uint64_t>(ruvia::http3_error_code::message_error);
    // nghttp3 read errors are connection-fatal: its documented API forbids
    // further calls except destruction. Core-only validation can reset a stream.
    return (result.status_ == ruvia::http3_status::protocol_error &&
               result.error_code_ == message_error && connection.connection_error() == message_error) ||
           (result.status_ == ruvia::http3_status::accepted && has_message_error(log, stream_id));
}

}  // namespace

// RFC 9114 §4.1-§4.2: a buffered response becomes :status, lowercase regular
// fields, a generated Date and the planned Content-Length, followed by DATA.
TEST(Ruvia, http3_buffered_response_head_maps_status_fields_and_content_length) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::ok);
    response.header("X-Trace-Id", "trace-7");
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(plan.send_body());
    EXPECT_EQ(plan.content_length(), std::uint64_t{11});

    const auto submitted = peers.server_.submit_response_head(0, response, plan);
    const auto* accepted = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted);
    EXPECT_TRUE(accepted != nullptr && accepted->content_length() == 11);
    // A second final head on the same stream is refused.
    EXPECT_TRUE(rejected_with(peers.server_.submit_response_head(0, response, plan),
        ruvia::http3_response_head_error::invalid_state));

    const auto data = peers.server_.submit_data(0, ruvia::test::http3_bytes(response.body()));
    EXPECT_TRUE(data.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(data.accepted_bytes_, std::size_t{11});
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HDE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{1});
    if (client.headers_.size() != 1) {
        return;
    }
    const auto& head = client.headers_[0];
    EXPECT_TRUE(!head.stream_ended_);
    EXPECT_TRUE(head.status_ && head.status_->value() == 200);
    EXPECT_FALSE(head.field(":status").has_value());
    EXPECT_TRUE(head.field("x-trace-id") == std::string_view("trace-7"));
    EXPECT_TRUE(!head.field("X-Trace-Id").has_value());
    EXPECT_TRUE(head.field("content-length") == std::string_view("11"));
    const auto date = head.field("date");
    EXPECT_TRUE(date.has_value() && date->ends_with(" GMT"));
    for (const auto& field : head.fields_) {
        for (const char byte : field.name_) {
            EXPECT_TRUE(byte < 'A' || byte > 'Z');
        }
    }
    EXPECT_EQ(client.data(0), std::string("hello world"));
    EXPECT_TRUE(client.ended(0));
}

TEST(Ruvia, http3_field_sections_keep_interleaved_names_and_values_after_submission) {
    for (const std::size_t extra_fields : {std::size_t{0}, std::size_t{34}}) {
        SCOPED_TRACE(extra_fields);
        ruvia::test::http3_peers peers;
        EXPECT_TRUE(peers.bind());
        ruvia::test::http3_event_log request;
        EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

        std::vector<std::string> names{std::string(80, 'a'), "X-Small", std::string(64, 'b'),
            std::string(31, 'c') + "Z" + std::string(31, 'c'),
            "Z" + std::string(63, 'd'), std::string(64, 'e') + "Z"};
        std::vector<std::string> normalized{std::string(80, 'a'), "x-small", std::string(64, 'b'),
            std::string(31, 'c') + "z" + std::string(31, 'c'),
            "z" + std::string(63, 'd'), std::string(64, 'e') + "z"};
        std::vector<std::string> values{"value-0", "value-1", "value-2", "value-3", "value-4", "value-5"};
        for (std::size_t index = 0; index < extra_fields; ++index) {
            names.push_back("X-Extra-" + std::to_string(index));
            normalized.push_back("x-extra-" + std::to_string(index));
            values.push_back("value-extra-" + std::to_string(index));
        }
        const auto expected_values = values;
        std::vector<ruvia::http_header_view> fields;
        for (std::size_t index = 0; index < names.size(); ++index) {
            fields.emplace_back(names[index], values[index]);
        }
        {
            const ruvia::http_interim_response_head hints(
                ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(fields));
            EXPECT_TRUE(peers.server_.submit_interim_response_head(0, hints) == ruvia::http3_status::accepted);
            ruvia::http_response response;
            for (const auto& field : fields) {
                response.header_stable_view(field.name(), field.value());
            }
            const auto submitted = peers.server_.submit_streaming_response_head(
                0, ruvia::http_known_method::get, std::move(response), ruvia::http_response_trailer_intent::present);
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(submitted));
            const auto data = peers.server_.submit_data(0, ruvia::test::http3_bytes("payload"));
            EXPECT_TRUE(data.status_ == ruvia::http3_status::accepted && data.accepted_bytes_ == 7);
            EXPECT_TRUE(peers.server_.finish_response(0, ruvia::validate_http_response_trailers(
                                                             std::span<const ruvia::http_header_view>(fields))) ==
                        ruvia::http3_status::accepted);
        }
        // QPACK encoding happens after every source has been replaced.
        for (auto& name : names) {
            name.assign(name.size(), 'q');
        }
        for (auto& value : values) {
            value.assign(value.size(), 'r');
        }
        ruvia::test::http3_event_log client;
        EXPECT_TRUE(peers.to_client(client));
        EXPECT_EQ(client.sequence_, std::string("HHDHE"));
        EXPECT_EQ(client.data(0), std::string("payload"));
        EXPECT_TRUE(client.ended(0));
        EXPECT_EQ(client.headers_.size(), std::size_t{3});
        if (client.headers_.size() != 3) {
            return;
        }
        EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 103);
        EXPECT_TRUE(client.headers_[1].status_ && client.headers_[1].status_->value() == 200);
        for (const auto& section : client.headers_) {
            for (std::size_t index = 0; index < normalized.size(); ++index) {
                EXPECT_TRUE(section.field(normalized[index]) == expected_values[index]);
            }
        }
    }
}

TEST(Ruvia, http3_interim_allocation_failure_leaves_only_the_final_response_on_wire) {
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        GTEST_SKIP() << "requires STL without noexcept debug metadata allocation";
    }
    ruvia::test::failing_memory_resource resource;
    {
        ruvia::test::http3_peers peers({.resource_ = &resource});
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log request;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, request));
        const std::vector<ruvia::http_header_view> fields(80, ruvia::http_header_view{"x-hint", "value"});
        const ruvia::http_interim_response_head hints(
            ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(fields));
        const auto live = resource.live_allocations();
        resource.fail_after_stl_operation(0);
        EXPECT_THROW((void)peers.server_.submit_interim_response_head(0, hints), std::bad_alloc);
        EXPECT_EQ(resource.live_allocations(), live);
        resource.allow_allocations();

        ruvia::http_response final_response;
        final_response.status(ruvia::http_status::no_content);
        ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
            peers.server_.submit_response_head(0, final_response,
                ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, final_response))));
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(peers.to_client(client));
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 204);
        EXPECT_TRUE(client.ended(0));
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

// RFC 9110 §9.3.2: a HEAD response is the head alone, ending the stream.
TEST(Ruvia, http3_buffered_response_head_to_head_request_ends_with_head) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, head_request_head, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    EXPECT_TRUE(!plan.send_body());
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    // The head ended the stream: no content can follow.
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("x")).status_ ==
                ruvia::http3_status::invalid_state);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::invalid_state);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HE"));
    EXPECT_TRUE(client.headers_.size() == 1 && client.headers_[0].stream_ended_ &&
                client.headers_[0].status_ && client.headers_[0].status_->value() == 200);
    EXPECT_TRUE(client.data(0).empty());
    EXPECT_TRUE(client.ended(0));
}

// RFC 9114 §4.2: connection-specific fields have no HTTP/3 meaning.
TEST(Ruvia, http3_response_head_rejects_connection_specific_fields) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.header("Connection", "close");
    response.body("bye");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(rejected_with(peers.server_.submit_response_head(0, response, plan),
        ruvia::http3_response_head_error::invalid_message));

    ruvia::http_response streamed(&resource);
    streamed.header("Keep-Alive", "timeout=5");
    EXPECT_TRUE(rejected_with(peers.server_.submit_streaming_response_head(0, ruvia::http_known_method::get,
                                  std::move(streamed), ruvia::http_response_trailer_intent::none),
        ruvia::http3_response_head_error::invalid_message));

    // A rejected head sends nothing and leaves the stream able to answer.
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.empty());
    response.remove_header("Connection");
    const auto valid_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, valid_plan)));
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("bye")).accepted_bytes_ == 3);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && !client.headers_[0].field("connection").has_value());
    EXPECT_EQ(client.data(0), std::string("bye"));
    EXPECT_TRUE(client.ended(0));
}

// The planned Content-Length binds the DATA total exactly: excess is refused
// before anything is copied and an early end leaves the stream open.
TEST(Ruvia, http3_response_content_length_mismatch_throws_length_error) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_head, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("hello");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(4, response, plan)));

    EXPECT_TRUE(throws_as<std::length_error>(
        [&] { (void)peers.server_.submit_data(0, ruvia::test::http3_bytes("hello!")); }));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("hello")).accepted_bytes_ == 5);
    EXPECT_TRUE(throws_as<std::length_error>(
        [&] { (void)peers.server_.submit_data(0, ruvia::test::http3_bytes("!")); }));
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    EXPECT_TRUE(peers.server_.submit_data(4, ruvia::test::http3_bytes("hel")).accepted_bytes_ == 3);
    EXPECT_TRUE(throws_as<std::length_error>([&] { (void)peers.server_.finish_stream(4); }));
    EXPECT_TRUE(peers.server_.submit_data(4, ruvia::test::http3_bytes("lo")).accepted_bytes_ == 2);
    EXPECT_TRUE(peers.server_.finish_stream(4) == ruvia::http3_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.data(0), std::string("hello"));
    EXPECT_EQ(client.data(4), std::string("hello"));
    EXPECT_TRUE(client.ended(0) && client.ended(4));
    EXPECT_TRUE(!peers.client_.connection_error().has_value());
}

// RFC 9114 §4.1: a streaming response carries its trailer section in a
// HEADERS frame after the DATA frames.
TEST(Ruvia, http3_streaming_response_head_sends_trailers_after_data) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response head(&resource);
    head.header("Content-Type", "text/plain");
    const auto submitted = peers.server_.submit_streaming_response_head(
        0, ruvia::http_known_method::get, std::move(head), ruvia::http_response_trailer_intent::present);
    const auto* commit = std::get_if<ruvia::http_response_stream_commit_plan>(&submitted);
    EXPECT_TRUE(commit != nullptr);
    if (commit == nullptr) {
        return;
    }
    EXPECT_TRUE(commit->framing() == ruvia::http_response_stream_framing::http3_frames);
    EXPECT_TRUE(commit->trailers_available());
    EXPECT_TRUE(commit->head_disposition() == ruvia::http_response_stream_head_disposition::body_open);

    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("chunk-1;")).accepted_bytes_ == 8);
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("chunk-2")).accepted_bytes_ == 7);
    const std::array trailer_fields{ruvia::http_header_view{"X-Checksum", "c2f1"}};
    EXPECT_TRUE(peers.server_.finish_response(0, ruvia::validate_http_response_trailers(
                                                     std::span<const ruvia::http_header_view>(trailer_fields))) ==
                ruvia::http3_status::accepted);
    // The trailer section was the last thing the stream may carry.
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("late")).status_ ==
                ruvia::http3_status::invalid_state);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HDHE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{2});
    if (client.headers_.size() != 2) {
        return;
    }
    EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 200);
    EXPECT_TRUE(client.headers_[0].field("content-type") == std::string_view("text/plain"));
    EXPECT_TRUE(!client.headers_[0].field("content-length").has_value());
    EXPECT_TRUE(!client.headers_[0].stream_ended_);
    EXPECT_EQ(client.data(0), std::string("chunk-1;chunk-2"));
    EXPECT_EQ(client.headers_[1].fields_.size(), std::size_t{1});
    EXPECT_TRUE(client.headers_[1].field("x-checksum") == std::string_view("c2f1"));
    EXPECT_TRUE(client.headers_[1].stream_ended_);
    EXPECT_TRUE(client.headers_[1].trailers_);
    EXPECT_FALSE(client.headers_[1].status_.has_value());
    EXPECT_TRUE(client.ended(0));
}

// RFC 9114 §4.1: interim responses precede the final head on the same stream.
TEST(Ruvia, http3_interim_response_precedes_final_head) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    const std::array links{ruvia::http_header_view{"Link", "</style.css>; rel=preload; as=style"}};
    const ruvia::http_interim_response_head early_hints(
        ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(links));
    EXPECT_TRUE(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_status::accepted);

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("page");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    // Once the final head is committed no interim response may follow.
    EXPECT_TRUE(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_status::invalid_state);
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("page")).accepted_bytes_ == 4);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.sequence_, std::string("HHDE"));
    EXPECT_EQ(client.headers_.size(), std::size_t{2});
    if (client.headers_.size() != 2) {
        return;
    }
    EXPECT_TRUE(client.headers_[0].status_ && client.headers_[0].status_->value() == 103);
    EXPECT_TRUE(client.headers_[0].field("link") == std::string_view("</style.css>; rel=preload; as=style"));
    EXPECT_TRUE(!client.headers_[0].stream_ended_);
    EXPECT_TRUE(client.headers_[1].status_ && client.headers_[1].status_->value() == 200);
    EXPECT_TRUE(client.headers_[1].field("content-length") == std::string_view("4"));
    EXPECT_EQ(client.data(0), std::string("page"));
}

// submit_data() holds at most max_stream_output_bytes_ unacknowledged DATA per
// stream; acknowledgements free the budget and delivery stays in order.
TEST(Ruvia, http3_submit_data_is_bounded_by_unacknowledged_output) {
    ruvia::test::http3_peers peers({.max_stream_output_bytes_ = 8});
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response head(&resource);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        peers.server_.submit_streaming_response_head(
            0, ruvia::http_known_method::get, std::move(head), ruvia::http_response_trailer_intent::none)));

    constexpr std::string_view payload = "abcdefghijklmnopqrst";
    const auto first = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload));
    EXPECT_TRUE(first.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(first.accepted_bytes_, std::size_t{8});
    const auto blocked = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8)));
    EXPECT_TRUE(blocked.status_ == ruvia::http3_status::would_block);
    EXPECT_EQ(blocked.accepted_bytes_, std::size_t{0});
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    // Delivery alone frees nothing: the budget waits for the acknowledgement.
    ruvia::test::http3_event_log client;
    std::vector<ruvia::test::http3_withheld_acknowledgement> withheld;
    EXPECT_TRUE(peers.to_client(client, &withheld));
    EXPECT_EQ(client.data(0), std::string(payload.substr(0, 8)));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8))).status_ ==
                ruvia::http3_status::would_block);

    EXPECT_TRUE(ruvia::test::acknowledge_http3(peers.server_, withheld));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto second = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(8)));
    EXPECT_TRUE(second.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(second.accepted_bytes_, std::size_t{8});
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto third = peers.server_.submit_data(0, ruvia::test::http3_bytes(payload.substr(16)));
    EXPECT_TRUE(third.status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(third.accepted_bytes_, std::size_t{4});
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_client(client));

    EXPECT_EQ(client.data(0), std::string(payload));
    EXPECT_TRUE(client.ended(0));
    EXPECT_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
}

TEST(Ruvia, http3_connect_response_has_no_content_framing_and_preserves_half_close) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0,
                    ruvia::http3_connect_request_head_view{.authority_ = "echo.test:443"}) == ruvia::http3_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.header("content-length", "0");
    EXPECT_TRUE(peers.server_.submit_connect_response_head(0, response) ==
                ruvia::http3_response_head_error::invalid_message);
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(!peers.server_.submit_connect_response_head(0, accepted));
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && !client.headers_[0].stream_ended_ &&
                !client.headers_[0].field("content-length") && !client.headers_[0].field("transfer-encoding"));
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("request")).accepted_bytes_ == 7);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_server(server));
    EXPECT_TRUE(server.data(0) == "request" && server.ended(0));
    EXPECT_TRUE(peers.server_.submit_data(0, ruvia::test::http3_bytes("reply")).accepted_bytes_ == 5);
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.data(0) == "reply" && client.ended(0));
}

TEST(Ruvia, http3_websocket_extended_connect_commits_negotiated_200) {
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    constexpr std::array fields{ruvia::http_header_view{"sec-websocket-version", "13"}};
    ASSERT_TRUE(peers.client_.submit_request_head(0,
                    ruvia::http3_extended_connect_request_head_view{
                        .protocol_ = "websocket", .authority_ = "example.test", .target_ = "/ws", .headers_ = fields}) ==
                ruvia::http3_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* headers = std::get_if<ruvia::http3_headers_event>(&event)) {
            if (auto parsed = ruvia::http3_request_head::parse(
                    std::move(*headers), std::pmr::get_default_resource())) {
                head.emplace(std::move(*parsed));
            } else {
                head.reset();
            }
        }
    }));
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    const auto request = head->build_request(std::pmr::get_default_resource());
    const auto validation = ruvia::validate_http3_websocket_handshake(*head, request);
    EXPECT_TRUE(validation.accepted() != nullptr);
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(
        peers.server_.submit_websocket_handshake(0, request, validation)));
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.headers_.size() == 1 && client.headers_[0].status_ && client.headers_[0].status_->value() == 200 &&
                !client.headers_[0].field("content-length") && !client.headers_[0].field("sec-websocket-accept"));
}

TEST(Ruvia, http3_client_processes_all_600_to_999_statuses_as_server_errors) {
    for (unsigned code = 600; code <= 999; ++code) {
        SCOPED_TRACE(code);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string digits(3, '0');
        digits[0] = static_cast<char>('0' + code / 100);
        digits[1] = static_cast<char>('0' + code / 10 % 10);
        digits[2] = static_cast<char>('0' + code % 10);
        std::string wire;
        append_http3_wire_headers(wire, {{":status", digits}, {"content-length", "3"}});
        append_http3_wire_frame(wire, 0, "err");
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, true, client).status_ == ruvia::http3_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        ASSERT_TRUE(client.headers_[0].status_.has_value());
        EXPECT_EQ(client.headers_[0].status_->value(), code);
        EXPECT_TRUE(client.headers_[0].status_->status() == ruvia::http_status::internal_server_error);
        EXPECT_FALSE(client.headers_[0].field(":status").has_value());
        EXPECT_EQ(client.data(0), "err");
        EXPECT_TRUE(client.ended(0));
        EXPECT_TRUE(client.transport_actions_.empty());
        EXPECT_FALSE(peers.client_.connection_error().has_value());
    }
}

// The official nghttp3 dependency rejects 000..099 before the application
// callback. Keep this explicitly accepted dependency exception visible.
TEST(Ruvia, http3_official_nghttp3_rejects_000_to_099_before_publishing_client_heads) {
    for (const std::string_view digits : {"000", "099"}) {
        SCOPED_TRACE(digits);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", digits}});
        ruvia::test::http3_event_log client;
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_TRUE(client.headers_.empty());
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_rejects_invalid_response_heads_before_publication) {
    const std::vector<std::vector<ruvia::http3_field_view>> invalid_heads{
        {{":status", "101"}},
        {{":status", "20"}},
        {{":status", "2000"}},
        {{":status", "2x0"}},
        {{":status", " 200"}},
        {{"x-before", "value"}, {":status", "200"}},
        {{":status", "200"}, {":status", "204"}},
        {{":method", "GET"}, {":status", "200"}},
        {{"x-no-status", "value"}},
        {{":status", "200"}, {"connection", "close"}},
        {{":status", "200"}, {"transfer-encoding", "chunked"}},
        {{":status", "200"}, {"content-length", "bad"}},
        {{":status", "200"}, {"content-length", "2"}, {"content-length", "3"}},
        {{":status", "200"}, {"te", "trailers"}},
    };
    for (std::size_t index = 0; index < invalid_heads.size(); ++index) {
        SCOPED_TRACE(index);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, invalid_heads[index]);
        ruvia::test::http3_event_log client;
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_TRUE(client.headers_.empty());
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_validates_interim_final_and_retained_trailer_fields_on_wire) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    std::string wire;
    append_http3_wire_headers(wire, {{":status", "103"}, {"link", "</style.css>; rel=preload"}});
    append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "3"}, {"x-head", "valid"},
                                        {"x-invalid", "bad\r\nvalue"}});
    append_http3_wire_frame(wire, 0, "abc");
    append_http3_wire_headers(wire, {{"content-length", "999"}, {"host", "wrong.test"},
                                        {"x-checksum", "sha-256=abc"}, {"server-timing", "db;dur=5"}, {"x-invalid", "bad\r\nvalue"}});
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, true, client).status_ == ruvia::http3_status::accepted);
    ASSERT_EQ(client.headers_.size(), std::size_t{3});
    ASSERT_TRUE(client.headers_[0].status_.has_value());
    ASSERT_TRUE(client.headers_[1].status_.has_value());
    EXPECT_EQ(client.headers_[0].status_->value(), 103);
    EXPECT_EQ(client.headers_[1].status_->value(), 200);
    EXPECT_TRUE(client.headers_[1].field("x-head") == "valid");
    EXPECT_FALSE(client.headers_[1].field("x-invalid").has_value());
    const auto& trailers = client.headers_[2];
    EXPECT_TRUE(trailers.trailers_);
    EXPECT_FALSE(trailers.status_.has_value());
    EXPECT_EQ(trailers.fields_.size(), std::size_t{2});
    EXPECT_FALSE(trailers.field("content-length").has_value());
    EXPECT_FALSE(trailers.field("host").has_value());
    EXPECT_FALSE(trailers.field("x-invalid").has_value());
    EXPECT_TRUE(trailers.field("x-checksum") == "sha-256=abc");
    EXPECT_TRUE(trailers.field("server-timing") == "db;dur=5");
    EXPECT_EQ(client.data(0), "abc");
    EXPECT_EQ(client.sequence_, "HHDHE");
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(client.transport_actions_.empty());
}

TEST(Ruvia, http3_client_rejects_invalid_trailer_sections_without_publishing_partial_fields) {
    const std::vector<std::vector<ruvia::http3_field_view>> invalid_trailers{
        {{"x-checksum", "valid"}, {":status", "200"}},
        {{"x-checksum", "valid"}, {":path", "/"}},
        {{"x-checksum", "valid"}, {"te", "trailers"}},
    };
    for (std::size_t index = 0; index < invalid_trailers.size(); ++index) {
        SCOPED_TRACE(index);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "3"}});
        append_http3_wire_frame(wire, 0, "abc");
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, false, client).status_ == ruvia::http3_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        wire.clear();
        append_http3_wire_headers(wire, invalid_trailers[index]);
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_EQ(client.headers_.size(), std::size_t{1});
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_client_rejects_short_and_excess_content_without_publishing_completion) {
    for (const std::string_view declared : {"2", "4"}) {
        SCOPED_TRACE(declared);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
        std::string wire;
        append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", declared}});
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(receive_http3_wire_response(peers, 0, wire, false, client).status_ == ruvia::http3_status::accepted);
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        wire.clear();
        append_http3_wire_frame(wire, 0, "abc");
        const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
        EXPECT_FALSE(client.ended(0));
        EXPECT_TRUE(rejected_http3_message(result, peers.client_, client, 0));
    }
}

TEST(Ruvia, http3_core_response_validation_resets_only_the_malformed_stream) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    ASSERT_TRUE(peers.send_request(4, get_request_head, true, server));
    std::string wire;
    // nghttp3 permits request-style TE: trailers in a response. The core's
    // response-specific rule rejects it without invalidating the codec.
    append_http3_wire_headers(wire, {{":status", "200"}, {"te", "trailers"}});
    ruvia::test::http3_event_log client;
    const auto result = receive_http3_wire_response(peers, 0, wire, true, client);
    EXPECT_TRUE(result.status_ == ruvia::http3_status::accepted);
    EXPECT_TRUE(has_message_error(client, 0));
    EXPECT_TRUE(client.headers_.empty());
    EXPECT_FALSE(client.ended(0));
    EXPECT_FALSE(peers.client_.connection_error().has_value());
    wire.clear();
    append_http3_wire_headers(wire, {{":status", "200"}, {"content-length", "2"}});
    append_http3_wire_frame(wire, 0, "ok");
    EXPECT_TRUE(receive_http3_wire_response(peers, 4, wire, true, client).status_ == ruvia::http3_status::accepted);
    EXPECT_EQ(client.data(4), "ok");
    EXPECT_TRUE(client.ended(4));
}
