#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http1_request_message_fixture.h"
#include "test_exceptions.h"

namespace {

ruvia::http1_request_content_writer chunked_request_writer() {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    if (prepared_req == nullptr || prepared_req->content_plan().streaming() == nullptr) {
        throw std::logic_error("cannot prepare test request content");
    }
    return ruvia::http1_request_content_writer(*prepared_req->content_plan().streaming());
}

}  // namespace

TEST(Ruvia, http1_streaming_request_chunked_upload_and_trailers) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::https({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    auto* plan = prepared_req->content_plan().streaming();
    EXPECT_TRUE(plan != nullptr);
    EXPECT_EQ(prepared_req->head(), std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n"));
    ruvia::http1_request_content_writer writer(*plan);
    const std::string payload_value = "hello";
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_EQ(std::get<0>(chunk).prefix(), std::string_view("5\r\n"));
    EXPECT_EQ(std::get<0>(chunk).suffix(), std::string_view("\r\n"));
    writer.commit_chunk();
    std::array<char, 128> finish_value{};
    const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
    auto final = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE((final.index() == 0));
    EXPECT_EQ(std::get<0>(final), std::string_view("0\r\nDigest: value\r\n\r\n"));
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

// RFC 9110 section 6.6.2: a sender that will generate trailer fields SHOULD
// declare them in a Trailer field; only chunked content carries trailers.
TEST(Ruvia, http1_streaming_request_declares_trailer_only_for_chunked_content) {
    const std::array declared{ruvia::http_header_view{"Trailer", "Digest"}};
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = declared}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    if (prepared_req != nullptr) {
        EXPECT_EQ(prepared_req->head(), std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTrailer: Digest\r\n"
                                                         "Transfer-Encoding: chunked\r\n\r\n"));
        ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
        std::array<char, 128> finish_value{};
        const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
        auto final = writer.plan_finish(finish_value, trailers);
        EXPECT_TRUE((final.index() == 0));
    }

    auto known_length = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}),
        {.method_ = "POST", .headers_ = declared, .content_length_ = 3}, head);
    const auto* failure = std::get_if<ruvia::http1_client_request_prepare_error>(&known_length);
    EXPECT_TRUE(failure != nullptr && *failure == ruvia::http1_client_request_prepare_error::trailer_requires_chunked_content);

    const std::array forbidden{ruvia::http_header_view{"Trailer", "Content-Length"}};
    auto rejected = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = forbidden}, head);
    const auto* invalid = std::get_if<ruvia::http1_client_request_prepare_error>(&rejected);
    EXPECT_TRUE(invalid != nullptr && *invalid == ruvia::http1_client_request_prepare_error::invalid_header);
}
TEST(Ruvia, http1_streaming_request_known_length_and_continue_gate) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}),
        {.method_ = "PUT", .content_length_ = 3}, head, {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("Content-Length: 3\r\n") != std::string_view::npos));
    ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
    const std::string payload_value = "abc";
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.release_content();
    EXPECT_TRUE(!(writer.plan_finish({}).index() == 0));
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_TRUE(std::get<0>(chunk).prefix().empty());
    EXPECT_TRUE(std::get<0>(chunk).suffix().empty());
    writer.commit_chunk();
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    auto finish_value = writer.plan_finish({});
    EXPECT_TRUE((finish_value.index() == 0));
    EXPECT_TRUE(std::get<0>(finish_value).empty());
    writer.commit_finish();
    EXPECT_TRUE(ruvia::test::throws_on([&] { writer.commit_chunk(); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { writer.commit_finish(); }));
}
TEST(Ruvia, http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    ruvia::http1_request_content_writer writer(*std::get<0>(prepared).content_plan().streaming());
    std::array<char, 128> finish_value{};
    finish_value.fill('X');
    const std::array trailers{ruvia::http_header_view{"Content-Length", "0"}};
    auto rejected = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE(!(rejected.index() == 0));
    EXPECT_EQ(finish_value.front(), 'X');
    EXPECT_TRUE((writer.plan_finish(finish_value).index() == 0));
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailers_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        cross_field };
    for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
             alias_kind::overlapping, alias_kind::cross_field}) {
        auto writer = chunked_request_writer();
        std::array<char, 128> buffer{};
        buffer.fill('#');
        std::array<ruvia::http_header_view, 2> fields{};
        std::size_t count = 1;
        std::string expected = "0\r\nDigest: value\r\n\r\n";
        const auto copy = [&](std::size_t offset, std::string_view text) {
            std::copy(text.begin(), text.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            return std::string_view(buffer.data() + offset, text.size());
        };
        switch (kind) {
            case alias_kind::name:
                fields[0] = {copy(0, "Digest"), "value"};
                break;
            case alias_kind::value:
                fields[0] = {"Digest", copy(0, "value")};
                break;
            case alias_kind::disjoint:
                fields[0] = {copy(0, "Digest"), copy(16, "value")};
                break;
            case alias_kind::overlapping: {
                const auto text = copy(0, "DigestValue");
                fields[0] = {text.substr(0, 6), text.substr(3)};
                expected = "0\r\nDigest: estValue\r\n\r\n";
                break;
            }
            case alias_kind::cross_field:
                fields[0] = {"X-First", copy(32, "first")};
                fields[1] = {copy(0, "Digest"), "second"};
                count = 2;
                expected = "0\r\nX-First: first\r\nDigest: second\r\n\r\n";
                break;
        }
        const auto trailers = std::span(fields).first(count);
        const auto original = buffer;
        const auto too_small = writer.plan_finish(std::span(buffer).first(expected.size() - 1), trailers);
        EXPECT_TRUE(too_small.index() == 1);
        if (too_small.index() == 1) {
            EXPECT_TRUE(std::get<1>(too_small) == ruvia::http1_request_content_write_error::output_too_small);
        }
        EXPECT_TRUE(buffer == original);
        EXPECT_TRUE(!writer.finished());

        const auto planned = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(planned.index() == 0);
        if (planned.index() == 0) {
            EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
        }
        EXPECT_EQ(std::string_view(buffer.data() + expected.size(), buffer.size() - expected.size()), std::string_view(original.data() + expected.size(), original.size() - expected.size()));
        const auto repeated = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(repeated.index() == 1);
        if (repeated.index() == 1) {
            EXPECT_TRUE(std::get<1>(repeated) == ruvia::http1_request_content_write_error::write_pending);
        }
        writer.commit_finish();
        EXPECT_TRUE(writer.finished());
        const auto finished = writer.plan_finish(buffer);
        EXPECT_TRUE(finished.index() == 1);
        if (finished.index() == 1) {
            EXPECT_TRUE(std::get<1>(finished) == ruvia::http1_request_content_write_error::stopped);
        }
    }
}

