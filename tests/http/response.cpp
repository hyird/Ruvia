#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "test_exceptions.h"

namespace {

using ruvia::http_header_view;
using ruvia::http_request;

http_request request(std::string_view method) {
    return ruvia::make_parsed_http_request(
        method, method == "CONNECT" ? "example.test:443" : "/", {}, {});
}

void add(http_request& req, std::string_view name, std::string_view value) {
    std::vector<http_header_view> fields(req.headers().begin(), req.headers().end());
    fields.emplace_back(name, value);
    req = ruvia::make_parsed_http_request(
        req.method(), req.target(), fields, req.body_bytes());
}

constexpr ruvia::http_selected_representation_metadata representation{
    .length_ = 10,
    .etag_ = R"("v1")",
    .last_modified_ = std::time_t{784111777},
    .strong_date_validator_ = true,
};

constexpr std::string_view representation_date = "Sun, 06 Nov 1994 08:49:37 GMT";

ruvia::http_status_code planned_status(std::string_view method, std::string_view name, std::string_view value,
    const ruvia::http_selected_representation_metadata& metadata = representation,
    ruvia::http_representation_response_options options = {}) {
    auto req = request(method);
    add(req, name, value);
    return ruvia::plan_http_representation_response(req, metadata, options).status();
}

}  // namespace

