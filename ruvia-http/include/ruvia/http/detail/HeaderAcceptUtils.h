#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// Response content-codings the server can negotiate (RFC 9110 section 8.4.1). gzip is
// RFC 1952, br (Brotli) is RFC 7932, and zstd follows RFC 8878 plus the
// mandatory 8 MiB HTTP window limit from RFC 9659.
enum class HttpContentCoding : std::uint8_t {
    kIdentity,
    kGzip,
    kBrotli,
    kZstd,
};

[[nodiscard]] inline int httpParseQualityValue(std::string_view value) noexcept {
    value = httpTrimOws(value);
    if (value == "1") {
        return 1000;
    }
    if (value == "0") {
        return 0;
    }
    if (value.size() >= 2 && value[1] == '.' && (value[0] == '0' || value[0] == '1')) {
        int quality = value[0] == '1' ? 1000 : 0;
        if (value[0] == '1') {
            for (std::size_t i = 2; i < value.size(); ++i) {
                if (value[i] != '0') {
                    return -1;
                }
            }
            return quality;
        }

        int scale = 100;
        for (std::size_t i = 2; i < value.size(); ++i) {
            if (i > 4 || value[i] < '0' || value[i] > '9') {
                return -1;
            }
            quality += (value[i] - '0') * scale;
            scale /= 10;
        }
        return quality;
    }
    return -1;
}

[[nodiscard]] inline int httpQualityParameter(std::string_view value) noexcept {
    // Reuse the shared quote-aware parameter scanner so a ';' inside a quoted media-range
    // parameter (RFC 7231 section 5.3.1: token "=" (token / quoted-string)) is not mistaken for a
    // parameter separator ; the same helper multipart Content-Type parsing uses. The leading
    // media-type / coding token has no '=', so it is skipped exactly as before; first q wins.
    int quality = 1000;
    httpVisitSemicolonParametersQuoted(
        value, [&quality](std::string_view name, std::string_view parameter) noexcept {
            if (httpAsciiEqualsIgnoreCase(name, "q")) {
                const auto parsed = httpParseQualityValue(parameter);
                quality = parsed < 0 ? 0 : parsed;
                return false;  // first q wins; later parameters are accept-ext
            }
            return true;
        });
    return quality;
}

