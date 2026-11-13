#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::http1_buffered_response_plan;
using ruvia::http1_chunked_response_stream_head_plan;
using ruvia::http1_close_delimited_response_stream_head_plan;
using ruvia::http1_request_connection_plan;
using ruvia::http1_response_head_plan;
using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::http_response_header_options;
using ruvia::plan_http_response_body;

ruvia::http1_request_connection_plan connection_plan_for(
    ruvia::http_protocol_version protocol_version) {
    return protocol_version == ruvia::http_protocol_version::http10
               ? ruvia::plan_http10_request_connection(false, false)
               : ruvia::plan_http11_request_connection(false);
}

std::string emit_head(http_response& response, const http1_response_head_plan& plan) {
    ruvia::http_response_head_buffer buffer;
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
bool throws_head_error(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const ruvia::http1_response_head_error&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, http1_head_metadata_body_preserves_declared_or_unknown_representation_length) {
    for (const auto version : {ruvia::http_protocol_version::http10, ruvia::http_protocol_version::http11}) {
        http_response response;
        response.head_metadata_body();
        const auto unknown = emit_buffered_head(response, http_known_method::head, version);
        EXPECT_TRUE(unknown.find("Content-Length:") == std::string::npos);

        response.header("Content-Length", "256");
        const auto declared = emit_buffered_head(response, http_known_method::head, version);
        EXPECT_TRUE(declared.find("Content-Length: 256\r\n") != std::string::npos);
        const auto get = emit_buffered_head(response, http_known_method::get, version);
        EXPECT_TRUE(get.find("Content-Length: 0\r\n") != std::string::npos);

        response.remove_header("Content-Length");
        response.body("");
        const auto empty = emit_buffered_head(response, http_known_method::head, version);
        EXPECT_TRUE(empty.find("Content-Length: 0\r\n") != std::string::npos);
    }
}

TEST(Ruvia, http1_response_head_append_accepts_overlapping_written_inline_ranges) {
    for (const auto back : {std::size_t{0}, std::size_t{1}, ruvia::response_head_stack_bytes / 8, ruvia::response_head_stack_bytes / 4}) {
        ruvia::http_response_head_buffer buffer;
        const auto size = ruvia::response_head_stack_bytes / 4;
        std::string prefix(size, 'a');
        std::string generated(size, 'A');
        for (std::size_t index = 0; index < size; ++index) {
            prefix[index] = static_cast<char>('a' + index % 26);
            generated[index] = static_cast<char>('A' + index % 26);
        }
        buffer.append(prefix);
        auto* const cursor = buffer.stack_cursor(size);
        EXPECT_TRUE(cursor != nullptr);
        if (cursor == nullptr) {
            continue;
        }
        std::memcpy(cursor, generated.data(), size);
        const std::string_view source(cursor - back, size);
        const std::string expected = prefix + std::string(source);
        buffer.append(source);
        EXPECT_EQ(buffer.view(), std::string_view(expected));
    }
}

TEST(Ruvia, http1_response_head_commit_checks_owner_and_accepts_inline_boundaries) {
    ruvia::http_response_head_buffer buffer;
    ruvia::http_response_head_buffer other;
    buffer.append("prefix");
    EXPECT_TRUE(throws_invalid([&] { buffer.commit_stack(nullptr); }));
    EXPECT_EQ(buffer.view(), std::string_view("prefix"));
    EXPECT_TRUE(throws_invalid([&] { buffer.commit_stack(other.stack_cursor(1)); }));
    EXPECT_EQ(buffer.view(), std::string_view("prefix"));

    const auto remaining = ruvia::response_head_stack_bytes - buffer.view().size();
    auto* const cursor = buffer.stack_cursor(remaining);
    EXPECT_TRUE(cursor != nullptr);
    if (cursor == nullptr) {
        return;
    }
    std::memset(cursor, 's', remaining);
    buffer.commit_stack(cursor + remaining);
    const std::string expected = "prefix" + std::string(remaining, 's');
    EXPECT_EQ(buffer.view(), std::string_view(expected));
    buffer.commit_stack(cursor);
    EXPECT_EQ(buffer.view(), std::string_view("prefix"));
    buffer.commit_stack(cursor - 6);
    EXPECT_TRUE(buffer.view().empty());
    buffer.append("reused");
    EXPECT_EQ(buffer.view(), std::string_view("reused"));
}

TEST(Ruvia, http1_response_head_commit_after_spill_rejects_without_losing_content) {
    ruvia::http_response_head_buffer buffer;
    buffer.append("prefix");
    auto* const cursor = buffer.stack_cursor(1);
    EXPECT_TRUE(cursor != nullptr);
    if (cursor == nullptr) {
        return;
    }
    cursor[0] = 'x';
    buffer.reserve_additional(ruvia::response_head_stack_bytes);
    bool rejected = false;
    try {
        buffer.commit_stack(cursor + 1);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(buffer.view(), std::string_view("prefix"));
    buffer.append("-retained");
    EXPECT_EQ(buffer.view(), std::string_view("prefix-retained"));
    buffer.reset();
    auto* const fresh = buffer.stack_cursor(1);
    EXPECT_TRUE(fresh != nullptr);
    if (fresh == nullptr) {
        return;
    }
    fresh[0] = 'y';
    buffer.commit_stack(fresh + 1);
    EXPECT_EQ(buffer.view(), std::string_view("y"));
}

TEST(Ruvia, http1_response_trailers_preserve_fields_borrowed_from_output) {
    for (const std::size_t size : {std::size_t{8}, std::size_t{32}, std::size_t{4096}}) {
        for (const bool reserve_first : {false, true}) {
            std::string output;
            output = "X-Tail";
            output.append(size, 'v');
            if (reserve_first) {
                output.reserve(output.size() * 4 + 64);
            }
            const std::string prefix(output);
            const std::array<ruvia::http_header_view, 2> fields{{
                {std::string_view(output).substr(0, 6), std::string_view(output).substr(6)},
                {"X-Again", std::string_view(output).substr(6)},
            }};
            const auto section = ruvia::validate_http_response_trailers(fields);
            const std::string expected = prefix + "X-Tail: " + std::string(size, 'v') + "\r\nX-Again: " + std::string(size, 'v') + "\r\n";
            ruvia::append_http1_response_trailers(output, section);
            EXPECT_EQ(std::string_view(output), std::string_view(expected));
        }
    }
}

TEST(Ruvia, http1_buffered_response_plan_owns_request_version_and_length) {
    using ruvia::plan_buffered_http_response_write;
    using ruvia::test::parse_http1_request_message;

    const auto emit_for = [&](std::string_view request) {
        http_response response;
        response.body("hello");
        const auto connection_plan = parse_http1_request_message(request).state_.take_connection_plan();
        if (connection_plan.protocol_version() == ruvia::http_protocol_version::http10) {
            response.header("Connection", "keep-alive");
        }
        const auto response_plan = get_http1_buffered_response_plan(
            plan_buffered_http_response_write(http_known_method::get, response), connection_plan);
        EXPECT_EQ(response_plan.head_plan().buffered()->content_length(), response_plan.content_length());
        return std::pair(
            emit_head(response, response_plan.head_plan()), response_plan.head_plan().protocol_version());
    };

    const auto [http10_head, http10_version] =
        emit_for("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    EXPECT_TRUE(http10_version == ruvia::http_protocol_version::http10);
    EXPECT_TRUE(http10_head.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE((http10_head.find("Content-Length: 5\r\n") != std::string_view::npos));
    EXPECT_TRUE((http10_head.find("Connection: keep-alive\r\n") != std::string_view::npos));

    const auto [http11_head, http11_version] = emit_for("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(http11_version == ruvia::http_protocol_version::http11);
    EXPECT_TRUE(http11_head.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(!(http11_head.find("Connection:") != std::string_view::npos));
}

TEST(Ruvia, http1_response_head_rejects_status_plan_mismatch) {
    http_response response;
    response.status(ruvia::http_status::multi_status);
    response.body("planned");
    const auto plan = get_http1_buffered_response_plan(
        ruvia::plan_buffered_http_response_write(http_known_method::get, response),
        connection_plan_for(ruvia::http_protocol_version::http11));

    response.status(ruvia::http_status::already_reported);
    EXPECT_TRUE(throws_invalid([&] { (void)emit_head(response, plan.head_plan()); }));
    EXPECT_EQ(plan.head_plan().body_plan().response_status(), ruvia::http_status::multi_status);
}

TEST(Ruvia, http1_response_head_rejects_representation_plan_mismatch) {
    http_response response;
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto plan = get_http1_buffered_response_plan(
        ruvia::plan_buffered_http_response_write(http_known_method::get, response),
        connection_plan_for(ruvia::http_protocol_version::http11));

    response.body("longer");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_head(response, plan.head_plan()); }));
    EXPECT_EQ(plan.content_length(), std::uint64_t{3});
}

TEST(Ruvia, http1_response_head_requires_upgrade_connection_option) {
    for (const std::string_view connection : {"", "keep-alive", "close"}) {
        http_response response;
        response.header("Upgrade", "example/1");
        if (!connection.empty()) {
            response.header("Connection", connection);
        }
        const auto plan = get_http1_buffered_response_plan(
            ruvia::plan_buffered_http_response_write(http_known_method::get, response),
            connection_plan_for(ruvia::http_protocol_version::http11));
        ruvia::http_response_head_buffer buffer;
        buffer.append("existing output");

        EXPECT_TRUE(throws_invalid([&] {
            ruvia::append_http1_response_head(response, buffer, plan.head_plan());
        }));
        EXPECT_EQ(buffer.view(), std::string_view("existing output"));
    }
}

TEST(Ruvia, http1_response_head_upgrade_connection_fields_round_trip) {
    for (unsigned int combination = 0; combination != 3; ++combination) {
        http_response response;
        response.header("uPgRaDe", "example/1");
        response.body("hello");
        if (combination == 0) {
            response.header("cOnNeCtIoN", "keep-alive, UpGrAdE");
        } else {
            response.header("Connection", combination == 1 ? "keep-alive" : "UpGrAdE");
            response.header("cOnNeCtIoN", combination == 1 ? "UpGrAdE" : "keep-alive",
                http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
        }
        const auto head = emit_buffered_head(response);
        EXPECT_TRUE(head.find("UpGrAdE") != std::string_view::npos);

        std::array<char, 256> request_buffer;
        auto request = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::http({.host_ = "example.test"}), {}, request_buffer);
        auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&request);
        EXPECT_TRUE(prepared_req != nullptr);
        if (prepared_req == nullptr) {
            continue;
        }
        ruvia::http1_client_response_parser parser(
            std::move(*prepared_req).take_exchange_state());
        const auto parsed = parser.parse(head);
        EXPECT_TRUE(parsed.parsed() != nullptr);
        if (const auto* accepted = parsed.parsed()) {
            EXPECT_EQ(accepted->head().status(), ruvia::http_status::ok);
            EXPECT_EQ(accepted->consumed_bytes(), head.size());
            EXPECT_TRUE(accepted->plan().known_length() != nullptr);
            if (const auto* known_length = accepted->plan().known_length()) {
                EXPECT_EQ(known_length->content_length(), std::size_t{5});
                EXPECT_EQ(known_length->persistence(), ruvia::http1_close_policy::allow_reuse);
            }
        }
    }
}

TEST(Ruvia, http1_response_head_validates_trailer_field_names) {
    const auto rejects = [](std::string_view value) {
        http_response response;
        EXPECT_TRUE(throws_invalid([&] { response.header_stable_view("Trailer", value); }));
        EXPECT_TRUE(throws_invalid([&] { response.header("Trailer", value); }));
    };

    rejects("Content-Length");
    rejects("X-Checksum, bad field");
    rejects(",");

    http_response valid;
    valid.header("Trailer", "ETag, X-Checksum");
    const auto valid_head = emit_chunked_stream_head(valid);
    EXPECT_TRUE(valid_head.find("Trailer: ETag, X-Checksum\r\n") != std::string_view::npos);

    http_response empty;
    empty.header("Trailer", "");
    EXPECT_TRUE(!throws_invalid([&] { (void)emit_buffered_head(empty); }));
}

TEST(Ruvia, stable_view_headers_validate_representation_metadata_on_insertion) {
    for (const auto invalid : {ruvia::http_header_view{"Content-Type", "text plain"},
             ruvia::http_header_view{"Content-Encoding", "gzip;q=1"},
             ruvia::http_header_view{"Trailer", "Content-Length"}}) {
        http_response response;
        EXPECT_TRUE(throws_invalid([&] { response.header_stable_view(invalid.name(), invalid.value()); }));
        EXPECT_TRUE(!response.header(invalid.name()).has_value());
    }
}

TEST(Ruvia, http1_response_head_rejects_non_empty_trailer_without_chunked_framing) {
    http_response buffered;
    buffered.header("Trailer", "ETag");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_buffered_head(buffered); }));

    http_response close_delimited;
    close_delimited.header("Trailer", "ETag");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_close_delimited_stream_head(close_delimited); }));

    http_response head;
    head.header("Trailer", "ETag");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_chunked_stream_head(head, http_known_method::head); }));

    http_response chunked;
    chunked.header("Trailer", "ETag");
    EXPECT_TRUE(!throws_invalid([&] { (void)emit_chunked_stream_head(chunked); }));
}

