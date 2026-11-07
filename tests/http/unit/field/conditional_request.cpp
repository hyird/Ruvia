#include <gtest/gtest.h>

#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/http_conditional_request.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"

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

    for (const auto method : {ruvia::http_known_method::post, ruvia::http_known_method::put,
             ruvia::http_known_method::delete_value, ruvia::http_known_method::patch}) {
        const auto plan = get_http_conditional_method_plan(method);
        EXPECT_TRUE(plan.evaluates_preconditions_);
        EXPECT_TRUE(!plan.uses_not_modified_response_);
        EXPECT_TRUE(!plan.evaluates_if_modified_since_);
        EXPECT_TRUE(!plan.evaluates_range_);
    }

    for (const auto method : {ruvia::http_known_method::options, ruvia::http_known_method::connect,
             ruvia::http_known_method::unknown}) {
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
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", fields, {}, std::pmr::get_default_resource());

    const auto matched = get_http_etag_preconditions(request, R"("current")");
    EXPECT_TRUE(matched.if_none_match_.present_);
    EXPECT_TRUE(matched.if_none_match_.matches());

    const http_header_view wildcard_fields[]{{"If-Match", "*"}};
    const auto wildcard = ruvia::make_parsed_http_request(
        "GET", "/", wildcard_fields, {}, std::pmr::get_default_resource());
    const auto existence = get_http_etag_preconditions(wildcard, R"("unused")");
    EXPECT_TRUE(existence.if_match_.present_);
    EXPECT_TRUE(existence.if_match_.matches());
    EXPECT_TRUE(existence.if_match_.wildcard_);
}

namespace {

template <typename request_type>
concept conditional_headers_accepts_request = requires(request_type&& request) {
    ruvia::get_http_conditional_headers(std::forward<request_type>(request));
};

static_assert(conditional_headers_accepts_request<const ruvia::http_request&>);
static_assert(!conditional_headers_accepts_request<ruvia::http_request>);

}  // namespace
