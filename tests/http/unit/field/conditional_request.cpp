#include <ctime>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_representation_response_plan.h"
#include "ruvia/http/http_request.h"

#include "test_harness.h"

// Conditional-request evaluation (If-Match / If-None-Match / If-Modified-Since /
// If-Unmodified-Since / If-Range, RFC 9110 section 13) observed through
// plan_http_representation_response. Date formatting and parsing coverage lives
// in date_parsing.cpp; Range resolution lives in representation_response_plan.cpp.

namespace {

using ruvia::http_header_view;
using ruvia::http_representation_response_plan;
using ruvia::http_selected_representation_metadata;
namespace http_status = ruvia::http_status;

constexpr std::time_t canonical{784111777};
constexpr std::string_view canonical_date = "Sun, 06 Nov 1994 08:49:37 GMT";

constexpr http_selected_representation_metadata representation{
    .length_ = 10,
    .etag_ = R"("abc")",
    .last_modified_ = canonical,
    .strong_date_validator_ = true,
};

constexpr ruvia::http_representation_response_options honor_ranges{
    .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};

http_representation_response_plan plan(std::string_view method, const std::vector<http_header_view>& fields,
    http_selected_representation_metadata metadata = representation,
    ruvia::http_representation_response_options options = {}) {
    const auto request = ruvia::make_parsed_http_request(method, method == "CONNECT" ? "example.test:443" : "/",
        fields, {}, std::pmr::get_default_resource());
    return ruvia::plan_http_representation_response(request, metadata, options);
}

http_selected_representation_metadata modified_at(std::optional<std::time_t> last_modified) {
    auto metadata = representation;
    metadata.last_modified_ = last_modified;
    return metadata;
}

// The plan accessors are lvalue-only; these read one alternative of a plan.
[[nodiscard]] bool full(const http_representation_response_plan& value) {
    return value.full() != nullptr;
}
[[nodiscard]] bool partial(const http_representation_response_plan& value) {
    return value.partial() != nullptr;
}
[[nodiscard]] bool not_modified(const http_representation_response_plan& value) {
    return value.not_modified() != nullptr;
}
[[nodiscard]] bool precondition_failed(const http_representation_response_plan& value) {
    return value.precondition_failed() != nullptr;
}

}  // namespace

RUVIA_TEST(conditional_methods_follow_precondition_and_range_semantics) {
    const std::vector<http_header_view> failed_match{{"If-Match", R"("stale")"}};
    const std::vector<http_header_view> matched_none{{"If-None-Match", R"("abc")"}};
    const std::vector<http_header_view> not_modified_since{{"If-Modified-Since", canonical_date}};
    const std::vector<http_header_view> range{{"Range", "bytes=2-4"}};

    for (const auto method : {"GET", "HEAD"}) {
        RUVIA_CHECK(precondition_failed(plan(method, failed_match)));
        RUVIA_CHECK(not_modified(plan(method, matched_none)));
        RUVIA_CHECK(not_modified(plan(method, not_modified_since)));
    }
    RUVIA_CHECK(partial(plan("GET", range, representation, honor_ranges)));
    RUVIA_CHECK(full(plan("HEAD", range, representation, honor_ranges)));

    // State-changing and extension methods evaluate preconditions, but answer a
    // matched If-None-Match with 412 and ignore If-Modified-Since and Range.
    for (const auto method : {"POST", "PUT", "DELETE", "PATCH", "PROPFIND"}) {
        RUVIA_CHECK(precondition_failed(plan(method, failed_match)));
        RUVIA_CHECK(precondition_failed(plan(method, matched_none)));
        RUVIA_CHECK(full(plan(method, not_modified_since)));
        RUVIA_CHECK(full(plan(method, range, representation, honor_ranges)));
    }

    for (const auto method : {"OPTIONS", "CONNECT", "TRACE"}) {
        RUVIA_CHECK(full(plan(method, failed_match)));
        RUVIA_CHECK(full(plan(method, matched_none)));
        RUVIA_CHECK(full(plan(method, not_modified_since)));
    }
}

