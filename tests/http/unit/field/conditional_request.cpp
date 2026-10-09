#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/http_conditional_request.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"

#include "test_harness.h"

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 9110) for static file responses.

// Date formatting, cache framing, and locale-independent IMF-fixdate coverage
// live in date_parsing.cpp. This file covers precondition evaluation.

RUVIA_TEST(conditional_method_plan_follows_precondition_and_range_semantics) {
    using ruvia::http_conditional_method_plan;

    const auto get = get_http_conditional_method_plan(ruvia::http_known_method::get);
    RUVIA_CHECK(get.evaluates_preconditions_);
    RUVIA_CHECK(get.uses_not_modified_response_);
    RUVIA_CHECK(get.evaluates_if_modified_since_);
    RUVIA_CHECK(get.evaluates_range_);

    const auto head = get_http_conditional_method_plan(ruvia::http_known_method::head);
    RUVIA_CHECK(head.evaluates_preconditions_);
    RUVIA_CHECK(head.uses_not_modified_response_);
    RUVIA_CHECK(head.evaluates_if_modified_since_);
    RUVIA_CHECK(!head.evaluates_range_);

    for (const auto method : {ruvia::http_known_method::post, ruvia::http_known_method::put,
             ruvia::http_known_method::delete_value, ruvia::http_known_method::patch}) {
        const auto plan = get_http_conditional_method_plan(method);
        RUVIA_CHECK(plan.evaluates_preconditions_);
        RUVIA_CHECK(!plan.uses_not_modified_response_);
        RUVIA_CHECK(!plan.evaluates_if_modified_since_);
        RUVIA_CHECK(!plan.evaluates_range_);
    }

    for (const auto method : {ruvia::http_known_method::options, ruvia::http_known_method::connect,
             ruvia::http_known_method::unknown}) {
        const auto plan = get_http_conditional_method_plan(method);
        RUVIA_CHECK(!plan.evaluates_preconditions_);
        RUVIA_CHECK(!plan.uses_not_modified_response_);
        RUVIA_CHECK(!plan.evaluates_if_modified_since_);
        RUVIA_CHECK(!plan.evaluates_range_);
    }
}

RUVIA_TEST(http_date_precondition_comparisons) {
    using ruvia::http_date_not_modified;
    using ruvia::http_date_unmodified;

    constexpr std::time_t canonical{784111777};
    const auto later = canonical + 1;
    const auto earlier = canonical - 1;
    constexpr auto header_value = "Sun, 06 Nov 1994 08:49:37 GMT";

    RUVIA_CHECK(http_date_not_modified(header_value, canonical));
    RUVIA_CHECK(http_date_not_modified(header_value, earlier));
    RUVIA_CHECK(!http_date_not_modified(header_value, later));
    RUVIA_CHECK(!http_date_not_modified("not-a-date", canonical));

    RUVIA_CHECK(http_date_unmodified(header_value, canonical));
    RUVIA_CHECK(http_date_unmodified(header_value, earlier));
    RUVIA_CHECK(!http_date_unmodified(header_value, later));
    RUVIA_CHECK(http_date_unmodified("not-a-date", later));
}

RUVIA_TEST(http_if_range_requires_exact_validator) {
    using ruvia::http_if_range_allows;

    constexpr std::time_t canonical{784111777};
    constexpr auto etag = R"("abc")";
    constexpr auto date = "Sun, 06 Nov 1994 08:49:37 GMT";

    RUVIA_CHECK(!http_if_range_allows("", etag, canonical, true));
    RUVIA_CHECK(http_if_range_allows(etag, etag, canonical, true));
    RUVIA_CHECK(!http_if_range_allows(R"(W/"abc")", etag, canonical, true));
    RUVIA_CHECK(http_if_range_allows(date, etag, canonical, true));
    RUVIA_CHECK(!http_if_range_allows(date, etag, canonical + 1, true));
    RUVIA_CHECK(!http_if_range_allows(date, etag, canonical, false));
    RUVIA_CHECK(!http_if_range_allows(date, etag, std::nullopt, true));
    RUVIA_CHECK(http_if_range_allows(etag, etag, std::nullopt, false));
}

RUVIA_TEST(http_etag_preconditions_fold_repeated_field_lines) {
    using ruvia::http_etag_preconditions;
    using ruvia::http_header_view;

    const http_header_view fields[]{{"If-None-Match", R"("current")"}, {"If-None-Match", R"("stale")"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/", fields, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());

    const auto matched = get_http_etag_preconditions(request, R"("current")");
    RUVIA_CHECK(matched.if_none_match_.present_);
    RUVIA_CHECK(matched.if_none_match_.matches());
    RUVIA_CHECK_EQ(matched.if_none_match_.line_count_, std::size_t{2});

    const http_header_view wildcard_fields[]{{"If-Match", "*"}};
    auto [wildcard, wildcard_error] = ruvia::make_parsed_http_request(
        "GET", "/", wildcard_fields, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!wildcard_error.has_value());
    const auto existence = get_http_etag_preconditions(wildcard, R"("unused")");
    RUVIA_CHECK(existence.if_match_.present_);
    RUVIA_CHECK(existence.if_match_.matches());
    RUVIA_CHECK(existence.if_match_.wildcard_);
}
