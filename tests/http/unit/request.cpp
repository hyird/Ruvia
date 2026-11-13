#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "content_decoding_fixture.h"

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

namespace {

using ruvia::classify_http_method;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::is_valid_http_header_name;
using ruvia::is_valid_http_header_value;
using ruvia::is_valid_http_method_token;
using ruvia::is_valid_http_request_application_field;
using ruvia::is_valid_http_response_field;
using ruvia::known_http_method_token;

}  // namespace

// What bytes a field name or field value (including a reason phrase) may carry.

TEST(Ruvia, http_header_name_validation) {
    EXPECT_TRUE(is_valid_http_header_name("Content-Type"));
    EXPECT_TRUE(is_valid_http_header_name("X-Custom-Header"));
    EXPECT_TRUE(is_valid_http_header_name("a"));
    EXPECT_TRUE(!is_valid_http_header_name(""));                           // empty name is invalid
    EXPECT_TRUE(!is_valid_http_header_name("X Y"));                        // space is not a token char
    EXPECT_TRUE(!is_valid_http_header_name("X:Y"));                        // ':' is a separator
    EXPECT_TRUE(!is_valid_http_header_name("(comment)"));                  // '(' ')' are separators
    EXPECT_TRUE(!is_valid_http_header_name("a@b"));                        // '@' is a separator
    EXPECT_TRUE(!is_valid_http_header_name(std::string_view("a\rb", 3)));  // control char
}

TEST(Ruvia, http_header_value_rejects_injection) {
    EXPECT_TRUE(is_valid_http_header_value("text/html; charset=utf-8"));  // spaces allowed
    EXPECT_TRUE(is_valid_http_header_value("a b"));                       // inner spaces allowed
    EXPECT_TRUE(is_valid_http_header_value(""));                          // empty value is valid
    EXPECT_TRUE(!is_valid_http_header_value(" leading"));
    EXPECT_TRUE(!is_valid_http_header_value("trailing "));
    EXPECT_TRUE(!is_valid_http_header_value("\ttab"));
    EXPECT_TRUE(!is_valid_http_header_value("tab\t"));
    // CR, LF and NUL must be rejected -- these are response/header-injection bytes.
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\rb", 3)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\nb", 3)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\r\nInjected: x", 14)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\0b", 3)));
}

