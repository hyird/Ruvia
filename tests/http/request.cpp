#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_decoding_fixture.h"
#include "http_cache.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_server.h"
#include "test_exceptions.h"

namespace {

// Appends the trailer section that ends the chunked `wire` the way an HTTP/1
// server receives it.
std::optional<ruvia::http_request_trailer_error> append_received_http1(ruvia::http_request_trailers& trailers, std::string_view wire) {
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
    EXPECT_TRUE(!append_received_http1(trailers, "0\r\nX-Second: 2\r\nContent-Length: 3\r\nCache-Control: no-cache\r\n\r\n").has_value());
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
    const std::array oversized{ruvia::http_header("x-fill", "v"), ruvia::http_header("x-oversize", std::string(ruvia::max_http_header_bytes, 'x'))};
    EXPECT_TRUE(trailers.append_received(oversized) == ruvia::http_request_trailer_error::section_too_large);
    EXPECT_EQ(trailers.fields().size(), std::size_t{3});
    // X-First used 12 normalized bytes, x-second 13, x-third 12, the discarded
    // content-length 19 and the terminating empty line 2; the remaining budget
    // is intact only if the failed sections rolled back.
    EXPECT_TRUE(trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 67, 'z')) == ruvia::http_request_trailer_error::section_too_large);
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
        const auto second_value = std::string(ruvia::max_http_header_bytes - 2 * (3 + 4) - 2 - first_value.size() + excess, 'b');
        ruvia::http_request_trailers received;
        EXPECT_TRUE(!received.append("X-A", first_value).has_value());
        const auto appended = received.append("X-A", second_value);
        const std::array forwarded{ruvia::http_header_view{"X-A", first_value}, ruvia::http_header_view{"X-A", second_value}};
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
    const auto decoded = decoder.decode("0\r\nContent-Type: text/plain\r\nX-Sum: abc\r\nContent-Length: 5\r\nETag: \"v\"\r\n\r\n");
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
    auto request = ruvia::make_parsed_http_request("GET", "https://example.test/path?key=value", headers, {});
    {
        const http_header_view conflicting_host[]{{"Host", "conflicting.example"}};
        auto rebound_req = ruvia::make_parsed_http_request("GET", "https://example.test/path", conflicting_host, {});
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
    const auto request = ruvia::make_parsed_http_request("GET", "/", headers, {});
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
    for (const auto headers : {std::span<const http_header_view>(duplicate_range), std::span<const http_header_view>(duplicate_if_range), std::span<const http_header_view>(duplicate_host)}) {
        bool rejected = false;
        try {
            (void)ruvia::make_parsed_http_request("GET", "/", headers, {});
        } catch (const ruvia::http_protocol_error& error) {
            rejected = error.status() == ruvia::http_status::bad_request;
        }
        EXPECT_TRUE(rejected);
    }

    const http_header_view repeated_list[]{{"Accept", "text/plain"}, {"Accept", "text/html"}, {"User-Agent", "first/1"}, {"User-Agent", "second/2"}};
    const auto request = ruvia::make_parsed_http_request("GET", "/", repeated_list, {});
    EXPECT_EQ(request.headers().size(), std::size_t{4});
}

TEST(Ruvia, request_content_coding_accumulates_repeated_header_fields_in_order) {
    const http_header_view headers[]{{"Content-Encoding", "br"}, {"Content-Encoding", "gzip"}};
    const auto request = ruvia::make_parsed_http_request("POST", "/", headers, {});
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
    const auto request = ruvia::make_parsed_http_request("POST", "/", headers, {});

    const auto coding = request_content_coding(request);
    EXPECT_TRUE(coding.invalid() == nullptr);
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == http_content_coding::gzip);
    }
}

TEST(Ruvia, request_access_raw_query_lookup_preserves_encoding_and_uses_last_match) {
    const auto request = ruvia::make_parsed_http_request("GET", "/?a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue", {}, {});

    const auto value = request.last_raw_query_value("a");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second+value"));
    EXPECT_TRUE(!request.last_raw_query_value("encoded key").has_value());
    EXPECT_EQ(*request.last_raw_query_value("encoded%20key"), std::string_view("raw%2Fvalue"));
}

TEST(Ruvia, request_access_cookie_lookup_uses_last_match) {
    const http_header_view headers[]{{"Cookie", "sid=first; theme=dark; sid=second"}};
    const auto request = ruvia::make_parsed_http_request("GET", "/", headers, {});

    const auto value = request.cookie("sid");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second"));
}

TEST(Ruvia, request_access_cookie_lookup_scans_repeated_cookie_fields) {
    const http_header_view headers[]{{"Cookie", "a=1"}, {"Cookie", "b=2"}};
    const auto request = ruvia::make_parsed_http_request("GET", "/", headers, {});

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
    const auto request = ruvia::make_parsed_http_request("GET", "/", headers, {});
    EXPECT_EQ(request.headers().size(), std::size_t{2});
    EXPECT_EQ(request.header("Cookie").value_or("missing"), std::string_view("sid=old"));
    EXPECT_EQ(request.cookie("sid").value_or("missing"), std::string_view("old"));
    EXPECT_EQ(request.cookie("token").value_or("missing"), std::string_view("present"));
    EXPECT_TRUE(!request.cookie("absent").has_value());
}

TEST(Ruvia, request_body_string_view_and_known_method_bits_match_contracts) {
    const std::array<std::byte, 4> payload{std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    const auto request = ruvia::make_parsed_http_request("POST", "/", {}, payload);
    EXPECT_EQ(request.body(), std::string_view("data"));
    EXPECT_EQ(request.body_bytes().size(), std::size_t{4});
    static_assert(ruvia::http_known_method_bit(http_known_method::get) == 1U);
    static_assert(ruvia::http_known_method_bit(http_known_method::unknown) == 0U);
}

namespace {

template <typename method_type, typename target_type, typename body_type>
concept make_parsed_request_accepts = requires(method_type&& method, target_type&& target, body_type&& body) { ruvia::make_parsed_http_request(std::forward<method_type>(method), std::forward<target_type>(target), std::span<const ruvia::http_header_view>{}, std::forward<body_type>(body)); };

static_assert(make_parsed_request_accepts<const std::string&, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<std::string, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, std::string, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::vector<std::byte>>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::array<std::byte, 1>>);

template <typename request_type>
concept request_allows_body = requires(request_type&& req) { std::forward<request_type>(req).body(); };
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

[[nodiscard]] http_target_origin decide(std::string_view method, std::string_view target, std::string_view host, http_target_origin_connection connection) {
    const std::array headers{ruvia::http_header_view("Host", host)};
    const auto request = ruvia::make_parsed_http_request(method, target, headers, {});
    return ruvia::decide_http_target_origin(request, connection);
}

template <typename origin_type>
concept target_origin_allows_host = requires(origin_type&& origin) { std::forward<origin_type>(origin).host(); };

template <typename origin_type>
concept target_origin_allows_address = requires(origin_type&& origin) { std::forward<origin_type>(origin).address(); };

static_assert(target_origin_allows_host<const http_target_origin&>);
static_assert(!target_origin_allows_host<http_target_origin>);
static_assert(target_origin_allows_address<const http_target_origin&>);
static_assert(!target_origin_allows_address<http_target_origin>);
}  // namespace

TEST(Ruvia, target_origin_dns_reference_names_follow_label_and_full_name_boundaries) {
    const auto maximal = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    for (const std::string_view host : {std::string_view(maximal), std::string_view("7.example"), std::string_view("xn--bcher-kva.example")}) {
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
    for (const std::string_view name : {"Host", "hOsT", "Content-Length", "cOnTeNt-LeNgTh", "Transfer-Encoding", "tRaNsFeR-EnCoDiNg", "Trailer", "tRaIlEr", "Expect", "eXpEcT", "Connection", "cOnNeCtIoN", "Keep-Alive", "kEeP-aLiVe", "Proxy-Connection", "pRoXy-CoNnEcTiOn", "TE", "tE", "Upgrade", "uPgRaDe"}) {
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
    const auto conflicting_length = http_parse_protocol_error(http_parse_error::conflicting_content_length);
    EXPECT_EQ(invalid_length.code(), std::string_view("invalid_content_length"));
    EXPECT_EQ(conflicting_length.code(), std::string_view("conflicting_content_length"));
    EXPECT_TRUE(invalid_length.status() == conflicting_length.status());
    // Reachable errors all map to a non-empty message.
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_transfer_encoding).what()).size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_connection).what()).size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_upgrade).what()).size() != 0);
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
    for (const std::string_view field : {"text/plain;", "text/plain;;", "text/plain; \t; charset=utf-8; ;", R"(text/plain;;note="a;b";;charset="utf-8";)", R"(text/plain;;note="";)", "text/plain;q=invalid", R"(text/plain;q="0.5")"}) {
        EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(field));
    }
}