TEST(Ruvia, representation_response_plan_only_evaluates_conditions_for_eligible_responses) {
    for (const auto status : {ruvia::http_status::not_found, ruvia::http_status::temporary_redirect}) {
        auto req = request("GET");
        add(req, "If-None-Match", R"("v1")");
        add(req, "If-Match", R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(
            req, representation, {.normal_status_ = status});
        EXPECT_TRUE(plan.full() != nullptr);
        EXPECT_EQ(plan.status(), status);
    }

    auto created = request("GET");
    add(created, "Range", "bytes=1-2");
    const auto created_plan = ruvia::plan_http_representation_response(
        created, representation, {.normal_status_ = ruvia::http_status::created, .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
    EXPECT_TRUE(created_plan.full() != nullptr);
    EXPECT_EQ(created_plan.status(), ruvia::http_status::created);
}

TEST(Ruvia, representation_response_plan_resolves_supported_range_outcomes) {
    const auto options = ruvia::http_representation_response_options{
        .normal_status_ = ruvia::http_status::ok,
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    auto partial_req = request("GET");
    add(partial_req, "Range", "bytes=2-4");
    const auto partial = ruvia::plan_http_representation_response(partial_req, representation, options);
    EXPECT_TRUE(partial.partial() != nullptr);
    if (const auto* range = partial.partial()) {
        EXPECT_EQ(range->offset_, std::uint64_t{2});
        EXPECT_EQ(range->length_, std::uint64_t{3});
    }
    EXPECT_EQ(partial.status(), ruvia::http_status::partial_content);

    auto unsat_req = request("GET");
    add(unsat_req, "Range", "bytes=20-");
    const auto unsat = ruvia::plan_http_representation_response(unsat_req, representation, options);
    EXPECT_TRUE(unsat.range_unsatisfiable() != nullptr);
    EXPECT_EQ(unsat.status(), ruvia::http_status::range_not_satisfiable);

    const auto empty = ruvia::plan_http_representation_response(partial_req, {}, options);
    EXPECT_TRUE(empty.range_unsatisfiable() != nullptr);
    EXPECT_EQ(empty.status(), ruvia::http_status::range_not_satisfiable);
    const auto disabled = ruvia::plan_http_representation_response(partial_req, representation);
    EXPECT_TRUE(disabled.full() != nullptr);

    for (const auto range : {"items=1-2", "bytes=garbage", "bytes=", ""}) {
        auto req = request("GET");
        add(req, "Range", range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, options);
        EXPECT_TRUE(plan.full() != nullptr);
    }
    auto multi_req = request("GET");
    add(multi_req, "Range", "bytes=1-2,4-5");
    const auto multi = ruvia::plan_http_representation_response(multi_req, representation, options);
    EXPECT_TRUE(multi.multipart_ranges() != nullptr);
    EXPECT_EQ(multi.status(), ruvia::http_status::partial_content);
    if (const auto* ranges = multi.multipart_ranges()) {
        static_assert(std::is_same_v<decltype(ranges),
            const ruvia::http_representation_response_plan::multipart_type*>);
        EXPECT_EQ(ranges->size(), std::size_t{2});
    }
}

TEST(Ruvia, representation_response_plan_preserves_unsatisfiable_ranges_after_empty_members) {
    const auto options = ruvia::http_representation_response_options{
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"20-", "20-29", "-0"}) {
        const auto value = prefix + tail;
        auto req = request("GET");
        add(req, "Range", value);
        const auto plan = ruvia::plan_http_representation_response(req, representation, options);
        EXPECT_TRUE(plan.range_unsatisfiable() != nullptr);
        EXPECT_EQ(plan.status(), ruvia::http_status::range_not_satisfiable);
    }
    const auto over_limit = prefix + ",20-";
    auto req = request("GET");
    add(req, "Range", over_limit);
    const auto plan = ruvia::plan_http_representation_response(req, representation, options);
    EXPECT_TRUE(plan.full() != nullptr);
    EXPECT_EQ(plan.status(), ruvia::http_status::ok);
}

TEST(Ruvia, representation_response_plan_obeys_method_precondition_and_presence_precedence) {
    for (const auto method : {"GET", "HEAD", "POST"}) {
        auto req = request(method);
        add(req, "If-Match", R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation);
        EXPECT_TRUE(plan.precondition_failed() != nullptr);
    }
    for (const auto method : {"OPTIONS", "CONNECT"}) {
        auto req = request(method);
        add(req, "If-Match", R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation);
        EXPECT_TRUE(plan.full() != nullptr);
    }

    auto precedence = request("GET");
    add(precedence, "If-Match", R"("v1")");
    add(precedence, "If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT");
    add(precedence, "If-None-Match", R"("stale")");
    add(precedence, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto precedence_plan = ruvia::plan_http_representation_response(precedence, representation);
    EXPECT_TRUE(precedence_plan.full() != nullptr);

    auto repeated = request("GET");
    add(repeated, "If-None-Match", R"("stale")");
    add(repeated, "If-None-Match", R"("v1")");
    const auto repeated_plan = ruvia::plan_http_representation_response(repeated, representation);
    EXPECT_TRUE(repeated_plan.not_modified() != nullptr);

    auto empty_none_match = request("GET");
    add(empty_none_match, "If-None-Match", "");
    add(empty_none_match, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto empty_none_match_plan = ruvia::plan_http_representation_response(empty_none_match, representation);
    EXPECT_TRUE(empty_none_match_plan.full() != nullptr);

    auto failed_first = request("GET");
    add(failed_first, "If-Match", R"("stale")");
    add(failed_first, "If-None-Match", R"("v1")");
    add(failed_first, "Range", "bytes=2-4");
    const auto failed_first_plan = ruvia::plan_http_representation_response(failed_first, representation,
        {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
    EXPECT_TRUE(failed_first_plan.precondition_failed() != nullptr);
    EXPECT_EQ(failed_first_plan.status(), ruvia::http_status::precondition_failed);
}

TEST(Ruvia, representation_response_plan_evaluates_supported_representation_extension_methods) {
    for (const auto method : {"UPDATE", "MERGE", "get", "trace"}) {
        auto exists = request(method);
        add(exists, "If-None-Match", "*");
        const auto exists_plan = ruvia::plan_http_representation_response(exists, {.length_ = 10});
        EXPECT_EQ(exists_plan.status(), ruvia::http_status::precondition_failed);

        auto none_match = request(method);
        add(none_match, "If-None-Match", R"(W/"v1")");
        const auto none_match_plan = ruvia::plan_http_representation_response(none_match, representation);
        EXPECT_TRUE(none_match_plan.precondition_failed() != nullptr);

        auto match = request(method);
        add(match, "If-Match", R"(W/"v1")");
        const auto match_plan = ruvia::plan_http_representation_response(match, representation);
        EXPECT_TRUE(match_plan.precondition_failed() != nullptr);

        auto unmodified = request(method);
        add(unmodified, "If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT");
        const auto unmodified_plan = ruvia::plan_http_representation_response(unmodified, representation);
        EXPECT_TRUE(unmodified_plan.precondition_failed() != nullptr);

        auto matched = request(method);
        add(matched, "If-Match", R"("v1")");
        add(matched, "If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT");
        add(matched, "If-None-Match", R"("stale")");
        add(matched, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:38 GMT");
        add(matched, "Range", "bytes=2-4");
        const auto matched_plan = ruvia::plan_http_representation_response(matched, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        EXPECT_TRUE(matched_plan.full() != nullptr);
        EXPECT_EQ(matched_plan.status(), ruvia::http_status::ok);

        auto modified_since = request(method);
        add(modified_since, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:38 GMT");
        const auto modified_since_plan = ruvia::plan_http_representation_response(modified_since, representation);
        EXPECT_EQ(modified_since_plan.status(), ruvia::http_status::ok);

        auto range = request(method);
        add(range, "Range", "bytes=2-4");
        add(range, "If-Range", R"("v1")");
        const auto range_plan = ruvia::plan_http_representation_response(range, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        EXPECT_TRUE(range_plan.full() != nullptr);
        EXPECT_EQ(range_plan.status(), ruvia::http_status::ok);

        auto wildcard_match = request(method);
        add(wildcard_match, "If-Match", "*");
        const auto wildcard_match_plan = ruvia::plan_http_representation_response(wildcard_match, {.length_ = 10});
        EXPECT_EQ(wildcard_match_plan.status(), ruvia::http_status::ok);

        const auto no_content = ruvia::plan_http_representation_response(exists, representation,
            {.normal_status_ = ruvia::http_status::no_content});
        EXPECT_EQ(no_content.status(), ruvia::http_status::precondition_failed);

        const auto precondition = ruvia::plan_http_representation_response(matched, representation,
            {.normal_status_ = ruvia::http_status::precondition_failed});
        EXPECT_TRUE(precondition.full() != nullptr);
        EXPECT_EQ(precondition.status(), ruvia::http_status::precondition_failed);
    }
}

TEST(Ruvia, representation_response_plan_ignores_extension_conditions_for_ineligible_responses) {
    for (const auto status : {ruvia::http_status::not_found, ruvia::http_status::not_implemented,
             ruvia::http_status::method_not_allowed, ruvia::http_status::temporary_redirect}) {
        auto req = request("UPDATE");
        add(req, "If-None-Match", "*");
        add(req, "If-Match", R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation, {.normal_status_ = status});
        EXPECT_TRUE(plan.full() != nullptr);
        EXPECT_EQ(plan.status(), status);
    }
}

TEST(Ruvia, representation_response_plan_uses_last_modified_only_when_present_and_if_range_is_strong) {
    auto not_modified = request("GET");
    add(not_modified, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto not_modified_plan = ruvia::plan_http_representation_response(not_modified, representation);
    EXPECT_TRUE(not_modified_plan.not_modified() != nullptr);

    auto absent_date = request("GET");
    add(absent_date, "If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto absent_date_plan = ruvia::plan_http_representation_response(absent_date,
        {.length_ = representation.length_, .etag_ = representation.etag_});
    EXPECT_TRUE(absent_date_plan.full() != nullptr);

    const auto range_options = ruvia::http_representation_response_options{
        .normal_status_ = ruvia::http_status::ok,
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    for (const auto if_range : {R"("v1")", "Sun, 06 Nov 1994 08:49:37 GMT"}) {
        auto req = request("GET");
        add(req, "Range", "bytes=1-2");
        add(req, "If-Range", if_range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, range_options);
        EXPECT_TRUE(plan.partial() != nullptr);
    }
    for (const auto if_range : {R"(W/"v1")", "Sun, 06 Nov 1994 08:49:38 GMT", ""}) {
        auto req = request("GET");
        add(req, "Range", "bytes=1-2");
        add(req, "If-Range", if_range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, range_options);
        EXPECT_TRUE(plan.full() != nullptr);
    }
    auto missing_date = request("GET");
    add(missing_date, "Range", "bytes=1-2");
    add(missing_date, "If-Range", "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto missing_date_plan = ruvia::plan_http_representation_response(missing_date,
        {.length_ = representation.length_, .etag_ = representation.etag_}, range_options);
    EXPECT_TRUE(missing_date_plan.full() != nullptr);
    auto weak_date_metadata = representation;
    weak_date_metadata.strong_date_validator_ = false;
    const auto weak_date_plan = ruvia::plan_http_representation_response(missing_date, weak_date_metadata, range_options);
    EXPECT_TRUE(weak_date_plan.full() != nullptr);

    auto tag_only = request("GET");
    add(tag_only, "Range", "bytes=1-2");
    add(tag_only, "If-Range", R"("v1")");
    const auto tag_only_plan = ruvia::plan_http_representation_response(tag_only,
        {.length_ = 10, .etag_ = representation.etag_}, range_options);
    EXPECT_TRUE(tag_only_plan.partial() != nullptr);
}

TEST(Ruvia, representation_response_plan_ignores_repeated_date_conditions) {
    for (const auto field : {
             http_header_view{"If-Modified-Since", "Sun, 06 Nov 1994 08:49:38 GMT"},
             http_header_view{"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT"}}) {
        for (const std::string_view method : {"GET", "HEAD", "POST"}) {
            for (const auto first_value : {std::string_view{}, field.value()}) {
                auto req = request(method);
                add(req, field.name(), first_value);
                add(req, field.name(), field.value());
                const auto semantic = ruvia::plan_http_representation_response(req, representation);
                EXPECT_TRUE(semantic.full() != nullptr);
                EXPECT_EQ(semantic.status(), ruvia::http_status::ok);

                std::string wire(method);
                wire.append(" / HTTP/1.1\r\nHost: example.test\r\n");
                wire.append(field.name()).append(": ").append(first_value).append("\r\n");
                wire.append(field.name()).append(": ").append(field.value()).append("\r\n\r\n");
                ruvia::http1_server_request_parse_state parsed;
                parsed.parse_head(wire, 0);
                EXPECT_TRUE(parsed.head_ready() != nullptr);
                if (parsed.head_ready() != nullptr) {
                    const auto plan = ruvia::plan_http_representation_response(parsed.request(), representation);
                    EXPECT_TRUE(plan.full() != nullptr);
                    EXPECT_EQ(plan.status(), ruvia::http_status::ok);
                }
            }
        }
    }
}

TEST(Ruvia, representation_response_plan_checks_existing_representation_wildcards_and_method_roles) {
    for (const auto method : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "CONNECT", "TRACE"}) {
        auto req = request(method);
        add(req, "If-None-Match", "*");
        add(req, "Range", "bytes=2-4");
        const auto plan = ruvia::plan_http_representation_response(req,
            {.length_ = 10}, {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        if (std::string_view(method) == "GET" || std::string_view(method) == "HEAD") {
            EXPECT_TRUE(plan.not_modified() != nullptr);
            EXPECT_EQ(plan.status(), ruvia::http_status::not_modified);
        } else if (std::string_view(method) == "OPTIONS" || std::string_view(method) == "CONNECT" || std::string_view(method) == "TRACE") {
            EXPECT_TRUE(plan.full() != nullptr);
        } else {
            EXPECT_TRUE(plan.precondition_failed() != nullptr);
        }
    }
    for (const auto method : {"HEAD", "POST"}) {
        auto req = request(method);
        add(req, "Range", "bytes=2-4");
        const auto plan = ruvia::plan_http_representation_response(req, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        EXPECT_TRUE(plan.full() != nullptr);
    }
    auto created = request("GET");
    add(created, "If-None-Match", "*");
    const auto created_plan = ruvia::plan_http_representation_response(created, representation,
        {.normal_status_ = ruvia::http_status::created});
    EXPECT_TRUE(created_plan.not_modified() != nullptr);
    auto precondition = request("GET");
    add(precondition, "If-Match", R"("v1")");
    const auto precondition_plan = ruvia::plan_http_representation_response(precondition, representation,
        {.normal_status_ = ruvia::http_status::precondition_failed});
    EXPECT_TRUE(precondition_plan.full() != nullptr);
    EXPECT_EQ(precondition_plan.status(), ruvia::http_status::precondition_failed);
}

TEST(Ruvia, representation_response_plan_respects_date_failures_and_strong_versus_weak_tag_comparison) {
    auto unmodified = request("GET");
    add(unmodified, "If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT");
    const auto failed = ruvia::plan_http_representation_response(unmodified, representation);
    EXPECT_TRUE(failed.precondition_failed() != nullptr);
    const auto unavailable = ruvia::plan_http_representation_response(unmodified, {.length_ = 10});
    EXPECT_TRUE(unavailable.full() != nullptr);
    auto malformed = request("GET");
    add(malformed, "If-Unmodified-Since", "invalid-date");
    add(malformed, "If-Modified-Since", "invalid-date");
    const auto ignored = ruvia::plan_http_representation_response(malformed, representation);
    EXPECT_TRUE(ignored.full() != nullptr);
    auto strong = request("GET");
    add(strong, "If-Match", R"(W/"v1")");
    const auto strong_plan = ruvia::plan_http_representation_response(strong, representation);
    EXPECT_TRUE(strong_plan.precondition_failed() != nullptr);
    auto weak = request("GET");
    add(weak, "If-None-Match", R"(W/"v1")");
    const auto weak_plan = ruvia::plan_http_representation_response(weak, representation);
    EXPECT_TRUE(weak_plan.not_modified() != nullptr);
}

TEST(Ruvia, representation_response_plan_applies_each_condition_by_method_role) {
    const ruvia::http_representation_response_options honor_ranges{
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    for (const auto method : {"GET", "HEAD"}) {
        EXPECT_EQ(planned_status(method, "If-None-Match", R"("v1")"), ruvia::http_status::not_modified);
        EXPECT_EQ(
            planned_status(method, "If-Modified-Since", representation_date), ruvia::http_status::not_modified);
    }

    // State-changing and extension methods evaluate preconditions, but answer a
    // matched If-None-Match with 412 and ignore If-Modified-Since and Range.
    for (const auto method : {"POST", "PUT", "DELETE", "PATCH", "PROPFIND"}) {
        EXPECT_EQ(planned_status(method, "If-Match", R"("stale")"), ruvia::http_status::precondition_failed);
        EXPECT_EQ(planned_status(method, "If-None-Match", R"("v1")"), ruvia::http_status::precondition_failed);
        EXPECT_EQ(planned_status(method, "If-Modified-Since", representation_date), ruvia::http_status::ok);
        EXPECT_EQ(
            planned_status(method, "Range", "bytes=2-4", representation, honor_ranges), ruvia::http_status::ok);
    }

    for (const auto method : {"OPTIONS", "CONNECT", "TRACE"}) {
        EXPECT_EQ(planned_status(method, "If-Match", R"("stale")"), ruvia::http_status::ok);
        EXPECT_EQ(planned_status(method, "If-None-Match", R"("v1")"), ruvia::http_status::ok);
        EXPECT_EQ(planned_status(method, "If-Modified-Since", representation_date), ruvia::http_status::ok);
    }
}

TEST(Ruvia, representation_response_plan_compares_condition_dates_with_last_modified) {
    auto earlier = representation;
    earlier.last_modified_ = *representation.last_modified_ - 1;
    auto later = representation;
    later.last_modified_ = *representation.last_modified_ + 1;

    // If-Modified-Since: 304 when the representation was modified at or before the date.
    EXPECT_EQ(planned_status("GET", "If-Modified-Since", representation_date, earlier),
        ruvia::http_status::not_modified);
    EXPECT_EQ(planned_status("GET", "If-Modified-Since", representation_date, later), ruvia::http_status::ok);

    // If-Unmodified-Since: 412 only when the representation is newer.
    EXPECT_EQ(planned_status("GET", "If-Unmodified-Since", representation_date), ruvia::http_status::ok);
    EXPECT_EQ(planned_status("GET", "If-Unmodified-Since", representation_date, earlier), ruvia::http_status::ok);
    EXPECT_EQ(planned_status("GET", "If-Unmodified-Since", representation_date, later),
        ruvia::http_status::precondition_failed);
}

TEST(Ruvia, representation_response_plan_folds_repeated_entity_tag_lines) {
    auto req = request("GET");
    add(req, "If-None-Match", R"("v1")");
    add(req, "If-None-Match", R"("stale")");
    EXPECT_EQ(ruvia::plan_http_representation_response(req, representation).status(),
        ruvia::http_status::not_modified);

    // A wildcard If-Match matches any current representation.
    EXPECT_EQ(planned_status("GET", "If-Match", "*", {.length_ = 10}), ruvia::http_status::ok);
}

TEST(Ruvia, representation_response_plan_keeps_resolved_values_after_input_lifetimes_end) {
    const auto plan = [] {
        std::string range = "bytes=2-4";
        std::string etag = R"("temporary")";
        auto req = request("GET");
        add(req, "Range", range);
        add(req, "If-Range", etag);
        auto result_value = ruvia::plan_http_representation_response(req,
            {.length_ = 10, .etag_ = etag}, {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        range.assign(range.size(), 'x');
        etag.assign(etag.size(), 'x');
        return result_value;
    }();
    EXPECT_EQ(plan.status(), ruvia::http_status::partial_content);
    EXPECT_TRUE(plan.partial() != nullptr);
    if (const auto* range = plan.partial()) {
        EXPECT_EQ(range->offset_, std::uint64_t{2});
        EXPECT_EQ(range->length_, std::uint64_t{3});
    }
}

// RFC 9110 §8.4 / §14.6: multipart/byteranges cannot carry the coding of the
// representation it slices, so several ranges of a coded representation are
// answered in full (Range MAY be ignored, §14.2); a single range stays 206.
TEST(Ruvia, representation_response_plan_answers_multiple_ranges_of_coded_representations_in_full) {
    const auto options = ruvia::http_representation_response_options{
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    auto coded = representation;
    coded.content_coded_ = true;

    auto multi_req = request("GET");
    add(multi_req, "Range", "bytes=0-1,4-5");
    const auto coded_multi = ruvia::plan_http_representation_response(multi_req, coded, options);
    EXPECT_TRUE(coded_multi.full() != nullptr);
    EXPECT_EQ(coded_multi.status(), ruvia::http_status::ok);
    const auto identity_multi = ruvia::plan_http_representation_response(multi_req, representation, options);
    EXPECT_TRUE(identity_multi.multipart_ranges() != nullptr);

    auto single_req = request("GET");
    add(single_req, "Range", "bytes=0-1");
    const auto coded_single = ruvia::plan_http_representation_response(single_req, coded, options);
    EXPECT_TRUE(coded_single.partial() != nullptr);

    auto unsatisfiable_req = request("GET");
    add(unsatisfiable_req, "Range", "bytes=20-30,40-50");
    const auto unsatisfiable = ruvia::plan_http_representation_response(unsatisfiable_req, coded, options);
    EXPECT_TRUE(unsatisfiable.range_unsatisfiable() != nullptr);
}

// RFC 9110 §15.3.7: whether If-Range authorized the 206 decides which
// representation fields the response repeats.
TEST(Ruvia, representation_response_plan_reports_if_range_authorization) {
    const auto options = ruvia::http_representation_response_options{
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    for (const auto* range : {"bytes=2-4", "bytes=0-1,4-5"}) {
        auto plain = request("GET");
        add(plain, "Range", range);
        const auto plain_plan = ruvia::plan_http_representation_response(plain, representation, options);
        EXPECT_EQ(plain_plan.status(), ruvia::http_status::partial_content);
        EXPECT_TRUE(!plain_plan.authorized_by_if_range());

        auto conditional = request("GET");
        add(conditional, "Range", range);
        add(conditional, "If-Range", R"("v1")");
        const auto conditional_plan = ruvia::plan_http_representation_response(conditional, representation, options);
        EXPECT_EQ(conditional_plan.status(), ruvia::http_status::partial_content);
        EXPECT_TRUE(conditional_plan.authorized_by_if_range());
    }
    auto full = request("GET");
    EXPECT_TRUE(!ruvia::plan_http_representation_response(full, representation, options).authorized_by_if_range());
}

TEST(Ruvia, multipart_range_plan_owns_rfc_framing_and_reports_exact_length) {
    auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-2,7-9", 10);
    std::string media_type = "text/plain";
    std::string boundary = "unit_boundary_42";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary);
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'y');

    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=unit_boundary_42");
    EXPECT_TRUE(plan.metadata().find("Content-Type: text/plain") != std::string_view::npos);
    EXPECT_TRUE(plan.metadata().find("Content-Range: bytes 7-9/10") != std::string_view::npos);
    std::uint64_t content_length = 0;
    for (const auto& segment : plan.segments()) {
        content_length += segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::file
                              ? segment.file_length_
                              : segment.metadata_length_;
    }
    EXPECT_EQ(plan.content_length(), content_length);
    EXPECT_TRUE(plan.metadata().ends_with("--\r\n"));
}

TEST(Ruvia, multipart_range_plan_quotes_boundary_parameters) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", "range boundary");
    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    EXPECT_TRUE(plan.metadata().find("Content-Range: bytes 8-9/10") <
                plan.metadata().find("Content-Range: bytes 0-1/10"));
    auto clone = plan.clone();
    EXPECT_EQ(clone.content_type(), plan.content_type());
    EXPECT_EQ(clone.metadata(), plan.metadata());
}

TEST(Ruvia, multipart_range_plan_rejects_invalid_media_type) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain\r\nX-Evil: yes", "valid_boundary"));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, multipart_range_plan_rejects_content_length_overflow) {
    auto ranges = ruvia::resolve_http_byte_range_set(
        "bytes=0-9223372036854775806,9223372036854775808-18446744073709551614",
        (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(ranges.size(), std::size_t{2});
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, (std::numeric_limits<std::uint64_t>::max)(), "application/octet-stream",
            "overflow_check"));
    } catch (const std::length_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, representation_response_plan_rejects_unknown_policy) {
    bool threw = false;
    try {
        (void)ruvia::plan_http_representation_response(request("GET"), representation,
            {.range_policy_ = static_cast<ruvia::http_range_request_policy>(99)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, representation_response_plan_accepts_named_owner_etag_and_evaluates_preconditions) {
    std::string std_etag(R"("named-etag")");
    std::string pmr_etag(R"(W/"named-etag")");
    const ruvia::http_selected_representation_metadata std_metadata{
        .length_ = 10,
        .etag_ = std_etag,
    };
    const ruvia::http_selected_representation_metadata pmr_metadata{
        .length_ = 10,
        .etag_ = pmr_etag,
    };
    EXPECT_TRUE(std_metadata.etag_.data() == std_etag.data());
    EXPECT_TRUE(pmr_metadata.etag_.data() == pmr_etag.data());
    EXPECT_EQ(planned_status("GET", "If-None-Match", R"("named-etag")", std_metadata),
        ruvia::http_status::not_modified);
    EXPECT_EQ(planned_status("GET", "If-None-Match", R"("named-etag")", pmr_metadata),
        ruvia::http_status::not_modified);
}

namespace {

using ruvia::http_response;
using ruvia::http_response_header_options;

http_response make_response() {
    return http_response{};
}

template <typename fn_type>
bool throws_invalid(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, response_header_append_preserves_multiplicity_across_table_spill) {
    http_response response;
    response.header("X-Repeated", "first");
    for (int i = 0; i < 7; ++i) {
        response.header(std::string("X-Filler-") + std::to_string(i).c_str(), "value");
    }
    response.header("x-repeated", "second", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(response.headers().size(), std::size_t{9});
    std::size_t marked = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::http_ascii_equals_ignore_case(header.name(), "X-Repeated")) {
            ++marked;
        }
    }
    EXPECT_EQ(marked, std::size_t{2});
    response.header("X-Repeated", "replacement");
    EXPECT_EQ(response.headers().size(), std::size_t{8});
    EXPECT_EQ(response.header("X-Repeated"), std::string_view("replacement"));
}

TEST(Ruvia, response_status_is_version_neutral_code_only) {
    auto response = make_response();
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    response.status(ruvia::http_status::not_found);
    EXPECT_EQ(response.status(), ruvia::http_status::not_found);
    response.status(ruvia::http_status_code::from_value(599));
    EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(599));
    response.status(ruvia::http_status_code::from_value(299));
    EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(299));
}

TEST(Ruvia, response_header_distinguishes_missing_from_present_empty) {
    auto response = make_response();
    EXPECT_TRUE(!response.header("X-Empty").has_value());

    response.header("X-Empty", "");
    const auto present_empty = response.header("x-empty");
    EXPECT_TRUE(present_empty.has_value());
    EXPECT_TRUE(present_empty.value_or("missing").empty());
}

TEST(Ruvia, response_status_code_range_validated) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status::continue_value); }));
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status_code::from_value(199)); }));
    EXPECT_TRUE(!throws_invalid([&] { response.status(ruvia::http_status::ok); }));  // lower boundary
    EXPECT_TRUE(!throws_invalid(
        [&] { response.status(ruvia::http_status_code::from_value(599)); }));  // upper boundary
}

TEST(Ruvia, response_switching_protocols_requires_a_dedicated_driver) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status::switching_protocols); }));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, interim_response_head_owns_the_non_switching_1xx_status_space) {
    const ruvia::http_header_view headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::http_interim_response_head early_hints(ruvia::http_status::early_hints, headers);
    EXPECT_EQ(early_hints.status(), ruvia::http_status::early_hints);
    EXPECT_EQ(early_hints.headers().size(), std::size_t{1});
    EXPECT_EQ(early_hints.headers()[0].name(), std::string_view("Link"));

    for (const ruvia::http_status_code status : {ruvia::http_status::switching_protocols,
             ruvia::http_status::ok, ruvia::http_status_code::from_value(599)}) {
        EXPECT_TRUE(throws_invalid([&] { (void)ruvia::http_interim_response_head(status); }));
    }
}

TEST(Ruvia, response_header_replace_append_and_remove) {
    auto response = make_response();

    // A plain set replaces: the latest value wins as a single header.
    response.header("X-Test", "first");
    response.header("X-Test", "second");
    EXPECT_EQ(response.header("X-Test"), std::string_view("second"));

    // append emits an additional header line (needed for multi-valued fields
    // like Set-Cookie).
    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    std::size_t set_cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++set_cookie_count;
        }
    }
    EXPECT_EQ(set_cookie_count, std::size_t{2});

    // Passing nullopt removes the header entirely.
    response.remove_header("X-Test");
    EXPECT_TRUE(!response.header("X-Test").has_value());
}

TEST(Ruvia, response_header_rejects_invalid_write_mode) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] {
        response.header("X-Test", "value",
            http_response_header_options{.mode_ = static_cast<ruvia::http_response_header_mode>(255)});
    }));
    EXPECT_TRUE(response.headers().empty());
}

