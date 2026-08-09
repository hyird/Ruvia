#include "ruvia/http/HttpSetCookie.h"

#include <charconv>

#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia {
namespace {

std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

std::pair<std::string_view, std::string_view> splitAttribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) return {trimOws(value), {}};
    return {trimOws(value.substr(0, equals)), trimOws(value.substr(equals + 1))};
}

}  // namespace

std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept {
    const auto firstEnd = value.find(';');
    const auto cookiePair = value.substr(0, firstEnd);
    if (cookiePair.find('=') == std::string_view::npos) return std::nullopt;
    auto [name, cookieValue] = splitAttribute(cookiePair);
    if (cookieValue.size() >= 2 && cookieValue.front() == '"' && cookieValue.back() == '"') {
        cookieValue = cookieValue.substr(1, cookieValue.size() - 2);
    } else if ((!cookieValue.empty() && cookieValue.front() == '"') || (!cookieValue.empty() && cookieValue.back() == '"')) {
        return std::nullopt;
    }
    if (name.empty() || !isValidHttpHeaderName(name) || !detail::isValidCookieValue(cookieValue)) return std::nullopt;

    HttpSetCookieView result{
        .name = name,
        .value = cookieValue,
        .path = {},
        .domain = {},
        .expires = std::nullopt,
        .maxAgeSeconds = std::nullopt,
        .secure = false,
    };
    std::size_t offset = firstEnd == std::string_view::npos ? value.size() : firstEnd + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = splitAttribute(value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (detail::httpAsciiEqualsIgnoreCase(attribute, "Path") && detail::isValidCookieAttribute(argument)) {
            result.path = argument;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Domain")) {
            auto domain = argument;
            if (!domain.empty() && domain.front() == '.') domain.remove_prefix(1);
            if (domain.empty() || !detail::isValidCookieDomain(domain)) return std::nullopt;
            result.domain = domain;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Expires")) {
            result.expires = parseHttpDate(argument);
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Max-Age")) {
            std::int64_t seconds = 0;
            const auto [parsed, error] = std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
            if (error == std::errc{} && parsed == argument.data() + argument.size()) result.maxAgeSeconds = seconds;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Secure")) {
            result.secure = true;
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia
