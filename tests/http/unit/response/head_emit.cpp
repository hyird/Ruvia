#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

namespace {

using ruvia::http1_buffered_response_plan;
using ruvia::http1_chunked_response_stream_head_plan;
using ruvia::http1_close_delimited_response_stream_head_plan;
using ruvia::http1_known_length_response_stream_head_plan;
using ruvia::http1_request_connection_plan;
using ruvia::http1_response_head_plan;
using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::plan_http_response_body;

ruvia::http1_request_connection_plan connection_plan_for(
    ruvia::http_protocol_version protocol_version) {
    return protocol_version == ruvia::http_protocol_version::http10
               ? ruvia::plan_http10_request_connection(false, false)
               : ruvia::plan_http11_request_connection(false);
}

std::string emit_head(http_response& response, const http1_response_head_plan& plan) {
    ruvia::http_response_head_buffer buffer(std::pmr::new_delete_resource());
    ruvia::append_http1_response_head(response, buffer, plan);
    const auto view = buffer.view();
    return std::string(view.data(), view.size());
}

std::string emit_buffered_head(http_response& response,
    http_known_method request_method = http_known_method::get,
    ruvia::http_protocol_version protocol_version = ruvia::http_protocol_version::http11) {
    const auto write_plan = ruvia::plan_buffered_http_response_write(request_method, response);
    const auto response_plan =
        get_http1_buffered_response_plan(write_plan, connection_plan_for(protocol_version));
    return emit_head(response, response_plan.head_plan());
}

std::string emit_chunked_stream_head(http_response& response,
    http_known_method request_method = http_known_method::get,
    ruvia::http_protocol_version protocol_version = ruvia::http_protocol_version::http11) {
    return emit_head(response,
        http1_chunked_response_stream_head_plan(plan_http_response_body(request_method, response.status()),
            connection_plan_for(protocol_version)));
}

std::string emit_known_length_stream_head(http_response& response, std::uint64_t content_length,
    http_known_method request_method = http_known_method::get,
    ruvia::http_protocol_version protocol_version = ruvia::http_protocol_version::http11) {
    return emit_head(response, http1_known_length_response_stream_head_plan(
                                   plan_http_response_body(request_method, response.status()),
                                   connection_plan_for(protocol_version), content_length));
}

std::string emit_close_delimited_stream_head(http_response& response,
    http_known_method request_method = http_known_method::get,
    ruvia::http_protocol_version protocol_version = ruvia::http_protocol_version::http11) {
    return emit_head(response, http1_close_delimited_response_stream_head_plan(
                                   plan_http_response_body(request_method, response.status()),
                                   connection_plan_for(protocol_version)));
}

std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = haystack.find(needle); pos != std::string_view::npos;
        pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

template <typename fn_type>
bool throws_invalid(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

template <typename fn_type>
bool throws_length(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::length_error&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(http1_buffered_response_plan_owns_request_version_and_length) {
    using ruvia::http1_server_request_parser;
    using ruvia::plan_buffered_http_response_write;

    http1_server_request_parser parser;
    const auto emit_for = [&](std::string_view request) {
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        response.body("hello");
        const auto connection_plan = parser.parse_message(request).connection_plan_;
        if (connection_plan.protocol_version() == ruvia::http_protocol_version::http10) {
            response.header("Connection", "keep-alive");
        }
        const auto response_plan = get_http1_buffered_response_plan(
            plan_buffered_http_response_write(http_known_method::get, response), connection_plan);
        RUVIA_CHECK_EQ(
            response_plan.head_plan().buffered()->content_length(), response_plan.content_length());
        return std::pair(
            emit_head(response, response_plan.head_plan()), response_plan.head_plan().protocol_version());
    };

    const auto [http10_head, http10_version] =
        emit_for("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    RUVIA_CHECK(http10_version == ruvia::http_protocol_version::http10);
    RUVIA_CHECK(http10_head.starts_with("HTTP/1.0 200 OK\r\n"));
    RUVIA_CHECK((http10_head.find("Content-Length: 5\r\n") != std::string_view::npos));
    RUVIA_CHECK((http10_head.find("Connection: keep-alive\r\n") != std::string_view::npos));

    const auto [http11_head, http11_version] = emit_for("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(http11_version == ruvia::http_protocol_version::http11);
    RUVIA_CHECK(http11_head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(!(http11_head.find("Connection:") != std::string_view::npos));
}

RUVIA_TEST(http1_response_head_rejects_status_plan_mismatch) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::multi_status);
    response.body("planned");
    const auto plan = get_http1_buffered_response_plan(
        ruvia::plan_buffered_http_response_write(http_known_method::get, response),
        connection_plan_for(ruvia::http_protocol_version::http11));

    response.status(ruvia::http_status::already_reported);
    RUVIA_CHECK(throws_invalid([&] { (void)emit_head(response, plan.head_plan()); }));
    RUVIA_CHECK_EQ(plan.response_status(), ruvia::http_status::multi_status);
    RUVIA_CHECK_EQ(plan.head_plan().body_plan().response_status(), ruvia::http_status::multi_status);
}

RUVIA_TEST(http1_response_head_rejects_representation_plan_mismatch) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto plan = get_http1_buffered_response_plan(
        ruvia::plan_buffered_http_response_write(http_known_method::get, response),
        connection_plan_for(ruvia::http_protocol_version::http11));

    response.body("longer");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_head(response, plan.head_plan()); }));
    RUVIA_CHECK_EQ(plan.content_length(), std::uint64_t{3});
}

RUVIA_TEST(http1_response_head_validates_trailer_field_names) {
    const auto rejects = [&ruvia_ctx](std::string_view value) {
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        response.header_stable_view("Trailer", value);
        RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(response); }));
    };

    rejects("Content-Length");
    rejects("X-Checksum, bad field");
    rejects(",");

    http_response valid({.resource_ = std::pmr::new_delete_resource()});
    valid.header("Trailer", "ETag, X-Checksum");
    const auto valid_head = emit_chunked_stream_head(valid);
    RUVIA_CHECK(valid_head.find("Trailer: ETag, X-Checksum\r\n") != std::string_view::npos);

    http_response empty({.resource_ = std::pmr::new_delete_resource()});
    empty.header("Trailer", "");
    RUVIA_CHECK(!throws_invalid([&] { (void)emit_buffered_head(empty); }));
}

RUVIA_TEST(http1_response_head_rejects_non_empty_trailer_without_chunked_framing) {
    http_response buffered({.resource_ = std::pmr::new_delete_resource()});
    buffered.header("Trailer", "ETag");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(buffered); }));

    http_response known_length({.resource_ = std::pmr::new_delete_resource()});
    known_length.header("Trailer", "ETag");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_known_length_stream_head(known_length, 5); }));

    http_response close_delimited({.resource_ = std::pmr::new_delete_resource()});
    close_delimited.header("Trailer", "ETag");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_close_delimited_stream_head(close_delimited); }));

    http_response head({.resource_ = std::pmr::new_delete_resource()});
    head.header("Trailer", "ETag");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_chunked_stream_head(head, http_known_method::head); }));

    http_response chunked({.resource_ = std::pmr::new_delete_resource()});
    chunked.header("Trailer", "ETag");
    RUVIA_CHECK(!throws_invalid([&] { (void)emit_chunked_stream_head(chunked); }));
}

RUVIA_TEST(response_head_emits_well_formed_normal) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::ok);
    response.header("X-Foo", "bar");
    response.body("hello");
    const auto head = emit_buffered_head(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK((head.find("X-Foo: bar\r\n") != std::string_view::npos));
    RUVIA_CHECK(!(head.find("Server:") != std::string_view::npos));               // product policy is explicit
    RUVIA_CHECK((head.find("Date: ") != std::string_view::npos));                 // auto-injected
    RUVIA_CHECK((head.find("Content-Length: 5\r\n") != std::string_view::npos));  // auto, body size
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));                                      // blank-line terminator
}

RUVIA_TEST(response_head_extension_status_uses_an_empty_reason_phrase) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status_code::from_value(299));
    const auto head = emit_buffered_head(response);

    // RFC 9112 section 4 keeps the SP before the optional reason-phrase.
    // An unregistered status must not be mislabeled as a generic client error.
    RUVIA_CHECK(head.starts_with("HTTP/1.1 299 \r\n"));
    RUVIA_CHECK(!(head.find("Bad Request") != std::string_view::npos));
}