TEST(Ruvia, http_content_type_empty_slots_do_not_relax_nonempty_parameter_syntax) {
    for (const std::string_view field : {"text/plain;;charset;", "text/plain;;charset=;", "text/plain;;=utf-8;", "text/plain;;charset =utf-8;", "text/plain;;charset= utf-8;", "text/plain;;charset=utf-8;;CHARSET=utf-8;", "text/plain;;note=\"unterminated;", R"(text/plain;note="closed"suffix)", "text/plain;note=a/b"}) {
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
    for (const auto time : std::array<std::int64_t, 5>{-315619200, -1, 0, 784111777, 253402300799}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto parsed = ruvia::parse_http_date(format_date(static_cast<std::time_t>(time)));
        EXPECT_TRUE(parsed.has_value());
        EXPECT_EQ(static_cast<std::int64_t>(*parsed), time);
    }
}

TEST(Ruvia, http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL}, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
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
    const auto no_content = expectations.server_plan(http_request_content_indication::no_content, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(no_content == ruvia::http_server_expectation_plan::no_action);
    const auto with_content = expectations.server_plan(http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(with_content == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, expectations_preserve_unsupported_extensions_as_semantics) {
    http_request_expectations expectations;
    expectations.parse_field("100-continue");
    expectations.parse_field(R"(custom="a,b")");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
    const auto rejected = expectations.server_plan(http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_EQ(ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
    const auto ignored = expectations.server_plan(http_request_content_indication::will_follow, http_unsupported_expectation_policy::ignore);
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
    EXPECT_TRUE(classify_http_request_content_semantics("CONNECT") == http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("TRACE") == http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("OPTIONS") == http_request_content_semantics::content_type_required);
    EXPECT_TRUE(classify_http_request_content_semantics("POST") == http_request_content_semantics::no_additional_requirements);
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
    const http_known_method methods[] = {http_known_method::get, http_known_method::post, http_known_method::put, http_known_method::delete_value, http_known_method::patch, http_known_method::head, http_known_method::options, http_known_method::connect};
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
    ruvia::http_visit_semicolon_parameters_quoted_field(R"(for="[2001:db8::1];zone"; proto=https)", [&](std::string_view name, std::string_view value) {
        parameter_name = name;
        parameter_value = ruvia::http_trim_quoted_field_value(value);
        return true;
    });
    EXPECT_EQ(parameter_name, std::string_view("proto"));
    EXPECT_EQ(parameter_value, std::string_view("https"));

    std::size_t item_count = 0;
    ruvia::http_visit_comma_separated_quoted_field_items(R"(for="192.0.2.1,192.0.2.2", proto=https)", [&](std::string_view) {
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
    EXPECT_EQ(parameters, (std::vector<std::pair<std::string_view, std::string_view>>{{"name", R"("a;\"still quoted")"}, {"empty", ""}, {"", "value"}}));
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

namespace {

std::string media_type_with_parameters(int count) {
    std::string value = "text/plain";
    for (int index = 0; index < count; ++index) {
        value += std::string(";p") + std::to_string(index).c_str() + "=\"a;b\"";
    }
    return value;
}

}  // namespace

TEST(Ruvia, http_media_type_parameters_up_to_the_cap_are_matched) {
    const auto full = media_type_with_parameters(64);
    EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(full));

    ruvia::http_accept_match wildcard;
    wildcard.update_media_type("*/*", full);
    EXPECT_EQ(wildcard.quality(), 1000);

    ruvia::http_accept_match tail;
    tail.update_media_type("text/plain;p63=\"a;b\";q=0.4", full);
    EXPECT_EQ(tail.quality(), 400);

    // One slot below the cap leaves room for q in the media range.
    const auto offered = media_type_with_parameters(63);
    ruvia::http_accept_match specific;
    specific.update_media_type("text/plain;q=0.9", offered);
    specific.update_media_type(offered + ";q=0.2", offered);
    EXPECT_EQ(specific.quality(), 200);

    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";p62=v"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";P0=v"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered + ";broken=\"unterminated"));

    ruvia::http_accept_match malformed;
    malformed.update_media_type("text/*;q=0.7", offered);
    malformed.update_media_type(offered + ";broken=", offered);
    EXPECT_EQ(malformed.quality(), 700);
}

TEST(Ruvia, http_media_type_parameters_beyond_the_cap_are_invalid) {
    for (const int count : {65, 512}) {
        const auto offered = media_type_with_parameters(count);
        EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(offered));

        ruvia::http_accept_match wildcard;
        wildcard.update_media_type("*/*", offered);
        EXPECT_TRUE(!wildcard.matched());
    }

    // q counts toward the cap: the over-long range is ignored, the shorter one applies.
    const auto full = media_type_with_parameters(64);
    ruvia::http_accept_match range;
    range.update_media_type("text/*;q=0.3", full);
    range.update_media_type(full + ";q=0.9", full);
    EXPECT_EQ(range.quality(), 300);
}

TEST(Ruvia, http_accept_match_accumulates_fields_and_respects_specific_exclusion) {
    ruvia::http_accept_match media;
    media.update_media_type("text/*;q=0.8", "text/html");
    EXPECT_TRUE(media.matched());
    EXPECT_EQ(media.quality(), 800);
    media.update_media_type("text/html;q=0", "text/html");
    EXPECT_TRUE(!media.matched());
    EXPECT_EQ(media.quality(), 0);

    ruvia::http_accept_match language;
    language.update_token("en;q=0.5", "en-US", ruvia::http_accept_token_match_mode::language_prefix);
    EXPECT_TRUE(language.matched());
    EXPECT_EQ(language.quality(), 500);
}

TEST(Ruvia, http_accept_match_language_uses_longest_matching_range) {
    constexpr auto mode = ruvia::http_accept_token_match_mode::language_prefix;
    struct language_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr language_case cases[] = {
        {"zh;q=1, zh-Hant;q=0", 0},
        {"zh-Hant;q=0, zh;q=1", 0},
        {"zh;q=1, zh-Hant;q=0.6", 600},
        {"zh-Hant;q=0.6, zh;q=1", 600},
        {"zh;q=0, ZH-hant;q=0.6", 600},
        {"*;q=1, zh-Hant;q=0", 0},
        {"zh-Hant;q=0.3, zh-Hant;q=0.8", 800},
        {"zh-Hant;q=0.8, zh-Hant;q=0.3", 800},
        {"zh-Hant;q=1, zh-Hant-TW;q=0", 0},
        {"zh-Hant;q=0, zh-Hant-TW;q=0.4", 400},
    };
    for (const auto& item : cases) {
        ruvia::http_accept_match match;
        match.update_token(item.field_, "zh-Hant-TW", mode);
        EXPECT_EQ(match.quality(), item.quality_);
        EXPECT_EQ(match.matched(), item.quality_ > 0);
    }
}

TEST(Ruvia, http_accept_match_language_combines_field_lines) {
    constexpr auto mode = ruvia::http_accept_token_match_mode::language_prefix;
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match split;
        split.update_token(specific_first ? "de-DE;q=0" : "de;q=1", "de-DE-1996", mode);
        split.update_token(specific_first ? "de;q=1" : "de-DE;q=0", "de-DE-1996", mode);
        ruvia::http_accept_match joined;
        joined.update_token("de;q=1, de-DE;q=0", "de-DE-1996", mode);
        EXPECT_EQ(split.quality(), joined.quality());
        EXPECT_TRUE(!split.matched());
    }

    ruvia::http_accept_match exact;
    exact.update_token("de-DE;q=1", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_TRUE(!exact.matched());
    exact.update_token("*;q=0.8", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_EQ(exact.quality(), 800);
    exact.update_token("DE-de-1996;q=0.2", "de-DE-1996", ruvia::http_accept_token_match_mode::exact);
    EXPECT_EQ(exact.quality(), 200);
}

TEST(Ruvia, http_accept_match_token_fields_allow_only_an_optional_weight) {
    struct token_weight_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr token_weight_case cases[] = {
        {"en", 1000},
        {"EN \t; \tQ=0.5", 500},
        {"en;q=0", 0},
        {"en;q=0.999", 999},
        {"en;level=1", 0},
        {"en;q=0.5;level=1", 0},
        {"en;level=1;q=0.5", 0},
        {"en;q=\"0.5\"", 0},
        {"en;q=0.8;q=0.6", 0},
        {"en;q =1", 0},
        {"en;q= 1", 0},
        {"en;q=0.1234", 0},
        {"*;level=1", 0},
        {"*;q=0.5;level=1", 0},
        {"en;", 0},
        {"en;;q=0.5", 0},
        {"en;q=0.5;", 0},
        {"*;;", 0},
        {"*;q=0.4", 400},
    };
    for (const auto mode : {ruvia::http_accept_token_match_mode::exact, ruvia::http_accept_token_match_mode::language_prefix}) {
        const auto offered = mode == ruvia::http_accept_token_match_mode::exact ? "en" : "en-US";
        for (const auto& item : cases) {
            ruvia::http_accept_match match;
            match.update_token(item.field_, offered, mode);
            EXPECT_EQ(match.quality(), item.quality_);
            EXPECT_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

TEST(Ruvia, http_accept_match_token_weights_accumulate_across_field_lines) {
    for (const auto mode : {ruvia::http_accept_token_match_mode::exact, ruvia::http_accept_token_match_mode::language_prefix}) {
        const auto offered = mode == ruvia::http_accept_token_match_mode::exact ? "en" : "en-US";
        for (const bool invalid_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_token(invalid_first ? "en;level=1" : "en;q=0.6", offered, mode);
            match.update_token(invalid_first ? "en;q=0.6" : "en;level=1", offered, mode);
            EXPECT_TRUE(match.matched());
            EXPECT_EQ(match.quality(), 600);
        }
    }
}

TEST(Ruvia, media_range_specificity_ordering) {
    // Full type/subtype overrides type/*, which overrides */*, even at lower q.
    constexpr std::string_view field = "*/*;q=0.9, text/*;q=0.7, text/html;q=0.2";
    ruvia::http_accept_match exact;
    exact.update_media_type(field, "text/html");
    EXPECT_EQ(exact.quality(), 200);
    ruvia::http_accept_match subtype_wildcard;
    subtype_wildcard.update_media_type(field, "text/plain");
    EXPECT_EQ(subtype_wildcard.quality(), 700);
    ruvia::http_accept_match wildcard;
    wildcard.update_media_type(field, "application/json");
    EXPECT_EQ(wildcard.quality(), 900);
}

TEST(Ruvia, http_accept_match_media_parameters_decide_specificity_across_fields) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    constexpr std::string_view less_specific = "text/plain;format=flowed;q=0.8";
    constexpr std::string_view more_specific = "text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(specific_first ? more_specific : less_specific, offered);
        match.update_media_type(specific_first ? less_specific : more_specific, offered);
        EXPECT_EQ(match.quality(), 200);
    }
    ruvia::http_accept_match joined;
    joined.update_media_type("text/plain;format=flowed;q=0.8, text/plain;CHARSET=\"UTF-8\";format=flowed;q=0.2", offered);
    EXPECT_EQ(joined.quality(), 200);

    // q is a weight, not an additional matching media parameter.
    for (const bool weighted_first : {false, true}) {
        ruvia::http_accept_match match;
        match.update_media_type(weighted_first ? "text/plain;q=0.9" : "text/plain", offered);
        match.update_media_type(weighted_first ? "text/plain" : "text/plain;q=0.9", offered);
        EXPECT_EQ(match.quality(), 1000);
    }
}

TEST(Ruvia, http_accept_match_media_invalid_parameter_suffix_preserves_the_best_match) {
    constexpr std::string_view offered = "text/plain;charset=utf-8;format=flowed";
    for (const std::string_view field : {"text/plain;format=flowed;FORMAT=flowed;q=1", "text/plain;format=flowed;charset=\"unterminated", "text/plain;format=flowed;charset;q=1", "text/plain;format=flowed;unknown=present;q=1"}) {
        ruvia::http_accept_match match;
        match.update_media_type("text/plain;q=0.5", offered);
        match.update_media_type(field, offered);
        EXPECT_EQ(match.quality(), 500);
    }
}

TEST(Ruvia, http_accept_match_media_parameter_lookup_handles_order_and_quoted_values) {
    constexpr std::string_view field = R"(text/plain;note="a;b=c";format="flowed";profile=aB;charset=utf-8;q=0.8)";
    for (const std::string_view offered : {R"(text/plain;FORMAT=flowed;note="a;b=c";profile="\a\B";CHARSET="UTF-8")", R"(text/plain;CHARSET="UTF-8";profile="\a\B";note="a;b=c";FORMAT=flowed)", R"(text/plain;profile=aB;note="a;b=c";charset=utf-8;format="flowed")"}) {
        ruvia::http_accept_match match;
        match.update_media_type(field, offered);
        EXPECT_EQ(match.quality(), 800);
    }
}

TEST(Ruvia, http_accept_match_plain_text_parameters_ignore_value_case) {
    struct media_case final {
        std::string_view field_;
        std::string_view offered_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {"text/plain;format=Flowed;q=0.7", "text/plain;format=flowed", 700},
        {"text/plain;format=fixed;q=0.4", "TEXT/PLAIN;FORMAT=FIXED", 400},
        {"text/plain;delsp=Yes;q=0.6", "text/plain;format=flowed;delsp=yes", 600},
        {R"(text/plain;format="\F\L\O\W\E\D";delsp="\N\O";q=0.5)", "text/plain;format=flowed;delsp=no", 500},
        {"text/plain;format=flowed;delsp=yes;q=0.3", R"(text/plain;format="\F\L\O\W\E\D";DELSP="\Y\E\S")", 300},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.field_);
        ruvia::http_accept_match match;
        match.update_media_type(item.field_, item.offered_);
        EXPECT_EQ(match.quality(), item.quality_);
    }
}

TEST(Ruvia, http_accept_match_plain_text_parameter_exclusions_survive_field_order) {
    constexpr std::string_view offered = "text/plain;format=flowed;delsp=yes";
    constexpr std::string_view general = "*/*;q=0.8";
    for (const std::string_view excluded : {"text/plain;format=FLOWED;q=0", "text/*;delsp=YES;q=0", "*/*;format=FLOWED;delsp=YES;q=0"}) {
        SCOPED_TRACE(excluded);
        for (const bool exclusion_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_media_type(exclusion_first ? excluded : general, offered);
            match.update_media_type(exclusion_first ? general : excluded, offered);
            EXPECT_EQ(match.quality(), 0);
            EXPECT_FALSE(match.matched());
        }
        ruvia::http_accept_match joined;
        joined.update_media_type(std::string(general) + ", " + std::string(excluded), offered);
        EXPECT_FALSE(joined.matched());
    }
}

TEST(Ruvia, http_accept_match_preserves_other_parameter_value_case) {
    struct media_case final {
        std::string_view field_;
        std::string_view offered_;
    };
    constexpr media_case cases[] = {
        {"application/plain;format=Flowed", "application/plain;format=flowed"},
        {"text/example;format=Flowed", "text/example;format=flowed"},
        {"application/plain;delsp=Yes", "application/plain;delsp=yes"},
        {"text/example;delsp=Yes", "text/example;delsp=yes"},
        {"text/plain;profile=Example", "text/plain;profile=example"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.field_);
        ruvia::http_accept_match match;
        match.update_media_type(item.field_, item.offered_);
        EXPECT_FALSE(match.matched());
    }
}

TEST(Ruvia, accepts_media_type_requires_a_valid_complete_offered_value) {
    constexpr std::string_view field = "text/plain;charset=utf-8;q=1";
    for (const std::string_view offered : {"text/plain;charset=utf-8;profile", "text/plain;charset=utf-8;profile=", "text/plain;charset=utf-8;profile=\"unterminated", "text/plain;charset=utf-8;CHARSET=utf-8", "text/plain;charset=utf-8;CHARSET=latin1", "text/plain;charset=utf-8;profile =example"}) {
        ruvia::http_accept_match match;
        match.update_media_type(field, offered);
        EXPECT_TRUE(!match.matched());
        EXPECT_EQ(match.quality(), 0);
    }
}

TEST(Ruvia, http_accept_match_media_empty_parameter_slots_preserve_quality) {
    struct media_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {"", 0},
        {", ,", 0},
        {"text/plain;", 1000},
        {"text/plain; \t; charset=utf-8;;", 1000},
        {"text/plain;;q=0.6; ;", 600},
        {"text/plain;;q=0;;", 0},
        {"text/*;;q=0.4;", 400},
        {"*/*;;q=0.3;", 300},
        {"text/plain;;q=;", 0},
        {"text/plain;;q=\"0.6\";", 0},
        {"text/plain;;q=0.6;;Q=0.8;", 0},
        {"text/plain;;q=0.1234;", 0},
        {"text/plain;;charset=;q=1;", 0},
        {"text/plain;;charset =utf-8;q=1;", 0},
        {"text/plain;;charset= utf-8;q=1;", 0},
        {"text/plain;;charset=latin1;q=1;", 0},
    };
    for (const std::string_view offered : {"text/plain;charset=utf-8", "text/plain;;charset=utf-8; ;"}) {
        for (const auto& item : cases) {
            ruvia::http_accept_match match;
            match.update_media_type(item.field_, offered);
            EXPECT_EQ(match.quality(), item.quality_);
            EXPECT_EQ(match.matched(), item.quality_ > 0);
        }
    }
}

TEST(Ruvia, http_accept_match_media_empty_slots_preserve_specificity_across_fields) {
    constexpr std::string_view general = "text/plain;;q=0.8;";
    constexpr std::string_view specific = "text/plain; ;charset=utf-8;;q=0;";
    for (const bool specific_first : {false, true}) {
        ruvia::http_accept_match excluded;
        excluded.update_media_type(specific_first ? specific : general, "text/plain;;charset=utf-8;");
        excluded.update_media_type(specific_first ? general : specific, "text/plain;;charset=utf-8;");
        EXPECT_TRUE(!excluded.matched());

        ruvia::http_accept_match other_charset;
        other_charset.update_media_type(specific_first ? specific : general, "text/plain;;charset=latin1;");
        other_charset.update_media_type(specific_first ? general : specific, "text/plain;;charset=latin1;");
        EXPECT_EQ(other_charset.quality(), 800);
    }
    for (const bool higher_first : {false, true}) {
        ruvia::http_accept_match repeated;
        repeated.update_media_type(higher_first ? "text/plain;;q=0.8;" : "text/plain;q=0.2", "text/plain;");
        repeated.update_media_type(higher_first ? "text/plain;q=0.2" : "text/plain;;q=0.8;", "text/plain;");
        EXPECT_EQ(repeated.quality(), 800);
    }
}

TEST(Ruvia, http_accept_match_keeps_quality_and_parameter_validation_consistent) {
    constexpr std::string_view offered = R"(text/plain;profile="a;b=c";charset=UTF-8)";
    struct media_case final {
        std::string_view field_;
        int quality_;
    };
    constexpr media_case cases[] = {
        {R"(text/plain;q=0.2;profile="a;b=c";charset=utf-8)", 200},
        {R"(text/plain;charset="utf-8";profile="a;b=c";q=0.3)", 300},
        {R"(text/plain;profile="a;b=c";q=0)", 0},
        {R"(text/plain;q=invalid;profile="a;b=c")", 0},
        {R"(text/plain;profile="a;b=c";q="0.5")", 0},
        {R"(text/plain;q=;profile="a;b=c")", 800},
        {R"(text/plain;q=0.5;Q=0.4;profile="a;b=c")", 800},
        {R"(text/plain;profile="a;b=c"suffix;q=0.5)", 800},
        {R"(text/plain;profile=a/b;q=0.5)", 800},
        {R"(text/plain;profile="a;b=c";charset =utf-8;q=0.5)", 800},
        {R"(text/plain;profile="a;b=c";charset= utf-8;q=0.5)", 800},
    };
    for (const auto& item : cases) {
        for (const bool specific_first : {false, true}) {
            ruvia::http_accept_match match;
            match.update_media_type(specific_first ? item.field_ : "text/*;q=0.8", offered);
            match.update_media_type(specific_first ? "text/*;q=0.8" : item.field_, offered);
            EXPECT_EQ(match.quality(), item.quality_);
        }
    }
}

namespace {

using ruvia::http_byte_range_set;
using ruvia::resolve_http_byte_range_set;

[[nodiscard]] bool is_ignored_range(std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).ignored();
}

[[nodiscard]] bool is_unsatisfiable_range(std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).unsatisfiable();
}

}  // namespace

TEST(Ruvia, byte_range_set_discriminates_outcomes) {
    const auto resolved = resolve_http_byte_range_set("bytes=10-19", 100);
    EXPECT_TRUE(!resolved.ignored());
    EXPECT_TRUE(!resolved.unsatisfiable());
    EXPECT_EQ(resolved.size(), std::size_t{1});
    if (resolved.size() == 1) {
        const auto range = resolved[0];
        EXPECT_EQ(range.offset_, std::uint64_t{10});
        EXPECT_EQ(range.length_, std::uint64_t{10});
    }

    const auto ignored = resolve_http_byte_range_set("items=10-19", 100);
    EXPECT_TRUE(ignored.ignored());
    EXPECT_EQ(ignored.size(), std::size_t{0});

    const auto unsatisfiable = resolve_http_byte_range_set("bytes=100-", 100);
    EXPECT_TRUE(!unsatisfiable.ignored());
    EXPECT_TRUE(unsatisfiable.unsatisfiable());
    EXPECT_EQ(unsatisfiable.size(), std::size_t{0});
}

TEST(Ruvia, byte_range_set_bounded_and_open_ended) {
    const auto bounded = resolve_http_byte_range_set("bytes=100-199", 1000);
    EXPECT_EQ(bounded.size(), std::size_t{1});
    if (bounded.size() == 1) {
        const auto range = bounded[0];
        EXPECT_EQ(range.offset_, std::uint64_t{100});
        EXPECT_EQ(range.length_, std::uint64_t{100});
    }

    const auto open = resolve_http_byte_range_set("bytes=500-", 1000);
    EXPECT_EQ(open.size(), std::size_t{1});
    if (open.size() == 1) {
        const auto range = open[0];
        EXPECT_EQ(range.offset_, std::uint64_t{500});
        EXPECT_EQ(range.length_, std::uint64_t{500});
    }

    const auto last_byte = resolve_http_byte_range_set("bytes=999-999", 1000);
    EXPECT_EQ(last_byte.size(), std::size_t{1});
    if (last_byte.size() == 1) {
        EXPECT_EQ(last_byte[0].offset_, std::uint64_t{999});
        EXPECT_EQ(last_byte[0].length_, std::uint64_t{1});
    }

    const auto clamped_end = resolve_http_byte_range_set("bytes=0-2000", 1000);
    EXPECT_EQ(clamped_end.size(), std::size_t{1});
    if (clamped_end.size() == 1) {
        EXPECT_EQ(clamped_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(clamped_end[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_suffix) {
    const auto suffix = resolve_http_byte_range_set("bytes=-100", 1000);
    EXPECT_EQ(suffix.size(), std::size_t{1});
    if (suffix.size() == 1) {
        EXPECT_EQ(suffix[0].offset_, std::uint64_t{900});
        EXPECT_EQ(suffix[0].length_, std::uint64_t{100});
    }

    const auto whole = resolve_http_byte_range_set("bytes=-2000", 1000);
    EXPECT_EQ(whole.size(), std::size_t{1});
    if (whole.size() == 1) {
        EXPECT_EQ(whole[0].offset_, std::uint64_t{0});
        EXPECT_EQ(whole[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_unsatisfiable_is_payload_free) {
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1000-", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1500-1600", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-0", 1000));
}

TEST(Ruvia, byte_range_set_ignores_invalid_and_unknown_values) {
    EXPECT_TRUE(is_ignored_range("items=0-99", 1000));
    EXPECT_TRUE(is_ignored_range("0-99", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=500-100", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=abc", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=x-9", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=0-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=", 1000));
}

TEST(Ruvia, byte_range_set_supports_multiple_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=0-99,200-299", 1000);
    EXPECT_TRUE(!ranges.ignored());
    EXPECT_TRUE(!ranges.unsatisfiable());
    EXPECT_EQ(ranges.size(), std::size_t{2});
    if (ranges.size() == 2) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{100});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{200});
        EXPECT_EQ(ranges[1].length_, std::uint64_t{100});
    }
}

TEST(Ruvia, byte_range_set_unit_is_case_insensitive) {
    const auto title_case = resolve_http_byte_range_set("Bytes=0-9", 100);
    const auto upper_case = resolve_http_byte_range_set("BYTES=90-", 100);
    EXPECT_EQ(title_case.size(), std::size_t{1});
    EXPECT_EQ(upper_case.size(), std::size_t{1});
    if (title_case.size() == 1) {
        EXPECT_EQ(title_case[0].offset_, std::uint64_t{0});
        EXPECT_EQ(title_case[0].length_, std::uint64_t{10});
    }
    if (upper_case.size() == 1) {
        EXPECT_EQ(upper_case[0].offset_, std::uint64_t{90});
        EXPECT_EQ(upper_case[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_allows_ows_after_equals) {
    const auto spaced = resolve_http_byte_range_set("bytes= \t10-19", 100);
    EXPECT_EQ(spaced.size(), std::size_t{1});
    if (spaced.size() == 1) {
        EXPECT_EQ(spaced[0].offset_, std::uint64_t{10});
        EXPECT_EQ(spaced[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_trims_field_value_ows) {
    const auto trailing = resolve_http_byte_range_set("bytes=10-19 \t", 100);
    const auto wrapped = resolve_http_byte_range_set(" \tbytes=20-29\t ", 100);
    EXPECT_EQ(trailing.size(), std::size_t{1});
    EXPECT_EQ(wrapped.size(), std::size_t{1});
    if (trailing.size() == 1) {
        EXPECT_EQ(trailing[0].offset_, std::uint64_t{10});
        EXPECT_EQ(trailing[0].length_, std::uint64_t{10});
    }
    if (wrapped.size() == 1) {
        EXPECT_EQ(wrapped[0].offset_, std::uint64_t{20});
        EXPECT_EQ(wrapped[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_huge_decimal_numerals_preserve_semantics) {
    const auto huge_start = resolve_http_byte_range_set("bytes=184467440737095516160-", 1000);
    EXPECT_TRUE(huge_start.unsatisfiable());

    const auto huge_end = resolve_http_byte_range_set("bytes=0-184467440737095516160", 1000);
    EXPECT_EQ(huge_end.size(), std::size_t{1});
    if (huge_end.size() == 1) {
        EXPECT_EQ(huge_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_end[0].length_, std::uint64_t{1000});
    }

    const auto huge_suffix = resolve_http_byte_range_set("bytes=-184467440737095516160", 1000);
    EXPECT_EQ(huge_suffix.size(), std::size_t{1});
    if (huge_suffix.size() == 1) {
        EXPECT_EQ(huge_suffix[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_suffix[0].length_, std::uint64_t{1000});
    }

    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160x-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160-184467440737095516159", 1000));
}

TEST(Ruvia, byte_range_set_preserves_disjoint_order_and_coalesces_touching_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=50-59,0-9,20-29,90-", 100);
    EXPECT_EQ(ranges.size(), std::size_t{4});
    if (ranges.size() == 4) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{50});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[2].offset_, std::uint64_t{20});
        EXPECT_EQ(ranges[3].offset_, std::uint64_t{90});
    }
    // A coalesced part takes the place of its earliest member, whatever order
    // its members were received in.
    const auto coalesced = resolve_http_byte_range_set("bytes=20-29,0-9,10-19,18-22,90-", 100);
    EXPECT_TRUE(!coalesced.ignored());
    EXPECT_TRUE(!coalesced.unsatisfiable());
    EXPECT_EQ(coalesced.size(), std::size_t{2});
    if (coalesced.size() == 2) {
        EXPECT_EQ(coalesced[0].offset_, std::uint64_t{0});
        EXPECT_EQ(coalesced[0].length_, std::uint64_t{30});
        EXPECT_EQ(coalesced[1].offset_, std::uint64_t{90});
        EXPECT_EQ(coalesced[1].length_, std::uint64_t{10});
    }
    const auto partial = resolve_http_byte_range_set("bytes=0-1,900-", 100);
    EXPECT_EQ(partial.size(), std::size_t{1});
    const auto none = resolve_http_byte_range_set("bytes=100-,200-", 100);
    EXPECT_TRUE(none.unsatisfiable());
}

TEST(Ruvia, byte_range_set_never_repeats_representation_bytes) {
    // Descending open-ended and suffix ranges each overlap every earlier one;
    // as separate parts they would multiply the response size.
    std::string open_ended = "bytes=";
    std::string suffixes = "bytes=";
    for (std::size_t index = 0; index < http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            open_ended.push_back(',');
            suffixes.push_back(',');
        }
        open_ended.append(std::to_string(http_byte_range_set::capacity - 1 - index) + "-");
        suffixes.append(std::string("-") + std::to_string(1000 - index).c_str());
    }
    for (const auto& field : {open_ended, suffixes}) {
        const auto ranges = resolve_http_byte_range_set(field, 1000);
        EXPECT_EQ(ranges.size(), std::size_t{1});
        if (ranges.size() == 1) {
            EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
            EXPECT_EQ(ranges[0].length_, std::uint64_t{1000});
        }
    }

    // Ranges that only touch once a later range bridges them become one part.
    const auto bridged = resolve_http_byte_range_set("bytes=30-39,0-9,50-59,5-35", 100);
    EXPECT_EQ(bridged.size(), std::size_t{2});
    if (bridged.size() == 2) {
        EXPECT_EQ(bridged[0].offset_, std::uint64_t{0});
        EXPECT_EQ(bridged[0].length_, std::uint64_t{40});
        EXPECT_EQ(bridged[1].offset_, std::uint64_t{50});
        EXPECT_EQ(bridged[1].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_tolerates_bounded_empty_members) {
    const auto ranges = resolve_http_byte_range_set("bytes=,0-1,,2-3,", 100);
    EXPECT_EQ(ranges.size(), std::size_t{1});
    if (ranges.size() == 1) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{4});
    }
    EXPECT_TRUE(resolve_http_byte_range_set("bytes=,,", 100).ignored());
    EXPECT_EQ(resolve_http_byte_range_set("bytes=0-1,,3-4", 100).size(), std::size_t{2});
    EXPECT_TRUE(resolve_http_byte_range_set("bytes=0-1,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,", 100).ignored());
}

TEST(Ruvia, byte_range_set_counts_only_received_empty_members) {
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"100-", "100-199", "-0"}) {
        const auto ranges = resolve_http_byte_range_set(prefix + tail, 100);
        EXPECT_TRUE(!ranges.ignored());
        EXPECT_TRUE(ranges.unsatisfiable());
    }
    const auto satisfiable = resolve_http_byte_range_set(prefix + "0-1", 100);
    EXPECT_EQ(satisfiable.size(), std::size_t{1});
    if (satisfiable.size() == 1) {
        EXPECT_EQ(satisfiable[0].offset_, std::uint64_t{0});
        EXPECT_EQ(satisfiable[0].length_, std::uint64_t{2});
    }
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",100-", 100).ignored());
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",0-1", 100).ignored());
}

TEST(Ruvia, byte_range_set_ignores_malformed_and_over_limit_sets) {
    std::string ranges = "bytes=";
    for (std::size_t index = 0; index <= http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            ranges.push_back(',');
        }
        ranges.append(std::to_string(index * 2));
        ranges.push_back('-');
        ranges.append(std::to_string(index * 2));
    }
    EXPECT_TRUE(resolve_http_byte_range_set(ranges, 100).ignored());
}

TEST(Ruvia, byte_range_set_empty_representation_validates_syntax_before_unsatisfiable) {
    EXPECT_TRUE(is_unsatisfiable_range("bytes=0-0", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=0-99", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-5", 0));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-1", 0));
    EXPECT_TRUE(is_unsatisfiable_range("BYTES=0-", 0));

    EXPECT_TRUE(is_ignored_range("bytes=abc", 0));
    EXPECT_TRUE(is_ignored_range("bytes=5-2", 0));
    EXPECT_TRUE(is_ignored_range("bytes=", 0));
    EXPECT_TRUE(is_ignored_range("bytes=-", 0));
    EXPECT_TRUE(is_ignored_range("bytes=0-0, invalid", 0));

    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)resolve_http_byte_range_set("bytes=0-1", invalid);
        }));
    }
}

TEST(Ruvia, byte_range_set_rejects_out_of_range_index) {
    const auto resolved = resolve_http_byte_range_set("bytes=0-9", 100);
    EXPECT_EQ(resolved.size(), std::size_t{1});
    const auto throws_out_of_range = [](auto&& access) {
        try {
            (void)access();
        } catch (const std::out_of_range&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(!throws_out_of_range([&] {
        return resolved[0];
    }));
    EXPECT_TRUE(throws_out_of_range([&] {
        return resolved[1];
    }));
    EXPECT_TRUE(throws_out_of_range([&] {
        return resolved[http_byte_range_set::capacity];
    }));
    const auto ignored = resolve_http_byte_range_set("items=0-9", 100);
    EXPECT_TRUE(throws_out_of_range([&] {
        return ignored[0];
    }));
}

TEST(Ruvia, parse_cache_control_flags_and_ages) {
    const auto cc = ruvia::parse_cache_control("public, max-age=60, s-maxage=120, stale-while-revalidate=30, immutable");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::immutable));
    EXPECT_TRUE(!cc.has(static_cast<ruvia::cache_control_directive>(static_cast<std::uint16_t>(ruvia::cache_control_directive::public_value) | static_cast<std::uint16_t>(ruvia::cache_control_directive::immutable))));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{60});
    EXPECT_EQ(*cc.s_maxage(), std::uint64_t{120});
    EXPECT_EQ(*cc.stale_while_revalidate(), std::uint64_t{30});
}

TEST(Ruvia, parse_cache_control_request_directives) {
    const auto request = ruvia::parse_cache_control("only-if-cached, max-age=0, min-fresh=15, max-stale=30");
    EXPECT_TRUE(request.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_EQ(request.max_age().value_or(1), std::uint64_t{0});
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{15});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{30});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));

    const auto any_stale = ruvia::parse_cache_control("max-stale");
    EXPECT_TRUE(any_stale.has(ruvia::cache_control_directive::max_stale_any));
    EXPECT_TRUE(!any_stale.max_stale().has_value());

    const auto invalid = ruvia::parse_cache_control("only-if-cached=yes, min-fresh=bad, max-stale=bad");
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::only_if_cached));
    EXPECT_TRUE(!invalid.min_fresh().has_value());
    EXPECT_TRUE(!invalid.max_stale().has_value());
    EXPECT_TRUE(!invalid.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_request_freshness_uses_first_occurrence) {
    ruvia::cache_control_field_parser parser;
    parser.update("min-fresh=5, max-stale=10");
    parser.update("min-fresh=50, max-stale");
    const auto request = parser.finish();
    EXPECT_EQ(request.min_fresh().value_or(0), std::uint64_t{5});
    EXPECT_EQ(request.max_stale().value_or(0), std::uint64_t{10});
    EXPECT_TRUE(!request.has(ruvia::cache_control_directive::max_stale_any));
}

