#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "http1_request_message_fixture.h"

namespace {

// Appends the trailer section that ends the chunked `wire` the way an HTTP/1
// server receives it.
std::optional<ruvia::http_request_trailer_error> append_received_http1(
    ruvia::http_request_trailers& trailers, std::string_view wire) {
    ruvia::http1_chunked_body_decoder decoder;
    const auto decoded = decoder.decode(wire);
    const auto* complete = decoded.complete();
    EXPECT_TRUE(complete != nullptr);
    return complete != nullptr ? trailers.append_received(complete->trailers()) : std::nullopt;
}

}  // namespace

TEST(Ruvia, request_trailers_append_received_is_atomic_and_discards_fields_not_permitted_as_trailers) {
    ruvia::http_request_trailers trailers;
    EXPECT_TRUE(!trailers.append("X-First", "1").has_value());
    EXPECT_TRUE(trailers.append("Content-Length", "3") == ruvia::http_request_trailer_error::forbidden_field);

    // RFC 9112 section 7.1.2: a recipient may discard received trailer fields;
    // the sender-only prohibition of RFC 9110 section 6.5.1 does not fail the section.
    EXPECT_TRUE(!append_received_http1(
        trailers, "0\r\nX-Second: 2\r\nContent-Length: 3\r\nCache-Control: no-cache\r\n\r\n")
            .has_value());
    const std::array received{ruvia::http_header("x-third", "3"), ruvia::http_header("content-length", "3")};
    EXPECT_TRUE(!trailers.append_received(received).has_value());
    EXPECT_EQ(trailers.fields().size(), std::size_t{3});
    EXPECT_EQ(trailers.field("X-Second").value_or(""), "2");
    EXPECT_EQ(trailers.field("X-Third").value_or(""), "3");
    EXPECT_TRUE(!trailers.field("Content-Length").has_value());
    EXPECT_TRUE(!trailers.field("Cache-Control").has_value());

    // A pseudo-header field makes the whole section malformed.
    const std::array invalid{ruvia::http_header("x-fourth", "4"), ruvia::http_header(":path", "/")};
    EXPECT_TRUE(trailers.append_received(invalid) == ruvia::http_request_trailer_error::invalid_field);
    EXPECT_EQ(trailers.fields().size(), std::size_t{3});

    // A section that would exceed the remaining byte budget leaves no partial fields behind.
    const std::array oversized{ruvia::http_header("x-fill", "v"),
        ruvia::http_header("x-oversize", std::string(ruvia::max_http_header_bytes, 'x'))};
    EXPECT_TRUE(trailers.append_received(oversized) == ruvia::http_request_trailer_error::section_too_large);
    EXPECT_EQ(trailers.fields().size(), std::size_t{3});
    // X-First used 12 normalized bytes, x-second 13, x-third 12, the discarded
    // content-length 19 and the terminating empty line 2; the remaining budget
    // is intact only if the failed sections rolled back.
    EXPECT_TRUE(trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 67, 'z')) ==
                ruvia::http_request_trailer_error::section_too_large);
    EXPECT_TRUE(!trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 68, 'z')).has_value());
}

TEST(Ruvia, request_trailers_from_http1_retain_lowercase_names) {
    ruvia::http_request_trailers trailers;
    EXPECT_TRUE(!trailers.append("X-Appended", "1").has_value());
    EXPECT_TRUE(!append_received_http1(trailers, "0\r\nX-Mixed-Case: 2\r\n\r\n").has_value());
    const auto fields = trailers.fields();
    EXPECT_EQ(fields.size(), std::size_t{2});
    if (fields.size() == 2) {
        EXPECT_EQ(fields[0].name(), std::string_view("x-appended"));
        EXPECT_EQ(fields[1].name(), std::string_view("x-mixed-case"));
        EXPECT_EQ(fields[1].value(), std::string_view("2"));
    }
}

