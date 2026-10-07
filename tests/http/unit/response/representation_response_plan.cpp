#include <ctime>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRepresentationResponsePlan.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"

#include "test_harness.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestHeaderKind;

HttpRequest request(std::string_view method) {
    auto result = HttpRequestAccess::make();
    HttpRequestAccess::setMethod(result, method);
    return result;
}

void add(HttpRequest& req, RequestHeaderKind name, std::string_view value) {
    std::string_view field;
    switch (name) {
        case RequestHeaderKind::kIfMatch:
            field = "If-Match";
            break;
        case RequestHeaderKind::kIfNoneMatch:
            field = "If-None-Match";
            break;
        case RequestHeaderKind::kIfModifiedSince:
            field = "If-Modified-Since";
            break;
        case RequestHeaderKind::kIfUnmodifiedSince:
            field = "If-Unmodified-Since";
            break;
        case RequestHeaderKind::kIfRange:
            field = "If-Range";
            break;
        case RequestHeaderKind::kRange:
            field = "Range";
            break;
        default:
            throw std::logic_error("unexpected representation request field");
    }
    if (!HttpRequestAccess::addHeader(
            req, HttpHeaderView(field, value), HttpRequestAccess::knownHeaderSlot(name))) {
        throw std::logic_error("representation request header rejected");
    }
}

constexpr ruvia::HttpSelectedRepresentationMetadata kRepresentation{
    .length = 10,
    .etag = R"("v1")",
    .lastModified = std::time_t{784111777},
    .strongDateValidator = true,
};

}  // namespace

RUVIA_TEST(representation_response_plan_only_evaluates_conditions_for_eligible_responses) {
    for (const auto status : {ruvia::http_status::kNotFound, ruvia::http_status::kTemporaryRedirect}) {
        auto req = request("GET");
        add(req, RequestHeaderKind::kIfNoneMatch, R"("v1")");
        add(req, RequestHeaderKind::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(
            req, kRepresentation, {.normalStatus = status});
        RUVIA_CHECK(plan.full() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), status);
    }

    auto created = request("GET");
    add(created, RequestHeaderKind::kRange, "bytes=1-2");
    const auto createdPlan = ruvia::planHttpRepresentationResponse(
        created, kRepresentation, {.normalStatus = ruvia::http_status::kCreated, .rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
    RUVIA_CHECK(createdPlan.full() != nullptr);
    RUVIA_CHECK_EQ(createdPlan.status(), ruvia::http_status::kCreated);
}

RUVIA_TEST(representation_response_plan_resolves_supported_range_outcomes) {
    const auto options = ruvia::HttpRepresentationResponseOptions{
        .normalStatus = ruvia::http_status::kOk,
        .rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges};
    auto partialReq = request("GET");
    add(partialReq, RequestHeaderKind::kRange, "bytes=2-4");
    const auto partial = ruvia::planHttpRepresentationResponse(partialReq, kRepresentation, options);
    RUVIA_CHECK(partial.partial() != nullptr);
    if (const auto* range = partial.partial()) {
        RUVIA_CHECK_EQ(range->offset(), std::uint64_t{2});
        RUVIA_CHECK_EQ(range->length(), std::uint64_t{3});
    }
    RUVIA_CHECK_EQ(partial.status(), ruvia::http_status::kPartialContent);

    auto unsatReq = request("GET");
    add(unsatReq, RequestHeaderKind::kRange, "bytes=20-");
    const auto unsat = ruvia::planHttpRepresentationResponse(unsatReq, kRepresentation, options);
    RUVIA_CHECK(unsat.rangeUnsatisfiable() != nullptr);
    RUVIA_CHECK_EQ(unsat.status(), ruvia::http_status::kRangeNotSatisfiable);

    const auto empty = ruvia::planHttpRepresentationResponse(partialReq, {}, options);
    RUVIA_CHECK(empty.full() != nullptr);
    const auto disabled = ruvia::planHttpRepresentationResponse(partialReq, kRepresentation);
    RUVIA_CHECK(disabled.full() != nullptr);

    for (const auto range : {"items=1-2", "bytes=garbage", "bytes=", ""}) {
        auto req = request("GET");
        add(req, RequestHeaderKind::kRange, range);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, options);
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto multiReq = request("GET");
    add(multiReq, RequestHeaderKind::kRange, "bytes=1-2,4-5");
    const auto multi = ruvia::planHttpRepresentationResponse(multiReq, kRepresentation, options);
    RUVIA_CHECK(multi.multipart_ranges() != nullptr);
    RUVIA_CHECK_EQ(multi.status(), ruvia::http_status::kPartialContent);
    if (const auto* ranges = multi.multipart_ranges()) {
        RUVIA_CHECK_EQ(ranges->size(), std::size_t{2});
    }
}

RUVIA_TEST(representation_response_plan_preserves_unsatisfiable_ranges_after_empty_members) {
    const auto options = ruvia::HttpRepresentationResponseOptions{
        .rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges};
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"20-", "20-29", "-0"}) {
        const auto value = prefix + tail;
        auto req = request("GET");
        add(req, RequestHeaderKind::kRange, value);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, options);
        RUVIA_CHECK(plan.rangeUnsatisfiable() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::kRangeNotSatisfiable);
    }
    const auto over_limit = prefix + ",20-";
    auto req = request("GET");
    add(req, RequestHeaderKind::kRange, over_limit);
    const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, options);
    RUVIA_CHECK(plan.full() != nullptr);
    RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::kOk);
}