[[nodiscard]] inline std::string_view httpHeaderTokenBeforeParameters(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

inline void httpAccumulateAcceptedQuality(int candidate, int& accumulated) noexcept {
    if (candidate > accumulated) {
        accumulated = candidate;
    }
}

inline void httpUpdateAcceptedEncodingQuality(
    std::string_view acceptEncoding,
    std::string_view coding,
    int& explicitQuality,
    int& wildcardQuality) noexcept {
    httpVisitCommaSeparatedQuoted(
        acceptEncoding,
        [coding, &explicitQuality, &wildcardQuality](std::string_view item) noexcept {
            const auto token = httpHeaderTokenBeforeParameters(item);
            if (httpAsciiEqualsIgnoreCase(token, coding)) {
                httpAccumulateAcceptedQuality(
                    httpQualityParameter(item), explicitQuality);
            } else if (token == "*") {
                httpAccumulateAcceptedQuality(
                    httpQualityParameter(item), wildcardQuality);
            }
            return true;
        });
}

[[nodiscard]] inline bool httpAcceptedEncodingAllows(int explicitQuality, int wildcardQuality) noexcept {
    return explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality > 0;
}

struct HttpAcceptedEncodingQuality {
    int explicitQuality{-1};
    int wildcardQuality{-1};

    void update(std::string_view acceptEncoding, std::string_view coding) noexcept {
        httpUpdateAcceptedEncodingQuality(acceptEncoding, coding, explicitQuality, wildcardQuality);
    }

    [[nodiscard]] bool accepts() const noexcept {
        return httpAcceptedEncodingAllows(explicitQuality, wildcardQuality);
    }
};

[[nodiscard]] inline bool httpAcceptsEncoding(std::string_view acceptEncoding, std::string_view coding) noexcept {
    HttpAcceptedEncodingQuality quality;
    quality.update(acceptEncoding, coding);
    return quality.accepts();
}

[[nodiscard]] inline int httpAcceptedEncodingScore(const HttpAcceptedEncodingQuality& quality) noexcept {
    if (!quality.accepts()) {
        return -1;
    }
    return quality.explicitQuality >= 0 ? quality.explicitQuality : quality.wildcardQuality;
}

struct HttpResponseCodingQualities final {
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
    HttpAcceptedEncodingQuality identity;

    void update(std::string_view acceptEncoding) noexcept {
        httpVisitCommaSeparatedQuoted(
            acceptEncoding,
            [this](std::string_view item) noexcept {
                const auto token = httpHeaderTokenBeforeParameters(item);
                if (httpAsciiEqualsIgnoreCase(token, "gzip")) {
                    httpAccumulateAcceptedQuality(
                        httpQualityParameter(item), gzip.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                    httpAccumulateAcceptedQuality(
                        httpQualityParameter(item), brotli.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                    httpAccumulateAcceptedQuality(
                        httpQualityParameter(item), zstd.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "identity")) {
                    httpAccumulateAcceptedQuality(
                        httpQualityParameter(item), identity.explicitQuality);
                } else if (token == "*") {
                    const auto wildcard = httpQualityParameter(item);
                    httpAccumulateAcceptedQuality(wildcard, gzip.wildcardQuality);
                    httpAccumulateAcceptedQuality(wildcard, brotli.wildcardQuality);
                    httpAccumulateAcceptedQuality(wildcard, zstd.wildcardQuality);
                    httpAccumulateAcceptedQuality(wildcard, identity.wildcardQuality);
                }
                return true;
            });
    }
};

[[nodiscard]] inline int httpAcceptedIdentityScore(
    const HttpAcceptedEncodingQuality& identity) noexcept {
    if (identity.explicitQuality >= 0) {
        return identity.explicitQuality;
    }
    // RFC 9110 section 12.5.3: identity is acceptable by default. A wildcard
    // only excludes it when the wildcard explicitly carries q=0; a positive
    // wildcard quality describes otherwise-unlisted content codings and does
    // not lower identity's implicit quality.
    return identity.wildcardQuality == 0 ? 0 : 1000;
}

// Picks the best response coding from all candidate qualities. The highest client
// q-value wins; ties resolve by server preference br > zstd > gzip (Brotli gives
// the best ratio for text and is the most widely supported of the three), then
// identity. A coding with q=0 or one the client never accepts is excluded.
[[nodiscard]] inline HttpContentCoding httpSelectResponseCodingFromQualities(
    const HttpResponseCodingQualities& qualities) noexcept {
    struct Candidate final {
        HttpContentCoding coding;
        int score;
    };
    const Candidate candidates[] = {
        {HttpContentCoding::kBrotli, httpAcceptedEncodingScore(qualities.brotli)},
        {HttpContentCoding::kZstd, httpAcceptedEncodingScore(qualities.zstd)},
        {HttpContentCoding::kGzip, httpAcceptedEncodingScore(qualities.gzip)},
        {HttpContentCoding::kIdentity, httpAcceptedIdentityScore(qualities.identity)},
    };
    HttpContentCoding best = HttpContentCoding::kIdentity;
    int bestScore = -1;
    for (const auto& candidate : candidates) {
        if (candidate.score > bestScore) {
            bestScore = candidate.score;
            best = candidate.coding;
        }
    }
    return best;
}

[[nodiscard]] inline HttpContentCoding httpSelectResponseCoding(std::string_view acceptEncoding) noexcept {
    HttpResponseCodingQualities qualities;
    qualities.update(acceptEncoding);
    return httpSelectResponseCodingFromQualities(qualities);
}

[[nodiscard]] inline std::string_view httpMediaTypeOnly(std::string_view value) noexcept {
    return httpHeaderTokenBeforeParameters(value);
}

[[nodiscard]] inline bool httpMediaToken(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const auto ch : token) {
        if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

struct HttpMediaTypeParts final {
    std::string_view type;
    std::string_view subtype;
};

// Compare media-type parameter values after removing quoted-string syntax and
// decoding quoted-pairs.  A token and its quoted equivalent therefore compare
// equal (for example, utf-8 and "utf-8") without allocating temporary strings.
// Parameter values are otherwise case-sensitive; individual media-type
// registrations define any value-specific case folding.
[[nodiscard]] inline bool httpMediaParameterValueEquals(
    std::string_view left,
    std::string_view right,
    bool asciiCaseInsensitive = false) noexcept {
    struct Cursor final {
        std::string_view value;
        std::size_t position{0};
        std::size_t end{0};
        bool quoted{false};
        bool valid{true};

        explicit Cursor(std::string_view input) noexcept : value(httpTrimOws(input)) {
            if (value.empty()) {
                valid = false;
                return;
            }
            if (value.front() == '"') {
                if (value.size() < 2 || value.back() != '"') {
                    valid = false;
                    return;
                }
                quoted = true;
                position = 1;
                end = value.size() - 1;
                return;
            }
            end = value.size();
            for (const auto ch : value) {
                if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
                    valid = false;
                    return;
                }
            }
        }

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (!valid || position >= end) {
                return false;
            }
            auto ch = static_cast<unsigned char>(value[position++]);
            if (quoted) {
                if (ch == '\\') {
                    if (position >= end) {
                        valid = false;
                        return false;
                    }
                    ch = static_cast<unsigned char>(value[position++]);
                } else if (ch == '"' || !isHttpFieldValueChar(ch)) {
                    valid = false;
                    return false;
                }
                if (!isHttpFieldValueChar(ch)) {
                    valid = false;
                    return false;
                }
            }
            out = ch;
            return true;
        }
    };

    Cursor lhs(left);
    Cursor rhs(right);
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    while (true) {
        unsigned char lhsChar = 0;
        unsigned char rhsChar = 0;
        const bool hasLeft = lhs.next(lhsChar);
        const bool hasRight = rhs.next(rhsChar);
        if (!lhs.valid || !rhs.valid) {
            return false;
        }
        if (hasLeft != hasRight) {
            return false;
        }
        if (!hasLeft) {
            return true;
        }
        if (asciiCaseInsensitive) {
            lhsChar = httpAsciiToLower(lhsChar);
            rhsChar = httpAsciiToLower(rhsChar);
        }
        if (lhsChar != rhsChar) {
            return false;
        }
    }
}

template <typename Visitor>
[[nodiscard]] inline bool httpVisitMediaTypeParameters(
    std::string_view value,
    bool skipQualityParameter,
    Visitor&& visitor) noexcept {
    auto start = httpFindUnquotedDelimiter(value, 0, ';');
    if (start >= value.size()) {
        return true;
    }
    ++start;
    while (start <= value.size()) {
        const auto end = httpFindUnquotedDelimiter(value, start, ';');
        const auto part = httpTrimOws(value.substr(start, end - start));
        const auto equals = part.find('=');
        if (part.empty() || equals == std::string_view::npos) {
            return false;
        }
        const auto name = httpTrimOws(part.substr(0, equals));
        const auto parameterValue = httpTrimOws(part.substr(equals + 1));
        if (!httpMediaToken(name)) {
            return false;
        }
        if (skipQualityParameter && httpAsciiEqualsIgnoreCase(name, "q")) {
            // RFC 9110 removed the old accept-ext grammar. q is the weight
            // wherever it appears, but media-range parameters after it still
            // participate in matching, so skip q itself and continue scanning.
            if (end >= value.size()) {
                return true;
            }
            start = end + 1;
            continue;
        }
        // Comparing a value with itself performs syntax validation as well.
        if (!httpMediaParameterValueEquals(parameterValue, parameterValue) ||
            !visitor(name, parameterValue)) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline bool httpOfferedMediaTypeHasParameter(
    std::string_view offered,
    std::string_view expectedName,
    std::string_view expectedValue) noexcept {
    bool found = false;
    const bool valid = httpVisitMediaTypeParameters(
        offered,
        false,
        [expectedName, expectedValue, &found](
            std::string_view name,
            std::string_view value) noexcept {
            if (httpAsciiEqualsIgnoreCase(name, expectedName) &&
                httpMediaParameterValueEquals(
                    value,
                    expectedValue,
                    httpAsciiEqualsIgnoreCase(name, "charset"))) {
                found = true;
            }
            return true;
        });
    return valid && found;
}

[[nodiscard]] inline bool httpParseMediaTypeParts(
    std::string_view value,
    bool allowWildcard,
    HttpMediaTypeParts& parts) noexcept {
    value = httpMediaTypeOnly(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type = value.substr(0, slash);
    parts.subtype = value.substr(slash + 1);
    const bool typeWildcard = parts.type == "*";
    const bool subtypeWildcard = parts.subtype == "*";
    if ((typeWildcard || subtypeWildcard) && !allowWildcard) {
        return false;
    }
    if (typeWildcard && !subtypeWildcard) {
        return false;
    }
    return (typeWildcard || httpMediaToken(parts.type)) &&
        (subtypeWildcard || httpMediaToken(parts.subtype));
}

[[nodiscard]] inline bool httpMediaRangeMatches(std::string_view range, std::string_view offered) noexcept {
    HttpMediaTypeParts rangeParts;
    HttpMediaTypeParts offeredParts;
    if (!httpParseMediaTypeParts(range, true, rangeParts) ||
        !httpParseMediaTypeParts(offered, false, offeredParts)) {
        return false;
    }

    if (rangeParts.type != "*" &&
        !httpAsciiEqualsIgnoreCase(rangeParts.type, offeredParts.type)) {
        return false;
    }
    if (rangeParts.subtype != "*" &&
        !httpAsciiEqualsIgnoreCase(rangeParts.subtype, offeredParts.subtype)) {
        return false;
    }

    // RFC 9110 section 12.5.1: media-type parameters are part of the media range
    // and must match the selected representation. q is skipped as the weight
    // wherever it appears; parameters on either side of it still participate.
    return httpVisitMediaTypeParameters(
        range,
        true,
        [offered](std::string_view name, std::string_view value) noexcept {
            return httpOfferedMediaTypeHasParameter(offered, name, value);
        });
}

[[nodiscard]] inline int httpMediaRangeSpecificity(std::string_view range) noexcept {
    HttpMediaTypeParts parts;
    if (!httpParseMediaTypeParts(range, true, parts)) {
        return -1;
    }
    if (parts.type == "*" && parts.subtype == "*") {
        return 0;
    }
    if (parts.subtype == "*") {
        return 1;
    }
    return 2;
}

[[nodiscard]] inline int httpMediaRangeParameterCount(std::string_view range) noexcept {
    int count = 0;
    if (!httpVisitMediaTypeParameters(
            range,
            true,
            [&count](std::string_view, std::string_view) noexcept {
                if (count < 0xFFFF) {
                    ++count;
                }
                return true;
            })) {
        return -1;
    }
    return count;
}

// Fold the media-ranges of one Accept field value into a running best-match
// accumulator (most specific range wins; ties break on higher q). Kept separate
// from httpAcceptsMediaType so a caller with an Accept field split across several
// field lines (RFC 9110 5.3: equivalent to one comma-joined value) can feed every
// line into the SAME accumulator -- yielding the comma-joined result, including a
// q=0 exclusion whose range is more specific than an accepting range on another
// line -- without allocating to concatenate them.
inline void httpAccumulateMediaTypeAcceptance(
    std::string_view accept,
    std::string_view offered,
    int& bestSpecificity,
    int& bestQuality) noexcept {
    httpVisitCommaSeparatedQuoted(accept, [offered, &bestSpecificity, &bestQuality](std::string_view item) noexcept {
        if (httpMediaRangeMatches(item, offered)) {
            const auto typeSpecificity = httpMediaRangeSpecificity(item);
            const auto parameterCount = httpMediaRangeParameterCount(item);
            if (typeSpecificity < 0 || parameterCount < 0) {
                return true;
            }
            // Type/subtype precedence dominates any number of parameters; within
            // the same range shape, more matching parameters are more specific.
            const auto specificity = (typeSpecificity << 16) | parameterCount;
            const auto quality = httpQualityParameter(item);
            if (specificity > bestSpecificity || (specificity == bestSpecificity && quality > bestQuality)) {
                bestSpecificity = specificity;
                bestQuality = quality;
            }
        }
        return true;
    });
}

[[nodiscard]] inline bool httpAcceptsMediaType(std::string_view accept, std::string_view offered) noexcept {
    if (accept.empty()) {
        return true;
    }

    int bestSpecificity = -1;
    int bestQuality = 0;
    httpAccumulateMediaTypeAcceptance(accept, offered, bestSpecificity, bestQuality);
    return bestSpecificity >= 0 && bestQuality > 0;
}

}  // namespace ruvia::detail