RUVIA_TEST(conditional_date_comparisons) {
    const auto later = modified_at(canonical + 1);
    const auto earlier = modified_at(canonical - 1);
    const std::vector<http_header_view> modified_since{{"If-Modified-Since", canonical_date}};
    const std::vector<http_header_view> unmodified_since{{"If-Unmodified-Since", canonical_date}};

    // If-Modified-Since: 304 when the representation was modified at or before the date.
    RUVIA_CHECK(not_modified(plan("GET", modified_since)));
    RUVIA_CHECK(not_modified(plan("GET", modified_since, earlier)));
    RUVIA_CHECK(full(plan("GET", modified_since, later)));
    RUVIA_CHECK(full(plan("GET", {{"If-Modified-Since", "not-a-date"}})));

    // If-Unmodified-Since: 412 only when the representation is newer; a malformed date is ignored.
    RUVIA_CHECK(full(plan("GET", unmodified_since)));
    RUVIA_CHECK(full(plan("GET", unmodified_since, earlier)));
    RUVIA_CHECK(precondition_failed(plan("GET", unmodified_since, later)));
    RUVIA_CHECK(full(plan("GET", {{"If-Unmodified-Since", "not-a-date"}}, later)));
}

RUVIA_TEST(conditional_if_range_requires_exact_validator) {
    const auto if_range = [](std::string_view value, http_selected_representation_metadata metadata = representation) {
        return plan("GET", {{"Range", "bytes=2-4"}, {"If-Range", value}}, metadata, honor_ranges);
    };
    const auto weak_dates = [] {
        auto metadata = representation;
        metadata.strong_date_validator_ = false;
        return metadata;
    }();
    const auto undated_weak = [] {
        auto metadata = representation;
        metadata.last_modified_ = std::nullopt;
        metadata.strong_date_validator_ = false;
        return metadata;
    }();

    RUVIA_CHECK(full(if_range("")));
    const auto matched = if_range(R"("abc")");
    RUVIA_CHECK(partial(matched));
    RUVIA_CHECK(matched.authorized_by_if_range());
    RUVIA_CHECK_EQ(matched.status(), http_status::partial_content);
    RUVIA_CHECK(full(if_range(R"(W/"abc")")));
    RUVIA_CHECK(partial(if_range(canonical_date)));
    RUVIA_CHECK(full(if_range(canonical_date, modified_at(canonical + 1))));
    RUVIA_CHECK(full(if_range(canonical_date, weak_dates)));
    RUVIA_CHECK(full(if_range(canonical_date, modified_at(std::nullopt))));
    RUVIA_CHECK(partial(if_range(R"("abc")", undated_weak)));
}

RUVIA_TEST(conditional_etag_comparison_strength) {
    // If-Match compares strongly; If-None-Match compares weakly.
    RUVIA_CHECK(precondition_failed(plan("GET", {{"If-Match", R"(W/"abc")"}})));
    RUVIA_CHECK(full(plan("GET", {{"If-Match", R"("abc")"}})));
    RUVIA_CHECK(not_modified(plan("GET", {{"If-None-Match", R"(W/"abc")"}})));
}

RUVIA_TEST(conditional_etag_preconditions_fold_repeated_field_lines) {
    RUVIA_CHECK(not_modified(plan("GET", {{"If-None-Match", R"("abc")"}, {"If-None-Match", R"("stale")"}})));
    RUVIA_CHECK(not_modified(plan("GET", {{"If-None-Match", R"("stale")"}, {"If-None-Match", R"("abc")"}})));

    // A wildcard If-Match matches any current representation.
    const auto existence = plan("GET", {{"If-Match", "*"}}, modified_at(std::nullopt));
    RUVIA_CHECK(full(existence));
    RUVIA_CHECK_EQ(existence.status(), http_status::ok);
}