TEST(Ruvia, request_trailers_cap_field_count) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        std::string block;
        for (std::size_t index = 0; index < ruvia::max_http_header_fields + excess; ++index) {
            block.append("X:a\r\n");
        }
        const std::string wire = "0\r\n" + block + "\r\n";
        ruvia::http1_chunked_body_decoder decoder;
        const auto decoded = decoder.decode(wire);
        ruvia::http_request_trailers trailers;
        if (excess == 0) {
            EXPECT_TRUE(decoded.complete() != nullptr);
            if (const auto* complete = decoded.complete()) {
                EXPECT_TRUE(!trailers.append_received(complete->trailers()).has_value());
            }
            EXPECT_EQ(trailers.fields().size(), ruvia::max_http_header_fields);
            // A full section admits no further field through either entry.
            EXPECT_TRUE(trailers.append("X", "b") == ruvia::http_request_trailer_error::section_too_large);
            const std::array more{ruvia::http_header("x", "b")};
            EXPECT_TRUE(trailers.append_received(more) == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_EQ(trailers.fields().size(), ruvia::max_http_header_fields);
        } else {
            EXPECT_TRUE(decoded.failure() != nullptr);
            if (const auto* failure = decoded.failure()) {
                EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
            }
        }
    }
}

TEST(Ruvia, request_trailers_enforce_normalized_section_limit_during_chunk_decoding) {
    for (const std::size_t field_count : {std::size_t{3}, std::size_t{80}}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
            // Each "X:a" field costs 6 normalized bytes and the last field 5 plus
            // its value; the section also pays 2 for its terminating empty line.
            const auto value_size = ruvia::max_http_header_bytes - 6 * (field_count - 1) - 5 - 2 + excess;
            std::string block;
            for (std::size_t index = 1; index < field_count; ++index) {
                block.append("X:a\r\n");
            }
            block.append("X:");
            block.append(value_size, 'z');
            const std::string wire = "0\r\n" + block + "\r\n\r\n";
            ruvia::http1_chunked_body_decoder decoder;
            const auto decoded = decoder.decode(wire);
            ruvia::http_request_trailers trailers;
            if (excess == 0) {
                EXPECT_TRUE(decoded.complete() != nullptr);
                if (const auto* complete = decoded.complete()) {
                    EXPECT_TRUE(!trailers.append_received(complete->trailers()).has_value());
                    EXPECT_EQ(trailers.fields().size(), field_count);
                    EXPECT_EQ(trailers.field("X").value_or(""), std::string(value_size, 'z'));
                }
            } else {
                EXPECT_TRUE(decoded.failure() != nullptr);
                if (const auto* failure = decoded.failure()) {
                    EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
                }
            }
        }
    }
}

TEST(Ruvia, request_and_response_trailer_sections_share_one_byte_rule) {
    // Two fields named "X-A": each costs name + value + 4 normalized bytes, and
    // the section pays 2 for its terminating empty line.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        const auto first_value = std::string(1000, 'a');
        const auto second_value =
            std::string(ruvia::max_http_header_bytes - 2 * (3 + 4) - 2 - first_value.size() + excess, 'b');
        ruvia::http_request_trailers received;
        EXPECT_TRUE(!received.append("X-A", first_value).has_value());
        const auto appended = received.append("X-A", second_value);
        const std::array forwarded{ruvia::http_header_view{"X-A", first_value},
            ruvia::http_header_view{"X-A", second_value}};
        bool response_rejected = false;
        try {
            (void)ruvia::validate_http_response_trailers(forwarded);
        } catch (const std::exception&) {
            response_rejected = true;
        }
        if (excess == 0) {
            // A received request section can always be forwarded as response trailers.
            EXPECT_TRUE(!appended.has_value());
            EXPECT_TRUE(!response_rejected);
        } else {
            EXPECT_TRUE(appended == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_TRUE(response_rejected);
        }
    }
}

TEST(Ruvia, largest_accepted_response_trailer_section_decodes) {
    // One field whose encoded section "X: <value>\r\n\r\n" is exactly the limit
    // is accepted for sending and decoded by a chunk decoder; one more value
    // byte is rejected before anything is emitted.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        const std::string value(ruvia::max_http_header_bytes - 1 - 4 - 2 + excess, 'v');
        const std::array trailers{ruvia::http_header_view{"X", value}};
        bool rejected = false;
        try {
            (void)ruvia::validate_http_response_trailers(trailers);
        } catch (const std::exception&) {
            rejected = true;
        }
        EXPECT_EQ(rejected, excess != 0);
        if (excess != 0) {
            continue;
        }
        const std::string wire = "0\r\nX: " + value + "\r\n\r\n";
        ruvia::http1_chunked_body_decoder decoder;
        const auto decoded = decoder.decode(wire);
        EXPECT_TRUE(decoded.complete() != nullptr);
        if (const auto* complete = decoded.complete()) {
            EXPECT_EQ(complete->trailers().size(), std::size_t{1});
            if (complete->trailers().size() == 1) {
                EXPECT_EQ(complete->trailers()[0].name(), "x");
                EXPECT_EQ(complete->trailers()[0].value(), value);
            }
        }
    }
}

