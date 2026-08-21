#pragma once

#include "ruvia/web/detail/server/response/HttpResponseCompression.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

#include <exception>
#include <optional>
#include <variant>

namespace ruvia::detail {

class HttpResponseCodingPolicyDisabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. Keeping that state distinct from the protocol result removes the
// old optional/reset channel that conflated "406 negotiation" with "do not
// transform this error again".
class HttpResponseCodingPolicy final {
public:
    [[nodiscard]] static HttpResponseCodingPolicy disabled() noexcept {
        return HttpResponseCodingPolicy(HttpResponseCodingPolicyDisabled{});
    }

    [[nodiscard]] static HttpResponseCodingPolicy selected(HttpResponseCodingSelection selection) noexcept {
        return HttpResponseCodingPolicy(selection);
    }

    // Keep a negotiation failure alive until a buffered handler has produced
    // its response status. A 204/304 has no response content, so rejecting it
    // before dispatch would turn a representation-free response into a false
    // 406. The identity selection is only a dispatch placeholder; the failure
    // bit below prevents it from becoming a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static HttpResponseCodingPolicy noAcceptableCoding() noexcept {
        HttpResponseCodingQualities absentHeader;
        const auto fallback = HttpResponseCodingSelection::select(absentHeader);
        const auto* selection = fallback.selected();
        if (selection == nullptr) {
            std::terminate();
        }
        return HttpResponseCodingPolicy(*selection, true);
    }

    [[nodiscard]] const HttpResponseCodingSelection* selection() const& noexcept {
        return std::get_if<HttpResponseCodingSelection>(&value_);
    }
    const HttpResponseCodingSelection* selection() const&& = delete;

    [[nodiscard]] bool negotiationFailed() const noexcept {
        return negotiationFailed_;
    }

private:
    explicit HttpResponseCodingPolicy(HttpResponseCodingPolicyDisabled disabled) noexcept
        : value_(disabled) {}

    explicit HttpResponseCodingPolicy(HttpResponseCodingSelection selection, bool negotiationFailed = false) noexcept
        : value_(selection),
          negotiationFailed_(negotiationFailed) {}

    using Value = std::variant<HttpResponseCodingSelection, HttpResponseCodingPolicyDisabled>;
    Value value_;
    bool negotiationFailed_{false};
};

// The wire plan alone cannot tell the protocol driver why a selected coding
// was not installed. Keep the compression outcome beside the finalized plan so
// a policy miss remains 406 while an encoder failure becomes a server error.
class HttpBufferedResponsePreparation final {
public:
    [[nodiscard]] HttpBufferedResponseWritePlan writePlan() const noexcept {
        return writePlan_;
    }

    [[nodiscard]] const HttpResponseCompressionResult& compressionResult() const& noexcept {
        return compressionResult_;
    }
    const HttpResponseCompressionResult& compressionResult() const&& = delete;

private:
    friend HttpBufferedResponsePreparation prepareBufferedHttpResponse(const HttpRequest&, const HttpResponseCodingPolicy&, HttpResponse&, const HttpServerOptions&);
    friend Task<HttpBufferedResponsePreparation> prepareBufferedHttpResponseAsync(const HttpRequest&, const HttpResponseCodingPolicy&, HttpResponse&, const HttpServerOptions&, const WorkerHandle&);

    HttpBufferedResponsePreparation(HttpBufferedResponseWritePlan writePlan, HttpResponseCompressionResult compressionResult) noexcept
        : writePlan_(writePlan),
          compressionResult_(compressionResult) {}

    HttpBufferedResponseWritePlan writePlan_;
    HttpResponseCompressionResult compressionResult_;
};

[[nodiscard]] inline HttpResponseCodingQualities httpResponseCodingQualitiesFor(const HttpRequest& request) noexcept {
    HttpResponseCodingQualities qualities;
    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return qualities;
}

[[nodiscard]] inline HttpResponseCodingSelectionResult httpResponseCodingFor(const HttpRequest& request) noexcept {
    return HttpResponseCodingSelection::select(httpResponseCodingQualitiesFor(request));
}

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool httpResponseNeedsNotAcceptable(
    const HttpResponseCodingPolicy& policy,
    const HttpRequest& request,
    const HttpResponse& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    if (policy.negotiationFailed()) {
        // The policy carries the client's empty acceptable set. Only a
        // response status that permits content can violate it; 204/205/304
        // are representation-free and must not be rejected before the
        // handler's final status is known.
        return httpResponseBodyPlan(request.knownMethod(), response.status()).statusAllowsBody();
    }
    return httpResponseCodingFallbackForbidden(
        *selection,
        request.knownMethod(),
        response);
}

[[nodiscard]] inline std::optional<HttpErrorInfo> httpBufferedResponsePreparationError(
    const HttpResponseCodingPolicy& policy,
    const HttpRequest& request,
    const HttpResponse& response,
    const HttpResponseCompressionResult& compressionResult) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && compressionResult.failed() && selection->coding() != HttpContentCoding::kIdentity && !selection->identityAccepted() && httpResponseBodyPlan(request.knownMethod(), response.status()).statusAllowsBody()) {
        return HttpErrorInfo({.status = http_status::kInternalServerError, .code = "response_compression_failed", .message = "response compression failed"});
    }
    if (httpResponseNeedsNotAcceptable(policy, request, response)) {
        return HttpErrorInfo({.status = http_status::kNotAcceptable, .code = "not_acceptable", .message = "no acceptable response content coding"});
    }
    return std::nullopt;
}

// This returns the one HTTP-owned snapshot both protocol drivers must consume;
// neither driver may re-plan after Web compression/CORS has finalized the
// response representation. A disabled policy is reserved for a terminal
// response that must not be transformed again.
[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(const HttpRequest& request, const HttpResponseCodingPolicy& policy, HttpResponse& response, const HttpServerOptions& options) {
    materializeResponseBody(response);
    if (options.cors.has_value()) {
        applyCorsHeaders(request, response, *options.cors);
    }
    auto compressionResult = HttpResponseCompressionResult::makeNotApplicable();
    if (options.compression.has_value()) {
        if (const auto* selection = policy.selection()) {
            compressionResult = applyResponseCompression(*selection, request.knownMethod(), response, *options.compression);
        }
    }
    return HttpBufferedResponsePreparation(httpBufferedResponseWritePlan(request.knownMethod(), response), compressionResult);
}

// Runtime preparation preserves the synchronous fast path for small in-memory
// responses and offloads larger eligible bodies through the configured bounded
// pool. Tests and non-runtime consumers can keep using the synchronous helper
// above when no worker resumption boundary exists.
[[nodiscard]] inline Task<HttpBufferedResponsePreparation> prepareBufferedHttpResponseAsync(const HttpRequest& request, const HttpResponseCodingPolicy& policy, HttpResponse& response, const HttpServerOptions& options, const WorkerHandle& worker) {
    materializeResponseBody(response);
    if (options.cors.has_value()) {
        applyCorsHeaders(request, response, *options.cors);
    }
    auto compressionResult = HttpResponseCompressionResult::makeNotApplicable();
    if (options.compression.has_value()) {
        if (const auto* selection = policy.selection()) {
            compressionResult = co_await applyResponseCompressionAsync(
                *selection,
                request.knownMethod(),
                response,
                *options.compression,
                options.blockingPool,
                worker);
        }
    }
    co_return HttpBufferedResponsePreparation(httpBufferedResponseWritePlan(request.knownMethod(), response), compressionResult);
}

}  // namespace ruvia::detail