TEST(Ruvia, response_head_emits_well_formed_normal) {
    http_response response;
    response.status(ruvia::http_status::ok);
    response.header("X-Foo", "bar");
    response.body("hello");
    const auto head = emit_buffered_head(response);

    EXPECT_TRUE(head.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE((head.find("X-Foo: bar\r\n") != std::string_view::npos));
    EXPECT_TRUE(!(head.find("Server:") != std::string_view::npos));               // product policy is explicit
    EXPECT_TRUE((head.find("Date: ") != std::string_view::npos));                 // auto-injected
    EXPECT_TRUE((head.find("Content-Length: 5\r\n") != std::string_view::npos));  // auto, body size
    EXPECT_TRUE(head.ends_with("\r\n\r\n"));                                      // blank-line terminator
}

TEST(Ruvia, response_head_extension_status_uses_an_empty_reason_phrase) {
    http_response response;
    response.status(ruvia::http_status_code::from_value(299));
    const auto head = emit_buffered_head(response);

    // RFC 9112 section 4 keeps the SP before the optional reason-phrase.
    // An unregistered status must not be mislabeled as a generic client error.
    EXPECT_TRUE(head.starts_with("HTTP/1.1 299 \r\n"));
    EXPECT_TRUE(!(head.find("Bad Request") != std::string_view::npos));
}

TEST(Ruvia, response_head_preserves_explicit_server_and_does_not_duplicate_date) {
    http_response response;
    response.header("Server", "custom");
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.body("x");
    const auto head = emit_buffered_head(response);

    EXPECT_TRUE((head.find("Server: custom\r\n") != std::string_view::npos));
    EXPECT_EQ(count_occurrences(head, "Server: "), std::size_t{1});
    EXPECT_EQ(count_occurrences(head, "Date: "), std::size_t{1});  // exactly one Date
}

TEST(Ruvia, response_head_suppresses_auto_content_length) {
    // A streaming/chunked writer owns framing itself. The handler's chunked
    // hint is replaced by one canonical chunked field and no Content-Length
    // survives.
    http_response response;
    response.body("hello");
    response.header("Transfer-Encoding", "ChUnKeD");
    response.header("Content-Length", "999");
    const auto head = emit_chunked_stream_head(response);
    EXPECT_TRUE(!(head.find("Content-Length:") != std::string_view::npos));
    EXPECT_TRUE((head.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
    EXPECT_TRUE(!(head.find("ChUnKeD") != std::string_view::npos));
    EXPECT_EQ(count_occurrences(head, "Transfer-Encoding: "), std::size_t{1});
}

TEST(Ruvia, response_head_canonicalizes_managed_framing_fields_across_case) {
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
    const auto ascii_lower = [](std::string_view text) {
        std::string out(text);
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    };

    for (std::size_t sample = 0; sample < 512; ++sample) {
        http_response buffered;
        buffered.body("payload");
        buffered.header(random_case("Content-Length"), "999");
        buffered.header(random_case("Transfer-Encoding"), random_case("chunked"));
        buffered.header("X-Sample", std::to_string(sample));
        const auto buffered_head = emit_buffered_head(buffered);
        EXPECT_EQ(count_occurrences(buffered_head, "Content-Length: "), std::size_t{1});
        EXPECT_TRUE((buffered_head.find("Content-Length: 7\r\n") != std::string_view::npos));
        EXPECT_TRUE(!(buffered_head.find("Content-Length: 999\r\n") != std::string_view::npos));
        EXPECT_TRUE(!(buffered_head.find("Transfer-Encoding:") != std::string_view::npos));
        EXPECT_EQ(count_occurrences(ascii_lower(buffered_head), "chunked"), std::size_t{0});
        EXPECT_TRUE(buffered_head.ends_with("\r\n\r\n"));

        http_response chunked;
        chunked.header(random_case("Content-Length"), "999");
        chunked.header(random_case("Transfer-Encoding"), random_case("chunked"));
        chunked.header("X-Sample", std::to_string(sample));
        const auto chunked_head = emit_chunked_stream_head(chunked);
        EXPECT_TRUE(!(chunked_head.find("Content-Length:") != std::string_view::npos));
        EXPECT_EQ(count_occurrences(chunked_head, "Transfer-Encoding: "), std::size_t{1});
        EXPECT_TRUE((chunked_head.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
        EXPECT_EQ(count_occurrences(ascii_lower(chunked_head), "transfer-encoding:"), std::size_t{1});
        EXPECT_EQ(count_occurrences(ascii_lower(chunked_head), "chunked"), std::size_t{1});
        EXPECT_TRUE(chunked_head.ends_with("\r\n\r\n"));
    }
}

TEST(Ruvia, http1_response_head_rejects_http10_chunked_payload_plan) {
    http_response response;
    response.body("hello");
    const auto plan = http1_chunked_response_stream_head_plan(
        plan_http_response_body(http_known_method::get, response.status()),
        connection_plan_for(ruvia::http_protocol_version::http10));

    EXPECT_TRUE(throws_invalid([&] { (void)emit_head(response, plan); }));
}

// RFC 9112 section 6.1: the writer alone frames a response. A handler-set
// Transfer-Encoding other than exactly `chunked` names a coding the writer
// never applies, so every final head rejects it before any output or mutation.
TEST(Ruvia, http1_response_head_rejects_transfer_encoding_other_than_chunked) {
    const auto rejected_without_output = [](const http_response& response, const http1_response_head_plan& plan) {
        ruvia::http_response_head_buffer buffer;
        buffer.append("existing output");
        const bool rejected = throws_head_error([&] { ruvia::append_http1_response_head(response, buffer, plan); });
        return rejected && buffer.view() == std::string_view("existing output");
    };
    for (const std::string_view coding : {"gzip", "gzip, chunked", "identity", "", "chunked, chunked", "chunked;q=1"}) {
        for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content,
                 ruvia::http_status::reset_content, ruvia::http_status::not_modified}) {
            for (const auto method : {http_known_method::get, http_known_method::head}) {
                http_response response;
                response.status(status);
                response.header("Transfer-Encoding", coding);
                const auto body_plan = plan_http_response_body(method, response.status());
                const auto buffered = get_http1_buffered_response_plan(
                    ruvia::plan_buffered_http_response_write(method, response),
                    connection_plan_for(ruvia::http_protocol_version::http11));
                EXPECT_TRUE(rejected_without_output(response, buffered.head_plan()));
                EXPECT_TRUE(rejected_without_output(response,
                    http1_chunked_response_stream_head_plan(
                        body_plan, connection_plan_for(ruvia::http_protocol_version::http11))));
                EXPECT_TRUE(rejected_without_output(response,
                    http1_close_delimited_response_stream_head_plan(
                        body_plan, connection_plan_for(ruvia::http_protocol_version::http10))));
            }
        }

        http_response committed;
        committed.header("Transfer-Encoding", coding);
        EXPECT_TRUE(throws_head_error([&] {
            (void)ruvia::http1_commit_final_response(committed, ruvia::plan_http11_request_connection(true));
        }));
        EXPECT_TRUE(!committed.header("Connection").has_value());

        for (const auto framing : {ruvia::http_response_stream_framing::http1_chunked,
                 ruvia::http_response_stream_framing::http1_close_delimited,
                 ruvia::http_response_stream_framing::http2_frames}) {
            http_response streamed;
            streamed.header("Transfer-Encoding", coding);
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, http_known_method::get, streamed.status(), ruvia::http_response_trailer_intent::none);
            bool head_error = false;
            bool invalid = false;
            try {
                (void)ruvia::prepare_http_response_stream_head(std::move(streamed), plan);
            } catch (const ruvia::http1_response_head_error&) {
                head_error = true;
            } catch (const std::invalid_argument&) {
                invalid = true;
            }
            if (framing == ruvia::http_response_stream_framing::http2_frames) {
                EXPECT_TRUE(invalid);
            } else {
                EXPECT_TRUE(head_error);
            }
        }
    }
}

// The chunked hint never reaches a wire that cannot carry it: only the HTTP/1
// chunk writer keeps it, as its own canonical field.
TEST(Ruvia, response_stream_head_replaces_chunked_transfer_encoding_hint_with_writer_framing) {
    for (const auto framing : {ruvia::http_response_stream_framing::http1_chunked,
             ruvia::http_response_stream_framing::http1_close_delimited,
             ruvia::http_response_stream_framing::http2_frames}) {
        http_response response;
        response.header("Transfer-Encoding", "CHUNKED");
        const auto plan = ruvia::plan_http_response_stream_commit(
            framing, http_known_method::get, response.status(), ruvia::http_response_trailer_intent::none);
        const auto prepared = ruvia::prepare_http_response_stream_head(std::move(response), plan);
        const auto transfer_encoding = prepared.response().header("Transfer-Encoding");
        if (framing == ruvia::http_response_stream_framing::http1_chunked) {
            EXPECT_EQ(transfer_encoding.value_or(""), std::string_view("chunked"));
        } else {
            EXPECT_TRUE(!transfer_encoding.has_value());
        }
    }
}

TEST(Ruvia, http1_chunked_stream_does_not_invent_framing_for_head) {
    http_response metadata;
    metadata.header("Content-Length", "5");
    metadata.header("Transfer-Encoding", "chunked");
    const auto metadata_wire = emit_chunked_stream_head(metadata, http_known_method::head);
    EXPECT_TRUE((metadata_wire.find("Content-Length: 5\r\n") != std::string_view::npos));
    EXPECT_TRUE(!(metadata_wire.find("Transfer-Encoding:") != std::string_view::npos));

    http_response unknown;
    const auto unknown_wire = emit_chunked_stream_head(unknown, http_known_method::head);
    EXPECT_TRUE(!(unknown_wire.find("Content-Length:") != std::string_view::npos));
    EXPECT_TRUE(!(unknown_wire.find("Transfer-Encoding:") != std::string_view::npos));
}

TEST(Ruvia, http1_buffered_head_keeps_declared_length_without_body) {
    // RFC 9110 section 8.6: HEAD Content-Length is what GET would send.
    http_response metadata;
    metadata.header("Content-Length", "1048576");
    const auto metadata_wire = emit_buffered_head(metadata, http_known_method::head);
    EXPECT_EQ(count_occurrences(metadata_wire, "Content-Length: "), std::size_t{1});
    EXPECT_TRUE((metadata_wire.find("Content-Length: 1048576\r\n") != std::string_view::npos));

    // A materialized representation remains authoritative.
    http_response materialized;
    materialized.body("hello");
    materialized.header("Content-Length", "999");
    const auto materialized_wire = emit_buffered_head(materialized, http_known_method::head);
    EXPECT_EQ(count_occurrences(materialized_wire, "Content-Length: "), std::size_t{1});
    EXPECT_TRUE((materialized_wire.find("Content-Length: 5\r\n") != std::string_view::npos));

    // A zero-byte buffered response without apply_content_encoding emits Content-Length: 0 on HEAD.
    http_response zero_representation;
    zero_representation.body("");
    const auto zero_wire = emit_buffered_head(zero_representation, http_known_method::head);
    EXPECT_EQ(count_occurrences(zero_wire, "Content-Length: "), std::size_t{1});
    EXPECT_TRUE((zero_wire.find("Content-Length: 0\r\n") != std::string_view::npos));

    // Applying a content coding without an encoded body removes Content-Length on HEAD.
    http_response coded_head;
    coded_head.header("Content-Length", "1024");
    coded_head.apply_content_encoding("gzip");
    const auto coded_wire = emit_buffered_head(coded_head, http_known_method::head);
    EXPECT_EQ(count_occurrences(coded_wire, "Content-Length: "), std::size_t{0});
    EXPECT_TRUE((coded_wire.find("Content-Encoding: gzip\r\n") != std::string_view::npos));
}

TEST(Ruvia, response_head_close_delimited_stream_rejects_declared_framing) {
    http_response response;
    response.body("streamed");
    response.header("Transfer-Encoding", "chunked");
    response.header("Content-Length", "8");

    const auto head = emit_close_delimited_stream_head(response);
    EXPECT_TRUE(!(head.find("Transfer-Encoding:") != std::string_view::npos));
    EXPECT_TRUE(!(head.find("Content-Length:") != std::string_view::npos));

    // A HEAD response has no payload and may retain representation length
    // metadata, but HTTP/1.0 still cannot carry Transfer-Encoding.
    const auto metadata_head = emit_close_delimited_stream_head(response, http_known_method::head);
    EXPECT_TRUE(!(metadata_head.find("Transfer-Encoding:") != std::string_view::npos));
    EXPECT_TRUE((metadata_head.find("Content-Length: 8\r\n") != std::string_view::npos));

    http_response not_modified;
    not_modified.status(ruvia::http_status::not_modified);
    not_modified.header("Transfer-Encoding", "chunked");
    not_modified.header("Content-Length", "123");
    const auto not_modified_head = emit_close_delimited_stream_head(not_modified);
    EXPECT_TRUE(!(not_modified_head.find("Transfer-Encoding:") != std::string_view::npos));
    EXPECT_TRUE((not_modified_head.find("Content-Length: 123\r\n") != std::string_view::npos));
}

TEST(Ruvia, response_head_validates_explicit_content_length_metadata) {
    http_response malformed;
    malformed.status(ruvia::http_status::not_modified);
    malformed.header("Content-Length", "invalid");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_buffered_head(malformed); }));

    http_response conflicting;
    conflicting.status(ruvia::http_status::not_modified);
    conflicting.header("Content-Length", "7, 8");
    EXPECT_TRUE(throws_invalid([&] { (void)emit_buffered_head(conflicting); }));

    http_response equivalent;
    equivalent.status(ruvia::http_status::not_modified);
    equivalent.header("Content-Length", "0007, 7");
    const auto canonical = emit_buffered_head(equivalent);
    EXPECT_EQ(count_occurrences(canonical, "Content-Length: "), std::size_t{1});
    EXPECT_TRUE((canonical.find("Content-Length: 7\r\n") != std::string_view::npos));

    http_response head_metadata;
    head_metadata.header("Content-Length", "bad");
    EXPECT_TRUE(throws_invalid(
        [&] { (void)emit_close_delimited_stream_head(head_metadata, http_known_method::head); }));
}

