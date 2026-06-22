#include "HttpCors.h"

#include "HttpRequestInternal.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ResponseHeaderUtils.h"

namespace ruvia::detail {
namespace {

void setCorsMaxAge(HttpResponse& response, std::chrono::seconds maxAge) {
    if (maxAge.count() <= 0 ||
        responseHasKnownHeader(response, kResponseHeaderAccessControlMaxAge)) {
        return;
    }
    setResponseHeaderUnsigned(
        response,
        "Access-Control-Max-Age",
        static_cast<std::uint64_t>(maxAge.count()),
        kResponseHeaderAccessControlMaxAge);
}

}  // namespace

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const HttpServerOptions::Cors& cors) {
    if (!cors.enabled) {
        return;
    }

    const auto origin = requestKnownHeader(request, RequestKnownHeader::kOrigin);
    if (origin.empty()) {
        return;
    }

    const auto configuredOrigin = std::string_view(cors.allowOrigin);
    const bool reflectOrigin = configuredOrigin == "*" && cors.allowCredentials;
    const auto allowOrigin = reflectOrigin ? origin : configuredOrigin;
    std::array<std::string_view, 3> varyTokens{};
    std::size_t varyTokenCount = 0;
    if (reflectOrigin) {
        setResponseHeaderIfMissing(
            response,
            kResponseHeaderAccessControlAllowOrigin,
            "Access-Control-Allow-Origin",
            allowOrigin);
    } else {
        setStableResponseHeaderIfMissing(
            response,
            kResponseHeaderAccessControlAllowOrigin,
            "Access-Control-Allow-Origin",
            allowOrigin);
    }
    if (allowOrigin != "*") {
        varyTokens[varyTokenCount++] = "Origin";
    }
    if (cors.allowCredentials &&
        !responseHasKnownHeader(response, kResponseHeaderAccessControlAllowCredentials)) {
        setResponseHeaderStableView(response, "Access-Control-Allow-Credentials", "true");
    }

    const bool preflight = request.method() == HttpMethod::kOptions &&
        !requestKnownHeader(request, RequestKnownHeader::kAccessControlRequestMethod).empty();
    if (preflight) {
        if (const auto allow = responseKnownHeader(response, kResponseHeaderAllow); !allow.empty()) {
            setResponseHeaderIfMissing(
                response,
                kResponseHeaderAccessControlAllowMethods,
                "Access-Control-Allow-Methods",
                allow);
        }
        const auto configuredHeaders = std::string_view(cors.allowHeaders);
        const auto requestedHeaders = requestKnownHeader(request, RequestKnownHeader::kAccessControlRequestHeaders);
        if (!configuredHeaders.empty()) {
            setStableResponseHeaderIfMissing(
                response,
                kResponseHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                configuredHeaders);
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
            cors.exposeHeaders);
    }
}

}  // namespace ruvia::detail