TEST(Ruvia, parse_cache_control_no_store_and_private) {
    const auto cc = ruvia::parse_cache_control("no-store, private, must-revalidate");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_no_transform_is_bare_and_quote_aware) {
    const auto present = ruvia::parse_cache_control("NO-TRANSFORM");
    EXPECT_TRUE(present.has(ruvia::cache_control_directive::no_transform));

    const auto quoted = ruvia::parse_cache_control(R"(extension="a, no-transform, b")");
    EXPECT_TRUE(!quoted.has(ruvia::cache_control_directive::no_transform));

    const auto qualified = ruvia::parse_cache_control("no-transform=ignored");
    EXPECT_TRUE(!qualified.has(ruvia::cache_control_directive::no_transform));
}

TEST(Ruvia, parse_cache_control_quoted_and_case_insensitive_and_unknown) {
    // Directive names are case-insensitive; a quoted delta-seconds is accepted; unknown ignored.
    const auto cc = ruvia::parse_cache_control("Max-Age=\"45\" , surrogate-control=foo, No-Cache");
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(cc.max_age().has_value());
    EXPECT_EQ(*cc.max_age(), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_decodes_quoted_pairs_in_delta_seconds) {
    const auto cc = ruvia::parse_cache_control(R"(max-age="6\0", s-maxage="1\20", stale-while-revalidate="\30", stale-if-error="4\5")");
    EXPECT_EQ(cc.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(cc.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(cc.stale_if_error().value_or(0), std::uint64_t{45});
}

TEST(Ruvia, parse_cache_control_rejects_bad_delta_seconds) {
    const auto cc = ruvia::parse_cache_control("max-age=abc, s-maxage=");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
}

TEST(Ruvia, parse_cache_control_rejects_whitespace_around_equals) {
    const auto cc = ruvia::parse_cache_control(
        "max-age =60, s-maxage= 120, stale-while-revalidate = 30, "
        "stale-if-error=\t45, no-cache =\"Set-Cookie\", private = auth, "
        "no-cache= \"ETag\", private= auth");
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(!cc.s_maxage().has_value());
    EXPECT_TRUE(!cc.stale_while_revalidate().has_value());
    EXPECT_TRUE(!cc.stale_if_error().has_value());
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::private_value));

    // OWS around comma separators remains valid list framing.
    const auto separated = ruvia::parse_cache_control(" max-age=60 \t,\t no-store ");
    EXPECT_EQ(separated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_TRUE(separated.has(ruvia::cache_control_directive::no_store));
}

TEST(Ruvia, parse_cache_control_freshness_uses_first_occurrence) {
    const auto duplicated = ruvia::parse_cache_control(
        "max-age=60, MAX-AGE=3600, s-maxage=120, s-maxage=7200, "
        "stale-while-revalidate=30, stale-while-revalidate=300, "
        "stale-if-error=45, stale-if-error=450");
    EXPECT_EQ(duplicated.max_age().value_or(0), std::uint64_t{60});
    EXPECT_EQ(duplicated.s_maxage().value_or(0), std::uint64_t{120});
    EXPECT_EQ(duplicated.stale_while_revalidate().value_or(0), std::uint64_t{30});
    EXPECT_EQ(duplicated.stale_if_error().value_or(0), std::uint64_t{45});

    // An invalid first occurrence cannot be repaired by a later value; caches
    // must not accidentally turn invalid freshness information into freshness.
    const auto invalid_first = ruvia::parse_cache_control("max-age=invalid, max-age=3600, s-maxage=, s-maxage=7200");
    EXPECT_TRUE(!invalid_first.max_age().has_value());
    EXPECT_TRUE(!invalid_first.s_maxage().has_value());
}

TEST(Ruvia, cache_control_field_parser_combines_repeated_lines) {
    ruvia::cache_control_field_parser parser;
    parser.update("public, max-age=invalid, s-maxage=120");
    parser.update(R"(extension="a, no-transform, b", no-transform, max-age=3600, s-maxage=7200)");

    const auto cc = parser.finish();
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_EQ(cc.s_maxage().value_or(0), std::uint64_t{120});
}

TEST(Ruvia, parse_cache_control_delta_seconds_overflow_saturates) {
    const auto cc = ruvia::parse_cache_control(
        "max-age=184467440737095516150, "
        "s-maxage=\"184467440737095516150\"");
    EXPECT_EQ(cc.max_age().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(cc.s_maxage().value_or(0), (std::numeric_limits<std::uint64_t>::max)());
}

TEST(Ruvia, parse_cache_control_does_not_split_quoted_extension_values) {
    const auto cc = ruvia::parse_cache_control("extension=\"a, public, max-age=999, b\", private");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.max_age().has_value());
    EXPECT_TRUE(cc.has(ruvia::cache_control_directive::private_value));

    // A quoted-pair keeps the escaped quote inside the extension value; its
    // commas likewise cannot introduce directives.
    const auto escaped = ruvia::parse_cache_control("extension=\"a\\\", no-store, b\", immutable");
    EXPECT_TRUE(!escaped.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(escaped.has(ruvia::cache_control_directive::immutable));

    // An unterminated quoted value is malformed through the end of the field,
    // so a comma within it must not accidentally enable caching directives.
    const auto unterminated = ruvia::parse_cache_control("extension=\"a, public, max-age=3600");
    EXPECT_TRUE(!unterminated.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!unterminated.max_age().has_value());
}

TEST(Ruvia, parse_cache_control_ignores_arguments_on_bare_directives) {
    const auto cc = ruvia::parse_cache_control(
        "public=ignored, no-store=ignored, must-revalidate=x, "
        "proxy-revalidate=\"x\", no-transform=ignored, immutable=");
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::public_value));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_store));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::must_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::proxy_revalidate));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::no_transform));
    EXPECT_TRUE(!cc.has(ruvia::cache_control_directive::immutable));

    // no-cache and private are different: their response forms can carry a
    // field-name list, so an argument does not invalidate the directive.
    const auto qualified = ruvia::parse_cache_control("no-cache=\"Set-Cookie\", private=\"Authorization\"");
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::no_cache));
    EXPECT_TRUE(qualified.has(ruvia::cache_control_directive::private_value));
}

