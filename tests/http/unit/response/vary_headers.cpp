#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "response/response_header_utils.h"
#include "test_harness.h"

namespace {

using ruvia::http_response;
using ruvia::detail::add_vary_token;
using ruvia::detail::add_vary_tokens;

http_response make_response() {
    return http_response({.resource_ = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(vary_adds_single_then_appends) {
    auto response = make_response();
    add_vary_token(response, "Origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
    add_vary_token(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_dedups_against_existing_case_insensitively) {
    auto response = make_response();
    add_vary_token(response, "Origin");
    // An already-present token (matched case-insensitively) is not appended again.
    add_vary_token(response, "origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
}

RUVIA_TEST(vary_batch_dedups_within_batch_and_existing) {
    auto response = make_response();
    add_vary_token(response, "Origin");
    // The batch repeats a token and re-lists an existing one; each survivor is
    // added exactly once.
    const std::string_view batch[] = {"Origin", "Accept-Encoding", "Accept-Encoding"};
    add_vary_tokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_skips_empty_tokens_and_null_batch) {
    auto response = make_response();
    const std::string_view batch[] = {std::string_view(), "Origin", std::string_view()};
    add_vary_tokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
    // A null pointer or zero count is a no-op, not a crash.
    add_vary_tokens(response, nullptr, 0);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
}

RUVIA_TEST(vary_batch_dedups_case_insensitively_within_batch) {
    auto response = make_response();
    // A batch listing the same field in DIFFERENT cases must collapse to one token:
    // the within-batch dedup is case-insensitive, matching the existing-header dedup.
    // (The prior batch test only used same-case repeats, so a regression to a
    // case-sensitive batch compare would have passed it.) The first-seen case wins.
    const std::string_view batch[] = {"Origin", "ORIGIN", "origin"};
    add_vary_tokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));

    // A differently-cased token already present is likewise not re-added.
    const std::string_view more[] = {"oRiGiN", "Accept-Encoding"};
    add_vary_tokens(response, more, 2);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_existing_wildcard_is_not_combined_with_field_names) {
    auto response = make_response();
    response.header("Vary", "*");

    add_vary_token(response, "Origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("*"));
}

RUVIA_TEST(vary_add_preserves_repeated_field_lines_in_combined_order) {
    auto response = make_response();
    response.header("Vary", "Origin");
    response.header("Vary", "Accept-Language",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    add_vary_token(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(
        response.header("Vary"), std::string_view("Origin, Accept-Language, Accept-Encoding"));
}

RUVIA_TEST(vary_add_dedups_against_later_repeated_field_line) {
    auto response = make_response();
    response.header("Vary", "Origin");
    response.header("Vary", "Accept-Encoding",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    add_vary_token(response, "accept-encoding");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(response.headers().begin()[0].value(), std::string_view("Origin"));
    RUVIA_CHECK_EQ(response.headers().begin()[1].value(), std::string_view("Accept-Encoding"));
}

RUVIA_TEST(vary_wildcard_in_later_repeated_field_line_dominates) {
    auto response = make_response();
    response.header("Vary", "Origin");
    response.header(
        "Vary", "*", http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    add_vary_token(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(response.headers().begin()[0].value(), std::string_view("Origin"));
    RUVIA_CHECK_EQ(response.headers().begin()[1].value(), std::string_view("*"));
}

RUVIA_TEST(vary_wildcard_in_batch_dominates_field_names) {
    auto response = make_response();
    const std::string_view batch[] = {"Origin", " * ", "Accept-Encoding"};

    add_vary_tokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("*"));
}
