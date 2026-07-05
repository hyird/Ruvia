#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "../HeaderTokenUtils.h"
#include "../parser/HttpRequestTarget.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

[[nodiscard]] inline bool isValidHttpClientOriginTarget(std::string_view target) noexcept {
    return isValidOriginFormTarget(target);
}

[[nodiscard]] inline bool isHttpClientRedirectStatus(std::uint16_t status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] inline std::string_view findUniqueHttpClientResponseHeader(
    const FetchResponse& response,
    std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : response.headers()) {
        if (asciiEqualsIgnoreCase(header.name(), name)) {
            if (seen) {
                return {};
            }
            seen = true;
            found = header.value();
        }
    }
    return found;
}

inline void applyHttpClientRedirectMethod(FetchOptions& options, std::uint16_t status) {
    if (status == 303) {
        if (!asciiEqualsIgnoreCase(options.method, "HEAD")) {
            options.method = "GET";
        }
        options.body = {};
        options.bodyStream = {};
        return;
    }
    if (status == 301 || status == 302) {
        if (!asciiEqualsIgnoreCase(options.method, "GET") &&
            !asciiEqualsIgnoreCase(options.method, "HEAD")) {
            options.method = "GET";
            options.body = {};
            options.bodyStream = {};
        }
    }
}

[[nodiscard]] inline bool canReplayHttpClientRedirectRequest(
    const FetchOptions& options,
    std::uint16_t status) noexcept {
    if (!options.bodyStream) {
        return true;
    }
    if (status == 303) {
        return true;
    }
    if (status == 301 || status == 302) {
        return !asciiEqualsIgnoreCase(options.method, "GET") &&
            !asciiEqualsIgnoreCase(options.method, "HEAD");
    }
    return false;
}

[[nodiscard]] inline bool httpClientAuthorityMatchesOrigin(
    const HttpClientConfig& config,
    std::string_view authority,
    std::uint16_t defaultPort) noexcept {
    if (authority.find('@') != std::string_view::npos) {
        return false;
    }

    std::string_view host;
    std::string_view portText;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = authority.substr(1, close - 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return false;
            }
            portText = after.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        }
    }

    std::uint16_t port = defaultPort;
    if (!portText.empty()) {
        std::uint32_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(
            portText.data(), portText.data() + portText.size(), parsed);
        if (ec != std::errc{} || ptr != portText.data() + portText.size() ||
            parsed == 0 || parsed > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    return port == config.port && asciiEqualsIgnoreCase(host, std::string_view(config.host));
}

[[nodiscard]] inline bool resolveHttpClientSameOriginRedirect(
    const HttpClientConfig& config,
    std::string_view location,
    std::pmr::string& outPath) {
    location = httpTrimOws(location);
    if (location.empty()) {
        return false;
    }

    std::string_view path;
    if (location.front() == '/' && (location.size() < 2 || location[1] != '/')) {
        path = location;
    } else {
        std::string_view afterScheme;
        if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
            afterScheme = location.substr(2);
        } else {
            const auto schemeEnd = location.find("://");
            if (schemeEnd == std::string_view::npos) {
                return false;
            }
            const auto scheme = location.substr(0, schemeEnd);
            const bool wantsTls = asciiEqualsIgnoreCase(scheme, "https");
            if (!wantsTls && !asciiEqualsIgnoreCase(scheme, "http")) {
                return false;
            }
            if (wantsTls != config.tls) {
                return false;
            }
            afterScheme = location.substr(schemeEnd + 3);
        }

        const auto authorityEnd = afterScheme.find_first_of("/?#");
        const auto authority = authorityEnd == std::string_view::npos
            ? afterScheme
            : afterScheme.substr(0, authorityEnd);
        if (!httpClientAuthorityMatchesOrigin(config, authority, config.tls ? 443 : 80)) {
            return false;
        }
        path = authorityEnd == std::string_view::npos
            ? std::string_view{}
            : afterScheme.substr(authorityEnd);
    }

    if (const auto hash = path.find('#'); hash != std::string_view::npos) {
        path = path.substr(0, hash);
    }
    outPath.clear();
    if (path.empty() || path.front() != '/') {
        outPath.push_back('/');
    }
    outPath.append(path.data(), path.size());
    return isValidHttpClientOriginTarget(outPath);
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