TEST(Ruvia, http_outbound_field_checks_normalized_syntax) {
    for (const std::string_view name : {"", "X Y", "X:Y", ":authority", "X-Name ", "\tX-Name"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field(name, "value"));
        EXPECT_TRUE(!is_valid_http_response_field(name, "value"));
    }
    EXPECT_TRUE(!is_valid_http_request_application_field(std::string_view("X\0Name", 6), "value"));
    EXPECT_TRUE(!is_valid_http_response_field(std::string_view("X\0Name", 6), "value"));
    for (const std::string_view value : {" leading", "trailing ", "\tleading", "trailing\t", " ", "\t"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field("X-Extension", value));
        EXPECT_TRUE(!is_valid_http_response_field("X-Extension", value));
    }
    // 同时覆盖短值与长值：内部 HTAB、SP、可打印 ASCII 和 obs-text 合法，
    // NUL、CR/LF、其余控制字节及 DEL 不得进入应用发送的字段值。
    for (const std::size_t size : {std::size_t{3}, std::size_t{40}}) {
        std::string value(size, 'a');
        for (unsigned byte = 0; byte < 256; ++byte) {
            value[size / 2] = static_cast<char>(byte);
            const bool allowed = byte == '\t' || (byte >= 0x20 && byte != 0x7f);
            EXPECT_EQ(is_valid_http_request_application_field("X-Extension", value), allowed);
            EXPECT_EQ(is_valid_http_response_field("X-Extension", value), allowed);
        }
    }
    EXPECT_TRUE(is_valid_http_request_application_field("X-Extension", ""));
    EXPECT_TRUE(is_valid_http_response_field("X-Extension", ""));
    EXPECT_TRUE(is_valid_http_request_application_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
    EXPECT_TRUE(is_valid_http_response_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
}

TEST(Ruvia, http_request_application_field_rejects_protocol_owned_names) {
    for (const std::string_view name : {"Host", "hOsT", "Content-Length", "cOnTeNt-LeNgTh",
             "Transfer-Encoding", "tRaNsFeR-EnCoDiNg", "Trailer", "tRaIlEr",
             "Expect", "eXpEcT", "Connection", "cOnNeCtIoN", "Keep-Alive", "kEeP-aLiVe",
             "Proxy-Connection", "pRoXy-CoNnEcTiOn", "TE", "tE", "Upgrade", "uPgRaDe"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field(name, "value"));
        EXPECT_TRUE(!is_valid_http_request_application_field(name, ""));
    }
    // 即使协议字段值本身合法，也不能从应用首部入口注入。
    const ruvia::http_header_view managed[] = {
        {"Host", "example.test"},
        {"Content-Length", "0"},
        {"Transfer-Encoding", "chunked"},
        {"Trailer", "Digest"},
        {"Expect", "100-continue"},
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Upgrade", "websocket"},
    };
    for (const auto& field : managed) {
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
    EXPECT_TRUE(is_valid_http_request_application_field("X-Host", "example.test"));
    EXPECT_TRUE(is_valid_http_request_application_field("Expect-Extension", "100-continue"));
}

TEST(Ruvia, http_request_application_field_uses_known_sender_semantics) {
    const ruvia::http_header_view invalid[] = {
        {"Content-Type", ""},
        {"cOnTeNt-TyPe", "text plain"},
        {"Content-Type", "*/plain"},
        {"Content-Type", "text/plain; charset"},
        {"Content-Type", "text/plain; charset="},
        {"Content-Type", "text/plain; charset =utf-8"},
        {"Content-Type", "text/plain; charset=utf-8; CHARSET=latin1"},
        {"Content-Type", "text/plain; charset=\"unfinished"},
        {"Origin", ""},
        {"oRiGiN", "https://example.test/path"},
        {"Origin", "null https://example.test"},
        {"Origin", "https://first.test\thttps://second.test"},
        {"Access-Control-Request-Method", ""},
        {"aCcEsS-cOnTrOl-ReQuEsT-mEtHoD", "GET, POST"},
        {"Access-Control-Request-Headers", ""},
        {"aCcEsS-cOnTrOl-ReQuEsT-hEaDeRs", "X-Good, Bad Header"},
        {"Content-Encoding", ""},
        {"cOnTeNt-EnCoDiNg", "gzip;level=9"},
        {"Content-Encoding", ",gzip"},
        {"Content-Encoding", "gzip,"},
    };
    for (const auto& field : invalid) {
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
    const ruvia::http_header_view valid[] = {
        {"Content-Type", "text/plain; charset=\"utf-8\""},
        {"content-type", "application/x-example"},
        {"Content-Type", "application/example; extension=\"\""},
        {"Origin", "null"},
        {"origin", "https://first.test https://second.test"},
        {"Access-Control-Request-Method", "CUSTOM"},
        {"Access-Control-Request-Headers", "X-First, X-Second"},
        {"Content-Encoding", "gzip, br"},
        {"content-encoding", "example-extension"},
        {"X-Extension", "not a media type"},
    };
    for (const auto& field : valid) {
        EXPECT_TRUE(is_valid_http_request_application_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_response_field_uses_known_sender_semantics) {
    const ruvia::http_header_view invalid[] = {
        {"Content-Type", ""},
        {"cOnTeNt-TyPe", "text plain"},
        {"Content-Type", "*/plain"},
        {"Content-Type", "text/plain; charset="},
        {"Content-Type", "text/plain; charset=utf-8; CHARSET=latin1"},
        {"Content-Type", "text/plain; charset=\"unfinished"},
        {"Content-Encoding", ""},
        {"cOnTeNt-EnCoDiNg", "gzip;level=9"},
        {"Content-Encoding", ",gzip"},
        {"Content-Encoding", "gzip,"},
        {"Trailer", "Content-Length"},
        {"tRaIlEr", "bad name"},
    };
    for (const auto& field : invalid) {
        EXPECT_TRUE(!is_valid_http_response_field(field.name(), field.value()));
    }
    const ruvia::http_header_view valid[] = {
        {"Content-Type", "text/plain; charset=\"utf-8\""},
        {"content-type", "application/x-example"},
        {"Content-Type", "application/example; extension=\"\""},
        {"Content-Encoding", "gzip, br"},
        {"content-encoding", "example-extension"},
        {"Trailer", "ETag, X-Checksum"},
        {"X-Extension", "not a media type"},
    };
    for (const auto& field : valid) {
        EXPECT_TRUE(is_valid_http_response_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_response_field_does_not_apply_request_admission_rules) {
    const ruvia::http_header_view fields[] = {
        {"Origin", "https://example.test/path"},
        {"Access-Control-Request-Method", "GET, POST"},
        {"Access-Control-Request-Headers", "X-Good, Bad Header"},
        {"Expect", "extension"},
        {"Origin", ""},
        {"Content-Length", "0"},
        {"Connection", "close"},
        {"Upgrade", "websocket"},
    };
    for (const auto& field : fields) {
        EXPECT_TRUE(is_valid_http_response_field(field.name(), field.value()));
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_parse_error_messages_and_codes) {
    const auto missing_host = http_parse_protocol_error(http_parse_error::missing_host);
    const auto header_too_large = http_parse_protocol_error(http_parse_error::header_too_large);
    const auto unsupported_version = http_parse_protocol_error(http_parse_error::unsupported_http_version);
    EXPECT_EQ(std::string_view(missing_host.what()), std::string_view("missing Host header"));
    EXPECT_EQ(missing_host.code(), std::string_view("missing_host"));
    EXPECT_EQ(std::string_view(header_too_large.what()), std::string_view("request header is too large"));
    EXPECT_EQ(header_too_large.code(), std::string_view("header_too_large"));
    EXPECT_EQ(std::string_view(unsupported_version.what()), std::string_view("unsupported HTTP version"));
    EXPECT_EQ(unsupported_version.code(), std::string_view("unsupported_http_version"));
    // The two Content-Length faults carry distinct machine codes.
    const auto invalid_length = http_parse_protocol_error(http_parse_error::invalid_content_length);
    const auto conflicting_length =
        http_parse_protocol_error(http_parse_error::conflicting_content_length);
    EXPECT_EQ(invalid_length.code(), std::string_view("invalid_content_length"));
    EXPECT_EQ(conflicting_length.code(), std::string_view("conflicting_content_length"));
    EXPECT_TRUE(invalid_length.status() == conflicting_length.status());
    // Reachable errors all map to a non-empty message.
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_transfer_encoding).what())
                    .size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_connection).what())
                    .size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_upgrade).what()).size() !=
                0);
}

TEST(Ruvia, http_field_ows_trims_only_space_and_tab) {
    EXPECT_EQ(ruvia::http_trim_ows(" \t text/plain \t"), std::string_view("text/plain"));
    EXPECT_EQ(ruvia::http_trim_ows("\ntext/plain\n"), std::string_view("\ntext/plain\n"));
}

TEST(Ruvia, http_serialized_origin_rejects_opaque_origin_and_origin_lists) {
    // A field can contain null or several origins; neither is a single
    // serialized origin usable in configuration or connection advertisements.
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("null"));
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("https://example.com http://other.example"));
}