TEST(Ruvia, received_response_trailers_discard_fields_not_permitted_as_trailers) {
    ruvia::http1_chunked_body_decoder decoder({.trailer_message_ = ruvia::http1_trailer_message::response});
    const auto decoded =
        decoder.decode("0\r\nContent-Type: text/plain\r\nX-Sum: abc\r\nContent-Length: 5\r\nETag: \"v\"\r\n\r\n");
    EXPECT_TRUE(decoded.complete() != nullptr);
    if (const auto* complete = decoded.complete()) {
        const auto fields = complete->trailers();
        EXPECT_EQ(fields.size(), std::size_t{2});
        if (fields.size() == 2) {
            EXPECT_EQ(fields[0].name(), "x-sum");
            EXPECT_EQ(fields[0].value(), "abc");
            EXPECT_EQ(fields[1].name(), "etag");
            EXPECT_EQ(fields[1].value(), "\"v\"");
        }
    }

    ruvia::http1_chunked_body_decoder malformed({.trailer_message_ = ruvia::http1_trailer_message::response});
    const auto rejected = malformed.decode("0\r\nContent-Type: text/plain\r\nbad line\r\n\r\n");
    EXPECT_TRUE(rejected.failure() != nullptr);
}

TEST(Ruvia, request_trailers_append_handles_self_aliasing_across_reallocation) {
    ruvia::http_request_trailers trailers;
    EXPECT_TRUE(!trailers.append("X-One", "first-value").has_value());
    EXPECT_TRUE(!trailers.append("X-Two", "second-value").has_value());
    // Appending the container's own span across capacity growth must not read
    // from invalidated elements.
    for (int round = 0; round < 3; ++round) {
        EXPECT_TRUE(!trailers.append(trailers.fields()).has_value());
    }
    EXPECT_EQ(trailers.fields().size(), std::size_t{16});
    EXPECT_EQ(trailers.fields()[0].name(), "x-one");
    EXPECT_EQ(trailers.fields()[0].value(), "first-value");
    EXPECT_EQ(trailers.fields()[15].name(), "x-two");
    EXPECT_EQ(trailers.fields()[15].value(), "second-value");
}

namespace {

using ruvia::http_content_coding;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_protocol_version;
using ruvia::http_request;
using ruvia::http_request_target_form;
using ruvia::request_content_coding;
using ruvia::test::parse_http1_request_message;

}  // namespace

TEST(Ruvia, request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    {
        auto retained = parse_http1_request_message("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n");
        EXPECT_TRUE(retained.complete());
        for (int i = 0; i < 64; ++i) {
            {
                auto result_value = parse_http1_request_message("GET / HTTP/1.1\r\nHost: second\r\n\r\n");
                EXPECT_TRUE(result_value.complete());
                auto moved = std::move(result_value);
                EXPECT_EQ(moved.state_.request().header("Host").value(), std::string_view("second"));
                EXPECT_EQ(retained.state_.request().header("X-Data").value(), std::string_view("saved"));
            }
            {
                const auto incomplete = parse_http1_request_message("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 3\r\n\r\nx");
                EXPECT_TRUE(incomplete.need_body());
            }
        }
        EXPECT_EQ(retained.state_.request().path(), std::string_view("/saved"));
    }
}