TEST(Ruvia, response_set_cookie_append_replaces_same_wire_name) {
    auto response = make_response();
    response.header("Set-Cookie", "session=old; Path=/");
    response.header("Set-Cookie", "theme=dark; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=new; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "Session=upper; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});

    std::size_t count = 0;
    bool has_old = false;
    bool has_new = false;
    bool has_theme = false;
    bool has_upper = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        has_old = has_old || header.value() == "session=old; Path=/";
        has_new = has_new || header.value() == "session=new; Path=/";
        has_theme = has_theme || header.value() == "theme=dark; Path=/";
        has_upper = has_upper || header.value() == "Session=upper; Path=/";
    }
    EXPECT_EQ(count, std::size_t{3});
    EXPECT_TRUE(!has_old);
    EXPECT_TRUE(has_new);
    EXPECT_TRUE(has_theme);
    EXPECT_TRUE(has_upper);  // cookie-name is case-sensitive
}

TEST(Ruvia, response_set_cookie_append_preserves_same_name_different_scope) {
    auto response = make_response();
    response.header("Set-Cookie", "session=root-old; Path=/");
    response.header("Set-Cookie", "session=admin; Path=/admin",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-old; Path=/; Domain=.Example.COM",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=root-new; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-new; Domain=example.com; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});

    std::size_t count = 0;
    bool has_root_old = false;
    bool has_root_new = false;
    bool has_admin = false;
    bool has_domain_old = false;
    bool has_domain_new = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        has_root_old = has_root_old || header.value() == "session=root-old; Path=/";
        has_root_new = has_root_new || header.value() == "session=root-new; Path=/";
        has_admin = has_admin || header.value() == "session=admin; Path=/admin";
        has_domain_old =
            has_domain_old || header.value() == "session=domain-old; Path=/; Domain=.Example.COM";
        has_domain_new =
            has_domain_new || header.value() == "session=domain-new; Domain=example.com; Path=/";
    }
    EXPECT_EQ(count, std::size_t{3});
    EXPECT_TRUE(!has_root_old);
    EXPECT_TRUE(has_root_new);
    EXPECT_TRUE(has_admin);
    EXPECT_TRUE(!has_domain_old);
    EXPECT_TRUE(has_domain_new);
}

