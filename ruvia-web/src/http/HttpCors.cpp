#include "ruvia/web/detail/http/HttpCors.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpCorsFields.h"

namespace ruvia::detail {
namespace {

bool hasResponseHeader(const HttpResponse& response, std::string_view name) {
    return response.header(name).has_value();
}

void setResponseHeaderIfMissing(
    HttpResponse& response, std::string_view name, std::string_view value) {
    if (!hasResponseHeader(response, name)) {
        response.header(name, value);
    }
}

void addVaryTokens(HttpResponse& response, const std::string_view* tokens, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        response.addVaryToken(tokens[i]);
    }
}

void setCorsMaxAge(HttpResponse& response, const std::optional<std::chrono::seconds>& maxAge) {
    if (!maxAge.has_value() || hasResponseHeader(response, "Access-Control-Max-Age")) {
        return;
    }
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), maxAge->count());
    if (error != std::errc{}) {
        throw std::logic_error("CORS max age integer formatting failed");
    }
    response.header("Access-Control-Max-Age",
        std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data())));
}

void reflectCorsRequestHeaderNames(const HttpRequest& request, HttpResponse& response) {
    if (hasResponseHeader(response, "Access-Control-Allow-Headers")) {
        return;
    }

    bool first = true;
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!httpAsciiEqualsIgnoreCase(
                header.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        HttpCorsRequestHeaderNames names(header.value());
        while (const auto name = names.next()) {
            if (first) {
                setResponseHeaderIfMissing(response, "Access-Control-Allow-Headers", *name);
                first = false;
            } else {
                response.header("Access-Control-Allow-Headers", *name,
                    HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
            }
        }
        if (!names.valid()) {
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
            if (config.origin.value != "null" &&
                !::ruvia::isValidHttpSerializedOrigin(config.origin.value)) {
                throw std::invalid_argument("CORS origin must be a WHATWG serialized origin");
            }
            break;
        default:
            throw std::invalid_argument("CORS origin mode is invalid");
    }

    switch (config.requestHeaders.mode) {
        case CorsRequestHeadersMode::kReflect:
            if (!config.requestHeaders.names.empty()) {
                throw std::invalid_argument(
                    "CORS reflected request headers must not include fixed names");
            }
            break;
        case CorsRequestHeadersMode::kFixed:
            validateCorsHeaderNames(
                config.requestHeaders.names, "CORS fixed request headers must not be empty");
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
    const auto originField = request.header("Origin");
    const auto origin = originField.value_or(std::string_view{});
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
    setResponseHeaderIfMissing(response, "Access-Control-Allow-Origin", allowOrigin);
    if (cors.originMode == CorsOriginMode::kCredentialedExact) {
        setResponseHeaderIfMissing(response, "Access-Control-Allow-Credentials", "true");
    }

    const auto requestedMethod = request.header("Access-Control-Request-Method");
    const bool preflight = options && !origin.empty() && requestedMethod.has_value() &&
                           !requestedMethod->empty();
    if (preflight) {
        if (const auto allow = response.header("Allow"); allow.has_value() && !allow->empty()) {
            setResponseHeaderIfMissing(response, "Access-Control-Allow-Methods", *allow);
        }
        if (cors.requestHeadersMode == CorsRequestHeadersMode::kFixed) {
            setResponseHeaderIfMissing(response, "Access-Control-Allow-Headers", cors.requestHeaders);
        } else {
            reflectCorsRequestHeaderNames(request, response);
        }
        addVaryTokens(response, varyTokens.data(), varyTokenCount);
        setCorsMaxAge(response, cors.maxAge);
        return;
    }

    addVaryTokens(response, varyTokens.data(), varyTokenCount);
    if (!cors.exposeHeaders.empty()) {
        setResponseHeaderIfMissing(response, "Access-Control-Expose-Headers", cors.exposeHeaders);
    }
}

}  // namespace ruvia::detail