TEST(Ruvia, response_head_bodyless_status_omits_auto_content_length) {
    http_response response;
    response.status(ruvia::http_status::no_content);
    const auto head = emit_buffered_head(response);
    EXPECT_TRUE(head.starts_with("HTTP/1.1 204 No Content\r\n"));
    EXPECT_TRUE(!(head.find("Content-Length:") != std::string_view::npos));
    EXPECT_TRUE(head.ends_with("\r\n\r\n"));
}

TEST(Ruvia, response_head_reset_content_canonicalizes_zero_length) {
    // RFC 9110 §15.3.6 forbids 205 content. Even if the application supplies a
    // body and contradictory framing, both buffered and streaming head emission
    // must suppress it and retain an unambiguous persistent HTTP/1 message.
    for (const bool streaming : {false, true}) {
        http_response response;
        response.status(ruvia::http_status::reset_content);
        response.body("must-not-be-sent");
        response.header("Content-Length", "16");
        response.header("Transfer-Encoding", "chunked");
        const auto head = streaming ? emit_chunked_stream_head(response) : emit_buffered_head(response);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 205 Reset Content\r\n"));
        EXPECT_EQ(count_occurrences(head, "Content-Length: "), std::size_t{1});
        EXPECT_TRUE((head.find("Content-Length: 0\r\n") != std::string_view::npos));
        EXPECT_TRUE(!(head.find("Content-Length: 16\r\n") != std::string_view::npos));
        EXPECT_TRUE(!(head.find("Transfer-Encoding:") != std::string_view::npos));
        EXPECT_TRUE(head.ends_with("\r\n\r\n"));
    }
}