TEST(Ruvia, http1_streaming_request_finish_accepts_output_alias_at_header_byte_limit) {
    auto writer = chunked_request_writer();
    constexpr std::size_t framing_bytes = std::string_view("0\r\nDigest: \r\n\r\n").size();
    const std::string value(ruvia::max_http_header_bytes - framing_bytes, 'v');
    std::string buffer(ruvia::max_http_header_bytes, 'v');
    const std::array trailers{ruvia::http_header_view{"Digest", std::string_view(buffer).substr(0, value.size())}};
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        const std::string expected = "0\r\nDigest: " + value + "\r\n\r\n";
        EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
    }
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailer_descriptors_stored_in_output) {
    auto writer = chunked_request_writer();
    // The descriptor array itself lives where the trailer section is written, so
    // every descriptor must be consumed before the first output byte lands.
    alignas(ruvia::http_header_view) std::array<char, 256> buffer{};
    auto* const descriptors = ::new (static_cast<void*>(buffer.data()))
        ruvia::http_header_view[2]{{"X-One", "first"}, {"X-Two", "second"}};
    const std::span<const ruvia::http_header_view> trailers(descriptors, 2);
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        EXPECT_EQ(std::get<0>(planned), std::string_view("0\r\nX-One: first\r\nX-Two: second\r\n\r\n"));
    }
    writer.commit_finish();
}

TEST(Ruvia, http_content_length_rejects_negative_sources_without_mutating_existing_options) {
    ruvia::http1_client_request_head_view request{.method_ = "POST", .target_ = "/upload", .content_length_ = 3};
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(!ruvia::http_content_length::try_from_value(invalid).has_value());
        EXPECT_TRUE(ruvia::test::throws_on([&] { request.content_length_ = invalid; }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::http_request_content_plan::known_length(invalid); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::http_request_content_plan::streaming(invalid); }));
    }
    std::array<char, 256> head{};
    const auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), request, head);
    const auto* retained = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(retained != nullptr && retained->content_plan().streaming() != nullptr);
    if (retained && retained->content_plan().streaming()) {
        EXPECT_TRUE(retained->content_plan().streaming()->content_length() == 3);
        EXPECT_TRUE(retained->head().find("Content-Length: 3\r\n") != std::string_view::npos);
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto h2_known = ruvia::http_request_content_plan::known_length(maximum);
    const auto h2_streaming = ruvia::http_request_content_plan::streaming(maximum);
    EXPECT_TRUE(h2_known.known_length_content() && h2_known.known_length_content()->length() == maximum);
    EXPECT_TRUE(h2_streaming.streaming_content() && h2_streaming.streaming_content()->expected_length() == maximum);
}

