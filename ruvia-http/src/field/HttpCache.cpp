#include "ruvia/http/HttpCache.h"

#include <algorithm>
#include <limits>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"  // httpTrimOws, httpAsciiEqualsIgnoreCase
#include "ruvia/http/detail/field/HttpDate.h"
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
        return std::ranges::all_of(value, [](char ch) noexcept { return detail::isHttpTokenChar(static_cast<unsigned char>(ch)); });
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
            if (escaped != '\t' && escaped != ' ' && (escaped < 0x21 || escaped > 0x7e) && escaped < 0x80) {
                return false;
            }
        } else if (ch == '"' || (ch != '\t' && ch != ' ' && ch != 0x21 && (ch < 0x23 || ch > 0x5b) && (ch < 0x5d || ch > 0x7e) && ch < 0x80)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t cacheDirectiveEnd(std::string_view value, std::size_t begin) noexcept {
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

void CacheControlFieldParser::update(std::string_view value) noexcept {
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
            value_.noStore = true;
        } else if ((!hasArgument || isValidCacheDirectiveArgument(arg)) && detail::httpAsciiEqualsIgnoreCase(name, "no-cache")) {
            value_.noCache = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "no-transform")) {
            value_.noTransform = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "must-revalidate")) {
            value_.mustRevalidate = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "proxy-revalidate")) {
            value_.proxyRevalidate = true;
        } else if ((!hasArgument || isValidCacheDirectiveArgument(arg)) && detail::httpAsciiEqualsIgnoreCase(name, "private")) {
            value_.isPrivate = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "public")) {
            value_.isPublic = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "immutable")) {
            value_.immutable = true;
        } else if (!hasArgument && detail::httpAsciiEqualsIgnoreCase(name, "only-if-cached")) {
            value_.onlyIfCached = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "max-age")) {
            if (!maxAgeSeen_) {
                maxAgeSeen_ = true;
                value_.maxAge = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "max-stale")) {
            if (!maxStaleSeen_) {
                maxStaleSeen_ = true;
                if (!hasArgument) {
                    value_.maxStaleAny = true;
                } else {
                    value_.maxStale = parseDeltaSeconds(arg);
                }
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "min-fresh")) {
            if (!minFreshSeen_) {
                minFreshSeen_ = true;
                value_.minFresh = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "s-maxage")) {
            if (!sMaxAgeSeen_) {
                sMaxAgeSeen_ = true;
                value_.sMaxAge = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-while-revalidate")) {
            if (!staleWhileRevalidateSeen_) {
                staleWhileRevalidateSeen_ = true;
                value_.staleWhileRevalidate = parseDeltaSeconds(arg);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "stale-if-error")) {
            if (!staleIfErrorSeen_) {
                staleIfErrorSeen_ = true;
                value_.staleIfError = parseDeltaSeconds(arg);
            }
        }
    }
}

CacheControl parseCacheControl(std::string_view value) noexcept {
    CacheControlFieldParser parser;
    parser.update(value);
    return parser.finish();
}

std::optional<std::time_t> parseHttpDate(std::string_view value) noexcept {
    return detail::httpParseHttpDate(detail::httpTrimOws(value));
}

}  // namespace ruvia