TEST(Ruvia, response_head_heap_spill_preserves_full_output) {
    // Force the emitted head well past the 512-byte stack buffer so the heap
    // (reserve_additional) emit path runs. Every header must survive intact and
    // the precomputed size bound must not undercount -- an undercount would let
    // the unchecked raw stack sink overflow or the output truncate.
    http_response response;
    response.status(ruvia::http_status::ok);
    const std::string big(200, 'v');
    for (int i = 0; i < 10; ++i) {
        response.header(std::string("X-Pad-") + std::to_string(i).c_str(), big);
    }
    response.body("body");
    const auto head = emit_buffered_head(response);

    EXPECT_TRUE(head.starts_with("HTTP/1.1 200 OK\r\n"));
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(head.find(std::string("X-Pad-") + std::to_string(i).c_str() + ": " + big + "\r\n") !=
                    std::string_view::npos);
    }
    EXPECT_TRUE((head.find("Content-Length: 4\r\n") != std::string_view::npos));
    EXPECT_TRUE(head.ends_with("\r\n\r\n"));
}

TEST(Ruvia, response_head_rejects_oversized_field_section) {
    http_response oversized;
    oversized.header("X-Oversized", std::string(ruvia::max_http_header_bytes, 'v'));
    EXPECT_TRUE(throws_head_error([&] { (void)emit_buffered_head(oversized); }));

    http_response many;
    for (std::size_t i = 0; i < 80; ++i) {
        many.header(std::string("X-Field-") + std::to_string(i).c_str(), "value");
    }
    EXPECT_TRUE(!emit_buffered_head(many).empty());
}

TEST(Ruvia, stable_view_headers_reject_malformed_name_and_value) {
    http_response response;
    EXPECT_TRUE(throws_invalid([&] { response.header_stable_view("Bad Name", "value"); }));
    EXPECT_TRUE(throws_invalid([&] { response.header_stable_view("X-Test", std::string_view("bad\r\nvalue", 10)); }));
    EXPECT_TRUE(response.headers().empty());
}

TEST(Ruvia, stable_view_headers_reject_request_only_te_field) {
    http_response response;
    EXPECT_TRUE(throws_invalid([&] { response.header_stable_view("TE", "trailers"); }));
    EXPECT_TRUE(!response.header("TE").has_value());
}

