#include "ruvia/http/HttpCache.h"

#include <limits>

#include "ruvia/http/detail/HeaderTokenUtils.h"       // httpTrimOws, httpAsciiEqualsIgnoreCase
#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parseDeltaSeconds(std::string_view value) noexcept {
    bool quoted = false;
    std::size_t begin = 0;
    std::size_t end = value.size();
    if (!value.empty() && value.front() == '"') {
        if (value.size() < 2 || value.back() != '"') {
            return std::nullopt;
        }
        quoted = true;
        begin = 1;
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }

    std::uint64_t result = 0;
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    for (std::size_t cursor = begin; cursor < end; ++cursor) {
        auto ch = static_cast<unsigned char>(value[cursor]);
        if (quoted && ch == '\\') {
            if (++cursor == end) {
                return std::nullopt;
            }
            ch = static_cast<unsigned char>(value[cursor]);
        }
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (maximum - digit) / 10) {
            result = maximum;
        } else {
            result = result * 10 + digit;
        }
    }
    return result;
}

[[nodiscard]] bool isValidCacheDirectiveArgument(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        for (const auto ch : value) {
            if (!detail::isHttpTokenChar(static_cast<unsigned char>(ch))) {
                return false;
            }
        }
        return true;
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }
    for (std::size_t cursor = 1; cursor + 1 < value.size(); ++cursor) {
        const auto ch = static_cast<unsigned char>(value[cursor]);
        if (ch == '\\') {
            if (++cursor + 1 >= value.size()) {
                return false;
            }
            const auto escaped = static_cast<unsigned char>(value[cursor]);
            if (escaped != '\t' && escaped != ' ' &&
                (escaped < 0x21 || escaped > 0x7e) && escaped < 0x80) {
                return false;
            }
        } else if (ch == '"' ||
                   (ch != '\t' && ch != ' ' && ch != 0x21 &&
                    (ch < 0x23 || ch > 0x5b) &&
                    (ch < 0x5d || ch > 0x7e) && ch < 0x80)) {
            return false;
        }
    }
    return true;
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
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "no-store")) {
            result.noStore = true;
        } else if ((!hasArgument || isValidCacheDirectiveArgument(arg)) &&
                   detail::httpAsciiEqualsIgnoreCase(name, "no-cache")) {
            result.noCache = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "must-revalidate")) {
            result.mustRevalidate = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "proxy-revalidate")) {
            result.proxyRevalidate = true;
        } else if ((!hasArgument || isValidCacheDirectiveArgument(arg)) &&
                   detail::httpAsciiEqualsIgnoreCase(name, "private")) {
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