RUVIA_TEST(response_head_preserves_explicit_server_and_does_not_duplicate_date) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.header("Server", "custom");
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.body("x");
    const auto head = emit_buffered_head(response);

    RUVIA_CHECK((head.find("Server: custom\r\n") != std::string_view::npos));
    RUVIA_CHECK_EQ(count_occurrences(head, "Server: "), std::size_t{1});
    RUVIA_CHECK_EQ(count_occurrences(head, "Date: "), std::size_t{1});  // exactly one Date
}

RUVIA_TEST(response_head_suppresses_auto_content_length) {
    // A streaming/chunked writer owns framing itself. Caller-provided framing is
    // replaced by one canonical chunked field and no Content-Length survives.
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.body("hello");
    response.header("Transfer-Encoding", "gzip, chunked");
    response.header("Content-Length", "999");
    const auto head = emit_chunked_stream_head(response);
    RUVIA_CHECK(!(head.find("Content-Length:") != std::string_view::npos));
    RUVIA_CHECK((head.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
    RUVIA_CHECK(!(head.find("gzip") != std::string_view::npos));
    RUVIA_CHECK_EQ(count_occurrences(head, "Transfer-Encoding: "), std::size_t{1});
}

RUVIA_TEST(response_head_canonicalizes_managed_framing_fields_across_case) {
    std::uint64_t state_value = 0x5d28'c4f1'9e73'ab06ULL;
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
        http_response buffered({.resource_ = std::pmr::new_delete_resource()});
        buffered.body("payload");
        buffered.header(random_case("Content-Length"), "999");
        buffered.header(random_case("Transfer-Encoding"), "gzip, chunked");
        buffered.header("X-Sample", std::to_string(sample));
        const auto buffered_head = emit_buffered_head(buffered);
        RUVIA_CHECK_EQ(count_occurrences(buffered_head, "Content-Length: "), std::size_t{1});
        RUVIA_CHECK((buffered_head.find("Content-Length: 7\r\n") != std::string_view::npos));
        RUVIA_CHECK(!(buffered_head.find("Content-Length: 999\r\n") != std::string_view::npos));
        RUVIA_CHECK(!(buffered_head.find("Transfer-Encoding:") != std::string_view::npos));
        RUVIA_CHECK(!(buffered_head.find("gzip") != std::string_view::npos));
        RUVIA_CHECK(buffered_head.ends_with("\r\n\r\n"));

        http_response chunked({.resource_ = std::pmr::new_delete_resource()});
        chunked.header(random_case("Content-Length"), "999");
        chunked.header(random_case("Transfer-Encoding"), "gzip, chunked");
        chunked.header("X-Sample", std::to_string(sample));
        const auto chunked_head = emit_chunked_stream_head(chunked);
        RUVIA_CHECK(!(chunked_head.find("Content-Length:") != std::string_view::npos));
        RUVIA_CHECK_EQ(count_occurrences(chunked_head, "Transfer-Encoding: "), std::size_t{1});
        RUVIA_CHECK((chunked_head.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
        RUVIA_CHECK(!(chunked_head.find("gzip") != std::string_view::npos));
        RUVIA_CHECK(chunked_head.ends_with("\r\n\r\n"));
    }
}

RUVIA_TEST(http1_response_head_rejects_http10_chunked_payload_plan) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.body("hello");
    const auto plan = http1_chunked_response_stream_head_plan(
        plan_http_response_body(http_known_method::get, response.status()),
        connection_plan_for(ruvia::http_protocol_version::http10));

    RUVIA_CHECK(throws_invalid([&] { (void)emit_head(response, plan); }));
}

RUVIA_TEST(http1_known_length_stream_owns_canonical_length_and_status_semantics) {
    http_response get({.resource_ = std::pmr::new_delete_resource()});
    get.header("Content-Length", "999");
    get.header("Transfer-Encoding", "chunked");
    const auto get_head = emit_known_length_stream_head(get, 5);
    RUVIA_CHECK_EQ(count_occurrences(get_head, "Content-Length: "), std::size_t{1});
    RUVIA_CHECK((get_head.find("Content-Length: 5\r\n") != std::string_view::npos));
    RUVIA_CHECK(!(get_head.find("Transfer-Encoding:") != std::string_view::npos));

    http_response head({.resource_ = std::pmr::new_delete_resource()});
    const auto head_wire = emit_known_length_stream_head(head, 5, http_known_method::head);
    RUVIA_CHECK((head_wire.find("Content-Length: 5\r\n") != std::string_view::npos));

    http_response no_content({.resource_ = std::pmr::new_delete_resource()});
    no_content.status(ruvia::http_status::no_content);
    no_content.header("Content-Length", "5");
    const auto no_content_wire = emit_known_length_stream_head(no_content, 5);
    RUVIA_CHECK(!(no_content_wire.find("Content-Length:") != std::string_view::npos));
    RUVIA_CHECK(!(no_content_wire.find("Transfer-Encoding:") != std::string_view::npos));
}

RUVIA_TEST(http1_chunked_stream_does_not_invent_framing_for_head) {
    http_response metadata({.resource_ = std::pmr::new_delete_resource()});
    metadata.header("Content-Length", "5");
    metadata.header("Transfer-Encoding", "chunked");
    const auto metadata_wire = emit_chunked_stream_head(metadata, http_known_method::head);
    RUVIA_CHECK((metadata_wire.find("Content-Length: 5\r\n") != std::string_view::npos));
    RUVIA_CHECK(!(metadata_wire.find("Transfer-Encoding:") != std::string_view::npos));

    http_response unknown({.resource_ = std::pmr::new_delete_resource()});
    const auto unknown_wire = emit_chunked_stream_head(unknown, http_known_method::head);
    RUVIA_CHECK(!(unknown_wire.find("Content-Length:") != std::string_view::npos));
    RUVIA_CHECK(!(unknown_wire.find("Transfer-Encoding:") != std::string_view::npos));
}

RUVIA_TEST(response_head_close_delimited_stream_rejects_declared_framing) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.body("streamed");
    response.header("Transfer-Encoding", "chunked");
    response.header("Content-Length", "8");

    const auto head = emit_close_delimited_stream_head(response);
    RUVIA_CHECK(!(head.find("Transfer-Encoding:") != std::string_view::npos));
    RUVIA_CHECK(!(head.find("Content-Length:") != std::string_view::npos));

    // A HEAD response has no payload and may retain representation length
    // metadata, but HTTP/1.0 still cannot carry Transfer-Encoding.
    const auto metadata_head = emit_close_delimited_stream_head(response, http_known_method::head);
    RUVIA_CHECK(!(metadata_head.find("Transfer-Encoding:") != std::string_view::npos));
    RUVIA_CHECK((metadata_head.find("Content-Length: 8\r\n") != std::string_view::npos));

    http_response not_modified({.resource_ = std::pmr::new_delete_resource()});
    not_modified.status(ruvia::http_status::not_modified);
    not_modified.header("Transfer-Encoding", "chunked");
    not_modified.header("Content-Length", "123");
    const auto not_modified_head = emit_close_delimited_stream_head(not_modified);
    RUVIA_CHECK(!(not_modified_head.find("Transfer-Encoding:") != std::string_view::npos));
    RUVIA_CHECK((not_modified_head.find("Content-Length: 123\r\n") != std::string_view::npos));
}

RUVIA_TEST(response_head_validates_explicit_content_length_metadata) {
    http_response malformed({.resource_ = std::pmr::new_delete_resource()});
    malformed.status(ruvia::http_status::not_modified);
    malformed.header("Content-Length", "invalid");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(malformed); }));

    http_response conflicting({.resource_ = std::pmr::new_delete_resource()});
    conflicting.status(ruvia::http_status::not_modified);
    conflicting.header("Content-Length", "7, 8");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(conflicting); }));

    http_response equivalent({.resource_ = std::pmr::new_delete_resource()});
    equivalent.status(ruvia::http_status::not_modified);
    equivalent.header("Content-Length", "0007, 7");
    const auto canonical = emit_buffered_head(equivalent);
    RUVIA_CHECK_EQ(count_occurrences(canonical, "Content-Length: "), std::size_t{1});
    RUVIA_CHECK((canonical.find("Content-Length: 7\r\n") != std::string_view::npos));

    http_response head_metadata({.resource_ = std::pmr::new_delete_resource()});
    head_metadata.header("Content-Length", "bad");
    RUVIA_CHECK(throws_invalid(
        [&] { (void)emit_close_delimited_stream_head(head_metadata, http_known_method::head); }));
}

