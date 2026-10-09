#include <ctime>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_representation_response_plan.h"
#include "ruvia/http/http_request.h"

#include "request/http_request_access.h"
#include "test_harness.h"

namespace {

using ruvia::http_header_view;
using ruvia::http_request;
using ruvia::detail::http_request_access;
using ruvia::detail::request_header_kind;

http_request request(std::string_view method) {
    auto result_value = http_request_access::make();
    http_request_access::set_method(result_value, method);
    return result_value;
}

void add(http_request& req, request_header_kind name, std::string_view value) {
    std::string_view field;
    switch (name) {
        case request_header_kind::if_match:
            field = "If-Match";
            break;
        case request_header_kind::if_none_match:
            field = "If-None-Match";
            break;
        case request_header_kind::if_modified_since:
            field = "If-Modified-Since";
            break;
        case request_header_kind::if_unmodified_since:
            field = "If-Unmodified-Since";
            break;
        case request_header_kind::if_range:
            field = "If-Range";
            break;
        case request_header_kind::range:
            field = "Range";
            break;
        default:
            throw std::logic_error("unexpected representation request field");
    }
    if (!http_request_access::add_header(
            req, http_header_view(field, value), http_request_access::known_header_slot(name))) {
        throw std::logic_error("representation request header rejected");
    }
}

constexpr ruvia::http_selected_representation_metadata representation{
    .length_ = 10,
    .etag_ = R"("v1")",
    .last_modified_ = std::time_t{784111777},
    .strong_date_validator_ = true,
};

}  // namespace

