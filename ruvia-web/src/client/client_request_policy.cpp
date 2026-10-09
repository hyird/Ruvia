#include "client/client_request_policy.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpSetCookie.h"

#include "client/ClientTransport.h"
#include "client/HttpClientConfigStorage.h"
#include "client/HttpClientRequestStorage.h"

namespace ruvia::detail {
namespace {

bool cookieDomainMatches(std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (httpAsciiEqualsIgnoreCase(host, domain)) {
        return true;
    }
    if (isClientIpAddress(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
           httpAsciiEqualsIgnoreCase(host.substr(host.size() - domain.size()), domain);
}

bool cookiePathMatches(std::string_view requestPath, std::string_view cookiePath) noexcept {
    if (cookiePath.empty() || cookiePath == "/") {
        return !requestPath.empty() && requestPath.front() == '/';
    }
    if (!requestPath.starts_with(cookiePath)) {
        return false;
    }
    return requestPath.size() == cookiePath.size() || cookiePath.back() == '/' ||
           requestPath[cookiePath.size()] == '/';
}

bool isValidReceivedCookieRequestValue(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return ::ruvia::isValidCookieValue(value);
}

bool canSerializeReceivedCookie(std::string_view name, std::string_view value) noexcept {
    return (name.empty() || isValidHttpHeaderName(name)) &&
           isValidReceivedCookieRequestValue(value);
}

std::string_view requestPathOnly(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view defaultCookiePath(std::string_view target) noexcept {
    const auto path = requestPathOnly(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                         : path.substr(0, slash);
}

std::chrono::system_clock::time_point cookieExpiration(
    std::chrono::system_clock::time_point now, std::int64_t maxAgeSeconds) noexcept {
    using Clock = std::chrono::system_clock;
    const std::chrono::duration<long double> requested{std::chrono::seconds(maxAgeSeconds)};
    const std::chrono::duration<long double> available{Clock::time_point::max() - now};
    if (requested >= available) {
        return Clock::time_point::max();
    }
    return now + std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(maxAgeSeconds));
}

}  // namespace

client_request_policy::client_request_policy(const HttpClientConfigStorage& config,
    std::pmr::memory_resource* resource)
    : config_(config),
      resource_(resource),
      cookies_(resource) {
    for (const auto& [name, value] : config.cookies) {
        add_cookie(name, value);
    }
}

void client_request_policy::add_cookie(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name) || !::ruvia::isValidCookieValue(value)) {
        throw std::invalid_argument("invalid HTTP client cookie");
    }
    const auto match = std::ranges::find_if(cookies_, [name](const stored_cookie& cookie) {
        return cookie.persistent && cookie.name == name && cookie.path == "/" &&
               cookie.domain.empty();
    });
    const auto replacementBytes = storage_bytes(name, value, "/", {});
    const auto replacedBytes =
        match == cookies_.end()
            ? 0
            : storage_bytes(match->name, match->value, match->path, match->domain);
    if (!has_capacity(replacedBytes, replacementBytes, match == cookies_.end())) {
        throw std::length_error("HTTP client cookie jar capacity exceeded");
    }
    if (match == cookies_.end()) {
        cookies_.emplace_back(name, value, resource_);
    } else {
        std::pmr::string replacementValue(value, resource_);
        match->value.swap(replacementValue);
    }
    cookie_bytes_ = cookie_bytes_ - replacedBytes + replacementBytes;
}

std::size_t client_request_policy::storage_bytes(std::string_view name, std::string_view value,
    std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += field.size();
    }
    return total;
}

bool client_request_policy::has_capacity(
    std::size_t replacedBytes, std::size_t replacementBytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= config_.maxCookies) {
        return false;
    }
    if (replacedBytes > cookie_bytes_) {
        return false;
    }
    const auto retainedBytes = cookie_bytes_ - replacedBytes;
    return replacementBytes <=
           config_.maxCookieBytes - std::min(retainedBytes, config_.maxCookieBytes);
}

void client_request_policy::discard_expired(std::chrono::system_clock::time_point now) {
    std::erase_if(cookies_, [this, now](const stored_cookie& cookie) {
        if (!cookie.expires.has_value() || cookie.expires.value() > now) {
            return false;
        }
        cookie_bytes_ -= storage_bytes(cookie.name, cookie.value, cookie.path, cookie.domain);
        return true;
    });
}

void client_request_policy::append_headers(const HttpClientRequestStorage& request,
    std::pmr::vector<HttpHeaderView>& headers, std::pmr::string& cookieHeader) {
    const auto hasHeader = [&headers](std::string_view name) {
        return std::ranges::any_of(headers,
            [name](const HttpHeaderView& header) { return httpAsciiEqualsIgnoreCase(header.name(), name); });
    };
    if (!config_.userAgent.empty() && !hasHeader("user-agent")) {
        headers.emplace_back("user-agent", config_.userAgent);
    }
    std::erase_if(headers, [&cookieHeader](const HttpHeaderView& header) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "cookie")) {
            return false;
        }
        if (!cookieHeader.empty()) {
            cookieHeader.append("; ");
        }
        cookieHeader.append(header.value());
        return true;
    });

    discard_expired(std::chrono::system_clock::now());
    const auto path = requestPathOnly(request.target());
    for (const auto& cookie : cookies_) {
        if (!cookie.persistent &&
            config_.receivedCookies == HttpClientReceivedCookiePolicy::kIgnore) {
            continue;
        }
        if (cookie.secure && config_.scheme != HttpScheme::kHttps) {
            continue;
        }
        if (!cookieDomainMatches(config_.host, cookie.domain) ||
            !cookiePathMatches(path, cookie.path)) {
            continue;
        }
        appendCookieRequestPair(cookieHeader, cookie.name, cookie.value);
    }
    if (!cookieHeader.empty()) {
        headers.emplace_back("cookie", cookieHeader);
    }
}