TEST(Ruvia, response_plain_set_collapses_prior_appended_fields) {
    auto response = make_response();
    response.header("Link", "</a>",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("link", "</b>",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("LINK", "</final>");

    std::size_t link_count = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::http_ascii_equals_ignore_case(header.name(), "Link")) {
            ++link_count;
            EXPECT_EQ(header.value(), std::string_view("</final>"));
        }
    }
    EXPECT_EQ(link_count, std::size_t{1});

    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "c=3");
    std::size_t cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++cookie_count;
            EXPECT_EQ(header.value(), std::string_view("c=3"));
        }
    }
    EXPECT_EQ(cookie_count, std::size_t{1});
}

TEST(Ruvia, response_header_removal_preserves_unrelated_fields) {
    for (const int filler_count : {0, 8}) {
        http_response response;
        response.header("Content-Type", "text/plain");
        response.header("Vary", "Accept-Encoding");
        response.header("Vary", "Origin", {.mode_ = ruvia::http_response_header_mode::append});
        for (int i = 0; i < filler_count; ++i) {
            response.header(std::string("X-Filler-") + std::to_string(i).c_str(), "value");
        }
        std::vector<std::pair<std::string_view, std::string_view>> fields;
        for (const auto& header : response.headers()) {
            fields.emplace_back(header.name(), header.value());
        }
        response.remove_header("content-length");
        response.remove_header("Set-Cookie");
        response.remove_header("X-Missing");
        EXPECT_EQ(response.headers().size(), fields.size());
        std::size_t index = 0;
        for (const auto& header : response.headers()) {
            EXPECT_EQ(header.name(), fields[index].first);
            EXPECT_EQ(header.value(), fields[index].second);
            ++index;
        }
        EXPECT_EQ(response.header("content-type"), std::string_view("text/plain"));
        EXPECT_EQ(response.header("vary"), std::string_view("Accept-Encoding"));
        EXPECT_TRUE(!response.header("Content-Length").has_value());

        // Removing an appended field removes every occurrence.
        response.header("Content-Length", "");
        EXPECT_TRUE(response.header("Content-Length").has_value());
        response.remove_header("CONTENT-LENGTH");
        EXPECT_TRUE(!response.header("Content-Length").has_value());
        response.remove_header("vary");
        EXPECT_TRUE(!response.header("Vary").has_value());
        EXPECT_EQ(response.headers().size(), fields.size() - 2);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        response.remove_header("Vary");
        EXPECT_EQ(response.headers().size(), fields.size() - 2);
    }
}

