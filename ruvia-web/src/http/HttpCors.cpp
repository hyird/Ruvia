#include "ruvia/web/detail/http/HttpCors.h"

#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/response/ResponseHeaderUtils.h"

namespace ruvia::detail {
namespace {

void setCorsMaxAge(HttpResponse& response, const std::optional<std::chrono::seconds>& maxAge) {
    if (!maxAge.has_value() || responseHasKnownHeader(response, kResponseHeaderAccessControlMaxAge)) {
        return;
    }
    setResponseHeaderUnsigned(response, "Access-Control-Max-Age", static_cast<std::uint64_t>(maxAge->count()), kResponseHeaderAccessControlMaxAge);
}

void reflectCorsRequestHeaderNames(const HttpRequest& request, HttpResponse& response) {
    if (responseHasKnownHeader(response, kResponseHeaderAccessControlAllowHeaders)) {
        return;
    }

    bool first = true;
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        const bool valid = visitHttpCorsRequestHeaderNames(header.value(), [&response, &first](std::string_view name) {
            if (first) {
                setResponseHeaderIfMissing(response, kResponseHeaderAccessControlAllowHeaders, "Access-Control-Allow-Headers", name);
                first = false;
            } else {
                response.header("Access-Control-Allow-Headers", name, HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
            }
            return true;
        });
        if (!valid) {
            throw std::logic_error("validated CORS request header list became invalid");
        }
    }
}

void validateCorsHeaderNames(const std::vector<std::string>& names, const char* emptyMessage) {
    if (emptyMessage != nullptr && names.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    for (const auto& name : names) {
        if (!isValidHttpHeaderName(name)) {
            throw std::invalid_argument("CORS header names must be valid HTTP field names");
        }
    }
}

void appendCorsHeaderNames(std::pmr::string& output, const std::vector<std::string>& names) {
    for (const auto& name : names) {
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(name);
    }
}

void validateCorsConfig(const CorsConfig& config) {
    switch (config.origin.mode) {
        case CorsOriginMode::kAny:
            if (!config.origin.value.empty()) {
                throw std::invalid_argument("CORS wildcard origin must not include a value");
            }
            break;
        case CorsOriginMode::kExact:
        case CorsOriginMode::kCredentialedExact:
            if (config.origin.value != "null" && !isValidHttpSerializedOrigin(config.origin.value)) {
                throw std::invalid_argument("CORS origin must be a WHATWG serialized origin");
            }
            break;
        default:
            throw std::invalid_argument("CORS origin mode is invalid");
    }

    switch (config.requestHeaders.mode) {
        case CorsRequestHeadersMode::kReflect:
            if (!config.requestHeaders.names.empty()) {
                throw std::invalid_argument("CORS reflected request headers must not include fixed names");
            }
            break;
        case CorsRequestHeadersMode::kFixed:
            validateCorsHeaderNames(config.requestHeaders.names, "CORS fixed request headers must not be empty");
            break;
        default:
            throw std::invalid_argument("CORS request headers mode is invalid");
    }
    validateCorsHeaderNames(config.exposeHeaders, nullptr);
    if (config.maxAge.has_value() && config.maxAge->count() < 0) {
        throw std::invalid_argument("CORS max age must not be negative");
    }
}

}  // namespace

CorsOptions makeCorsOptions(const CorsConfig& config, std::pmr::memory_resource* resource) {
    validateCorsConfig(config);

    CorsOptions stored(resource);
    stored.originMode = config.origin.mode;
    if (config.origin.mode != CorsOriginMode::kAny) {
        stored.origin = config.origin.value;
    }

    stored.requestHeadersMode = config.requestHeaders.mode;
    if (config.requestHeaders.mode == CorsRequestHeadersMode::kFixed) {
        appendCorsHeaderNames(stored.requestHeaders, config.requestHeaders.names);
    }
    appendCorsHeaderNames(stored.exposeHeaders, config.exposeHeaders);
    stored.maxAge = config.maxAge;
    return stored;
}

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const CorsOptions& cors) {
    const auto origin = requestKnownHeader(request, RequestKnownHeader::kOrigin);
    const bool wildcardOrigin = cors.originMode == CorsOriginMode::kAny;
    const auto allowOrigin = wildcardOrigin ? std::string_view("*") : std::string_view(cors.origin);
    std::array<std::string_view, 3> varyTokens{};
    std::size_t varyTokenCount = 0;
    const bool options = request.knownMethod() == HttpKnownMethod::kOptions;
    if (options) {
        varyTokens[varyTokenCount++] = "Origin";
        varyTokens[varyTokenCount++] = "Access-Control-Request-Method";
        if (cors.requestHeadersMode == CorsRequestHeadersMode::kReflect) {
            varyTokens[varyTokenCount++] = "Access-Control-Request-Headers";
        }
    }
    setStableResponseHeaderIfMissing(response, kResponseHeaderAccessControlAllowOrigin, "Access-Control-Allow-Origin", allowOrigin);
    if (cors.originMode == CorsOriginMode::kCredentialedExact && !responseHasKnownHeader(response, kResponseHeaderAccessControlAllowCredentials)) {
        setResponseHeaderStableView(response, "Access-Control-Allow-Credentials", "true");
    }

    const bool preflight = options && !origin.empty() && !requestKnownHeader(request, RequestKnownHeader::kAccessControlRequestMethod).empty();
    if (preflight) {
        if (const auto allow = responseKnownHeader(response, kResponseHeaderAllow); !allow.empty()) {
            setResponseHeaderIfMissing(response, kResponseHeaderAccessControlAllowMethods, "Access-Control-Allow-Methods", allow);
        }
        if (cors.requestHeadersMode == CorsRequestHeadersMode::kFixed) {
            setStableResponseHeaderIfMissing(response, kResponseHeaderAccessControlAllowHeaders, "Access-Control-Allow-Headers", cors.requestHeaders);
        } else {
            reflectCorsRequestHeaderNames(request, response);
        }
        addVaryTokens(response, varyTokens.data(), varyTokenCount);
        setCorsMaxAge(response, cors.maxAge);
        return;
    }

    addVaryTokens(response, varyTokens.data(), varyTokenCount);
    if (!cors.exposeHeaders.empty()) {
        setStableResponseHeaderIfMissing(response, kResponseHeaderAccessControlExposeHeaders, "Access-Control-Expose-Headers", cors.exposeHeaders);
    }
}

}  // namespace ruvia::detail