TEST(Ruvia, parse_http_date_imf_fixdate) {
    // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT.
    const auto t = ruvia::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    EXPECT_TRUE(t.has_value());
    EXPECT_EQ(static_cast<long long>(*t), 784111777LL);
    EXPECT_TRUE(!ruvia::parse_http_date("not a date").has_value());
}

TEST(Ruvia, format_http_vary_cache_key_distinguishes_absent_from_empty_header) {
    const auto vary = ruvia::parse_http_vary("Accept-Language, X-Mode");
    const std::array<ruvia::http_header_view, 1> absent{
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> empty_value{
        ruvia::http_header_view{"Accept-Language", ""},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const std::array<ruvia::http_header_view, 2> ows_only{
        ruvia::http_header_view{"Accept-Language", " \t "},
        ruvia::http_header_view{"X-Mode", "fast"},
    };
    const auto absent_key = ruvia::format_http_vary_cache_key(vary, absent);
    const auto empty_key = ruvia::format_http_vary_cache_key(vary, empty_value);
    const auto ows_key = ruvia::format_http_vary_cache_key(vary, ows_only);
    ASSERT_TRUE(absent_key.has_value());
    ASSERT_TRUE(empty_key.has_value());
    ASSERT_TRUE(ows_key.has_value());
    EXPECT_NE(*absent_key, *empty_key);
    EXPECT_EQ(*empty_key, *ows_key);
    EXPECT_FALSE(ruvia::http_vary_requests_match(vary, absent, empty_value));
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, empty_value, ows_only));
}

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 9110) for static file responses.

// Date formatting, cache framing, and locale-independent IMF-fixdate coverage
// live in date_parsing.cpp. This file covers precondition evaluation.

TEST(Ruvia, conditional_method_plan_follows_precondition_and_range_semantics) {
    using ruvia::http_conditional_method_plan;

    const auto get = get_http_conditional_method_plan(ruvia::http_known_method::get);
    EXPECT_TRUE(get.evaluates_preconditions_);
    EXPECT_TRUE(get.uses_not_modified_response_);
    EXPECT_TRUE(get.evaluates_if_modified_since_);
    EXPECT_TRUE(get.evaluates_range_);

    const auto head = get_http_conditional_method_plan(ruvia::http_known_method::head);
    EXPECT_TRUE(head.evaluates_preconditions_);
    EXPECT_TRUE(head.uses_not_modified_response_);
    EXPECT_TRUE(head.evaluates_if_modified_since_);
    EXPECT_TRUE(!head.evaluates_range_);

    for (const auto method : {ruvia::http_known_method::post, ruvia::http_known_method::put, ruvia::http_known_method::delete_value, ruvia::http_known_method::patch}) {
        const auto plan = get_http_conditional_method_plan(method);
        EXPECT_TRUE(plan.evaluates_preconditions_);
        EXPECT_TRUE(!plan.uses_not_modified_response_);
        EXPECT_TRUE(!plan.evaluates_if_modified_since_);
        EXPECT_TRUE(!plan.evaluates_range_);
    }

    for (const auto method : {ruvia::http_known_method::options, ruvia::http_known_method::connect, ruvia::http_known_method::unknown}) {
        const auto plan = get_http_conditional_method_plan(method);
        EXPECT_TRUE(!plan.evaluates_preconditions_);
        EXPECT_TRUE(!plan.uses_not_modified_response_);
        EXPECT_TRUE(!plan.evaluates_if_modified_since_);
        EXPECT_TRUE(!plan.evaluates_range_);
    }
}

