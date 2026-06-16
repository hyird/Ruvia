#include "HttpCors.h"

#include <array>
#include <charconv>
#include <system_error>

#include "ResponseHeaderUtils.h"

namespace ruvia::detail {
namespace {

void setCorsMaxAge(HttpResponse& response, std::chrono::seconds maxAge) {
    if (maxAge.count() <= 0 || response.hasKnownHeader(HttpResponse::kKnownHeaderAccessControlMaxAge)) {
        return;
    }

    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), maxAge.count());
    if (ec == std::errc{}) {
        response.setHeader(
            "Access-Control-Max-Age",
            std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
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
    setResponseHeaderIfMissing(
        response,
        HttpResponse::kKnownHeaderAccessControlAllowOrigin,
        "Access-Control-Allow-Origin",
        allowOrigin);
    if (allowOrigin != "*") {
        addVaryToken(response, "Origin");
    }
    if (cors.allowCredentials) {
        setResponseHeaderIfMissing(
            response,
            HttpResponse::kKnownHeaderAccessControlAllowCredentials,
            "Access-Control-Allow-Credentials",
            "true");
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
            addVaryToken(response, "Access-Control-Request-Headers");
        }
        addVaryToken(response, "Access-Control-Request-Method");
        setCorsMaxAge(response, cors.maxAge);
        return;
    }

    if (!cors.exposeHeaders.empty()) {
        setResponseHeaderIfMissing(
            response,
            HttpResponse::kKnownHeaderAccessControlExposeHeaders,
            "Access-Control-Expose-Headers",
            cors.exposeHeaders);
    }
}

}  // namespace ruvia::detail
