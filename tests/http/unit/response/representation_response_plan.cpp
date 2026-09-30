#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRepresentationResponsePlan.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

#include "test_harness.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;

HttpRequest request(std::string_view method) {
    auto result = HttpRequestAccess::make();
    HttpRequestAccess::setMethod(result, method);
    return result;
}

void add(HttpRequest& req, RequestKnownHeader name, std::string_view value) {
    std::string_view field;
    switch (name) {
        case RequestKnownHeader::kIfMatch:
            field = "If-Match";
            break;
        case RequestKnownHeader::kIfNoneMatch:
            field = "If-None-Match";
            break;
        case RequestKnownHeader::kIfModifiedSince:
            field = "If-Modified-Since";
            break;
        case RequestKnownHeader::kIfUnmodifiedSince:
            field = "If-Unmodified-Since";
            break;
        case RequestKnownHeader::kIfRange:
            field = "If-Range";
            break;
        case RequestKnownHeader::kRange:
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
        add(req, RequestKnownHeader::kIfNoneMatch, R"("v1")");
        add(req, RequestKnownHeader::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(
            req, kRepresentation, {.normalStatus = status});
        RUVIA_CHECK(plan.full() != nullptr);
        RUVIA_CHECK_EQ(plan.status(), status);
    }

    auto created = request("GET");
    add(created, RequestKnownHeader::kRange, "bytes=1-2");
    const auto createdPlan = ruvia::planHttpRepresentationResponse(
        created, kRepresentation, {.normalStatus = ruvia::http_status::kCreated, .rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange});
    RUVIA_CHECK(createdPlan.full() != nullptr);
    RUVIA_CHECK_EQ(createdPlan.status(), ruvia::http_status::kCreated);
}

RUVIA_TEST(representation_response_plan_resolves_supported_range_outcomes) {
    const auto options = ruvia::HttpRepresentationResponseOptions{
        .normalStatus = ruvia::http_status::kOk,
        .rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange};
    auto partialReq = request("GET");
    add(partialReq, RequestKnownHeader::kRange, "bytes=2-4");
    const auto partial = ruvia::planHttpRepresentationResponse(partialReq, kRepresentation, options);
    RUVIA_CHECK(partial.partial() != nullptr);
    if (const auto* range = partial.partial()) {
        RUVIA_CHECK_EQ(range->offset(), std::uint64_t{2});
        RUVIA_CHECK_EQ(range->length(), std::uint64_t{3});
    }
    RUVIA_CHECK_EQ(partial.status(), ruvia::http_status::kPartialContent);

    auto unsatReq = request("GET");
    add(unsatReq, RequestKnownHeader::kRange, "bytes=20-");
    const auto unsat = ruvia::planHttpRepresentationResponse(unsatReq, kRepresentation, options);
    RUVIA_CHECK(unsat.rangeUnsatisfiable() != nullptr);
    RUVIA_CHECK_EQ(unsat.status(), ruvia::http_status::kRangeNotSatisfiable);

    const auto empty = ruvia::planHttpRepresentationResponse(partialReq, {}, options);
    RUVIA_CHECK(empty.full() != nullptr);
    const auto disabled = ruvia::planHttpRepresentationResponse(partialReq, kRepresentation);
    RUVIA_CHECK(disabled.full() != nullptr);

    for (const auto range : {"items=1-2", "bytes=1-2,4-5", "bytes=garbage", "bytes=", ""}) {
        auto req = request("GET");
        add(req, RequestKnownHeader::kRange, range);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, options);
        RUVIA_CHECK(plan.full() != nullptr);
    }
}

