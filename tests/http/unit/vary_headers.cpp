#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/ResponseHeaderUtils.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::addVaryToken;
using ruvia::detail::addVaryTokens;

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(vary_adds_single_then_appends) {
    auto response = makeResponse();
    addVaryToken(response, "Origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
    addVaryToken(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_dedups_against_existing_case_insensitively) {
    auto response = makeResponse();
    addVaryToken(response, "Origin");
    // An already-present token (matched case-insensitively) is not appended again.
    addVaryToken(response, "origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
}

RUVIA_TEST(vary_batch_dedups_within_batch_and_existing) {
    auto response = makeResponse();
    addVaryToken(response, "Origin");
    // The batch repeats a token and re-lists an existing one; each survivor is
    // added exactly once.
    const std::string_view batch[] = {"Origin", "Accept-Encoding", "Accept-Encoding"};
    addVaryTokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_skips_empty_tokens_and_null_batch) {
    auto response = makeResponse();
    const std::string_view batch[] = {std::string_view(), "Origin", std::string_view()};
    addVaryTokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
    // A null pointer or zero count is a no-op, not a crash.
    addVaryTokens(response, nullptr, 0);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));
}

RUVIA_TEST(vary_batch_dedups_case_insensitively_within_batch) {
    auto response = makeResponse();
    // A batch listing the same field in DIFFERENT cases must collapse to one token:
    // the within-batch dedup is case-insensitive, matching the existing-header dedup.
    // (The prior batch test only used same-case repeats, so a regression to a
    // case-sensitive batch compare would have passed it.) The first-seen case wins.
    const std::string_view batch[] = {"Origin", "ORIGIN", "origin"};
    addVaryTokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin"));

    // A differently-cased token already present is likewise not re-added.
    const std::string_view more[] = {"oRiGiN", "Accept-Encoding"};
    addVaryTokens(response, more, 2);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
}

RUVIA_TEST(vary_existing_wildcard_is_not_combined_with_field_names) {
    auto response = makeResponse();
    response.header("Vary", "*");

    addVaryToken(response, "Origin");
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("*"));
}

RUVIA_TEST(vary_add_preserves_repeated_field_lines_in_combined_order) {
    auto response = makeResponse();
    response.header("Vary", "Origin");
    response.header(
        "Vary",
        "Accept-Language",
        HttpResponse::HeaderOptions{.append = true});

    addVaryToken(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(
        response.header("Vary"),
        std::string_view("Origin, Accept-Language, Accept-Encoding"));
}

RUVIA_TEST(vary_add_dedups_against_later_repeated_field_line) {
    auto response = makeResponse();
    response.header("Vary", "Origin");
    response.header(
        "Vary",
        "Accept-Encoding",
        HttpResponse::HeaderOptions{.append = true});

    addVaryToken(response, "accept-encoding");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(response.headers().begin()[0].value(), std::string_view("Origin"));
    RUVIA_CHECK_EQ(
        response.headers().begin()[1].value(),
        std::string_view("Accept-Encoding"));
}

RUVIA_TEST(vary_wildcard_in_later_repeated_field_line_dominates) {
    auto response = makeResponse();
    response.header("Vary", "Origin");
    response.header(
        "Vary",
        "*",
        HttpResponse::HeaderOptions{.append = true});

    addVaryToken(response, "Accept-Encoding");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(response.headers().begin()[0].value(), std::string_view("Origin"));
    RUVIA_CHECK_EQ(response.headers().begin()[1].value(), std::string_view("*"));
}

RUVIA_TEST(vary_wildcard_in_batch_dominates_field_names) {
    auto response = makeResponse();
    const std::string_view batch[] = {
        "Origin", " * ", "Accept-Encoding"};

    addVaryTokens(response, batch, 3);
    RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("*"));
}
