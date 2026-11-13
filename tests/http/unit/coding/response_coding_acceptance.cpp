#include <gtest/gtest.h>

#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_response.h"

#include "http1_request_message_fixture.h"

TEST(Ruvia, response_custom_coding_distinguishes_missing_and_empty_preferences) {
    const auto missing = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto empty = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding:\r\n\r\n");
    ASSERT_TRUE(missing.complete());
    ASSERT_TRUE(empty.complete());
    EXPECT_TRUE(ruvia::http_accepts_encoding(missing.state_.request(), "custom"));
    EXPECT_FALSE(ruvia::http_accepts_encoding(empty.state_.request(), "custom"));
    EXPECT_TRUE(ruvia::http_accepts_encoding(empty.state_.request(), "identity"));
    ruvia::http_response response;
    response.header("Content-Encoding", "custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(missing.state_.request()), response, &missing.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(empty.state_.request()), response, &empty.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_custom_coding_explicit_quality_overrides_wildcard_across_lines) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: *;q=0.8, custom;q=0, gzip\r\n"
        "Accept-Encoding: CUSTOM;q=0.4, x-compress;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    EXPECT_TRUE(selection.accepts("CuStOm", parsed.state_.request()));
    EXPECT_TRUE(selection.accepts("another", parsed.state_.request()));
    EXPECT_FALSE(selection.accepts("compress", parsed.state_.request()));
    ruvia::http_response response;
    response.header("Content-Encoding", "gzip, CUSTOM");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "compress", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_coding_stack_checks_every_known_and_custom_member_without_common_case_allocation) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, custom, other, br;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::absent);
    response.header("Content-Encoding", "gzip, CUSTOM, other, custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "br", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    EXPECT_THROW(response.header("Content-Encoding", "gzip;param=value"), std::invalid_argument);
}

TEST(Ruvia, response_coding_extension_stack_is_not_limited_by_decoder_depth) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: *, ninth;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, ninth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, tenth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
}