TEST(Ruvia, http1_chunk_header_encodes_lowercase_hex_and_crlf) {
    const ruvia::http1_chunk_header zero(0);
    const ruvia::http1_chunk_header fifteen(15);
    const ruvia::http1_chunk_header sixteen(16);
    const ruvia::http1_chunk_header abc(0xabc);
    EXPECT_EQ(zero.view(), std::string_view("0\r\n"));
    EXPECT_EQ(fifteen.view(), std::string_view("f\r\n"));
    EXPECT_EQ(sixteen.view(), std::string_view("10\r\n"));
    EXPECT_EQ(abc.view(), std::string_view("abc\r\n"));
}

TEST(Ruvia, http1_chunk_header_buffer_covers_size_t_max) {
    const ruvia::http1_chunk_header header_value((std::numeric_limits<std::size_t>::max)());
    const auto encoded = header_value.view();
    EXPECT_EQ(encoded.size(), sizeof(std::size_t) * 2 + 2);
    EXPECT_TRUE(encoded.ends_with("\r\n"));
}

TEST(Ruvia, http_authority_host_public_parse_preserves_ip_literal_brackets) {
    EXPECT_EQ(ruvia::parse_http_authority_host("[::1]:8080").value(), std::string_view("[::1]"));
    EXPECT_EQ(ruvia::parse_http_authority_host("example.test:443").value(), std::string_view("example.test"));
    EXPECT_TRUE(!ruvia::parse_http_authority_host("user@example.test"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("127.0.0.1"));
    EXPECT_TRUE(!ruvia::is_valid_http_ipv4_literal("999.0.0.1"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv6_literal("::1"));
    EXPECT_TRUE(!ruvia::is_valid_http_ipv6_literal("not-an-ip"));
}

TEST(Ruvia, http_authority_host_public_parse_distinguishes_empty_host_from_invalid_authority) {
    const auto empty_host = ruvia::parse_http_authority_host("");
    EXPECT_TRUE(empty_host.has_value());
    if (empty_host) {
        EXPECT_TRUE(empty_host->empty());
    }

    EXPECT_TRUE(!ruvia::parse_http_authority(""));
    EXPECT_TRUE(!ruvia::is_valid_http_connect_authority(""));
    EXPECT_TRUE(!ruvia::is_valid_http_connect_authority(":443"));
    for (const std::string_view invalid : {":", ":80", "user@example.test", " ", "[::1", "example.test:65536", "example.test:-1"}) {
        EXPECT_TRUE(!ruvia::parse_http_authority_host(invalid));
    }
}

TEST(Ruvia, http_ip_literal_validation_preserves_compression_and_tail_bounds) {
    for (const auto valid : {"::", "1::", "::1", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::",
             "1::2:3:4:5:6:7", "::ffff:192.0.2.1", "1:2:3:4:5:6:192.0.2.1"}) {
        EXPECT_TRUE(ruvia::is_valid_http_ipv6_literal(valid));
    }
    for (const auto invalid : {"", ":", ":::1", "1:::2", "1::2::3", "1:2:3:4:5:6:7",
             "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", "1:2:3:4:5:6::7:8",
             "1:2:3:4:5:6:7:192.0.2.1", "1:2:3:4:5::6:192.0.2.1",
             "::ffff:192.00.2.1", "::ffff:192.0.2.256", "::ffff:192.0.2.1:",
             "::ffff:192.0.2", "::ffff:192.0.2.1suffix", "12345::", "::1%zone", "[::1]"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_ipv6_literal(invalid));
    }
    for (const auto invalid : {"", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.256", "1.2.3.4suffix"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_ipv4_literal(invalid));
    }
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("0.0.0.0"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("255.255.255.255"));
}

namespace {

template <typename fn_type>
[[nodiscard]] bool throws_head_error(fn_type&& fn) {
    try {
        fn();
    } catch (const ruvia::http1_response_head_error&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(Ruvia, http1_connect_head_transfers_to_tunnel_without_message_framing_for_every_successful_status) {
    for (const auto version : {ruvia::http_protocol_version::http10, ruvia::http_protocol_version::http11}) {
        for (unsigned status = 200; status != 300; ++status) {
            ruvia::http_response response;
            response.status(ruvia::http_status_code::from_value(status));
            response.header("x-tunnel", "established");
            const auto plan = ruvia::prepare_http1_connect_response_head(response, version);
            ruvia::http_response_head_buffer head;
            ruvia::append_http1_response_head(response, head, plan);
            EXPECT_TRUE(head.view().starts_with("HTTP/1.1 "));
            EXPECT_TRUE(head.view().find("x-tunnel: established\r\n") != std::string_view::npos);
            EXPECT_TRUE(head.view().find("Content-Length:") == std::string_view::npos);
            EXPECT_TRUE(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            EXPECT_TRUE(head.view().ends_with("\r\n\r\n"));
        }
    }
}

TEST(Ruvia, http1_connect_head_rejects_failed_status_payload_and_framing_fields_before_commit) {
    for (const auto name : {"Content-Length", "Transfer-Encoding"}) {
        ruvia::http_response response;
        response.header(name, name == std::string_view("Content-Length") ? "0" : "chunked");
        EXPECT_TRUE(throws_head_error([&] {
            (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
        }));
    }
    ruvia::http_response response;
    response.status(ruvia::http_status::forbidden);
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
    response.status(ruvia::http_status::ok);
    response.body("HTTP payload");
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
}

namespace {

using ruvia::test::parse_http1_request_message;

[[nodiscard]] bool offers_upgrade(std::string_view wire) {
    const auto parsed = parse_http1_request_message(wire);
    EXPECT_TRUE(parsed.complete()) << wire;
    return parsed.complete() && ruvia::http1_request_offers_upgrade(parsed.state_.request());
}

[[nodiscard]] std::size_t field_count(const ruvia::http_response& response, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        count += ruvia::http_ascii_equals_ignore_case(header.name(), name) ? 1 : 0;
    }
    return count;
}

// The 101 head `response` becomes for `request_wire`, or nothing when
// preparing it throws http1_response_head_error.
[[nodiscard]] std::string upgrade_head(ruvia::http_response& response, std::string_view request_wire) {
    const auto parsed = parse_http1_request_message(request_wire);
    EXPECT_TRUE(parsed.complete()) << request_wire;
    try {
        const auto plan = ruvia::prepare_http1_upgrade_response_head(response, parsed.state_.request());
        EXPECT_TRUE(plan.close_delimited_stream() != nullptr);
        ruvia::http_response_head_buffer head;
        ruvia::append_http1_response_head(response, head, plan);
        return std::string(head.view());
    } catch (const ruvia::http1_response_head_error&) {
        return {};
    }
}

constexpr std::string_view offering_request =
    "GET /chat HTTP/1.1\r\nHost: example.test\r\nConnection: keep-alive, Upgrade\r\nUpgrade: echo/1, other\r\n\r\n";

}  // namespace

// RFC 9110 §7.8: only an HTTP/1.1 request that lists the Upgrade connection
// option beside an Upgrade field naming a protocol asks to switch; CONNECT
// establishes a tunnel through its 2xx instead.
TEST(Ruvia, http1_upgrade_offer_needs_http11_the_upgrade_option_and_a_protocol) {
    EXPECT_TRUE(offers_upgrade(offering_request));
    EXPECT_TRUE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nconnection: UPGRADE\r\nupgrade: Echo\r\n\r\n"));
    EXPECT_TRUE(offers_upgrade(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nConnection: upgrade\r\n"
        "Upgrade: , echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: ,\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.0\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade(
        "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test:443\r\n"
        "Connection: upgrade\r\nUpgrade: echo\r\n\r\n"));
}

// Without an application Upgrade field the 101 names the first offered
// protocol; the head carries no framing and the connection option is added
// once, beside the application's own options.
TEST(Ruvia, http1_upgrade_head_switches_to_an_offered_protocol_without_framing) {
    {
        ruvia::http_response response;
        response.header("x-tunnel", "echo");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_EQ(response.status().value(), 101);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nUpgrade: echo/1\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nx-tunnel: echo\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("Content-Length:"), std::string::npos) << head;
        EXPECT_EQ(head.find("Transfer-Encoding:"), std::string::npos) << head;
        EXPECT_TRUE(head.ends_with("\r\n\r\n")) << head;
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "OTHER");
        response.header("Connection", "upgrade");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_NE(head.find("\r\nUpgrade: OTHER\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("echo/1"), std::string::npos) << head;
        EXPECT_EQ(field_count(response, "Upgrade"), 1U);
        EXPECT_EQ(field_count(response, "Connection"), 1U);
    }
    {
        ruvia::http_response response;
        response.header("Connection", "x-hop");
        response.header("x-hop", "1");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nConnection: x-hop\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
    }
}

// A request without an offer, a protocol the client did not offer, content
// and framing fields are refused before the response changes.
TEST(Ruvia, http1_upgrade_head_rejects_unoffered_switches_content_and_framing_unchanged) {
    const auto refused_unchanged = [&](ruvia::http_response& response, std::string_view request_wire) {
        const auto fields = response.headers().size();
        const auto refused = upgrade_head(response, request_wire).empty();
        return refused && response.status().value() == 200 && response.headers().size() == fields;
    };
    {
        ruvia::http_response response;
        EXPECT_TRUE(refused_unchanged(response, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "echo/2");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "other, unoffered");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.body("payload");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"Content-Length", "0"},
             std::pair<std::string_view, std::string_view>{"Transfer-Encoding", "chunked"}}) {
        ruvia::http_response response;
        response.header(name, value);
        EXPECT_TRUE(refused_unchanged(response, offering_request)) << name;
    }
}

namespace {

using ruvia::http1_interim_connection_disposition;
using ruvia::http1_interim_response_buffer_too_small;
using ruvia::http1_response_head_error;
using ruvia::http_header_view;
using ruvia::http_interim_response_head;
using ruvia::prepare_http1_interim_response_head;
using ruvia::prepared_http1_interim_response;

[[nodiscard]] bool unchanged(const std::array<char, 64>& buffer, char sentinel) {
    return std::ranges::all_of(buffer, [sentinel](char value) { return value == sentinel; });
}

// Rejection throws before the head buffer is touched.
[[nodiscard]] bool rejects_without_writing(std::span<const http_header_view> fields_value, char sentinel) {
    std::array<char, 64> buffer;
    buffer.fill(sentinel);
    try {
        (void)prepare_http1_interim_response_head(
            http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
    } catch (const http1_response_head_error&) {
        return unchanged(buffer, sentinel);
    }
    return false;
}

}  // namespace

TEST(Ruvia, http1_interim_response_writer_emits_exact_typed_head) {
    std::array<char, 64> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_EQ(prepared->head(), std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));
        EXPECT_EQ(prepared->connection_disposition(), http1_interim_connection_disposition::unchanged);
    }

    const http_header_view hints[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    std::array<char, 256> hints_buffer{};
    const auto hints_result = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, hints), hints_buffer);
    const auto* const prepared_hints = std::get_if<prepared_http1_interim_response>(&hints_result);
    EXPECT_TRUE(prepared_hints != nullptr);
    if (prepared_hints != nullptr) {
        EXPECT_EQ(prepared_hints->head(), std::string_view("HTTP/1.1 103 Early Hints\r\n"
                                                           "Link: </style.css>; rel=preload\r\n"
                                                           "Content-Type: text/html; charset=utf-8\r\n"
                                                           "X-Hint: warm\r\n\r\n"));
        EXPECT_TRUE(!(prepared_hints->head().find("Server:") != std::string_view::npos));
        EXPECT_TRUE(!(prepared_hints->head().find("Date:") != std::string_view::npos));
    }
}

TEST(Ruvia, http1_interim_response_writer_preserves_required_status_line_space) {
    std::array<char, 32> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status_code::from_value(199)), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        // RFC 9112 section 4 requires the SP after status-code even when the
        // optional reason phrase is absent.
        EXPECT_EQ(prepared->head(), std::string_view("HTTP/1.1 199 \r\n\r\n"));
    }
}

TEST(Ruvia, http1_interim_response_writer_closes_after_containing_response) {
    const http_header_view fields_value[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "example/1"},
    };
    std::array<char, 128> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_EQ(prepared->connection_disposition(), http1_interim_connection_disposition::close_after_interim_response);
        EXPECT_TRUE((prepared->head().find("Upgrade: example/1\r\n") != std::string_view::npos));
    }
}