TEST(Ruvia, request_public_reset_clears_borrowed_views_and_owned_headers) {
    const http_header_view headers[]{{"Host", "example.test"}};
    auto request = ruvia::make_parsed_http_request(
        "GET", "https://example.test/path?key=value", headers, {});
    {
        const http_header_view conflicting_host[]{{"Host", "conflicting.example"}};
        auto rebound_req = ruvia::make_parsed_http_request(
            "GET", "https://example.test/path", conflicting_host, {});
        EXPECT_EQ(rebound_req.header("Host").value_or(""), std::string_view("example.test"));
    }
    request.reset();
    EXPECT_TRUE(request.method().empty());
    EXPECT_TRUE(request.target().empty());
    EXPECT_TRUE(request.scheme().empty());
    EXPECT_TRUE(request.authority().empty());
    EXPECT_TRUE(request.path().empty());
    EXPECT_TRUE(request.query_string().empty());
    EXPECT_TRUE(request.headers().empty());
    EXPECT_TRUE(!request.header("Host").has_value());
    EXPECT_TRUE(request.body_bytes().empty());
    EXPECT_TRUE(request.body().empty());
    EXPECT_TRUE(request.protocol_version() == http_protocol_version::http11);
    EXPECT_TRUE(request.target_form() == http_request_target_form::origin);
}

TEST(Ruvia, request_header_distinguishes_missing_from_present_empty) {
    const http_header_view headers[]{{"X-Empty", ""}, {"Host", ""}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {});
    EXPECT_TRUE(!request.header("X-Missing").has_value());
    const auto present_empty = request.header("x-empty");
    EXPECT_TRUE(present_empty.has_value());
    EXPECT_TRUE(present_empty.value_or("missing").empty());

    const auto known_present_empty = request.header("HOST");
    EXPECT_TRUE(known_present_empty.has_value());
    EXPECT_TRUE(known_present_empty.value_or("missing").empty());
}

TEST(Ruvia, parsed_request_applies_recipient_field_rules_like_wire_parsers) {
    // RFC 9110 section 5.3: only list fields may repeat; joined Range or
    // If-Range lines would otherwise resolve to their last line.
    const http_header_view duplicate_range[]{{"Range", "bytes=0-0"}, {"Range", "bytes=5-5"}};
    const http_header_view duplicate_if_range[]{{"If-Range", "\"y\""}, {"If-Range", "\"x\""}};
    const http_header_view duplicate_host[]{{"Host", "a.example"}, {"Host", "b.example"}};
    for (const auto headers : {std::span<const http_header_view>(duplicate_range),
             std::span<const http_header_view>(duplicate_if_range), std::span<const http_header_view>(duplicate_host)}) {
        bool rejected = false;
        try {
            (void)ruvia::make_parsed_http_request("GET", "/", headers, {});
        } catch (const ruvia::http_protocol_error& error) {
            rejected = error.status() == ruvia::http_status::bad_request;
        }
        EXPECT_TRUE(rejected);
    }

    const http_header_view repeated_list[]{{"Accept", "text/plain"}, {"Accept", "text/html"},
        {"User-Agent", "first/1"}, {"User-Agent", "second/2"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", repeated_list, {});
    EXPECT_EQ(request.headers().size(), std::size_t{4});
}

TEST(Ruvia, request_content_coding_accumulates_repeated_header_fields_in_order) {
    const http_header_view headers[]{{"Content-Encoding", "br"}, {"Content-Encoding", "gzip"}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", headers, {});
    const auto coding = request_content_coding(request);
    EXPECT_TRUE(coding.invalid() == nullptr);
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 2U);
    if (coding.codings().size() == 2) {
        EXPECT_TRUE(coding.codings()[0] == http_content_coding::brotli);
        EXPECT_TRUE(coding.codings()[1] == http_content_coding::gzip);
    }
}

TEST(Ruvia, request_content_coding_combines_field_lines_with_list_semantics) {
    const http_header_view headers[]{{"Content-Encoding", ","}, {"Content-Encoding", "gzip"}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", headers, {});

    const auto coding = request_content_coding(request);
    EXPECT_TRUE(coding.invalid() == nullptr);
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == http_content_coding::gzip);
    }
}

TEST(Ruvia, request_access_raw_query_lookup_preserves_encoding_and_uses_last_match) {
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/?a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue",
        {}, {});

    const auto value = request.last_raw_query_value("a");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second+value"));
    EXPECT_TRUE(!request.last_raw_query_value("encoded key").has_value());
    EXPECT_EQ(*request.last_raw_query_value("encoded%20key"), std::string_view("raw%2Fvalue"));
}