RUVIA_TEST(representation_response_plan_obeys_method_precondition_and_presence_precedence) {
    for (const auto method : {"GET", "HEAD", "POST"}) {
        auto req = request(method);
        add(req, RequestHeaderKind::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation);
        RUVIA_CHECK(plan.preconditionFailed() != nullptr);
    }
    for (const auto method : {"OPTIONS", "CONNECT"}) {
        auto req = request(method);
        add(req, RequestHeaderKind::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation);
        RUVIA_CHECK(plan.full() != nullptr);
    }

    auto precedence = request("GET");
    add(precedence, RequestHeaderKind::kIfMatch, R"("v1")");
    add(precedence, RequestHeaderKind::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
    add(precedence, RequestHeaderKind::kIfNoneMatch, R"("stale")");
    add(precedence, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto precedencePlan = ruvia::planHttpRepresentationResponse(precedence, kRepresentation);
    RUVIA_CHECK(precedencePlan.full() != nullptr);

    auto repeated = request("GET");
    add(repeated, RequestHeaderKind::kIfNoneMatch, R"("stale")");
    add(repeated, RequestHeaderKind::kIfNoneMatch, R"("v1")");
    const auto repeatedPlan = ruvia::planHttpRepresentationResponse(repeated, kRepresentation);
    RUVIA_CHECK(repeatedPlan.notModified() != nullptr);

    auto emptyNoneMatch = request("GET");
    add(emptyNoneMatch, RequestHeaderKind::kIfNoneMatch, "");
    add(emptyNoneMatch, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto emptyNoneMatchPlan = ruvia::planHttpRepresentationResponse(emptyNoneMatch, kRepresentation);
    RUVIA_CHECK(emptyNoneMatchPlan.full() != nullptr);

    auto failedFirst = request("GET");
    add(failedFirst, RequestHeaderKind::kIfMatch, R"("stale")");
    add(failedFirst, RequestHeaderKind::kIfNoneMatch, R"("v1")");
    add(failedFirst, RequestHeaderKind::kRange, "bytes=2-4");
    const auto failedFirstPlan = ruvia::planHttpRepresentationResponse(failedFirst, kRepresentation,
        {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
    RUVIA_CHECK(failedFirstPlan.preconditionFailed() != nullptr);
    RUVIA_CHECK_EQ(failedFirstPlan.status(), ruvia::http_status::kPreconditionFailed);
}

RUVIA_TEST(representation_response_plan_evaluates_supported_representation_extension_methods) {
    for (const auto method : {"UPDATE", "MERGE", "get", "trace"}) {
        auto exists = request(method);
        add(exists, RequestHeaderKind::kIfNoneMatch, "*");
        const auto exists_plan = ruvia::planHttpRepresentationResponse(exists, {.length = 10});
        RUVIA_CHECK_EQ(exists_plan.status(), ruvia::http_status::kPreconditionFailed);

        auto none_match = request(method);
        add(none_match, RequestHeaderKind::kIfNoneMatch, R"(W/"v1")");
        const auto none_match_plan = ruvia::planHttpRepresentationResponse(none_match, kRepresentation);
        RUVIA_CHECK(none_match_plan.preconditionFailed() != nullptr);

        auto match = request(method);
        add(match, RequestHeaderKind::kIfMatch, R"(W/"v1")");
        const auto match_plan = ruvia::planHttpRepresentationResponse(match, kRepresentation);
        RUVIA_CHECK(match_plan.preconditionFailed() != nullptr);

        auto unmodified = request(method);
        add(unmodified, RequestHeaderKind::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
        const auto unmodified_plan = ruvia::planHttpRepresentationResponse(unmodified, kRepresentation);
        RUVIA_CHECK(unmodified_plan.preconditionFailed() != nullptr);

        auto matched = request(method);
        add(matched, RequestHeaderKind::kIfMatch, R"("v1")");
        add(matched, RequestHeaderKind::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
        add(matched, RequestHeaderKind::kIfNoneMatch, R"("stale")");
        add(matched, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
        add(matched, RequestHeaderKind::kRange, "bytes=2-4");
        const auto matched_plan = ruvia::planHttpRepresentationResponse(matched, kRepresentation,
            {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
        RUVIA_CHECK(matched_plan.full() != nullptr);
        RUVIA_CHECK_EQ(matched_plan.status(), ruvia::http_status::kOk);

        auto modified_since = request(method);
        add(modified_since, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
        const auto modified_since_plan = ruvia::planHttpRepresentationResponse(modified_since, kRepresentation);
        RUVIA_CHECK_EQ(modified_since_plan.status(), ruvia::http_status::kOk);

        auto range = request(method);
        add(range, RequestHeaderKind::kRange, "bytes=2-4");
        add(range, RequestHeaderKind::kIfRange, R"("v1")");
        const auto range_plan = ruvia::planHttpRepresentationResponse(range, kRepresentation,
            {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
        RUVIA_CHECK(range_plan.full() != nullptr);
        RUVIA_CHECK_EQ(range_plan.status(), ruvia::http_status::kOk);

        auto wildcard_match = request(method);
        add(wildcard_match, RequestHeaderKind::kIfMatch, "*");
        const auto wildcard_match_plan = ruvia::planHttpRepresentationResponse(wildcard_match, {.length = 10});
        RUVIA_CHECK_EQ(wildcard_match_plan.status(), ruvia::http_status::kOk);

        const auto no_content = ruvia::planHttpRepresentationResponse(exists, kRepresentation,
            {.normalStatus = ruvia::http_status::kNoContent});
        RUVIA_CHECK_EQ(no_content.status(), ruvia::http_status::kPreconditionFailed);

        const auto precondition = ruvia::planHttpRepresentationResponse(matched, kRepresentation,
            {.normalStatus = ruvia::http_status::kPreconditionFailed});
        RUVIA_CHECK(precondition.full() != nullptr);
        RUVIA_CHECK_EQ(precondition.status(), ruvia::http_status::kPreconditionFailed);
    }
}

RUVIA_TEST(representation_response_plan_ignores_extension_conditions_for_ineligible_responses) {
    for (const auto status : {ruvia::http_status::kNotFound, ruvia::http_status::kNotImplemented,
             ruvia::http_status::kMethodNotAllowed, ruvia::http_status::kTemporaryRedirect}) {
        auto req = request("UPDATE");
        add(req, RequestHeaderKind::kIfNoneMatch, "*");
        add(req, RequestHeaderKind::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, {.normalStatus = status});
        RUVIA_CHECK(plan.full() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), status);
    }
}

RUVIA_TEST(representation_response_plan_uses_last_modified_only_when_present_and_if_range_is_strong) {
    auto notModified = request("GET");
    add(notModified, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto notModifiedPlan = ruvia::planHttpRepresentationResponse(notModified, kRepresentation);
    RUVIA_CHECK(notModifiedPlan.notModified() != nullptr);

    auto absentDate = request("GET");
    add(absentDate, RequestHeaderKind::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto absentDatePlan = ruvia::planHttpRepresentationResponse(absentDate,
        {.length = kRepresentation.length, .etag = kRepresentation.etag});
    RUVIA_CHECK(absentDatePlan.full() != nullptr);

    const auto rangeOptions = ruvia::HttpRepresentationResponseOptions{
        .normalStatus = ruvia::http_status::kOk,
        .rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges};
    for (const auto ifRange : {R"("v1")", "Sun, 06 Nov 1994 08:49:37 GMT"}) {
        auto req = request("GET");
        add(req, RequestHeaderKind::kRange, "bytes=1-2");
        add(req, RequestHeaderKind::kIfRange, ifRange);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, rangeOptions);
        RUVIA_CHECK(plan.partial() != nullptr);
    }
    for (const auto ifRange : {R"(W/"v1")", "Sun, 06 Nov 1994 08:49:38 GMT", ""}) {
        auto req = request("GET");
        add(req, RequestHeaderKind::kRange, "bytes=1-2");
        add(req, RequestHeaderKind::kIfRange, ifRange);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, rangeOptions);
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto missingDate = request("GET");
    add(missingDate, RequestHeaderKind::kRange, "bytes=1-2");
    add(missingDate, RequestHeaderKind::kIfRange, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto missingDatePlan = ruvia::planHttpRepresentationResponse(missingDate,
        {.length = kRepresentation.length, .etag = kRepresentation.etag}, rangeOptions);
    RUVIA_CHECK(missingDatePlan.full() != nullptr);
    auto weakDateMetadata = kRepresentation;
    weakDateMetadata.strongDateValidator = false;
    const auto weakDatePlan = ruvia::planHttpRepresentationResponse(missingDate, weakDateMetadata, rangeOptions);
    RUVIA_CHECK(weakDatePlan.full() != nullptr);

    auto tagOnly = request("GET");
    add(tagOnly, RequestHeaderKind::kRange, "bytes=1-2");
    add(tagOnly, RequestHeaderKind::kIfRange, R"("v1")");
    const auto tagOnlyPlan = ruvia::planHttpRepresentationResponse(tagOnly,
        {.length = 10, .etag = kRepresentation.etag}, rangeOptions);
    RUVIA_CHECK(tagOnlyPlan.partial() != nullptr);
}

RUVIA_TEST(representation_response_plan_checks_existing_representation_wildcards_and_method_roles) {
    for (const auto method : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "CONNECT", "TRACE"}) {
        auto req = request(method);
        add(req, RequestHeaderKind::kIfNoneMatch, "*");
        add(req, RequestHeaderKind::kRange, "bytes=2-4");
        const auto plan = ruvia::planHttpRepresentationResponse(req,
            {.length = 10}, {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
        if (std::string_view(method) == "GET" || std::string_view(method) == "HEAD") {
            RUVIA_CHECK(plan.notModified() != nullptr);
            RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::kNotModified);
        } else if (std::string_view(method) == "OPTIONS" || std::string_view(method) == "CONNECT" || std::string_view(method) == "TRACE") {
            RUVIA_CHECK(plan.full() != nullptr);
        } else {
            RUVIA_CHECK(plan.preconditionFailed() != nullptr);
        }
    }
    for (const auto method : {"HEAD", "POST"}) {
        auto req = request(method);
        add(req, RequestHeaderKind::kRange, "bytes=2-4");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation,
            {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto created = request("GET");
    add(created, RequestHeaderKind::kIfNoneMatch, "*");
    const auto createdPlan = ruvia::planHttpRepresentationResponse(created, kRepresentation,
        {.normalStatus = ruvia::http_status::kCreated});
    RUVIA_CHECK(createdPlan.notModified() != nullptr);
    auto precondition = request("GET");
    add(precondition, RequestHeaderKind::kIfMatch, R"("v1")");
    const auto preconditionPlan = ruvia::planHttpRepresentationResponse(precondition, kRepresentation,
        {.normalStatus = ruvia::http_status::kPreconditionFailed});
    RUVIA_CHECK(preconditionPlan.full() != nullptr);
    RUVIA_CHECK_EQ(preconditionPlan.status(), ruvia::http_status::kPreconditionFailed);
}

RUVIA_TEST(representation_response_plan_respects_date_failures_and_strong_versus_weak_tag_comparison) {
    auto unmodified = request("GET");
    add(unmodified, RequestHeaderKind::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
    const auto failed = ruvia::planHttpRepresentationResponse(unmodified, kRepresentation);
    RUVIA_CHECK(failed.preconditionFailed() != nullptr);
    const auto unavailable = ruvia::planHttpRepresentationResponse(unmodified, {.length = 10});
    RUVIA_CHECK(unavailable.full() != nullptr);
    auto malformed = request("GET");
    add(malformed, RequestHeaderKind::kIfUnmodifiedSince, "invalid-date");
    add(malformed, RequestHeaderKind::kIfModifiedSince, "invalid-date");
    const auto ignored = ruvia::planHttpRepresentationResponse(malformed, kRepresentation);
    RUVIA_CHECK(ignored.full() != nullptr);
    auto strong = request("GET");
    add(strong, RequestHeaderKind::kIfMatch, R"(W/"v1")");
    const auto strongPlan = ruvia::planHttpRepresentationResponse(strong, kRepresentation);
    RUVIA_CHECK(strongPlan.preconditionFailed() != nullptr);
    auto weak = request("GET");
    add(weak, RequestHeaderKind::kIfNoneMatch, R"(W/"v1")");
    const auto weakPlan = ruvia::planHttpRepresentationResponse(weak, kRepresentation);
    RUVIA_CHECK(weakPlan.notModified() != nullptr);
}

RUVIA_TEST(representation_response_plan_keeps_resolved_values_after_input_lifetimes_end) {
    const auto plan = [] {
        std::string range = "bytes=2-4";
        std::string etag = R"("temporary")";
        auto req = request("GET");
        add(req, RequestHeaderKind::kRange, range);
        add(req, RequestHeaderKind::kIfRange, etag);
        auto result = ruvia::planHttpRepresentationResponse(req,
            {.length = 10, .etag = etag}, {.rangePolicy = ruvia::HttpRangeRequestPolicy::honor_byte_ranges});
        range.assign(range.size(), 'x');
        etag.assign(etag.size(), 'x');
        return result;
    }();
    RUVIA_CHECK_EQ(plan.status(), ruvia::http_status::kPartialContent);
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
    RUVIA_CHECK(plan.segments()[1].kind ==
                ruvia::http_multipart_byte_range_plan::segment_kind::file);
    RUVIA_CHECK_EQ(plan.segments()[1].file_offset, std::uint64_t{0});
    RUVIA_CHECK_EQ(plan.segments()[1].file_length, std::uint64_t{3});
    RUVIA_CHECK(plan.metadata().find("Content-Type: text/plain") != std::string_view::npos);
    RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 7-9/10") != std::string_view::npos);
    std::uint64_t content_length = 0;
    for (const auto& segment : plan.segments()) {
        content_length += segment.kind == ruvia::http_multipart_byte_range_plan::segment_kind::file
                              ? segment.file_length
                              : segment.metadata_length;
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
        (void)ruvia::planHttpRepresentationResponse(request("GET"), kRepresentation,
            {.rangePolicy = static_cast<ruvia::HttpRangeRequestPolicy>(99)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
