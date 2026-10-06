#include "ruvia/http/HttpRepresentationResponsePlan.h"

#include <stdexcept>

#include "ruvia/http/HttpConditionalRequest.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia {

const HttpRepresentationResponsePlan::Full* HttpRepresentationResponsePlan::full() const& noexcept {
    return std::get_if<Full>(&value_);
}

const HttpRepresentationResponsePlan::NotModified*
HttpRepresentationResponsePlan::notModified() const& noexcept {
    return std::get_if<NotModified>(&value_);
}

const HttpRepresentationResponsePlan::PreconditionFailed*
HttpRepresentationResponsePlan::preconditionFailed() const& noexcept {
    return std::get_if<PreconditionFailed>(&value_);
}

const HttpRepresentationResponsePlan::RangeUnsatisfiable*
HttpRepresentationResponsePlan::rangeUnsatisfiable() const& noexcept {
    return std::get_if<RangeUnsatisfiable>(&value_);
}

const HttpRepresentationResponsePlan::Partial* HttpRepresentationResponsePlan::partial() const& noexcept {
    return std::get_if<Partial>(&value_);
}

const HttpRepresentationResponsePlan::multipart*
HttpRepresentationResponsePlan::multipart_ranges() const& noexcept {
    return std::get_if<multipart>(&value_);
}

HttpStatusCode HttpRepresentationResponsePlan::status() const noexcept {
    if (const auto* outcome = full()) {
        return outcome->status;
    }
    if (notModified() != nullptr) {
        return http_status::kNotModified;
    }
    if (preconditionFailed() != nullptr) {
        return http_status::kPreconditionFailed;
    }
    if (rangeUnsatisfiable() != nullptr) {
        return http_status::kRangeNotSatisfiable;
    }
    return http_status::kPartialContent;
}

HttpRepresentationResponsePlan planHttpRepresentationResponse(
    const HttpRequest& request, HttpSelectedRepresentationMetadata representation,
    HttpRepresentationResponseOptions options) {
    switch (options.rangePolicy) {
        case HttpRangeRequestPolicy::kIgnore:
        case HttpRangeRequestPolicy::honor_byte_ranges:
            break;
        default:
            throw std::invalid_argument("unknown HTTP range request policy");
    }

    const auto makeFull = [&]() noexcept {
        return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::Full{options.normalStatus});
    };
    auto method = httpConditionalMethodPlan(request.knownMethod());
    if (request.knownMethod() == HttpKnownMethod::kUnknown &&
        !request.method().empty() && request.method() != "TRACE") {
        method.evaluatesPreconditions = true;
    }
    if (!method.evaluatesPreconditions ||
        (!options.normalStatus.isSuccessful() && options.normalStatus != http_status::kPreconditionFailed)) {
        return makeFull();
    }

    const auto headers = httpConditionalHeaders(request);
    const auto etags = httpEtagPreconditions(request, representation.etag);
    if (etags.ifMatch.present) {
        if (!etags.ifMatch.matches()) {
            return HttpRepresentationResponsePlan(
                HttpRepresentationResponsePlan::PreconditionFailed{});
        }
    } else if (representation.lastModified && !headers.ifUnmodifiedSince.empty() &&
               !httpDateUnmodified(headers.ifUnmodifiedSince, *representation.lastModified)) {
        return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::PreconditionFailed{});
    }

    if (etags.ifNoneMatch.present) {
        if (etags.ifNoneMatch.matches()) {
            if (method.usesNotModifiedResponse) {
                return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::NotModified{});
            }
            return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::PreconditionFailed{});
        }
    } else if (method.evaluatesIfModifiedSince && representation.lastModified &&
               !headers.ifModifiedSince.empty() &&
               httpDateNotModified(headers.ifModifiedSince, *representation.lastModified)) {
        return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::NotModified{});
    }

    if (options.normalStatus != http_status::kOk ||
        !method.evaluatesRange ||
        options.rangePolicy != HttpRangeRequestPolicy::honor_byte_ranges || headers.range.empty()) {
        return makeFull();
    }

    if (headers.hasIfRange && !httpIfRangeAllows(headers.ifRange, representation.etag,
                                  representation.lastModified, representation.strongDateValidator)) {
        return makeFull();
    }

    const auto ranges = resolve_http_byte_range_set(headers.range, representation.length);
    if (ranges.ignored()) {
        return makeFull();
    }
    if (ranges.unsatisfiable()) {
        return HttpRepresentationResponsePlan(ranges.unsatisfiable_outcome());
    }
    if (ranges.size() == 1) {
        return HttpRepresentationResponsePlan(ranges.resolved_range(0));
    }
    return HttpRepresentationResponsePlan(ranges);
}

}  // namespace ruvia