RUVIA_TEST(response_head_bodyless_status_omits_auto_content_length) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::no_content);
    const auto head = emit_buffered_head(response);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 204 No Content\r\n"));
    RUVIA_CHECK(!(head.find("Content-Length:") != std::string_view::npos));
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_reset_content_canonicalizes_zero_length) {
    // RFC 9110 §15.3.6 forbids 205 content. Even if the application supplies a
    // body and contradictory framing, both buffered and streaming head emission
    // must suppress it and retain an unambiguous persistent HTTP/1 message.
    for (const bool streaming : {false, true}) {
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        response.status(ruvia::http_status::reset_content);
        response.body("must-not-be-sent");
        response.header("Content-Length", "16");
        response.header("Transfer-Encoding", "chunked");
        const auto head = streaming ? emit_chunked_stream_head(response) : emit_buffered_head(response);
        RUVIA_CHECK(head.starts_with("HTTP/1.1 205 Reset Content\r\n"));
        RUVIA_CHECK_EQ(count_occurrences(head, "Content-Length: "), std::size_t{1});
        RUVIA_CHECK((head.find("Content-Length: 0\r\n") != std::string_view::npos));
        RUVIA_CHECK(!(head.find("Content-Length: 16\r\n") != std::string_view::npos));
        RUVIA_CHECK(!(head.find("Transfer-Encoding:") != std::string_view::npos));
        RUVIA_CHECK(head.ends_with("\r\n\r\n"));
    }
}