TEST(Ruvia, response_header_remove_known_header_rebuilds_index) {
    auto response = make_response();
    // Content-Type is a KNOWN header tracked by a bit in the response's header index
    // (unlike the custom X-Test above, which is unindexed). Removing it must rebuild
    // that index, or the indexed lookup fast path could still resolve to the removed
    // slot -- returning a stale value.
    response.header("Content-Type", "text/plain");
    EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));

    response.remove_header("Content-Type");
    EXPECT_TRUE(!response.header("Content-Type").has_value());  // gone, not a stale index hit

    // Re-adding after removal replaces cleanly and leaves exactly one header line --
    // no duplicate resurrected from a stale index entry.
    response.header("Content-Type", "application/json");
    EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Content-Type")) {
            ++count;
        }
    }
    EXPECT_EQ(count, std::size_t{1});
}

TEST(Ruvia, response_header_append_rejects_body_framing_headers) {
    auto response = make_response();

    EXPECT_TRUE(throws_invalid([&] {
        response.header("Content-Length", "5",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Transfer-Encoding", "chunked",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Location", "/next",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] { response.header("Content-Length", "5"); }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_append_rejects_single_value_headers) {
    auto response = make_response();

    EXPECT_TRUE(throws_invalid([&] {
        response.header("Content-Type", "text/plain",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("ETag", "\"abc\"",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Access-Control-Allow-Origin", "https://example.com",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Server", "ruvia",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Vary", "Origin",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Cache-Control", "no-store",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_rejects_invalid_content_type_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"not a media type", "text/", "*/plain", "text/plain; charset"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Content-Type", invalid); }));
    }

    EXPECT_TRUE(!throws_invalid(
        [&] { response.header("Content-Type", "application/json; charset=utf-8"); }));
    EXPECT_EQ(response.header("Content-Type").value_or(std::string_view{}), std::string_view("application/json; charset=utf-8"));
}

TEST(Ruvia, response_header_rejects_invalid_content_encoding_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"gzip;level=9", "bad coding", "gzip/deflate", "", ",gzip", "gzip,", "gzip,,br"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Content-Encoding", invalid); }));
    }

    for (const std::string_view valid : {"deflate", "gzip, br"}) {
        EXPECT_TRUE(!throws_invalid([&] { response.header("Content-Encoding", valid); }));
    }
}

TEST(Ruvia, response_header_rejects_invalid_trailer_field_names) {
    auto response = make_response();

    for (const std::string_view invalid : {"Content-Length", "X-Checksum, bad field", ","}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Trailer", invalid); }));
        EXPECT_TRUE(throws_invalid([&] {
            response.header("Trailer", invalid,
                http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
        }));
    }

    EXPECT_TRUE(!throws_invalid([&] { response.header("Trailer", "ETag, X-Checksum"); }));
    EXPECT_EQ(response.header("Trailer"), std::string_view("ETag, X-Checksum"));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Trailer", "Server-Timing",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_rejects_name_and_value_injection) {
    auto response = make_response();
    // The developer-facing header setter is the header-injection chokepoint: a CR or
    // LF in the value (the classic response-splitting vector) is rejected rather than
    // written into the response head.
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\r\nInjected: x", 14)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\nb", 3)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\rb", 3)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\0b", 3)); }));  // NUL

    // The name is validated too: a CR/LF or a non-token byte (space) is rejected.
    EXPECT_TRUE(throws_invalid([&] { response.header(std::string_view("Bad\r\nName", 9), "v"); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("Bad Name", "v"); }));

    // The append path funnels through the same validation, not just replace.
    EXPECT_TRUE(throws_invalid([&] {
        response.header("X-Foo", std::string_view("a\r\nb", 4),
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    // A clean header is accepted.
    EXPECT_TRUE(!throws_invalid([&] { response.header("X-Clean", "ok"); }));
    EXPECT_EQ(response.header("X-Clean"), std::string_view("ok"));
}

TEST(Ruvia, response_header_rejects_invalid_connection_control_lists) {
    auto response = make_response();
    for (const auto value : {"close,", ", Upgrade", "close;invalid", ""}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"Content-Length", "Date", "Trailer", "Authorization", "Cookie", "Range", "Forwarded",
             "X-Forwarded-For", "X-Forwarded-Proto", "close, X-Forwarded-Host"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"websocket/", ", websocket", ""}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Upgrade", value); }));
    }
    EXPECT_TRUE(throws_invalid([&] { response.header("TE", "trailers"); }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("TE", "trailers",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Connection", "keep-alive, Upgrade");
        response.header("Upgrade", "custom/1, websocket");
    }));
}

namespace {

using ruvia::http_response;

template <typename function_type>
[[nodiscard]] bool throws_invalid_argument(function_type&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

template <typename prepare_type, typename inspect_type>
void check_response_body_moves(prepare_type&& prepare, inspect_type&& inspect) {
    for (const bool assign : {false, true}) {
        http_response source;
        prepare(source);
        source.header("X-Source", "retained");
        std::optional<http_response> target;
        if (assign) {
            target.emplace();
            target->body(std::string(256, 't'));
            *target = std::move(source);
        } else {
            target.emplace(std::move(source));
        }
        EXPECT_EQ(target->header("X-Source").value_or("missing"), std::string_view("retained"));
        inspect(*target);
        EXPECT_TRUE(source.headers().empty());
        EXPECT_TRUE(source.body_bytes().empty());
        EXPECT_TRUE(!source.file_body().has_value());
        EXPECT_TRUE(!source.has_multipart_file_body());
        EXPECT_EQ(source.body_segment_count(), std::size_t{0});
        EXPECT_EQ(ruvia::plan_buffered_http_response_write(
                      ruvia::http_known_method::get, source)
                      .content_length(),
            std::uint64_t{0});
        source.body("source reused");
        EXPECT_EQ(source.body_bytes(), std::string_view("source reused"));
    }
}

}  // namespace

TEST(Ruvia, response_public_body_owns_its_source) {
    http_response response;
    std::string source_value = "owned copy";

    response.body(source_value);
    source_value[0] = 'X';

    EXPECT_EQ(response.body_bytes(), std::string_view("owned copy"));
}

TEST(Ruvia, public_response_file_view_exposes_read_only_descriptor) {
    const std::filesystem::path path("public-fixture.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    const ruvia::http_response_file_view file(path.c_str(), 16, 3, 7, identity);

    EXPECT_TRUE(file.to_path() == path);
    EXPECT_EQ(file.size(), std::uint64_t{16});
    EXPECT_EQ(file.offset(), std::uint64_t{3});
    EXPECT_EQ(file.length(), std::uint64_t{7});
    EXPECT_TRUE(file.identity().requires_validation());
    EXPECT_TRUE(file.identity() == identity);
}

TEST(Ruvia, response_file_setter_keeps_typed_identity_and_validates_before_replacement) {
    http_response response;
    const auto path = std::filesystem::path("public-response.bin");
    const auto unchecked = ruvia::http_response_file_identity::unchecked();

    response.body("preserved");
    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(ruvia::http_native_path_view_type{}, 10, 0, 10, unchecked);
    }));
    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, unchecked);
    }));
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), invalid, 0, 0, unchecked); }));
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), 10, invalid, 0, unchecked); }));
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), 10, 0, invalid, unchecked); }));
    }
    EXPECT_TRUE(!response.file_body().has_value());
    EXPECT_EQ(response.body_bytes(), std::string_view("preserved"));

    response.file_body(path.native(), 16, 3, 7, unchecked);
    auto file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == path);
        EXPECT_TRUE(file->identity() == unchecked);
        EXPECT_TRUE(!file->identity().requires_validation());
    }

    const auto checked = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    response.file_body(std::filesystem::path("checked-response.bin").native(), 16, 3, 7, checked);
    file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->identity() == checked);
        EXPECT_TRUE(file->identity().requires_validation());
    }

    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, checked);
    }));
    file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == std::filesystem::path("checked-response.bin"));
        EXPECT_TRUE(file->identity() == checked);
    }
}

