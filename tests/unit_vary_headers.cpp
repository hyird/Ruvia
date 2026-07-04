#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "http/ResponseHeaderUtils.h"
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
