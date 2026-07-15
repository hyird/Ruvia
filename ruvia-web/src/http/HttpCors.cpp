#include "ruvia/web/detail/http/HttpCors.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/ResponseHeaderUtils.h"

namespace ruvia::detail {
namespace {

void setCorsMaxAge(
    HttpResponse& response,
    const std::optional<CorsMaxAge>& maxAge) {
    if (!maxAge.has_value() ||
        responseHasKnownHeader(response, kResponseHeaderAccessControlMaxAge)) {
        return;
    }
    setResponseHeaderUnsigned(
        response,
        "Access-Control-Max-Age",
        static_cast<std::uint64_t>(maxAge->value().count()),
        kResponseHeaderAccessControlMaxAge);
}

}  // namespace

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const CorsConfig& cors) {
    const auto origin = requestKnownHeader(request, RequestKnownHeader::kOrigin);
    if (origin.empty()) {
        return;
    }

    const bool wildcardOrigin =
        cors.origin.kind() == CorsOriginPolicy::Kind::kAny;
    const auto allowOrigin = wildcardOrigin
        ? std::string_view("*")
        : cors.origin.origin();
    std::array<std::string_view, 3> varyTokens{};
    std::size_t varyTokenCount = 0;
    setStableResponseHeaderIfMissing(
        response,
        kResponseHeaderAccessControlAllowOrigin,
        "Access-Control-Allow-Origin",
        allowOrigin);
    if (!wildcardOrigin) {
        varyTokens[varyTokenCount++] = "Origin";
    }
    if (cors.origin.kind() == CorsOriginPolicy::Kind::kCredentialedExact &&
        !responseHasKnownHeader(response, kResponseHeaderAccessControlAllowCredentials)) {
        setResponseHeaderStableView(response, "Access-Control-Allow-Credentials", "true");
    }

    const bool preflight = request.knownMethod() == HttpKnownMethod::kOptions &&
        !requestKnownHeader(request, RequestKnownHeader::kAccessControlRequestMethod).empty();
    if (preflight) {
        if (const auto allow = responseKnownHeader(response, kResponseHeaderAllow); !allow.empty()) {
            setResponseHeaderIfMissing(
                response,
                kResponseHeaderAccessControlAllowMethods,
                "Access-Control-Allow-Methods",
                allow);
        }
        const auto requestedHeaders = requestKnownHeader(request, RequestKnownHeader::kAccessControlRequestHeaders);
        if (cors.requestHeaders.kind() == CorsRequestHeadersPolicy::Kind::kFixed) {
            setStableResponseHeaderIfMissing(
                response,
                kResponseHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                cors.requestHeaders.headers());
        } else if (!requestedHeaders.empty()) {
            setResponseHeaderIfMissing(
                response,
                kResponseHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                requestedHeaders);
            varyTokens[varyTokenCount++] = "Access-Control-Request-Headers";
        }
        varyTokens[varyTokenCount++] = "Access-Control-Request-Method";
        addVaryTokens(response, varyTokens.data(), varyTokenCount);
        setCorsMaxAge(response, cors.maxAge);
        return;
    }

    addVaryTokens(response, varyTokens.data(), varyTokenCount);
    if (!cors.exposeHeaders.empty()) {
        setStableResponseHeaderIfMissing(
            response,
            kResponseHeaderAccessControlExposeHeaders,
            "Access-Control-Expose-Headers",
            cors.exposeHeaders.value());
    }
}

}  // namespace ruvia::detail

namespace ruvia {

CorsHeaderNames CorsHeaderNames::of(
    std::span<const std::string_view> names) {
    std::pmr::string value;
    for (const auto name : names) {
        if (!isValidHttpHeaderName(name)) {
            throw std::invalid_argument(
                "CORS header names must be valid HTTP field names");
        }
        if (!value.empty()) {
            value.append(", ");
        }
        value.append(name);
    }
    return CorsHeaderNames(std::move(value));
}

}  // namespace ruvia
