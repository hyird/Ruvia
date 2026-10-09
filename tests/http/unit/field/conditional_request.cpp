#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/http_conditional_request.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"

#include "field/http_entity_tag.h"
#include "request/http_request_access.h"
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

RUVIA_TEST(etag_strong_comparison) {
    using ruvia::detail::http_strong_etag_equals;
    // Strong compare: both must be strong (no "W/") and octet-equal.
    RUVIA_CHECK(http_strong_etag_equals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!http_strong_etag_equals(R"("abc")", R"("abd")"));    // different tag
    RUVIA_CHECK(!http_strong_etag_equals(R"(W/"abc")", R"("abc")"));  // one weak -> never strong-equal
    RUVIA_CHECK(
        !http_strong_etag_equals(R"(W/"abc")", R"(W/"abc")"));  // both weak -> not a strong match
}

RUVIA_TEST(etag_weak_comparison) {
    using ruvia::detail::http_weak_etag_equals;
    // Weak compare: equal after stripping an optional "W/" prefix.
    RUVIA_CHECK(http_weak_etag_equals(R"(W/"abc")", R"("abc")"));
    RUVIA_CHECK(http_weak_etag_equals(R"(W/"abc")", R"(W/"abc")"));
    RUVIA_CHECK(http_weak_etag_equals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!http_weak_etag_equals(R"(W/"abc")", R"(W/"abd")"));  // different opaque tags
}

RUVIA_TEST(etag_weak_prefix_detection) {
    using ruvia::detail::http_is_weak_etag;
    RUVIA_CHECK(http_is_weak_etag(R"(W/"abc")"));
    RUVIA_CHECK(!http_is_weak_etag(R"("abc")"));
    RUVIA_CHECK(!http_is_weak_etag("W"));  // too short to be the "W/" marker
}

RUVIA_TEST(etag_list_parses_opaque_commas_and_rejects_malformed_suffixes) {
    using ruvia::detail::http_etag_list_matches;
    using ruvia::detail::http_parse_etag_list_matches;
    RUVIA_CHECK(http_etag_list_matches(R"("stale,tag", "current")", R"("current")", true));
    RUVIA_CHECK(http_etag_list_matches(R"("stale", W/"current")", R"("current")", false));
    RUVIA_CHECK(!http_etag_list_matches(R"("stale, "current")", R"("current")", true));
    RUVIA_CHECK(!http_etag_list_matches(R"("current" trailing)", R"("current")", false));
    const auto malformed_after_match =
        http_parse_etag_list_matches(R"("current", malformed)", R"("current")", true);
    RUVIA_CHECK(!malformed_after_match.valid_);
    RUVIA_CHECK(!malformed_after_match.matched_);
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
    using ruvia::detail::http_request_access;
    using ruvia::detail::request_header_kind;

    auto request = http_request_access::make();
    const auto none_match_slot = http_request_access::known_header_slot(request_header_kind::if_none_match);
    RUVIA_CHECK(http_request_access::add_header(
        request, http_header_view("If-None-Match", R"("current")"), none_match_slot));
    RUVIA_CHECK(http_request_access::add_header(
        request, http_header_view("If-None-Match", R"("stale")"), none_match_slot));

    const auto matched = get_http_etag_preconditions(request, R"("current")");
    RUVIA_CHECK(matched.if_none_match_.present_);
    RUVIA_CHECK(matched.if_none_match_.matches());
    RUVIA_CHECK_EQ(matched.if_none_match_.line_count_, std::size_t{2});

    auto wildcard = http_request_access::make();
    RUVIA_CHECK(http_request_access::add_header(
        wildcard, http_header_view("If-Match", "*"),
        http_request_access::known_header_slot(request_header_kind::if_match)));
    const auto existence = get_http_etag_preconditions(wildcard, R"("unused")");
    RUVIA_CHECK(existence.if_match_.present_);
    RUVIA_CHECK(existence.if_match_.matches());
    RUVIA_CHECK(existence.if_match_.wildcard_);
}