RUVIA_TEST(response_head_heap_spill_preserves_full_output) {
    // Force the emitted head well past the 512-byte stack buffer so the heap
    // (reserve_additional) emit path runs. Every header must survive intact and
    // the precomputed size bound must not undercount -- an undercount would let
    // the unchecked raw stack sink overflow or the output truncate.
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::ok);
    const std::string big(200, 'v');
    for (int i = 0; i < 10; ++i) {
        response.header("X-Pad-" + std::to_string(i), big);
    }
    response.body("body");
    const auto head = emit_buffered_head(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    for (int i = 0; i < 10; ++i) {
        RUVIA_CHECK(head.find("X-Pad-" + std::to_string(i) + ": " + big + "\r\n") !=
                    std::string_view::npos);
    }
    RUVIA_CHECK((head.find("Content-Length: 4\r\n") != std::string_view::npos));
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_rejects_oversized_field_section) {
    http_response oversized({.resource_ = std::pmr::new_delete_resource()});
    oversized.header("X-Oversized", std::string(ruvia::max_http_header_bytes, 'v'));
    RUVIA_CHECK(throws_length([&] { (void)emit_buffered_head(oversized); }));

    http_response too_many({.resource_ = std::pmr::new_delete_resource()});
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        too_many.header("X-Field-" + std::to_string(i), "value");
    }
    RUVIA_CHECK(throws_length([&] { (void)emit_buffered_head(too_many); }));

    http_response generated_overflow({.resource_ = std::pmr::new_delete_resource()});
    for (std::size_t i = 0; i < ruvia::max_http_header_fields - 1; ++i) {
        generated_overflow.header("X-Generated-" + std::to_string(i), "value");
    }
    RUVIA_CHECK(throws_length([&] {
        // The generated Date and Content-Length fields also consume slots.
        (void)emit_buffered_head(generated_overflow);
    }));
}

RUVIA_TEST(response_head_rejects_malformed_header_name_and_value) {
    http_response bad_name({.resource_ = std::pmr::new_delete_resource()});
    bad_name.header_stable_view("Bad Name", "value");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(bad_name); }));

    http_response bad_value({.resource_ = std::pmr::new_delete_resource()});
    bad_value.header_stable_view("X-Test", std::string_view("bad\r\nvalue", 10));
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(bad_value); }));
}

RUVIA_TEST(response_head_rejects_request_only_te_field) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.header_stable_view("TE", "trailers");
    RUVIA_CHECK(throws_invalid([&] { (void)emit_buffered_head(response); }));
}
