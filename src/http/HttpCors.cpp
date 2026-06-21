#include "HttpCors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ResponseHeaderUtils.h"

namespace ruvia::detail {
namespace {

void setCorsMaxAge(HttpResponse& response, std::chrono::seconds maxAge) {
    if (maxAge.count() <= 0 || response.hasKnownHeader(HttpResponse::kKnownHeaderAccessControlMaxAge)) {
        return;
    }
    setResponseHeaderUnsigned(
        response,
        "Access-Control-Max-Age",
        static_cast<std::uint64_t>(maxAge.count()),
        HttpResponse::kKnownHeaderAccessControlMaxAge);
}

}  // namespace

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const HttpServerOptions::Cors& cors) {
    if (!cors.enabled) {
        return;
    }

    const auto origin = request.header(HttpRequest::KnownHeader::kOrigin);
    if (origin.empty()) {
        return;
    }

    const auto configuredOrigin = std::string_view(cors.allowOrigin);
    const auto allowOrigin = configuredOrigin == "*" && cors.allowCredentials ? origin : configuredOrigin;
    std::array<std::string_view, 3> varyTokens{};
    std::size_t varyTokenCount = 0;
    setResponseHeaderIfMissing(
        response,
        HttpResponse::kKnownHeaderAccessControlAllowOrigin,
        "Access-Control-Allow-Origin",
        allowOrigin);
    if (allowOrigin != "*") {
        varyTokens[varyTokenCount++] = "Origin";
    }
    if (cors.allowCredentials && !response.hasKnownHeader(HttpResponse::kKnownHeaderAccessControlAllowCredentials)) {
        setResponseHeaderStableView(response, "Access-Control-Allow-Credentials", "true");
    }

    const bool preflight = request.method() == HttpMethod::kOptions &&
        !request.header(HttpRequest::KnownHeader::kAccessControlRequestMethod).empty();
    if (preflight) {
        if (const auto allow = response.header(HttpResponse::kKnownHeaderAllow); !allow.empty()) {
            setResponseHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowMethods,
                "Access-Control-Allow-Methods",
                allow);
        }
        const auto configuredHeaders = std::string_view(cors.allowHeaders);
        const auto requestedHeaders = request.header(HttpRequest::KnownHeader::kAccessControlRequestHeaders);
        if (!configuredHeaders.empty()) {
            setResponseHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                configuredHeaders);
        } else if (!requestedHeaders.empty()) {
            setResponseHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowHeaders,
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
        setResponseHeaderIfMissing(
            response,
            HttpResponse::kKnownHeaderAccessControlExposeHeaders,
            "Access-Control-Expose-Headers",
            cors.exposeHeaders);
    }
}

}  // namespace ruvia::detail