TEST(Ruvia, http_field_name_list_preserves_fields_and_skips_empty_members) {
    ruvia::http_field_name_list names(" , X-One,\tX-Two\t, ");
    EXPECT_EQ(names.next().value(), std::string_view("X-One"));
    EXPECT_EQ(names.next().value(), std::string_view("X-Two"));
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_borrows_input_and_keeps_terminal_state) {
    constexpr std::string_view field = "\tX-One , X-Two";
    ruvia::http_field_name_list names(field);
    EXPECT_TRUE(!names.valid());
    const auto first = names.next();
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(first->data(), field.data() + 1);
    EXPECT_TRUE(!names.valid());
    const auto second = names.next();
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(second->data(), field.data() + 9);
    EXPECT_TRUE(names.valid());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_accepts_empty_lists_and_rejects_invalid_names) {
    ruvia::http_field_name_list empty(" , \t, ");
    EXPECT_TRUE(!empty.next());
    EXPECT_TRUE(empty.valid());

    ruvia::http_field_name_list invalid("X-Ok, bad name");
    EXPECT_EQ(invalid.next().value(), std::string_view("X-Ok"));
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
}

namespace {

using ruvia::http_media_type_only;

}  // namespace

TEST(Ruvia, http_media_type_only_strips_parameters_and_ows) {
    EXPECT_EQ(http_media_type_only("application/json"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json ; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only(""), std::string_view(""));
}

TEST(Ruvia, http_content_type_field_validation_checks_syntax) {
    EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value("text/plain; charset=utf-8"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value("text/plain; charset="));
}

TEST(Ruvia, http_content_type_allows_empty_parameter_slots) {
    for (const std::string_view field : {
             "text/plain;",
             "text/plain;;",
             "text/plain; \t; charset=utf-8; ;",
             R"(text/plain;;note="a;b";;charset="utf-8";)",
             R"(text/plain;;note="";)",
             "text/plain;q=invalid",
             R"(text/plain;q="0.5")"}) {
        EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(field));
    }
}

