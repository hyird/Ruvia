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
        case HttpRangeRequestPolicy::kHonorSingleByteRange:
            break;
        default:
            throw std::invalid_argument("unknown HTTP range request policy");
    }

    const auto makeFull = [&]() noexcept {
        return HttpRepresentationResponsePlan(HttpRepresentationResponsePlan::Full{options.normalStatus});
    };
    const auto method = httpConditionalMethodPlan(request.knownMethod());
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
        request.knownMethod() != HttpKnownMethod::kGet ||
        options.rangePolicy != HttpRangeRequestPolicy::kHonorSingleByteRange || headers.range.empty()) {
        return makeFull();
    }

    if (headers.hasIfRange && !httpIfRangeAllows(headers.ifRange, representation.etag,
                                  representation.lastModified, representation.strongDateValidator)) {
        return makeFull();
    }

    const auto range = resolveHttpByteRange(headers.range, representation.length);
    if (range.ignored() != nullptr) {
        return makeFull();
    }
    if (range.unsatisfiable() != nullptr) {
        return HttpRepresentationResponsePlan(*range.unsatisfiable());
    }
    return HttpRepresentationResponsePlan(*range.resolved());
}

}  // namespace ruvia
