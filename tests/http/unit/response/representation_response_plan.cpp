#include <gtest/gtest.h>

#include <ctime>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_representation_response_plan.h"
#include "ruvia/http/http_request.h"

namespace {

using ruvia::http_header_view;
using ruvia::http_request;

http_request request(std::string_view method) {
    return ruvia::make_parsed_http_request(
        method, method == "CONNECT" ? "example.test:443" : "/", {}, {},
        std::pmr::get_default_resource());
}

void add(http_request& req, std::string_view name, std::string_view value) {
    std::vector<http_header_view> fields(req.headers().begin(), req.headers().end());
    fields.emplace_back(name, value);
    req = ruvia::make_parsed_http_request(
        req.method(), req.target(), fields, req.body_bytes(), std::pmr::get_default_resource());
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
    EXPECT_TRUE(empty.full() != nullptr);
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
                parsed.parse_head(wire, 0, std::pmr::get_default_resource());
                EXPECT_TRUE(parsed.head_ready() != nullptr);
                if (parsed.head_ready() != nullptr) {
                    const auto plan = ruvia::plan_http_representation_response(parsed.request_, representation);
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
    std::pmr::monotonic_buffer_resource resource;
    auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-2,7-9", 10);
    std::string media_type = "text/plain";
    std::string boundary = "unit_boundary_42";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary, &resource);
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
    std::pmr::monotonic_buffer_resource resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", "range boundary", &resource);
    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    EXPECT_TRUE(plan.metadata().find("Content-Range: bytes 8-9/10") <
                plan.metadata().find("Content-Range: bytes 0-1/10"));
    auto clone = plan.clone(std::pmr::new_delete_resource());
    EXPECT_EQ(clone.content_type(), plan.content_type());
    EXPECT_EQ(clone.metadata(), plan.metadata());
}

TEST(Ruvia, multipart_range_plan_rejects_invalid_media_type) {
    std::pmr::monotonic_buffer_resource resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain\r\nX-Evil: yes", "valid_boundary", &resource));
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
    std::pmr::monotonic_buffer_resource resource;
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, (std::numeric_limits<std::uint64_t>::max)(), "application/octet-stream",
            "overflow_check", &resource));
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
    std::pmr::string pmr_etag(R"(W/"named-etag")");
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