namespace {

std::size_t released_response_file_handles = 0;
ruvia::http_response_file_descriptor::native_type last_released_response_file_handle = 0;

void release_counted_response_file_handle(ruvia::http_response_file_descriptor::native_type value) noexcept {
    ++released_response_file_handles;
    last_released_response_file_handle = value;
}

struct reentrant_response_file_release final {
    std::size_t releases_{0};
    ruvia::http_response_file_descriptor* replacement_target_{nullptr};
    ruvia::http_response_file_descriptor* replacement_source_{nullptr};
    ruvia::http_response* rewritten_response_{nullptr};

    ruvia::http_response_file_descriptor opened() & noexcept {
        return {reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(this), release};
    }

    static void release(ruvia::http_response_file_descriptor::native_type value) noexcept {
        auto& record = *reinterpret_cast<reentrant_response_file_release*>(value);
        ++record.releases_;
        if (record.replacement_target_ != nullptr) {
            *record.replacement_target_ = std::move(*record.replacement_source_);
        }
        if (record.rewritten_response_ != nullptr) {
            record.rewritten_response_->static_body("callback");
        }
    }
};

}  // namespace

TEST(Ruvia, response_file_body_owns_its_opened_handle_until_replacement) {
    released_response_file_handles = 0;
    const auto path = std::filesystem::path("opened-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    {
        http_response response;
        response.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_descriptor(7, &release_counted_response_file_handle));
        const auto file = response.file_body();
        EXPECT_TRUE(file.has_value());
        if (file) {
            EXPECT_TRUE(file->opened_file() != nullptr);
            if (file->opened_file() != nullptr) {
                EXPECT_EQ(file->opened_file()->native(), ruvia::http_response_file_descriptor::native_type{7});
            }
        }

        // Moving the response moves the handle; nothing is released.
        http_response target;
        target = std::move(response);
        EXPECT_EQ(released_response_file_handles, std::size_t{0});
        EXPECT_TRUE(!response.file_body().has_value());

        // Replacing the body releases the handle exactly once.
        target.body("replaced");
        EXPECT_EQ(released_response_file_handles, std::size_t{1});
        EXPECT_EQ(last_released_response_file_handle, ruvia::http_response_file_descriptor::native_type{7});

        // A rejected setter releases the handle it was given and keeps the body.
        EXPECT_TRUE(throws_invalid_argument([&] {
            target.file_body(path.native(), 10, 8, 3, identity,
                ruvia::http_response_file_descriptor(8, &release_counted_response_file_handle));
        }));
        EXPECT_EQ(released_response_file_handles, std::size_t{2});
        EXPECT_EQ(target.body_bytes(), std::string_view("replaced"));

        // A path-only file body exposes no opened handle.
        target.file_body(path.native(), 16, 0, 16, identity);
        const auto unopened = target.file_body();
        EXPECT_TRUE(unopened.has_value() && unopened->opened_file() == nullptr);

        target.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_descriptor(9, &release_counted_response_file_handle));
    }
    // Destroying the response releases the last handle.
    EXPECT_EQ(released_response_file_handles, std::size_t{3});
    EXPECT_EQ(last_released_response_file_handle, ruvia::http_response_file_descriptor::native_type{9});
}