TEST(Ruvia, http1_interim_response_writer_buffer_too_small_is_transactional) {
    std::array<char, 8> buffer;
    buffer.fill('#');
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const too_small = std::get_if<http1_interim_response_buffer_too_small>(&result_value);
    EXPECT_TRUE(too_small != nullptr);
    if (too_small != nullptr) {
        EXPECT_EQ(too_small->required_head_bytes(), std::size_t{25});
    }
    EXPECT_TRUE(std::ranges::all_of(buffer, [](char value) { return value == '#'; }));
}

TEST(Ruvia, http1_interim_response_writer_preserves_fields_borrowed_from_output) {
    constexpr std::string_view expected = "HTTP/1.1 103 Early Hints\r\nX-Hint: warm\r\nX-Next: ready\r\n\r\n";
    for (const std::size_t offset : {std::size_t{0}, std::size_t{24}, std::size_t{32}}) {
        std::array<char, 128> buffer;
        buffer.fill('#');
        constexpr std::string_view borrowed = "X-HintwarmX-Nextready";
        std::ranges::copy(borrowed, buffer.begin() + offset);
        const auto input = std::string_view(buffer.data() + offset, borrowed.size());
        const std::array fields_value{
            http_header_view(input.substr(0, 6), input.substr(6, 4)),
            http_header_view(input.substr(10, 6), input.substr(16, 5)),
        };
        const auto result_value = prepare_http1_interim_response_head(
            http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
        const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
        EXPECT_TRUE(prepared != nullptr);
        if (prepared != nullptr) {
            EXPECT_EQ(prepared->head(), expected);
            EXPECT_EQ(prepared->head().data(), buffer.data());
            EXPECT_EQ(buffer[expected.size()], '#');
        }
    }
}

TEST(Ruvia, http1_interim_response_writer_rejects_invalid_fields_transactionally) {
    const http_header_view malformed_name[] = {{"Bad Name", "value"}};
    const http_header_view malformed_value[] = {{"X-Test", "a\r\nb"}};
    const http_header_view malformed_content_encoding[] = {{"Content-Encoding", "gzip;level=9"}};
    const http_header_view empty_content_encoding[] = {{"Content-Encoding", ""}};
    const http_header_view malformed_content_type[] = {{"Content-Type", "not a media type"}};
    const http_header_view empty_content_type[] = {{"Content-Type", ""}};
    const http_header_view content_length[] = {{"Content-Length", "0"}};
    const http_header_view transfer_encoding[] = {{"Transfer-Encoding", "chunked"}};
    const http_header_view trailer[] = {{"Trailer", "X-Checksum"}};
    const http_header_view te[] = {{"TE", "trailers"}};
    const http_header_view duplicate_server[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    const http_header_view invalid_connection[] = {{"Connection", ","}};
    const http_header_view managed_connection[] = {
        {"Connection", "close, date"},
    };
    const http_header_view invalid_upgrade[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "bad protocol"},
    };
    const http_header_view unlisted_upgrade[] = {{"Upgrade", "example/1"}};

    for (const std::span<const http_header_view> fields_value :
        {std::span<const http_header_view>(malformed_name), std::span<const http_header_view>(malformed_value),
            std::span<const http_header_view>(malformed_content_encoding),
            std::span<const http_header_view>(empty_content_encoding),
            std::span<const http_header_view>(malformed_content_type),
            std::span<const http_header_view>(empty_content_type), std::span<const http_header_view>(content_length),
            std::span<const http_header_view>(transfer_encoding), std::span<const http_header_view>(trailer),
            std::span<const http_header_view>(te), std::span<const http_header_view>(duplicate_server),
            std::span<const http_header_view>(invalid_connection),
            std::span<const http_header_view>(managed_connection),
            std::span<const http_header_view>(invalid_upgrade), std::span<const http_header_view>(unlisted_upgrade)}) {
        EXPECT_TRUE(rejects_without_writing(fields_value, '@'));
    }
}

TEST(Ruvia, http1_interim_response_writer_enforces_size_limit) {
    std::vector<http_header_view> many(80, http_header_view("X-Hint", "value"));
    std::array<char, 2048> many_buffer{};
    const auto many_result = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, many),
        many_buffer);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_interim_response>(many_result));

    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const http_header_view oversized[] = {{"X-Hint", oversized_value}};
    EXPECT_TRUE(rejects_without_writing(oversized, '!'));
}

static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, std::vector<http_header_view>&>);
static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, const std::vector<http_header_view>&>);
static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, const std::array<http_header_view, 1>&>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::vector<http_header_view>>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::array<http_header_view, 1>>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::initializer_list<http_header_view>>);
static_assert(std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, const std::vector<http_header_view>&>);
static_assert(!std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, std::vector<http_header_view>>);
static_assert(!std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, std::array<http_header_view, 1>>);