RUVIA_TEST(representation_response_plan_only_evaluates_conditions_for_eligible_responses) {
    for (const auto status : {ruvia::http_status::not_found, ruvia::http_status::temporary_redirect}) {
        auto req = request("GET");
        add(req, request_header_kind::if_none_match, R"("v1")");
        add(req, request_header_kind::if_match, R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(
            req, representation, {.normal_status_ = status});
        RUVIA_CHECK(plan.full() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), status);
    }

    auto created = request("GET");
    add(created, request_header_kind::range, "bytes=1-2");
    const auto created_plan = ruvia::plan_http_representation_response(
        created, representation, {.normal_status_ = ruvia::http_status::created, .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
    RUVIA_CHECK(created_plan.full() != nullptr);
    RUVIA_CHECK_EQ(created_plan.status(), ruvia::http_status::created);
}

RUVIA_TEST(representation_response_plan_resolves_supported_range_outcomes) {
    const auto options = ruvia::http_representation_response_options{
        .normal_status_ = ruvia::http_status::ok,
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    auto partial_req = request("GET");
    add(partial_req, request_header_kind::range, "bytes=2-4");
    const auto partial = ruvia::plan_http_representation_response(partial_req, representation, options);
    RUVIA_CHECK(partial.partial() != nullptr);
    if (const auto* range = partial.partial()) {
        RUVIA_CHECK_EQ(range->offset(), std::uint64_t{2});
        RUVIA_CHECK_EQ(range->length(), std::uint64_t{3});
    }
    RUVIA_CHECK_EQ(partial.status(), ruvia::http_status::partial_content);

    auto unsat_req = request("GET");
    add(unsat_req, request_header_kind::range, "bytes=20-");
    const auto unsat = ruvia::plan_http_representation_response(unsat_req, representation, options);
    RUVIA_CHECK(unsat.range_unsatisfiable() != nullptr);
    RUVIA_CHECK_EQ(unsat.status(), ruvia::http_status::range_not_satisfiable);

    const auto empty = ruvia::plan_http_representation_response(partial_req, {}, options);
    RUVIA_CHECK(empty.full() != nullptr);
    const auto disabled = ruvia::plan_http_representation_response(partial_req, representation);
    RUVIA_CHECK(disabled.full() != nullptr);

    for (const auto range : {"items=1-2", "bytes=garbage", "bytes=", ""}) {
        auto req = request("GET");
        add(req, request_header_kind::range, range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, options);
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto multi_req = request("GET");
    add(multi_req, request_header_kind::range, "bytes=1-2,4-5");
    const auto multi = ruvia::plan_http_representation_response(multi_req, representation, options);
    RUVIA_CHECK(multi.multipart_ranges() != nullptr);
    RUVIA_CHECK_EQ(multi.status(), ruvia::http_status::partial_content);
    if (const auto* ranges = multi.multipart_ranges()) {
        RUVIA_CHECK_EQ(ranges->size(), std::size_t{2});
    }
}

RUVIA_TEST(representation_response_plan_preserves_unsatisfiable_ranges_after_empty_members) {
    const auto options = ruvia::http_representation_response_options{
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"20-", "20-29", "-0"}) {
        const auto value = prefix + tail;
        auto req = request("GET");
        add(req, request_header_kind::range, value);
        const auto plan = ruvia::plan_http_representation_response(req, representation, options);
        RUVIA_CHECK(plan.range_unsatisfiable() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::range_not_satisfiable);
    }
    const auto over_limit = prefix + ",20-";
    auto req = request("GET");
    add(req, request_header_kind::range, over_limit);
    const auto plan = ruvia::plan_http_representation_response(req, representation, options);
    RUVIA_CHECK(plan.full() != nullptr);
    RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::ok);
}

RUVIA_TEST(representation_response_plan_obeys_method_precondition_and_presence_precedence) {
    for (const auto method : {"GET", "HEAD", "POST"}) {
        auto req = request(method);
        add(req, request_header_kind::if_match, R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation);
        RUVIA_CHECK(plan.precondition_failed() != nullptr);
    }
    for (const auto method : {"OPTIONS", "CONNECT"}) {
        auto req = request(method);
        add(req, request_header_kind::if_match, R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation);
        RUVIA_CHECK(plan.full() != nullptr);
    }

    auto precedence = request("GET");
    add(precedence, request_header_kind::if_match, R"("v1")");
    add(precedence, request_header_kind::if_unmodified_since, "Sun, 06 Nov 1994 08:49:36 GMT");
    add(precedence, request_header_kind::if_none_match, R"("stale")");
    add(precedence, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto precedence_plan = ruvia::plan_http_representation_response(precedence, representation);
    RUVIA_CHECK(precedence_plan.full() != nullptr);

    auto repeated = request("GET");
    add(repeated, request_header_kind::if_none_match, R"("stale")");
    add(repeated, request_header_kind::if_none_match, R"("v1")");
    const auto repeated_plan = ruvia::plan_http_representation_response(repeated, representation);
    RUVIA_CHECK(repeated_plan.not_modified() != nullptr);

    auto empty_none_match = request("GET");
    add(empty_none_match, request_header_kind::if_none_match, "");
    add(empty_none_match, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto empty_none_match_plan = ruvia::plan_http_representation_response(empty_none_match, representation);
    RUVIA_CHECK(empty_none_match_plan.full() != nullptr);

    auto failed_first = request("GET");
    add(failed_first, request_header_kind::if_match, R"("stale")");
    add(failed_first, request_header_kind::if_none_match, R"("v1")");
    add(failed_first, request_header_kind::range, "bytes=2-4");
    const auto failed_first_plan = ruvia::plan_http_representation_response(failed_first, representation,
        {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
    RUVIA_CHECK(failed_first_plan.precondition_failed() != nullptr);
    RUVIA_CHECK_EQ(failed_first_plan.status(), ruvia::http_status::precondition_failed);
}

RUVIA_TEST(representation_response_plan_evaluates_supported_representation_extension_methods) {
    for (const auto method : {"UPDATE", "MERGE", "get", "trace"}) {
        auto exists = request(method);
        add(exists, request_header_kind::if_none_match, "*");
        const auto exists_plan = ruvia::plan_http_representation_response(exists, {.length_ = 10});
        RUVIA_CHECK_EQ(exists_plan.status(), ruvia::http_status::precondition_failed);

        auto none_match = request(method);
        add(none_match, request_header_kind::if_none_match, R"(W/"v1")");
        const auto none_match_plan = ruvia::plan_http_representation_response(none_match, representation);
        RUVIA_CHECK(none_match_plan.precondition_failed() != nullptr);

        auto match = request(method);
        add(match, request_header_kind::if_match, R"(W/"v1")");
        const auto match_plan = ruvia::plan_http_representation_response(match, representation);
        RUVIA_CHECK(match_plan.precondition_failed() != nullptr);

        auto unmodified = request(method);
        add(unmodified, request_header_kind::if_unmodified_since, "Sun, 06 Nov 1994 08:49:36 GMT");
        const auto unmodified_plan = ruvia::plan_http_representation_response(unmodified, representation);
        RUVIA_CHECK(unmodified_plan.precondition_failed() != nullptr);

        auto matched = request(method);
        add(matched, request_header_kind::if_match, R"("v1")");
        add(matched, request_header_kind::if_unmodified_since, "Sun, 06 Nov 1994 08:49:36 GMT");
        add(matched, request_header_kind::if_none_match, R"("stale")");
        add(matched, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:38 GMT");
        add(matched, request_header_kind::range, "bytes=2-4");
        const auto matched_plan = ruvia::plan_http_representation_response(matched, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        RUVIA_CHECK(matched_plan.full() != nullptr);
        RUVIA_CHECK_EQ(matched_plan.status(), ruvia::http_status::ok);

        auto modified_since = request(method);
        add(modified_since, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:38 GMT");
        const auto modified_since_plan = ruvia::plan_http_representation_response(modified_since, representation);
        RUVIA_CHECK_EQ(modified_since_plan.status(), ruvia::http_status::ok);

        auto range = request(method);
        add(range, request_header_kind::range, "bytes=2-4");
        add(range, request_header_kind::if_range, R"("v1")");
        const auto range_plan = ruvia::plan_http_representation_response(range, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        RUVIA_CHECK(range_plan.full() != nullptr);
        RUVIA_CHECK_EQ(range_plan.status(), ruvia::http_status::ok);

        auto wildcard_match = request(method);
        add(wildcard_match, request_header_kind::if_match, "*");
        const auto wildcard_match_plan = ruvia::plan_http_representation_response(wildcard_match, {.length_ = 10});
        RUVIA_CHECK_EQ(wildcard_match_plan.status(), ruvia::http_status::ok);

        const auto no_content = ruvia::plan_http_representation_response(exists, representation,
            {.normal_status_ = ruvia::http_status::no_content});
        RUVIA_CHECK_EQ(no_content.status(), ruvia::http_status::precondition_failed);

        const auto precondition = ruvia::plan_http_representation_response(matched, representation,
            {.normal_status_ = ruvia::http_status::precondition_failed});
        RUVIA_CHECK(precondition.full() != nullptr);
        RUVIA_CHECK_EQ(precondition.status(), ruvia::http_status::precondition_failed);
    }
}

RUVIA_TEST(representation_response_plan_ignores_extension_conditions_for_ineligible_responses) {
    for (const auto status : {ruvia::http_status::not_found, ruvia::http_status::not_implemented,
             ruvia::http_status::method_not_allowed, ruvia::http_status::temporary_redirect}) {
        auto req = request("UPDATE");
        add(req, request_header_kind::if_none_match, "*");
        add(req, request_header_kind::if_match, R"("stale")");
        const auto plan = ruvia::plan_http_representation_response(req, representation, {.normal_status_ = status});
        RUVIA_CHECK(plan.full() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), status);
    }
}

RUVIA_TEST(representation_response_plan_uses_last_modified_only_when_present_and_if_range_is_strong) {
    auto not_modified = request("GET");
    add(not_modified, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto not_modified_plan = ruvia::plan_http_representation_response(not_modified, representation);
    RUVIA_CHECK(not_modified_plan.not_modified() != nullptr);

    auto absent_date = request("GET");
    add(absent_date, request_header_kind::if_modified_since, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto absent_date_plan = ruvia::plan_http_representation_response(absent_date,
        {.length_ = representation.length_, .etag_ = representation.etag_});
    RUVIA_CHECK(absent_date_plan.full() != nullptr);

    const auto range_options = ruvia::http_representation_response_options{
        .normal_status_ = ruvia::http_status::ok,
        .range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges};
    for (const auto if_range : {R"("v1")", "Sun, 06 Nov 1994 08:49:37 GMT"}) {
        auto req = request("GET");
        add(req, request_header_kind::range, "bytes=1-2");
        add(req, request_header_kind::if_range, if_range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, range_options);
        RUVIA_CHECK(plan.partial() != nullptr);
    }
    for (const auto if_range : {R"(W/"v1")", "Sun, 06 Nov 1994 08:49:38 GMT", ""}) {
        auto req = request("GET");
        add(req, request_header_kind::range, "bytes=1-2");
        add(req, request_header_kind::if_range, if_range);
        const auto plan = ruvia::plan_http_representation_response(req, representation, range_options);
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto missing_date = request("GET");
    add(missing_date, request_header_kind::range, "bytes=1-2");
    add(missing_date, request_header_kind::if_range, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto missing_date_plan = ruvia::plan_http_representation_response(missing_date,
        {.length_ = representation.length_, .etag_ = representation.etag_}, range_options);
    RUVIA_CHECK(missing_date_plan.full() != nullptr);
    auto weak_date_metadata = representation;
    weak_date_metadata.strong_date_validator_ = false;
    const auto weak_date_plan = ruvia::plan_http_representation_response(missing_date, weak_date_metadata, range_options);
    RUVIA_CHECK(weak_date_plan.full() != nullptr);

    auto tag_only = request("GET");
    add(tag_only, request_header_kind::range, "bytes=1-2");
    add(tag_only, request_header_kind::if_range, R"("v1")");
    const auto tag_only_plan = ruvia::plan_http_representation_response(tag_only,
        {.length_ = 10, .etag_ = representation.etag_}, range_options);
    RUVIA_CHECK(tag_only_plan.partial() != nullptr);
}

RUVIA_TEST(representation_response_plan_checks_existing_representation_wildcards_and_method_roles) {
    for (const auto method : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "CONNECT", "TRACE"}) {
        auto req = request(method);
        add(req, request_header_kind::if_none_match, "*");
        add(req, request_header_kind::range, "bytes=2-4");
        const auto plan = ruvia::plan_http_representation_response(req,
            {.length_ = 10}, {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        if (std::string_view(method) == "GET" || std::string_view(method) == "HEAD") {
            RUVIA_CHECK(plan.not_modified() != nullptr);
            RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::not_modified);
        } else if (std::string_view(method) == "OPTIONS" || std::string_view(method) == "CONNECT" || std::string_view(method) == "TRACE") {
            RUVIA_CHECK(plan.full() != nullptr);
        } else {
            RUVIA_CHECK(plan.precondition_failed() != nullptr);
        }
    }
    for (const auto method : {"HEAD", "POST"}) {
        auto req = request(method);
        add(req, request_header_kind::range, "bytes=2-4");
        const auto plan = ruvia::plan_http_representation_response(req, representation,
            {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto created = request("GET");
    add(created, request_header_kind::if_none_match, "*");
    const auto created_plan = ruvia::plan_http_representation_response(created, representation,
        {.normal_status_ = ruvia::http_status::created});
    RUVIA_CHECK(created_plan.not_modified() != nullptr);
    auto precondition = request("GET");
    add(precondition, request_header_kind::if_match, R"("v1")");
    const auto precondition_plan = ruvia::plan_http_representation_response(precondition, representation,
        {.normal_status_ = ruvia::http_status::precondition_failed});
    RUVIA_CHECK(precondition_plan.full() != nullptr);
    RUVIA_CHECK_EQ(precondition_plan.status(), ruvia::http_status::precondition_failed);
}

RUVIA_TEST(representation_response_plan_respects_date_failures_and_strong_versus_weak_tag_comparison) {
    auto unmodified = request("GET");
    add(unmodified, request_header_kind::if_unmodified_since, "Sun, 06 Nov 1994 08:49:36 GMT");
    const auto failed = ruvia::plan_http_representation_response(unmodified, representation);
    RUVIA_CHECK(failed.precondition_failed() != nullptr);
    const auto unavailable = ruvia::plan_http_representation_response(unmodified, {.length_ = 10});
    RUVIA_CHECK(unavailable.full() != nullptr);
    auto malformed = request("GET");
    add(malformed, request_header_kind::if_unmodified_since, "invalid-date");
    add(malformed, request_header_kind::if_modified_since, "invalid-date");
    const auto ignored = ruvia::plan_http_representation_response(malformed, representation);
    RUVIA_CHECK(ignored.full() != nullptr);
    auto strong = request("GET");
    add(strong, request_header_kind::if_match, R"(W/"v1")");
    const auto strong_plan = ruvia::plan_http_representation_response(strong, representation);
    RUVIA_CHECK(strong_plan.precondition_failed() != nullptr);
    auto weak = request("GET");
    add(weak, request_header_kind::if_none_match, R"(W/"v1")");
    const auto weak_plan = ruvia::plan_http_representation_response(weak, representation);
    RUVIA_CHECK(weak_plan.not_modified() != nullptr);
}

RUVIA_TEST(representation_response_plan_keeps_resolved_values_after_input_lifetimes_end) {
    const auto plan = [] {
        std::string range = "bytes=2-4";
        std::string etag = R"("temporary")";
        auto req = request("GET");
        add(req, request_header_kind::range, range);
        add(req, request_header_kind::if_range, etag);
        auto result_value = ruvia::plan_http_representation_response(req,
            {.length_ = 10, .etag_ = etag}, {.range_policy_ = ruvia::http_range_request_policy::honor_byte_ranges});
        range.assign(range.size(), 'x');
        etag.assign(etag.size(), 'x');
        return result_value;
    }();
    RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::partial_content);
    RUVIA_CHECK(plan.partial() != nullptr);
    if (const auto* range = plan.partial()) {
        RUVIA_CHECK_EQ(range->offset(), std::uint64_t{2});
        RUVIA_CHECK_EQ(range->length(), std::uint64_t{3});
    }
}

RUVIA_TEST(multipart_range_plan_owns_rfc_framing_and_reports_exact_length) {
    std::pmr::monotonic_buffer_resource resource;
    auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-2,7-9", 10);
    std::string media_type = "text/plain";
    std::string boundary = "unit_boundary_42";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary, {}, &resource);
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'y');

    RUVIA_CHECK_EQ(plan.content_type(), "multipart/byteranges; boundary=unit_boundary_42");
    RUVIA_CHECK_EQ(plan.segments().size(), std::size_t{7});
    RUVIA_CHECK(plan.segments()[1].kind_ ==
                ruvia::http_multipart_byte_range_plan::segment_kind::file);
    RUVIA_CHECK_EQ(plan.segments()[1].file_offset_, std::uint64_t{0});
    RUVIA_CHECK_EQ(plan.segments()[1].file_length_, std::uint64_t{3});
    RUVIA_CHECK(plan.metadata().find("Content-Type: text/plain") != std::string_view::npos);
    RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 7-9/10") != std::string_view::npos);
    std::uint64_t content_length = 0;
    for (const auto& segment : plan.segments()) {
        content_length += segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::file
                              ? segment.file_length_
                              : segment.metadata_length_;
    }
    RUVIA_CHECK_EQ(plan.content_length(), content_length);
    RUVIA_CHECK(plan.metadata().ends_with("--\r\n"));
}

RUVIA_TEST(multipart_range_plan_is_move_only_and_quotes_boundary_parameters) {
    static_assert(!std::is_copy_constructible_v<ruvia::http_multipart_byte_range_plan>);
    static_assert(!std::is_copy_assignable_v<ruvia::http_multipart_byte_range_plan>);
    std::pmr::monotonic_buffer_resource resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "range boundary", "gzip", &resource);
    RUVIA_CHECK_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    RUVIA_CHECK(plan.metadata().find("Content-Encoding: gzip") != std::string_view::npos);
    RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 8-9/10") <
                plan.metadata().find("Content-Range: bytes 0-1/10"));
    auto clone = plan.clone(std::pmr::new_delete_resource());
    RUVIA_CHECK_EQ(clone.content_type(), plan.content_type());
    RUVIA_CHECK_EQ(clone.metadata(), plan.metadata());
}

RUVIA_TEST(multipart_range_plan_rejects_invalid_media_type) {
    std::pmr::monotonic_buffer_resource resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain\r\nX-Evil: yes", "valid_boundary", {}, &resource));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_range_plan_rejects_content_length_overflow) {
    auto ranges = ruvia::resolve_http_byte_range_set(
        "bytes=0-9223372036854775806,9223372036854775808-18446744073709551614",
        (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(ranges.size(), std::size_t{2});
    std::pmr::monotonic_buffer_resource resource;
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(
            ranges, (std::numeric_limits<std::uint64_t>::max)(), "application/octet-stream",
            "overflow_check", {}, &resource));
    } catch (const std::length_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(representation_response_plan_rejects_unknown_policy) {
    bool threw = false;
    try {
        (void)ruvia::plan_http_representation_response(request("GET"), representation,
            {.range_policy_ = static_cast<ruvia::http_range_request_policy>(99)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