TEST(Ruvia, http_content_type_empty_slots_do_not_relax_nonempty_parameter_syntax) {
    for (const std::string_view field : {
             "text/plain;;charset;",
             "text/plain;;charset=;",
             "text/plain;;=utf-8;",
             "text/plain;;charset =utf-8;",
             "text/plain;;charset= utf-8;",
             "text/plain;;charset=utf-8;;CHARSET=utf-8;",
             "text/plain;;note=\"unterminated;",
             R"(text/plain;note="closed"suffix)",
             "text/plain;note=a/b"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(field));
    }
}

namespace {

std::string format_date(std::time_t time) {
    const auto out = ruvia::format_http_date(time).value();
    return std::string(out.data(), out.size());
}

}  // namespace

TEST(Ruvia, http_format_date_known_vectors) {
    EXPECT_EQ(format_date(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    EXPECT_EQ(format_date(172800), std::string("Sat, 03 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(345600), std::string("Mon, 05 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(432000), std::string("Tue, 06 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(518400), std::string("Wed, 07 Jan 1970 00:00:00 GMT"));
}

TEST(Ruvia, http_format_date_round_trips_through_public_parser) {
    for (const auto time : std::array<std::int64_t, 5>{
             -315619200, -1, 0, 784111777, 253402300799}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto parsed = ruvia::parse_http_date(format_date(static_cast<std::time_t>(time)));
        EXPECT_TRUE(parsed.has_value());
        EXPECT_EQ(static_cast<std::int64_t>(*parsed), time);
    }
}

TEST(Ruvia, http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL},
             std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        EXPECT_TRUE(!ruvia::format_http_date(static_cast<std::time_t>(time)));
    }
    EXPECT_EQ(format_date(-315619200), std::string("Fri, 01 Jan 1960 00:00:00 GMT"));
    EXPECT_EQ(format_date(-1), std::string("Wed, 31 Dec 1969 23:59:59 GMT"));
    if (std::in_range<std::time_t>(253402300799LL)) {
        EXPECT_EQ(format_date(static_cast<std::time_t>(253402300799LL)), std::string("Fri, 31 Dec 9999 23:59:59 GMT"));
    }
}

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;

}  // namespace

// The Expect field and what a client that sends it must follow with.

TEST(Ruvia, client_expectation_requires_following_content) {
    EXPECT_TRUE(http_client_expectation_is_valid(false, http_request_content_indication::no_content));
    EXPECT_TRUE(!http_client_expectation_is_valid(true, http_request_content_indication::no_content));
    EXPECT_TRUE(http_client_expectation_is_valid(true, http_request_content_indication::will_follow));
}

TEST(Ruvia, expectations_parse_one_logical_recipient_list) {
    http_request_expectations expectations;
    expectations.parse_field(" , 100-continue, , 100-Continue, ");
    expectations.parse_field(" 100-CONTINUE ");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(!expectations.has_unsupported());
    const auto no_content = expectations.server_plan(
        http_request_content_indication::no_content, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(no_content == ruvia::http_server_expectation_plan::no_action);
    const auto with_content = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(with_content == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, expectations_preserve_unsupported_extensions_as_semantics) {
    http_request_expectations expectations;
    expectations.parse_field("100-continue");
    expectations.parse_field(R"(custom="a,b")");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
    const auto rejected = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_EQ(ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
    const auto ignored = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::ignore);
    EXPECT_TRUE(ignored == ruvia::http_server_expectation_plan::send_continue);

    expectations.ignore_continue();
    EXPECT_TRUE(!expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
}

namespace {

using ruvia::classify_http_method;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::is_valid_http_header_name;
using ruvia::is_valid_http_header_value;
using ruvia::is_valid_http_method_token;
using ruvia::known_http_method_token;

}  // namespace

// The method vocabulary: recognising tokens, spelling them back, and the
// safe and idempotent properties a recipient acts on.

TEST(Ruvia, http_request_method_content_semantics_are_shared_by_client_and_server) {
    using ruvia::classify_http_request_content_semantics;
    using ruvia::http_request_content_semantics;
    EXPECT_TRUE(classify_http_request_content_semantics("CONNECT") ==
                http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("TRACE") ==
                http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("OPTIONS") ==
                http_request_content_semantics::content_type_required);
    EXPECT_TRUE(classify_http_request_content_semantics("POST") ==
                http_request_content_semantics::no_additional_requirements);
}

TEST(Ruvia, http_method_parsing_is_exact_and_case_sensitive) {
    EXPECT_TRUE(classify_http_method("GET") == http_known_method::get);
    EXPECT_TRUE(classify_http_method("POST") == http_known_method::post);
    EXPECT_TRUE(classify_http_method("PUT") == http_known_method::put);
    EXPECT_TRUE(classify_http_method("DELETE") == http_known_method::delete_value);
    EXPECT_TRUE(classify_http_method("PATCH") == http_known_method::patch);
    EXPECT_TRUE(classify_http_method("HEAD") == http_known_method::head);
    EXPECT_TRUE(classify_http_method("OPTIONS") == http_known_method::options);
    EXPECT_TRUE(classify_http_method("CONNECT") == http_known_method::connect);
    // Methods are case-sensitive (RFC 9110 section 9.1); anything outside the
    // framework's fixed semantic set remains an unknown classification.
    EXPECT_TRUE(classify_http_method("get") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("Get") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("FOO") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("GETX") == http_known_method::unknown);
}

TEST(Ruvia, http_method_token_validation_is_separate_from_classification) {
    EXPECT_TRUE(is_valid_http_method_token("PROPFIND"));
    EXPECT_TRUE(is_valid_http_method_token("get"));
    EXPECT_TRUE(is_valid_http_method_token("M-SEARCH"));
    EXPECT_TRUE(!is_valid_http_method_token(""));
    EXPECT_TRUE(!is_valid_http_method_token("BAD METHOD"));
    EXPECT_TRUE(!is_valid_http_method_token("BAD(METHOD"));
    EXPECT_TRUE(!is_valid_http_method_token(std::string_view("BAD\x01METHOD", 10)));
}

TEST(Ruvia, http_method_safety_and_idempotency_follow_wire_semantics) {
    for (const std::string_view method : {"GET", "HEAD", "OPTIONS", "TRACE"}) {
        EXPECT_TRUE(ruvia::is_http_method_safe(method));
        EXPECT_TRUE(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"PUT", "DELETE"}) {
        EXPECT_TRUE(!ruvia::is_http_method_safe(method));
        EXPECT_TRUE(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"POST", "PATCH", "CONNECT", "CUSTOM", "get"}) {
        EXPECT_TRUE(!ruvia::is_http_method_safe(method));
        EXPECT_TRUE(!ruvia::is_http_method_idempotent(method));
    }
}

TEST(Ruvia, http_known_method_token_round_trips) {
    const http_known_method methods[] = {http_known_method::get, http_known_method::post,
        http_known_method::put, http_known_method::delete_value, http_known_method::patch,
        http_known_method::head, http_known_method::options, http_known_method::connect};
    for (const auto method : methods) {
        EXPECT_TRUE(classify_http_method(known_http_method_token(method)) == method);
    }
    // An unknown classification has no canonical wire spelling. Callers that
    // need it must retain the exact token instead of manufacturing "UNKNOWN".
    EXPECT_TRUE(known_http_method_token(http_known_method::unknown).empty());
}

// A quoted-string in a field value is opaque: a delimiter inside it never splits the field.

TEST(Ruvia, public_quoted_field_visitors_preserve_quoted_delimiters) {
    std::string_view parameter_name;
    std::string_view parameter_value;
    ruvia::http_visit_semicolon_parameters_quoted_field(
        R"(for="[2001:db8::1];zone"; proto=https)",
        [&](std::string_view name, std::string_view value) {
            parameter_name = name;
            parameter_value = ruvia::http_trim_quoted_field_value(value);
            return true;
        });
    EXPECT_EQ(parameter_name, std::string_view("proto"));
    EXPECT_EQ(parameter_value, std::string_view("https"));

    std::size_t item_count = 0;
    ruvia::http_visit_comma_separated_quoted_field_items(
        R"(for="192.0.2.1,192.0.2.2", proto=https)",
        [&](std::string_view) {
            ++item_count;
            return true;
        });
    EXPECT_EQ(item_count, std::size_t{2});
}

TEST(Ruvia, public_quoted_string_cursor_validates_and_decodes) {
    std::string decoded;
    ruvia::http_quoted_string_cursor cursor(R"("a\"b\\c";rest)");
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        decoded.push_back(static_cast<char>(byte));
    }
    EXPECT_TRUE(cursor.closed());
    EXPECT_EQ(decoded, std::string_view(R"(a"b\c)"));
    EXPECT_EQ(cursor.consumed(), std::size_t{9});

    EXPECT_EQ(ruvia::http_quoted_string_size(R"("x" tail)"), std::size_t{3});
    EXPECT_TRUE(ruvia::http_is_quoted_string(R"("")"));
    EXPECT_TRUE(ruvia::http_is_quoted_string("\"\t obs\x80\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("token"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("open)"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("a"b")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("trailing\")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"ctl\x01\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"del\x7f\""));

    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(a="x,\",y",b)", 0, ','), std::size_t{10});
}

TEST(Ruvia, public_unquoted_delimiter_preserves_quote_state_before_start) {
    constexpr std::string_view field = R"(p="a;b";q=x)";
    for (std::size_t start = 0; start <= 7; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, start, ';'), std::size_t{7});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, 8, ';'), field.size());
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, field.size() + 1, ';'), field.size());

    constexpr std::string_view escaped = R"(p="a\";b";q=x)";
    for (std::size_t start = 4; start <= 9; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(escaped, start, ';'), std::size_t{9});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(p="a;b)", 4, ';'), std::size_t{6});
}

