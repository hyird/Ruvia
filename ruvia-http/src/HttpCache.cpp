#include "ruvia/http/HttpCache.h"

#include <charconv>
#include <limits>

#include "ruvia/http/detail/HeaderTokenUtils.h"       // httpTrimOws, httpAsciiEqualsIgnoreCase
#include "ruvia/http/detail/HttpDate.h"

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Syntactically valid overflow saturates to the greatest convenient
// representation; non-digit / empty input remains invalid.
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
    if (ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    if (ec == std::errc::result_out_of_range) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::size_t cacheDirectiveEnd(
    std::string_view value,
    std::size_t begin) noexcept {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t cursor = begin; cursor < value.size(); ++cursor) {
        const auto ch = value[cursor];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = false;
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            return cursor;
        }
    }
    return value.size();
}

}  // namespace

CacheControl parseCacheControl(std::string_view value) noexcept {
    CacheControl result;
    bool maxAgeSeen = false;
    bool sMaxAgeSeen = false;
    bool staleWhileRevalidateSeen = false;
    bool staleIfErrorSeen = false;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = cacheDirectiveEnd(value, pos);
        auto token = value.substr(pos, comma - pos);
        pos = comma == value.size() ? value.size() : comma + 1;

        token = detail::httpTrimOws(token);
        if (token.empty()) {
            continue;
        }
        const auto eq = token.find('=');
        const bool hasArgument = eq != std::string_view::npos;
        const auto name = detail::httpTrimOws(eq == std::string_view::npos ? token : token.substr(0, eq));
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "no-store")) {
            result.noStore = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "no-cache")) {
            result.noCache = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "must-revalidate")) {
            result.mustRevalidate = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "proxy-revalidate")) {
            result.proxyRevalidate = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "private")) {
            result.isPrivate = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "public")) {
            result.isPublic = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "immutable")) {
            result.immutable = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "max-age")) {
            if (!maxAgeSeen) {
                maxAgeSeen = true;
                result.maxAge = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "s-maxage")) {
            if (!sMaxAgeSeen) {
                sMaxAgeSeen = true;
                result.sMaxAge = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-while-revalidate")) {
            if (!staleWhileRevalidateSeen) {
                staleWhileRevalidateSeen = true;
                result.staleWhileRevalidate = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-if-error")) {
            if (!staleIfErrorSeen) {
                staleIfErrorSeen = true;
                result.staleIfError = parseDeltaSeconds(arg);
            }
        }
    }
    return result;
}

std::optional<std::time_t> parseHttpDate(std::string_view value) noexcept {
    return detail::httpParseHttpDate(detail::httpTrimOws(value));
}

}  // namespace ruvia
