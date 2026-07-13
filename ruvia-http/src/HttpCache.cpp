#include "ruvia/http/HttpCache.h"

#include <charconv>

#include "ruvia/http/detail/HeaderTokenUtils.h"       // httpTrimOws, httpAsciiEqualsIgnoreCase
#include "ruvia/http/detail/HttpDate.h"

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Overflow / non-digit / empty yields nullopt.
[[nodiscard]] std::optional<std::uint64_t> parseDeltaSeconds(std::string_view value) noexcept {
    value = detail::httpTrimOws(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace

CacheControl parseCacheControl(std::string_view value) noexcept {
    CacheControl result;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = value.find(',', pos);
        auto token = value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = comma == std::string_view::npos ? value.size() : comma + 1;

        token = detail::httpTrimOws(token);
        if (token.empty()) {
            continue;
        }
        const auto eq = token.find('=');
        const auto name = detail::httpTrimOws(eq == std::string_view::npos ? token : token.substr(0, eq));
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (detail::httpAsciiEqualsIgnoreCase(name, "no-store")) {
            result.noStore = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "no-cache")) {
            result.noCache = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "must-revalidate")) {
            result.mustRevalidate = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "proxy-revalidate")) {
            result.proxyRevalidate = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "private")) {
            result.isPrivate = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "public")) {
            result.isPublic = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "immutable")) {
            result.immutable = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "max-age")) {
            result.maxAge = parseDeltaSeconds(arg);
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "s-maxage")) {
            result.sMaxAge = parseDeltaSeconds(arg);
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-while-revalidate")) {
            result.staleWhileRevalidate = parseDeltaSeconds(arg);
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-if-error")) {
            result.staleIfError = parseDeltaSeconds(arg);
        }
    }
    return result;
}

std::optional<std::time_t> parseHttpDate(std::string_view value) noexcept {
    return detail::httpParseHttpDate(detail::httpTrimOws(value));
}

}  // namespace ruvia