TEST(Ruvia, response_file_handle_move_assignment_preserves_reentrant_replacement) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    reentrant_response_file_release replacement;
    {
        auto target = previous.opened();
        auto source = incoming.opened();
        auto next = replacement.opened();
        previous.replacement_target_ = &target;
        previous.replacement_source_ = &next;

        target = std::move(source);

        EXPECT_FALSE(source);
        EXPECT_FALSE(next);
        EXPECT_EQ(target.native(), reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(&replacement));
        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 1U);
        EXPECT_EQ(replacement.releases_, 0U);
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
    EXPECT_EQ(replacement.releases_, 1U);
}

TEST(Ruvia, response_file_handle_move_assignment_detaches_source_before_retirement) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    {
        auto target = previous.opened();
        auto source = incoming.opened();
        ruvia::http_response_file_descriptor empty;
        previous.replacement_target_ = &source;
        previous.replacement_source_ = &empty;

        target = std::move(source);

        EXPECT_FALSE(source);
        EXPECT_EQ(target.native(), reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(&incoming));
        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 0U);
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
}

TEST(Ruvia, response_file_move_assignment_commits_before_old_body_reuses_source) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    const auto path = std::filesystem::path("response-retirement.bin");
    const auto identity = ruvia::http_response_file_identity::unchecked();
    {
        ruvia::http_response source;
        ruvia::http_response target;
        source.file_body(path.native(), 1, 0, 1, identity, incoming.opened());
        source.header("X-Source", "retained");
        target.file_body(path.native(), 1, 0, 1, identity, previous.opened());
        previous.rewritten_response_ = &source;

        target = std::move(source);

        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 0U);
        EXPECT_TRUE(target.file_body().has_value());
        EXPECT_EQ(target.header("X-Source").value_or(""), "retained");
        EXPECT_EQ(source.body_bytes(), "callback");
        EXPECT_FALSE(source.header("X-Source").has_value());
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
}

