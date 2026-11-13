#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_server.h"

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