TEST(Ruvia, http_date_precondition_comparisons) {
    using ruvia::http_date_not_modified;
    using ruvia::http_date_unmodified;

    constexpr std::time_t canonical{784111777};
    const auto later = canonical + 1;
    const auto earlier = canonical - 1;
    constexpr auto header_value = "Sun, 06 Nov 1994 08:49:37 GMT";

    EXPECT_TRUE(http_date_not_modified(header_value, canonical));
    EXPECT_TRUE(http_date_not_modified(header_value, earlier));
    EXPECT_TRUE(!http_date_not_modified(header_value, later));
    EXPECT_TRUE(!http_date_not_modified("not-a-date", canonical));

    EXPECT_TRUE(http_date_unmodified(header_value, canonical));
    EXPECT_TRUE(http_date_unmodified(header_value, earlier));
    EXPECT_TRUE(!http_date_unmodified(header_value, later));
    EXPECT_TRUE(http_date_unmodified("not-a-date", later));
}

TEST(Ruvia, http_if_range_requires_exact_validator) {
    using ruvia::http_if_range_allows;

    constexpr std::time_t canonical{784111777};
    constexpr auto etag = R"("abc")";
    constexpr auto date = "Sun, 06 Nov 1994 08:49:37 GMT";

    EXPECT_TRUE(!http_if_range_allows("", etag, canonical, true));
    EXPECT_TRUE(http_if_range_allows(etag, etag, canonical, true));
    EXPECT_TRUE(!http_if_range_allows(R"(W/"abc")", etag, canonical, true));
    EXPECT_TRUE(http_if_range_allows(date, etag, canonical, true));
    EXPECT_TRUE(!http_if_range_allows(date, etag, canonical + 1, true));
    EXPECT_TRUE(!http_if_range_allows(date, etag, canonical, false));
    EXPECT_TRUE(!http_if_range_allows(date, etag, std::nullopt, true));
    EXPECT_TRUE(http_if_range_allows(etag, etag, std::nullopt, false));
}

TEST(Ruvia, http_etag_preconditions_fold_repeated_field_lines) {
    using ruvia::http_etag_preconditions;
    using ruvia::http_header_view;

    const http_header_view fields[]{{"If-None-Match", R"("current")"}, {"If-None-Match", R"("stale")"}};
    const auto request = ruvia::make_parsed_http_request("GET", "/", fields, {});

    const auto matched = get_http_etag_preconditions(request, R"("current")");
    EXPECT_TRUE(matched.if_none_match_.present_);
    EXPECT_TRUE(matched.if_none_match_.matches());

    const http_header_view wildcard_fields[]{{"If-Match", "*"}};
    const auto wildcard = ruvia::make_parsed_http_request("GET", "/", wildcard_fields, {});
    const auto existence = get_http_etag_preconditions(wildcard, R"("unused")");
    EXPECT_TRUE(existence.if_match_.present_);
    EXPECT_TRUE(existence.if_match_.matches());
    EXPECT_TRUE(existence.if_match_.wildcard_);
}

namespace {

template <typename request_type>
concept conditional_headers_accepts_request = requires(request_type&& request) { ruvia::get_http_conditional_headers(std::forward<request_type>(request)); };

static_assert(conditional_headers_accepts_request<const ruvia::http_request&>);
static_assert(!conditional_headers_accepts_request<ruvia::http_request>);

}  // namespace