void client_request_policy::retain_response_cookies(
    const HttpClientRequestStorage& request, std::span<const HttpHeader> headers) {
    if (config_.receivedCookies == HttpClientReceivedCookiePolicy::kIgnore) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    discard_expired(now);
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "set-cookie")) {
            continue;
        }
        const auto parsed = parseSetCookie(header.value());
        if (!parsed) {
            continue;
        }
        const auto parsedName = parsed->name();
        const auto parsedValue = parsed->value();
        const auto parsedPath = parsed->path();
        const auto parsedDomain = parsed->domain();
        const bool parsedSecure = parsed->has(HttpSetCookieAttribute::kSecure);
        const bool parsedHasPath = parsed->has(HttpSetCookieAttribute::kPath);
        const bool parsedSameSiteNone = parsed->has(HttpSetCookieAttribute::kSameSiteNone);
        if ((parsedSecure && config_.scheme != HttpScheme::kHttps) ||
            !cookieDomainMatches(config_.host, parsedDomain) ||
            !canSerializeReceivedCookie(parsedName, parsedValue)) {
            continue;
        }

        const auto path = parsedPath.empty() || parsedPath.front() != '/'
                              ? defaultCookiePath(request.target())
                              : parsedPath;
        const bool securePrefixed = ::ruvia::cookieNameStartsWithIgnoreCase(parsedName, "__Secure-");
        const bool hostPrefixed = ::ruvia::cookieNameStartsWithIgnoreCase(parsedName, "__Host-");
        const bool namelessPrefix =
            parsedName.empty() && (::ruvia::cookieNameStartsWithIgnoreCase(parsedValue, "__Secure-") ||
                                      ::ruvia::cookieNameStartsWithIgnoreCase(parsedValue, "__Host-"));
        if (namelessPrefix || (parsedSameSiteNone && !parsedSecure) ||
            (securePrefixed && (!parsedSecure || config_.scheme != HttpScheme::kHttps)) ||
            (hostPrefixed && (!parsedSecure || config_.scheme != HttpScheme::kHttps ||
                                 !parsedHasPath || parsedPath != "/" || !parsedDomain.empty()))) {
            continue;
        }
        std::optional<std::chrono::system_clock::time_point> expires;
        bool remove = false;
        const auto maxAgeSeconds = parsed->maxAgeSeconds();
        const auto expiresAt = parsed->expires();
        if (maxAgeSeconds) {
            remove = *maxAgeSeconds <= 0;
            if (!remove) {
                expires = cookieExpiration(now, *maxAgeSeconds);
            }
        } else if (expiresAt) {
            remove = *expiresAt <= std::chrono::system_clock::to_time_t(now);
            if (!remove) {
                const auto expirationLimit = std::chrono::system_clock::to_time_t(
                    cookieExpiration(now, kMaxCookieAgeSeconds));
                expires =
                    std::chrono::system_clock::from_time_t(std::min(*expiresAt, expirationLimit));
            }
        }
        const auto parsedIdentityDomain =
            parsedDomain.empty() ? std::string_view(config_.host) : parsedDomain;
        const bool parsedHostOnly = parsedDomain.empty();
        const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& cookie) {
            const auto cookieIdentityDomain = cookie.domain.empty()
                                                  ? std::string_view(config_.host)
                                                  : std::string_view(cookie.domain);
            return !cookie.persistent && cookie.name == parsedName &&
                   cookie.host_only == parsedHostOnly && cookie.path == path &&
                   httpAsciiEqualsIgnoreCase(cookieIdentityDomain, parsedIdentityDomain);
        });
        if (remove) {
            if (match != cookies_.end()) {
                cookie_bytes_ -=
                    storage_bytes(match->name, match->value, match->path, match->domain);
                cookies_.erase(match);
            }
            continue;
        }
        const auto replacementBytes =
            storage_bytes(parsedName, parsedValue, path, parsedDomain);
        const auto replacedBytes =
            match == cookies_.end()
                ? 0
                : storage_bytes(match->name, match->value, match->path, match->domain);
        if (!has_capacity(replacedBytes, replacementBytes, match == cookies_.end())) {
            continue;
        }
        auto makeStoredCookie = [&]() {
            stored_cookie cookie(parsedName, parsedValue, resource_);
            cookie.path.assign(path);
            cookie.domain.assign(parsedDomain);
            cookie.expires = expires;
            cookie.secure = parsedSecure;
            cookie.host_only = parsedHostOnly;
            cookie.persistent = false;
            return cookie;
        };
        if (match == cookies_.end()) {
            const auto insertion = std::ranges::find_if(cookies_,
                [path](const stored_cookie& cookie) { return cookie.path.size() < path.size(); });
            const auto insertionIndex = static_cast<std::size_t>(insertion - cookies_.begin());
            auto cookie = makeStoredCookie();
            cookies_.reserve(cookies_.size() + 1);
            cookies_.emplace(
                cookies_.begin() + static_cast<std::ptrdiff_t>(insertionIndex), std::move(cookie));
        } else {
            auto replacement = makeStoredCookie();
            match->name.swap(replacement.name);
            match->value.swap(replacement.value);
            match->path.swap(replacement.path);
            match->domain.swap(replacement.domain);
            std::swap(match->expires, replacement.expires);
            std::swap(match->secure, replacement.secure);
            std::swap(match->host_only, replacement.host_only);
            std::swap(match->persistent, replacement.persistent);
        }
        cookie_bytes_ = cookie_bytes_ - replacedBytes + replacementBytes;
    }
}

}  // namespace ruvia::detail