TEST(Ruvia, response_file_body_move_preserves_owned_path_and_identity) {
    http_response source_value;
    const auto path = std::filesystem::path("moved-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({9, 8, 7, 6});
    source_value.file_body(path.native(), 23, 4, 9, identity);

    http_response target;
    target = std::move(source_value);
    const auto file = target.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == path);
        EXPECT_EQ(file->size(), std::uint64_t{23});
        EXPECT_EQ(file->offset(), std::uint64_t{4});
        EXPECT_EQ(file->length(), std::uint64_t{9});
        EXPECT_TRUE(file->identity() == identity);
    }
}

TEST(Ruvia, response_file_body_moves_leave_sources_empty_and_reusable) {
    const std::filesystem::path path("retained-response-file.bin");
    const auto identity = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    check_response_body_moves([&](http_response& source) { source.file_body(path.native(), 23, 4, 9, identity); }, [&](const http_response& target) {
        const auto file = target.file_body();
        EXPECT_TRUE(file.has_value());
        if (file) {
            EXPECT_TRUE(file->to_path() == path);
            EXPECT_EQ(file->size(), std::uint64_t{23});
            EXPECT_EQ(file->offset(), std::uint64_t{4});
            EXPECT_EQ(file->length(), std::uint64_t{9});
            EXPECT_TRUE(file->identity() == identity);
        }
        EXPECT_TRUE(target.body_bytes().empty());
        EXPECT_EQ(target.body_segment_count(), std::size_t{1}); });
}

TEST(Ruvia, empty_file_ranges_remain_segments_after_response_moves) {
    const std::filesystem::path path("empty-response-file.bin");
    const auto identity = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    for (const std::uint64_t size : {0, 23}) {
        check_response_body_moves([&](http_response& source) { source.file_body(path.native(), size, size, 0, identity); }, [&](const http_response& target) {
            EXPECT_EQ(target.body_segment_count(), std::size_t{1});
            const auto segment = target.body_segment(0);
            EXPECT_TRUE(segment.bytes_.empty());
            EXPECT_TRUE(segment.file_.has_value());
            if (segment.file_) {
                EXPECT_TRUE(segment.file_->to_path() == path);
                EXPECT_EQ(segment.file_->size(), size);
                EXPECT_EQ(segment.file_->offset(), size);
                EXPECT_EQ(segment.file_->length(), std::uint64_t{0});
                EXPECT_TRUE(segment.file_->identity() == identity);
            }
            EXPECT_EQ(ruvia::plan_buffered_http_response_write(
                               ruvia::http_known_method::get, target).content_length(), std::uint64_t{0}); });
    }
}