TEST(Ruvia, request_access_cookie_lookup_uses_last_match) {
    const http_header_view headers[]{{"Cookie", "sid=first; theme=dark; sid=second"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {});

    const auto value = request.cookie("sid");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second"));
}

TEST(Ruvia, request_access_cookie_lookup_scans_repeated_cookie_fields) {
    const http_header_view headers[]{{"Cookie", "a=1"}, {"Cookie", "b=2"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {});

    const auto first = request.cookie("a");
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(*first, std::string_view("1"));
    const auto second = request.cookie("b");
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(*second, std::string_view("2"));
}

TEST(Ruvia, request_access_cookie_lookup_preserves_distinct_fields_with_shared_storage) {
    constexpr std::string_view shared = "sid=old; sid=new; token=present";
    const http_header_view headers[]{{"Cookie", shared}, {"Cookie", shared.substr(0, 7)}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {});
    EXPECT_EQ(request.headers().size(), std::size_t{2});
    EXPECT_EQ(request.header("Cookie").value_or("missing"), std::string_view("sid=old"));
    EXPECT_EQ(request.cookie("sid").value_or("missing"), std::string_view("old"));
    EXPECT_EQ(request.cookie("token").value_or("missing"), std::string_view("present"));
    EXPECT_TRUE(!request.cookie("absent").has_value());
}

TEST(Ruvia, request_body_string_view_and_known_method_bits_match_contracts) {
    const std::array<std::byte, 4> payload{
        std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", {}, payload);
    EXPECT_EQ(request.body(), std::string_view("data"));
    EXPECT_EQ(request.body_bytes().size(), std::size_t{4});
    static_assert(ruvia::http_known_method_bit(http_known_method::get) == 1U);
    static_assert(ruvia::http_known_method_bit(http_known_method::unknown) == 0U);
}

namespace {

template <typename method_type, typename target_type, typename body_type>
concept make_parsed_request_accepts = requires(method_type&& method, target_type&& target, body_type&& body) {
    ruvia::make_parsed_http_request(std::forward<method_type>(method), std::forward<target_type>(target),
        std::span<const ruvia::http_header_view>{}, std::forward<body_type>(body));
};

static_assert(make_parsed_request_accepts<const std::string&, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<std::string, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, std::string, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::vector<std::byte>>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::array<std::byte, 1>>);

template <typename request_type>
concept request_allows_body = requires(request_type&& req) {
    std::forward<request_type>(req).body();
};
static_assert(request_allows_body<const http_request&>);
static_assert(!request_allows_body<http_request>);

}  // namespace

namespace {

using ruvia::http_target_host_kind;
using ruvia::http_target_origin;
using ruvia::http_target_origin_action;
using ruvia::http_target_origin_connection;

constexpr http_target_origin_connection cleartext{};
constexpr http_target_origin_connection secured{.secured_ = true};

[[nodiscard]] http_target_origin decide(std::string_view method, std::string_view target, std::string_view host,
    http_target_origin_connection connection) {
    const std::array headers{ruvia::http_header_view("Host", host)};
    const auto request = ruvia::make_parsed_http_request(method, target, headers, {});
    return ruvia::decide_http_target_origin(request, connection);
}

template <typename origin_type>
concept target_origin_allows_host = requires(origin_type&& origin) {
    std::forward<origin_type>(origin).host();
};

template <typename origin_type>
concept target_origin_allows_address = requires(origin_type&& origin) {
    std::forward<origin_type>(origin).address();
};

static_assert(target_origin_allows_host<const http_target_origin&>);
static_assert(!target_origin_allows_host<http_target_origin>);
static_assert(target_origin_allows_address<const http_target_origin&>);
static_assert(!target_origin_allows_address<http_target_origin>);
}  // namespace

TEST(Ruvia, target_origin_dns_reference_names_follow_label_and_full_name_boundaries) {
    const auto maximal = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                         std::string(63, 'c') + "." + std::string(61, 'd');
    for (const std::string_view host : {std::string_view(maximal), std::string_view("7.example"),
             std::string_view("xn--bcher-kva.example")}) {
        const auto origin = decide("GET", "/", host, secured);
        EXPECT_TRUE(origin.action() == http_target_origin_action::verify_certificate);
        EXPECT_TRUE(origin.host_kind() == http_target_host_kind::dns_name);
        EXPECT_EQ(origin.host(), host);
    }
    const auto rooted_name = maximal + ".";
    const auto rooted = decide("GET", "/", rooted_name, secured);
    EXPECT_TRUE(rooted.action() == http_target_origin_action::verify_certificate);
    EXPECT_EQ(rooted.host(), std::string_view(maximal));

    for (const auto& host : {maximal + "d", std::string(64, 'a') + ".example"}) {
        EXPECT_TRUE(decide("GET", "/", host, secured).action() == http_target_origin_action::misdirected);
    }
}

// RFC 9110 §7.4: an "https" target needs a secured connection whose
// certificate covers the target host; "http" targets and tunnel destinations
// impose nothing, and a trusted gateway vouches for the target itself.
TEST(Ruvia, target_origin_decision_follows_scheme_transport_and_gateway_trust) {
    EXPECT_TRUE(decide("GET", "https://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "HTTPS://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "https://a.example/x", "x", {.trusted_gateway_ = true}).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "http://a.example/x", "x", cleartext).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "/x", "a.example", cleartext).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "http://a.example/x", "x", secured).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("CONNECT", "a.example:443", "a.example:443", secured).action() == http_target_origin_action::accept);
    // RFC 9112 §3.3: an empty authority takes the configured default.
    EXPECT_TRUE(decide("GET", "/x", "", secured).action() == http_target_origin_action::accept);

    const auto name = decide("GET", "/x", "Example.COM.:8443", secured);
    EXPECT_TRUE(name.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(name.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(name.host() == "Example.COM");
    EXPECT_TRUE(name.address().empty());

    // The absolute-form authority, not a conflicting Host, is the target.
    const auto absolute = decide("GET", "https://b.example/x", "a.example", secured);
    EXPECT_TRUE(absolute.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(absolute.host() == "b.example");

    const auto asterisk = decide("OPTIONS", "*", "a.example", secured);
    EXPECT_TRUE(asterisk.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(asterisk.host() == "a.example");

    // RFC 3986 §3.2.2 / §6.2.2.2 and RFC 9110 §4.2.3: percent-encoded
    // unreserved octets in reg-name are decoded before host classification.
    const auto encoded_dns = decide("GET", "/x", "%6Cocalhost%2E:8443", secured);
    EXPECT_TRUE(encoded_dns.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_dns.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(encoded_dns.host() == "localhost");

    const auto encoded_abs = decide("GET", "https://%6Cocalhost/x", "localhost", secured);
    EXPECT_TRUE(encoded_abs.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_abs.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(encoded_abs.host() == "localhost");

    const auto encoded_ipv4 = decide("GET", "/x", "192.0.2.%37:443", secured);
    EXPECT_TRUE(encoded_ipv4.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_ipv4.host_kind() == http_target_host_kind::ipv4);
    EXPECT_TRUE(std::ranges::equal(encoded_ipv4.address(), std::array<std::uint8_t, 4>{192, 0, 2, 7}));

    const auto ipv4 = decide("GET", "/x", "192.0.2.7:443", secured);
    EXPECT_TRUE(ipv4.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(ipv4.host_kind() == http_target_host_kind::ipv4);
    constexpr std::array<std::uint8_t, 4> ipv4_bytes{192, 0, 2, 7};
    EXPECT_TRUE(std::ranges::equal(ipv4.address(), ipv4_bytes));

    const auto ipv6 = decide("GET", "/x", "[2001:db8::1]", secured);
    EXPECT_TRUE(ipv6.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(ipv6.host_kind() == http_target_host_kind::ipv6);
    constexpr std::array<std::uint8_t, 16> ipv6_bytes{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_TRUE(std::ranges::equal(ipv6.address(), ipv6_bytes));
    EXPECT_TRUE(ipv6.host().empty());

    // No certificate identifies an IPvFuture host.
    EXPECT_TRUE(decide("GET", "/x", "[v1.x]", secured).action() == http_target_origin_action::misdirected);

    // RFC 1034/1123 and RFC 9525 §6.2: leading dots, wildcards, multi-dot
    // suffixes, invalid LDH labels, and trailing-dot IPv4 literals are not
    // valid DNS reference identifiers.
    EXPECT_TRUE(decide("GET", "/x", ".example.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "%2Eexample.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "*.wild.test", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "Example.COM..", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "example..com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "-example.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "example-.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "192.0.2.7.", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "192.0.2.7%2E", secured).action() == http_target_origin_action::misdirected);
}