TEST(Ruvia, http1_commit_final_response_preserves_custom_connection_options_when_closing) {
    http_response response;
    response.status(ruvia::http_status::ok);
    response.header("X-Custom-Hop", "secret-hop");
    response.header("Connection", "keep-alive, X-Custom-Hop");
    const auto committed =
        ruvia::http1_commit_final_response(response, ruvia::plan_http11_request_connection(true));
    EXPECT_EQ(committed.disposition(), ruvia::http1_close_policy::close_after_response);
    EXPECT_EQ(response.header("Connection").value_or(""), std::string_view("close, X-Custom-Hop"));

    http_response upgrade_response;
    upgrade_response.status(ruvia::http_status::upgrade_required);
    upgrade_response.header("Upgrade", "example/1");
    upgrade_response.header("X-Hop-2", "v");
    upgrade_response.header("Connection", "X-Hop-2");
    const auto upgrade_committed = ruvia::http1_commit_final_response(
        upgrade_response, ruvia::plan_http11_request_connection(true));
    EXPECT_EQ(upgrade_committed.disposition(), ruvia::http1_close_policy::close_after_response);
    EXPECT_EQ(upgrade_response.header("Connection").value_or(""), std::string_view("close, Upgrade, X-Hop-2"));
}

TEST(Ruvia, response_write_plan_unifies_method_status_and_body_size) {
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.body("hello");

    const auto get_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(get_plan.body_plan().request_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(get_plan.matches_response(response));
    EXPECT_EQ(get_plan.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(get_plan.body_plan().status_allows_body());
    EXPECT_TRUE(get_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(!get_plan.body_plan().body_suppressed());
    EXPECT_TRUE(get_plan.send_body());
    EXPECT_EQ(get_plan.content_length(), static_cast<std::uint64_t>(5));

    const auto head_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    EXPECT_EQ(head_plan.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(head_plan.body_plan().status_allows_body());
    EXPECT_TRUE(head_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    EXPECT_TRUE(head_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!head_plan.send_body());
    EXPECT_EQ(head_plan.content_length(), static_cast<std::uint64_t>(5));

    response.status(ruvia::http_status::no_content);
    const auto no_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_EQ(no_content_plan.body_plan().response_status(), ruvia::http_status::no_content);
    EXPECT_TRUE(!no_content_plan.body_plan().status_allows_body());
    EXPECT_TRUE(no_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    EXPECT_TRUE(no_content_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!no_content_plan.send_body());
    EXPECT_EQ(no_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::reset_content);
    const auto reset_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(!reset_content_plan.body_plan().status_allows_body());
    EXPECT_TRUE(reset_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(reset_content_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!reset_content_plan.send_body());
    EXPECT_EQ(reset_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::ok);
    const auto connect_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, response);
    EXPECT_TRUE(connect_plan.body_plan().status_allows_body());
    EXPECT_TRUE(connect_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    EXPECT_TRUE(connect_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!connect_plan.send_body());
    EXPECT_EQ(connect_plan.content_length(), static_cast<std::uint64_t>(0));
}

TEST(Ruvia, response_body_plan_classifies_protocol_response_states) {
    const auto informational = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status_code::from_value(199));
    EXPECT_TRUE(informational.content_semantics() ==
                ruvia::http_response_content_semantics::informational);
    EXPECT_TRUE(informational.body_suppressed());
    EXPECT_TRUE(!informational.status_allows_body());

    const auto protocol_switch = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::switching_protocols);
    EXPECT_TRUE(protocol_switch.content_semantics() ==
                ruvia::http_response_content_semantics::protocol_switch);
    EXPECT_TRUE(protocol_switch.body_suppressed());

    const auto connect_tunnel = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::ok);
    EXPECT_TRUE(connect_tunnel.content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    EXPECT_TRUE(connect_tunnel.body_suppressed());
    EXPECT_TRUE(connect_tunnel.status_allows_body());

    const auto failed_connect = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::bad_request);
    EXPECT_TRUE(failed_connect.content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(!failed_connect.body_suppressed());
}

TEST(Ruvia, response_stream_plan_restricts_trailers_by_status_and_method) {
    for (const auto status : {ruvia::http_status_code::from_value(199),
             ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        const auto plan = ruvia::plan_http_response_stream_commit(
            ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
            status, ruvia::http_response_trailer_intent::present);
        EXPECT_TRUE(!plan.trailer_intent_allowed());
    }

    const auto head_plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::head,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::present);
    EXPECT_TRUE(head_plan.trailer_intent_allowed());
    EXPECT_TRUE(!head_plan.trailers_available());
    EXPECT_TRUE(head_plan.head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
}

TEST(Ruvia, response_write_plan_rejects_mutated_response_snapshot) {
    ruvia::http_response response;
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(plan.matches_response(response));

    response.body("longer");
    EXPECT_TRUE(!plan.matches_response(response));
    response.body("old");
    response.status(ruvia::http_status::already_reported);
    EXPECT_TRUE(!plan.matches_response(response));
}

TEST(Ruvia, http1_response_head_framing_is_an_exclusive_plan) {
    ruvia::http_response response;
    response.body("hello");
    const auto body_plan =
        ruvia::plan_http_response_body(ruvia::http_known_method::get, ruvia::http_status::ok);
    const auto connection_plan = ruvia::plan_http11_request_connection(false);
    const auto write_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto combined = ruvia::get_http1_buffered_response_plan(write_plan, connection_plan);
    const auto& buffered = combined.head_plan();
    const auto chunked =
        ruvia::http1_chunked_response_stream_head_plan(body_plan, connection_plan);
    const auto close_delimited =
        ruvia::http1_close_delimited_response_stream_head_plan(body_plan, connection_plan);

    EXPECT_TRUE(buffered.buffered() != nullptr);
    EXPECT_TRUE(buffered.chunked_stream() == nullptr);
    EXPECT_TRUE(buffered.close_delimited_stream() == nullptr);
    EXPECT_TRUE(chunked.buffered() == nullptr);
    EXPECT_TRUE(chunked.chunked_stream() != nullptr);
    EXPECT_TRUE(chunked.close_delimited_stream() == nullptr);
    EXPECT_TRUE(close_delimited.buffered() == nullptr);
    EXPECT_TRUE(close_delimited.chunked_stream() == nullptr);
    EXPECT_TRUE(close_delimited.close_delimited_stream() != nullptr);
    EXPECT_TRUE(close_delimited.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_EQ(buffered.buffered()->content_length(), std::uint64_t{5});
    EXPECT_EQ(combined.content_length(), std::uint64_t{5});
    EXPECT_EQ(combined.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(combined.send_body());
    EXPECT_TRUE(combined.body_plan().request_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(buffered.protocol_version() == ruvia::http_protocol_version::http11);

    EXPECT_EQ(combined.content_length(), combined.head_plan().buffered()->content_length());
}

namespace {

}  // namespace

TEST(Ruvia, response_commit_headers_leaves_staged_response_independent) {
    ruvia::http_response response;
    auto staged = response.headers().clone();
    staged.header("Content-Type", "text/plain");
    response.commit_headers(std::move(staged));
    EXPECT_EQ(response.header("Content-Type").value_or(""), "text/plain");

    // The moved-from staging block must not resolve headers into committed storage.
    EXPECT_TRUE(!staged.header("Content-Type").has_value());
    staged.header("Content-Type", "application/json");
    EXPECT_EQ(staged.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(response.header("Content-Type").value_or(""), "text/plain");
    EXPECT_EQ(response.headers().size(), std::size_t{1});
}

TEST(Ruvia, response_vary_token_ignores_surrounding_whitespace) {
    ruvia::http_response fresh;
    fresh.add_vary_token(" Origin ");
    fresh.add_vary_token("Origin");
    fresh.add_vary_token("  ");
    EXPECT_EQ(fresh.headers().size(), std::size_t{1});
    EXPECT_EQ(fresh.header("Vary").value_or(""), "Origin");

    ruvia::http_response existing;
    existing.header("Vary", "Origin");
    existing.add_vary_token(" Origin");
    existing.add_vary_token("\t");
    existing.add_vary_token(" Accept\t");
    EXPECT_EQ(existing.headers().size(), std::size_t{1});
    EXPECT_EQ(existing.header("Vary").value_or(""), "Origin, Accept");
}

TEST(Ruvia, response_header_transfer_appends_disjoint_fields_with_their_rules) {
    const ruvia::http_response_header_options append{.mode_ = ruvia::http_response_header_mode::append};
    for (const auto mode : {ruvia::http_response_header_transfer::merge, ruvia::http_response_header_transfer::assign,
             ruvia::http_response_header_transfer::apply}) {
        ruvia::http_response source_value;
        source_value.header("X-Source", "one");
        source_value.header("Vary", "Origin", append);
        source_value.header("Vary", "Accept", append);
        source_value.header("Set-Cookie", "a=1");
        source_value.header("Set-Cookie", "b=2", append);
        source_value.header("Content-Type", "text/csv");

        ruvia::http_response destination;
        destination.header("X-Existing", "kept");
        destination.transfer_headers_from(source_value, mode);

        // Assignment keeps the destination's (absent) Content-Type.
        const bool assign = mode == ruvia::http_response_header_transfer::assign;
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        EXPECT_EQ(destination.header("X-Existing").value_or(""), "kept");
        EXPECT_EQ(destination.header("x-source").value_or(""), "one");
        EXPECT_EQ(destination.header("Vary").value_or(""), "Origin");
        EXPECT_EQ(destination.headers().begin()[3].value(), "Accept");
        EXPECT_EQ(destination.header("Set-Cookie").value_or(""), "a=1");
        EXPECT_EQ(destination.headers().begin()[5].value(), "b=2");
        EXPECT_EQ(destination.header("Content-Type").value_or(""), assign ? "" : "text/csv");

        // Transferred fields keep their rules: cookies still upsert by name, and
        // a replacement collapses the appended field.
        destination.header("Set-Cookie", "a=3", append);
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        EXPECT_EQ(destination.header("Set-Cookie").value_or(""), "a=3");
        destination.header("Vary", "Cookie");
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 5U : 6U});
        EXPECT_EQ(destination.header("Vary").value_or(""), "Cookie");
        EXPECT_EQ(source_value.headers().size(), std::size_t{6});
    }

    // A field the destination already holds resolves by mode.
    ruvia::http_response source_value;
    source_value.header("Content-Type", "text/csv");
    source_value.header("X-Added", "yes");
    ruvia::http_response merged;
    merged.content_type(ruvia::http_content_type::application_json);
    merged.transfer_headers_from(source_value, ruvia::http_response_header_transfer::merge);
    EXPECT_EQ(merged.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(merged.header("X-Added").value_or(""), "yes");
    ruvia::http_response applied;
    applied.content_type(ruvia::http_content_type::application_json);
    applied.transfer_headers_from(source_value, ruvia::http_response_header_transfer::apply);
    EXPECT_EQ(applied.header("Content-Type").value_or(""), "text/csv");
    EXPECT_EQ(applied.headers().size(), std::size_t{2});
}

TEST(Ruvia, response_header_transfer_rejects_unknown_modes_without_mutation) {
    for (const std::string_view source_name : {"X-Added", "X-Existing", ""}) {
        SCOPED_TRACE(source_name);
        ruvia::http_response source_value;
        if (!source_name.empty()) {
            source_value.header(source_name, "new");
        }
        ruvia::http_response destination;
        destination.header("X-Existing", "old");

        EXPECT_THROW(
            destination.transfer_headers_from(source_value, static_cast<ruvia::http_response_header_transfer>(255)),
            std::invalid_argument);
        EXPECT_EQ(destination.headers().size(), std::size_t{1});
        EXPECT_EQ(destination.header("X-Existing").value_or(""), "old");
        EXPECT_FALSE(destination.header("X-Added").has_value());
    }
}

TEST(Ruvia, response_header_block_iterates_without_const_qualification) {
    ruvia::http_response response;
    response.header("X-First", "1");
    response.header("X-Second", "2");
    auto staged = response.headers().clone();
    std::size_t visited = 0;
    for (const auto& field : staged) {
        EXPECT_TRUE(!field.name().empty());
        ++visited;
    }
    EXPECT_EQ(visited, std::size_t{2});
    EXPECT_TRUE(std::distance(staged.begin(), staged.end()) == 2);
}

TEST(Ruvia, response_header_stable_view_and_allow_methods_handle_self_aliasing) {
    ruvia::http_response response;
    response.header("X-Owned", "X-CopyValue");
    const auto existing = response.header("X-Owned").value_or("");
    EXPECT_EQ(existing, "X-CopyValue");

    // A contiguous slice inside an existing owned header must be copied before
    // any table growth or replacement could release the source descriptor.
    response.header_stable_view(existing.substr(0, 6), existing.substr(6));
    response.header("X-Owned", "replaced-with-longer-owned-header-value");
    EXPECT_EQ(response.header("X-Copy").value_or(""), "Value");

    response.header("Allow", "GET, WEBDAX");
    const auto current_allow = response.header("Allow").value_or("");
    EXPECT_EQ(current_allow, "GET, WEBDAX");
    const std::array<std::string_view, 1> borrowed_ext{current_allow.substr(5)};
    response.allow_methods(ruvia::http_known_method_bit(ruvia::http_known_method::get), borrowed_ext);
    EXPECT_EQ(response.header("Allow").value_or(""), "GET, WEBDAX");
}

namespace {

using ruvia::http_response;

http_response make_response() {
    return http_response();
}

}  // namespace

TEST(Ruvia, response_header_transfer_is_atomic_and_keeps_body) {
    auto destination = make_response();
    destination.header("Content-Type", "application/json");
    destination.body("payload");
    destination.header("X-Existing", "old");

    auto source_value = make_response();
    source_value.header("Content-Type", "text/plain");
    source_value.header("X-Existing", "new");
    source_value.header("X-Added", "yes");

    destination.transfer_headers_from(source_value, ruvia::http_response_header_transfer::assign);
    EXPECT_EQ(destination.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(destination.header("X-Existing").value_or(""), "new");
    EXPECT_EQ(destination.header("X-Added").value_or(""), "yes");
    EXPECT_EQ(destination.header("Content-Length").value_or(""), "");
}

TEST(Ruvia, response_public_write_plan_distinguishes_head_from_bodyless_status) {
    auto response = make_response();
    response.body("head-representation");

    const auto head_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    EXPECT_TRUE(head_plan.body_plan().body_suppressed());
    EXPECT_EQ(head_plan.content_length(), std::uint64_t{19});
    EXPECT_TRUE(!head_plan.send_body());
    EXPECT_TRUE(head_plan.matches_response(response));
    EXPECT_EQ(response.body_bytes(), "head-representation");
    EXPECT_EQ(response.body(), "head-representation");
    EXPECT_TRUE(!response.file_body().has_value());

    const auto no_content_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::no_content);
    EXPECT_TRUE(!no_content_plan.status_allows_body());
    EXPECT_TRUE(no_content_plan.body_suppressed());
    const auto head_status_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::head, ruvia::http_status::ok);
    EXPECT_TRUE(head_status_plan.status_allows_body());
    EXPECT_TRUE(head_status_plan.body_suppressed());
}

TEST(Ruvia, response_public_server_plans_preserve_head_representation_length) {
    auto response = make_response();
    response.body("head-body");

    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    EXPECT_TRUE(write_plan.body_plan().body_suppressed());
    EXPECT_EQ(write_plan.content_length(), std::uint64_t{9});
    EXPECT_TRUE(!write_plan.send_body());
    EXPECT_TRUE(write_plan.matches_response(response));

    constexpr std::array framings{
        ruvia::http_response_stream_framing::http1_chunked,
        ruvia::http_response_stream_framing::http2_frames};
    for (const auto framing : framings) {
        const auto stream_plan = ruvia::plan_http_response_stream_commit(
            framing, ruvia::http_known_method::head,
            response.status(), ruvia::http_response_trailer_intent::none);
        EXPECT_TRUE(stream_plan.body_plan().body_suppressed());
        EXPECT_EQ(stream_plan.head_disposition(), ruvia::http_response_stream_head_disposition::message_ended);
    }
}

TEST(Ruvia, response_public_trailer_validation_returns_borrowed_section) {
    const std::array trailers{ruvia::http_header_view{"ETag", "\"v1\""},
        ruvia::http_header_view{"X-Trace", "trace-1"}};
    const auto section = ruvia::validate_http_response_trailers(trailers);
    EXPECT_TRUE(!section.empty());
    EXPECT_EQ(section.fields().size(), std::size_t{2});
    EXPECT_EQ(ruvia::response_trailer_intent(section), ruvia::http_response_trailer_intent::present);

    const std::array invalid_trailers{ruvia::http_header_view{"Content-Type", "text/plain"}};
    bool rejected = false;
    try {
        (void)ruvia::validate_http_response_trailers(invalid_trailers);
    } catch (const std::exception&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, response_public_cookie_and_metadata_capabilities) {
    auto response = make_response();
    const ruvia::cookie_options options{.path_ = "/"};
    const ruvia::set_cookie_plan plan("sid", "first", options);
    response.set_cookie(plan);
    const ruvia::set_cookie_plan replacement("sid", "second", options);
    response.set_cookie(replacement);
    EXPECT_EQ(response.headers().size(), std::size_t{1});
    EXPECT_TRUE(response.header("Set-Cookie").value_or("").find("second") != std::string_view::npos);

    response.content_range(5, 3, 10);
    response.add_vary_token("Accept-Encoding");
    EXPECT_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    EXPECT_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { response.content_range(invalid, 3, 10); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { response.content_range(5, invalid, 10); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { response.content_range(5, 3, invalid); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { response.content_range_unsatisfied(invalid); }));
    }
    EXPECT_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    response.add_vary_token(" Accept-Encoding ");
    EXPECT_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
    bool invalid_vary_rejected = false;
    try {
        response.add_vary_token("bad token");
    } catch (const std::invalid_argument&) {
        invalid_vary_rejected = true;
    }
    EXPECT_TRUE(invalid_vary_rejected);

    auto staged = response.headers().clone();
    staged.reserve(12);
    staged.allow_methods(ruvia::http_known_method_bit(ruvia::http_known_method::get));
    staged.content_range(0, 4, 10);
    staged.add_vary_token("Origin");
    EXPECT_EQ(staged.header("Allow").value_or(""), "GET");
    EXPECT_EQ(staged.header("Content-Range").value_or(""), "bytes 0-3/10");
    staged.content_range_unsatisfied(10);
    EXPECT_EQ(staged.header("Content-Range").value_or(""), "bytes */10");
    auto merged = response.headers().clone();
    merged.transfer_from(staged, ruvia::http_response_header_transfer::apply);
    EXPECT_EQ(merged.header("Allow").value_or(""), "GET");
    EXPECT_EQ(merged.header("Content-Range").value_or(""), "bytes */10");
}

namespace {

std::string materialize_multipart(const ruvia::http_multipart_byte_range_plan& plan, std::string_view payload_value) {
    std::string body;
    for (const auto& segment : plan.segments()) {
        if (segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            const auto metadata = plan.metadata();
            if (segment.metadata_offset_ > metadata.size() ||
                segment.metadata_length_ > metadata.size() - segment.metadata_offset_) {
                throw std::logic_error("multipart metadata segment is out of bounds");
            }
            body.append(metadata.substr(segment.metadata_offset_, segment.metadata_length_));
        } else {
            if (segment.file_offset_ > payload_value.size() ||
                segment.file_length_ > payload_value.size() - segment.file_offset_) {
                throw std::logic_error("multipart file segment is out of bounds");
            }
            body.append(payload_value.substr(static_cast<std::size_t>(segment.file_offset_),
                static_cast<std::size_t>(segment.file_length_)));
        }
    }
    return body;
}

}  // namespace

TEST(Ruvia, multipart_range_plan_serializes_owned_inputs_and_every_segment_exactly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    std::string media_type = "text/plain; charset=utf-8";
    std::string boundary = "range boundary";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary);
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'x');
    const std::string_view expected =
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 8-9/10\r\n\r\n89\r\n"
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 0-1/10\r\n\r\n01\r\n"
        "--range boundary--\r\n";
    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    EXPECT_EQ(materialize_multipart(plan, "0123456789"), expected);
    EXPECT_EQ(plan.content_length(), expected.size());
}

TEST(Ruvia, multipart_range_plan_preserves_all_ranges_across_metadata_growth) {
    std::string field = "bytes=";
    std::string expected;
    constexpr std::string_view payload_value = "0123456789abcdefghijklmnopqrstuv";
    for (std::size_t index = 0; index < ruvia::http_byte_range_set::capacity; ++index) {
        char offset_buffer[20];
        const auto offset_end = std::to_chars(offset_buffer, offset_buffer + sizeof(offset_buffer), index * 2).ptr;
        const std::string_view offset(offset_buffer, offset_end);
        if (index != 0) {
            field.push_back(',');
        }
        field.append(offset).append("-").append(offset);
        expected.append("--all_ranges\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes ");
        expected.append(offset).append("-").append(offset).append("/32\r\n\r\n");
        expected.push_back(payload_value[index * 2]);
        expected.append("\r\n");
    }
    expected.append("--all_ranges--\r\n");
    const auto ranges = ruvia::resolve_http_byte_range_set(field, payload_value.size());
    EXPECT_EQ(ranges.size(), ruvia::http_byte_range_set::capacity);
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload_value.size(), "application/octet-stream", "all_ranges");
    EXPECT_EQ(materialize_multipart(plan, payload_value), expected);
    EXPECT_EQ(plan.content_length(), expected.size());
}

TEST(Ruvia, multipart_range_plan_moved_from_and_empty_file_path_are_rejected_cleanly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "b");
    auto moved = std::move(plan);
    EXPECT_TRUE(plan.content_type().empty());
    EXPECT_TRUE(plan.metadata().empty());
    EXPECT_TRUE(plan.segments().empty());
    EXPECT_EQ(plan.content_length(), std::uint64_t{0});

    ruvia::http_response response;
    bool empty_path_rejected = false;
    try {
        response.multipart_file_body(
            {}, 10, ruvia::http_response_file_identity::unchecked(), std::move(moved));
    } catch (const std::invalid_argument&) {
        empty_path_rejected = true;
    }
    EXPECT_TRUE(empty_path_rejected);

    bool moved_plan_rejected = false;
    try {
        const ruvia::http_native_path_string_type non_empty(1, static_cast<ruvia::http_native_path_char_type>('a'));
        response.multipart_file_body(
            non_empty, 10, ruvia::http_response_file_identity::unchecked(), std::move(plan));
    } catch (const std::invalid_argument&) {
        moved_plan_rejected = true;
    }
    EXPECT_TRUE(moved_plan_rejected);
}

namespace {

using ruvia::http_known_method;
using ruvia::http_response;

constexpr std::uint32_t method_bit(http_known_method method) {
    return ruvia::http_known_method_bit(method);
}

}  // namespace

// Response fields Ruvia formats itself: Content-Range and Allow.

TEST(Ruvia, allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = make_response();
    many.allow_methods(method_bit(http_known_method::get) |
                       method_bit(http_known_method::post) |
                       method_bit(http_known_method::head));
    EXPECT_EQ(many.header("Allow").value_or(""), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = make_response();
    one.allow_methods(method_bit(http_known_method::delete_value));
    EXPECT_EQ(one.header("Allow").value_or(""), std::string_view("DELETE"));

    std::string extension = "PROPFIND";
    const std::string_view extension_methods[]{extension};
    one.allow_methods(method_bit(http_known_method::get), extension_methods);
    extension.assign("CHANGED!");
    EXPECT_EQ(one.header("Allow").value_or(""), std::string_view("GET, PROPFIND"));
}

TEST(Ruvia, allow_header_rejects_invalid_extension_methods_without_mutation) {
    const std::array invalid_methods{std::string_view{}, std::string_view("BAD METHOD"),
        std::string_view("GET, POST"), std::string_view("BAD\r\nX-Injected: yes"),
        std::string_view("BAD\0METHOD", 10), std::string_view("BAD\xff", 4)};
    for (const auto method : invalid_methods) {
        auto response = make_response();
        response.header("Allow", "GET");
        response.header("X-Keep", "unchanged");
        const std::array extension_methods{std::string_view("PROPFIND"), method};
        bool rejected = false;
        try {
            response.allow_methods(method_bit(http_known_method::post), extension_methods);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(response.header("Allow").value_or(""), std::string_view("GET"));
        EXPECT_EQ(response.header("X-Keep").value_or(""), std::string_view("unchanged"));
    }
}

TEST(Ruvia, allow_header_reorders_methods_borrowed_from_current_field) {
    auto response = make_response();
    response.header("Allow", "FOO, BAR");
    const auto current = response.header("Allow").value();
    const std::array extension_methods{current.substr(5), current.substr(0, 3)};
    response.allow_methods(0, extension_methods);
    EXPECT_EQ(response.header("Allow").value_or(""), std::string_view("BAR, FOO"));
}

TEST(Ruvia, allow_header_lists_connect_with_other_supported_methods) {
    auto response = make_response();
    response.allow_methods(method_bit(http_known_method::connect));
    EXPECT_EQ(response.header("Allow").value_or(""), std::string_view("CONNECT"));
    const std::array extension_methods{std::string_view("PROPFIND")};
    response.allow_methods(method_bit(http_known_method::get) | method_bit(http_known_method::options) |
                               method_bit(http_known_method::connect),
        extension_methods);
    EXPECT_EQ(response.header("Allow").value_or(""), std::string_view("GET, OPTIONS, CONNECT, PROPFIND"));
}

TEST(Ruvia, allow_header_accepts_extension_tokens_and_empty_method_set) {
    auto response = make_response();
    const std::array extension_methods{std::string_view("custom"), std::string_view("!#$%&'*+-.^_`|~")};
    response.allow_methods(method_bit(http_known_method::get), extension_methods);
    EXPECT_EQ(response.header("Allow").value_or(""), std::string_view("GET, custom, !#$%&'*+-.^_`|~"));
    response.allow_methods(0);
    EXPECT_EQ(response.header("Allow"), std::optional(std::string_view{}));
}

TEST(Ruvia, response_cookie_plan_and_raw_headers_share_replacement_semantics) {
    ruvia::http_response response;
    response.header("Set-Cookie", "first=one");
    const ruvia::cookie_options options;
    response.set_cookie(ruvia::set_cookie_plan("second", "two", options));
    EXPECT_EQ(response.headers().size(), std::size_t{2});

    response.header("Set-Cookie", "final=three");
    EXPECT_EQ(response.headers().size(), std::size_t{1});
    EXPECT_EQ(response.header("Set-Cookie").value_or(""), "final=three");
}

TEST(Ruvia, response_cookie_plan_replaces_itself_at_parser_limits) {
    ruvia::http_response response;
    const std::string path(1024, '/');
    const std::string value(4096 - 9 - 3, 'v');
    ruvia::cookie_options options;
    options.secure_ = ruvia::cookie_attribute_policy::emit;
    options.prefix_ = ruvia::cookie_prefix::secure;
    options.path_ = path;
    response.set_cookie(ruvia::set_cookie_plan("sid", value, options));
    response.set_cookie(ruvia::set_cookie_plan("sid", value, options));
    EXPECT_EQ(response.headers().size(), std::size_t{1});
}

TEST(Ruvia, response_cookie_partitioned_and_unpartitioned_storage_keys_are_distinct) {
    ruvia::http_response response;
    const ruvia::cookie_options partitioned{
        .secure_ = ruvia::cookie_attribute_policy::emit,
        .partitioned_ = ruvia::cookie_attribute_policy::emit};
    const ruvia::cookie_options ordinary{.secure_ = ruvia::cookie_attribute_policy::emit};
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-first", partitioned));
    response.set_cookie(ruvia::set_cookie_plan("session", "ordinary-first", ordinary));
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-middle", partitioned));
    EXPECT_EQ(response.headers().size(), std::size_t{2});
    response.header("Set-Cookie", "session=ordinary-final; Path=/; Secure",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=partitioned-final; Path=/; Secure; Partitioned",
        {.mode_ = ruvia::http_response_header_mode::append});

    std::size_t partitioned_count = 0;
    std::size_t ordinary_count = 0;
    for (const auto& header : response.headers()) {
        const auto cookie = ruvia::parse_set_cookie(header.value());
        EXPECT_TRUE(cookie.has_value());
        if (!cookie) {
            continue;
        }
        EXPECT_EQ(cookie->name(), "session");
        if (cookie->has(ruvia::http_set_cookie_attribute::partitioned)) {
            ++partitioned_count;
            EXPECT_EQ(cookie->value(), "partitioned-final");
        } else {
            ++ordinary_count;
            EXPECT_EQ(cookie->value(), "ordinary-final");
        }
    }
    EXPECT_EQ(partitioned_count, std::size_t{1});
    EXPECT_EQ(ordinary_count, std::size_t{1});
}

TEST(Ruvia, response_appended_values_can_borrow_existing_header_storage) {
    ruvia::http_response response;
    response.header("X-Source", "borrowed");
    const auto value = *response.header("X-Source");
    response.header("Link", std::string_view{}, {.mode_ = ruvia::http_response_header_mode::append});
    for (std::size_t i = 0; i < 64; ++i) {
        response.header("Link", value, {.mode_ = ruvia::http_response_header_mode::append});
    }
    EXPECT_EQ(response.headers().size(), std::size_t{66});
    std::size_t populated = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == "Link" && !header.value().empty()) {
            ++populated;
            EXPECT_EQ(header.value(), "borrowed");
        }
    }
    EXPECT_EQ(populated, std::size_t{64});
}

// RFC 6265 section 5.2.4: an empty Path selects the default-path, the same
// storage key as an absent Path, so each later field replaces the earlier one.

namespace {

using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::http_response_stream_framing;
using ruvia::http_response_trailer_intent;

}  // namespace

TEST(Ruvia, response_stream_commit_discards_trailers_without_wire_framing) {
    constexpr std::array framings{
        http_response_stream_framing::http1_close_delimited,
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    constexpr std::array methods{http_known_method::get, http_known_method::head};
    for (const auto framing : framings) {
        for (const auto method : methods) {
            if ((framing == http_response_stream_framing::http1_chunked ||
                    framing == http_response_stream_framing::http2_frames) &&
                method == http_known_method::get) {
                continue;
            }
            http_response response;
            response.header("Trailer", "X-Checksum");
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, method, response.status(), http_response_trailer_intent::present);
            EXPECT_TRUE(plan.trailer_intent_allowed());
            EXPECT_TRUE(!plan.trailers_available());
            const auto prepared = ruvia::prepare_http_response_stream_head(
                std::move(response), plan);
            EXPECT_TRUE(!prepared.response().header("Trailer").has_value());
            if (method == http_known_method::head) {
                EXPECT_EQ(prepared.commit_plan().head_disposition(), ruvia::http_response_stream_head_disposition::message_ended);
            }
        }
    }
}

TEST(Ruvia, response_stream_commit_keeps_allowed_trailers_open) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    for (const auto framing : framings) {
        http_response response;
        response.header("Trailer", "X-Checksum");
        const auto plan = ruvia::plan_http_response_stream_commit(
            framing, http_known_method::get, response.status(), http_response_trailer_intent::present);
        EXPECT_TRUE(plan.trailer_intent_allowed());
        EXPECT_TRUE(plan.trailers_available());
        const auto prepared = ruvia::prepare_http_response_stream_head(
            std::move(response), plan);
        EXPECT_TRUE(prepared.response().header("Trailer").has_value());
        EXPECT_EQ(prepared.commit_plan().head_disposition(), ruvia::http_response_stream_head_disposition::body_open);
    }
}

TEST(Ruvia, response_stream_commit_rejects_trailers_for_bodyless_statuses) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    constexpr std::array statuses{
        ruvia::http_status::no_content,
        ruvia::http_status::reset_content,
        ruvia::http_status::not_modified};
    for (const auto framing : framings) {
        for (const auto status : statuses) {
            http_response response;
            response.status(status);
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, http_known_method::get, status, http_response_trailer_intent::present);
            EXPECT_TRUE(!plan.trailer_intent_allowed());
            bool rejected = false;
            try {
                (void)ruvia::prepare_http_response_stream_head(
                    std::move(response), plan);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        }
    }
}