RUVIA_TEST(representation_response_plan_obeys_method_precondition_and_presence_precedence) {
    for (const auto method : {"GET", "HEAD", "POST"}) {
        auto req = request(method);
        add(req, RequestKnownHeader::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation);
        RUVIA_CHECK(plan.preconditionFailed() != nullptr);
    }
    for (const auto method : {"OPTIONS", "CONNECT"}) {
        auto req = request(method);
        add(req, RequestKnownHeader::kIfMatch, R"("stale")");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation);
        RUVIA_CHECK(plan.full() != nullptr);
    }

    auto precedence = request("GET");
    add(precedence, RequestKnownHeader::kIfMatch, R"("v1")");
    add(precedence, RequestKnownHeader::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
    add(precedence, RequestKnownHeader::kIfNoneMatch, R"("stale")");
    add(precedence, RequestKnownHeader::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto precedencePlan = ruvia::planHttpRepresentationResponse(precedence, kRepresentation);
    RUVIA_CHECK(precedencePlan.full() != nullptr);

    auto repeated = request("GET");
    add(repeated, RequestKnownHeader::kIfNoneMatch, R"("stale")");
    add(repeated, RequestKnownHeader::kIfNoneMatch, R"("v1")");
    const auto repeatedPlan = ruvia::planHttpRepresentationResponse(repeated, kRepresentation);
    RUVIA_CHECK(repeatedPlan.notModified() != nullptr);

    auto emptyNoneMatch = request("GET");
    add(emptyNoneMatch, RequestKnownHeader::kIfNoneMatch, "");
    add(emptyNoneMatch, RequestKnownHeader::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:38 GMT");
    const auto emptyNoneMatchPlan = ruvia::planHttpRepresentationResponse(emptyNoneMatch, kRepresentation);
    RUVIA_CHECK(emptyNoneMatchPlan.full() != nullptr);

    auto failedFirst = request("GET");
    add(failedFirst, RequestKnownHeader::kIfMatch, R"("stale")");
    add(failedFirst, RequestKnownHeader::kIfNoneMatch, R"("v1")");
    add(failedFirst, RequestKnownHeader::kRange, "bytes=2-4");
    const auto failedFirstPlan = ruvia::planHttpRepresentationResponse(failedFirst, kRepresentation,
        {.rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange});
    RUVIA_CHECK(failedFirstPlan.preconditionFailed() != nullptr);
    RUVIA_CHECK_EQ(failedFirstPlan.status(), ruvia::http_status::kPreconditionFailed);
}

RUVIA_TEST(representation_response_plan_uses_last_modified_only_when_present_and_if_range_is_strong) {
    auto notModified = request("GET");
    add(notModified, RequestKnownHeader::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto notModifiedPlan = ruvia::planHttpRepresentationResponse(notModified, kRepresentation);
    RUVIA_CHECK(notModifiedPlan.notModified() != nullptr);

    auto absentDate = request("GET");
    add(absentDate, RequestKnownHeader::kIfModifiedSince, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto absentDatePlan = ruvia::planHttpRepresentationResponse(absentDate,
        {.length = kRepresentation.length, .etag = kRepresentation.etag});
    RUVIA_CHECK(absentDatePlan.full() != nullptr);

    const auto rangeOptions = ruvia::HttpRepresentationResponseOptions{
        .normalStatus = ruvia::http_status::kOk,
        .rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange};
    for (const auto ifRange : {R"("v1")", "Sun, 06 Nov 1994 08:49:37 GMT"}) {
        auto req = request("GET");
        add(req, RequestKnownHeader::kRange, "bytes=1-2");
        add(req, RequestKnownHeader::kIfRange, ifRange);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, rangeOptions);
        RUVIA_CHECK(plan.partial() != nullptr);
    }
    for (const auto ifRange : {R"(W/"v1")", "Sun, 06 Nov 1994 08:49:38 GMT", ""}) {
        auto req = request("GET");
        add(req, RequestKnownHeader::kRange, "bytes=1-2");
        add(req, RequestKnownHeader::kIfRange, ifRange);
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation, rangeOptions);
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto missingDate = request("GET");
    add(missingDate, RequestKnownHeader::kRange, "bytes=1-2");
    add(missingDate, RequestKnownHeader::kIfRange, "Sun, 06 Nov 1994 08:49:37 GMT");
    const auto missingDatePlan = ruvia::planHttpRepresentationResponse(missingDate,
        {.length = kRepresentation.length, .etag = kRepresentation.etag}, rangeOptions);
    RUVIA_CHECK(missingDatePlan.full() != nullptr);
    auto weakDateMetadata = kRepresentation;
    weakDateMetadata.strongDateValidator = false;
    const auto weakDatePlan = ruvia::planHttpRepresentationResponse(missingDate, weakDateMetadata, rangeOptions);
    RUVIA_CHECK(weakDatePlan.full() != nullptr);

    auto tagOnly = request("GET");
    add(tagOnly, RequestKnownHeader::kRange, "bytes=1-2");
    add(tagOnly, RequestKnownHeader::kIfRange, R"("v1")");
    const auto tagOnlyPlan = ruvia::planHttpRepresentationResponse(tagOnly,
        {.length = 10, .etag = kRepresentation.etag}, rangeOptions);
    RUVIA_CHECK(tagOnlyPlan.partial() != nullptr);
}

RUVIA_TEST(representation_response_plan_checks_existing_representation_wildcards_and_method_roles) {
    for (const auto method : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "CONNECT", "TRACE"}) {
        auto req = request(method);
        add(req, RequestKnownHeader::kIfNoneMatch, "*");
        add(req, RequestKnownHeader::kRange, "bytes=2-4");
        const auto plan = ruvia::planHttpRepresentationResponse(req,
            {.length = 10}, {.rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange});
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
        add(req, RequestKnownHeader::kRange, "bytes=2-4");
        const auto plan = ruvia::planHttpRepresentationResponse(req, kRepresentation,
            {.rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange});
        RUVIA_CHECK(plan.full() != nullptr);
    }
    auto created = request("GET");
    add(created, RequestKnownHeader::kIfNoneMatch, "*");
    const auto createdPlan = ruvia::planHttpRepresentationResponse(created, kRepresentation,
        {.normalStatus = ruvia::http_status::kCreated});
    RUVIA_CHECK(createdPlan.notModified() != nullptr);
    auto precondition = request("GET");
    add(precondition, RequestKnownHeader::kIfMatch, R"("v1")");
    const auto preconditionPlan = ruvia::planHttpRepresentationResponse(precondition, kRepresentation,
        {.normalStatus = ruvia::http_status::kPreconditionFailed});
    RUVIA_CHECK(preconditionPlan.full() != nullptr);
    RUVIA_CHECK_EQ(preconditionPlan.status(), ruvia::http_status::kPreconditionFailed);
}

RUVIA_TEST(representation_response_plan_respects_date_failures_and_strong_versus_weak_tag_comparison) {
    auto unmodified = request("GET");
    add(unmodified, RequestKnownHeader::kIfUnmodifiedSince, "Sun, 06 Nov 1994 08:49:36 GMT");
    const auto failed = ruvia::planHttpRepresentationResponse(unmodified, kRepresentation);
    RUVIA_CHECK(failed.preconditionFailed() != nullptr);
    const auto unavailable = ruvia::planHttpRepresentationResponse(unmodified, {.length = 10});
    RUVIA_CHECK(unavailable.full() != nullptr);
    auto malformed = request("GET");
    add(malformed, RequestKnownHeader::kIfUnmodifiedSince, "invalid-date");
    add(malformed, RequestKnownHeader::kIfModifiedSince, "invalid-date");
    const auto ignored = ruvia::planHttpRepresentationResponse(malformed, kRepresentation);
    RUVIA_CHECK(ignored.full() != nullptr);
    auto strong = request("GET");
    add(strong, RequestKnownHeader::kIfMatch, R"(W/"v1")");
    const auto strongPlan = ruvia::planHttpRepresentationResponse(strong, kRepresentation);
    RUVIA_CHECK(strongPlan.preconditionFailed() != nullptr);
    auto weak = request("GET");
    add(weak, RequestKnownHeader::kIfNoneMatch, R"(W/"v1")");
    const auto weakPlan = ruvia::planHttpRepresentationResponse(weak, kRepresentation);
    RUVIA_CHECK(weakPlan.notModified() != nullptr);
}

RUVIA_TEST(representation_response_plan_keeps_resolved_values_after_input_lifetimes_end) {
    const auto plan = [] {
        std::string range = "bytes=2-4";
        std::string etag = R"("temporary")";
        auto req = request("GET");
        add(req, RequestKnownHeader::kRange, range);
        add(req, RequestKnownHeader::kIfRange, etag);
        auto result = ruvia::planHttpRepresentationResponse(req,
            {.length = 10, .etag = etag}, {.rangePolicy = ruvia::HttpRangeRequestPolicy::kHonorSingleByteRange});
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