TEST(Ruvia, accept_encoding_quality_unquoted_unchanged) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(http_accepts_encoding("", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("", "gzip"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "br"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "gzip"));
    EXPECT_TRUE(!http_accepts_encoding("gzip;q=0", "gzip"));
}

TEST(Ruvia, accept_quality_quoted_comma_does_not_split_item) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(!http_accepts_encoding(R"(gzip;note="a,b";q=0)", "gzip"));
}

static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, const std::string&>);
static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string>);

TEST(Ruvia, quoted_field_members_keep_escaped_quotes_delimiters_and_empty_items) {
    constexpr std::string_view input = R"( first , "second,\"still quoted",, last, )";
    std::vector<std::string_view> items;
    ruvia::http_visit_comma_separated_quoted_field_items(input, [&](std::string_view item) {
        items.push_back(item);
        return true;
    });
    EXPECT_EQ(items, (std::vector<std::string_view>{"first", R"("second,\"still quoted")", "", "last", ""}));
    EXPECT_EQ(items.front().data(), input.data() + 1);
    int empty_visits{};
    ruvia::http_visit_comma_separated_quoted_field_items({}, [&](std::string_view item) {
        ++empty_visits;
        EXPECT_TRUE(item.empty());
        return true;
    });
    EXPECT_EQ(empty_visits, 1);
}

TEST(Ruvia, quoted_field_parameters_preserve_quotes_and_skip_non_parameters) {
    constexpr std::string_view input = R"( text/plain ; name = "a;\"still quoted" ; ignored ; empty= ;=value; )";
    std::vector<std::pair<std::string_view, std::string_view>> parameters;
    ruvia::http_visit_semicolon_parameters_quoted_field(input, [&](std::string_view name, std::string_view value) {
        parameters.emplace_back(name, value);
        return true;
    });
    EXPECT_EQ(parameters, (std::vector<std::pair<std::string_view, std::string_view>>{
                              {"name", R"("a;\"still quoted")"}, {"empty", ""}, {"", "value"}}));
}

TEST(Ruvia, quoted_field_visitors_stop_without_visiting_remaining_segments) {
    std::vector<std::string_view> members;
    ruvia::http_visit_comma_separated_quoted_field_items("first,second,third", [&](std::string_view item) {
        members.push_back(item);
        return members.size() != 2;
    });
    EXPECT_EQ(members, (std::vector<std::string_view>{"first", "second"}));
    std::vector<std::string_view> names;
    ruvia::http_visit_semicolon_parameters_quoted_field("ignored;a=1;b=2;c=3", [&](std::string_view name, std::string_view) {
        names.push_back(name);
        return false;
    });
    EXPECT_EQ(names, (std::vector<std::string_view>{"a"}));
}
