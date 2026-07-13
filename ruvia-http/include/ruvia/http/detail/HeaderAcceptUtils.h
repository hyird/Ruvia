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
                explicitQuality = httpQualityParameter(item);
            } else if (token == "*") {
                wildcardQuality = httpQualityParameter(item);
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

// Picks the best response coding from per-coding qualities. The highest client
// q-value wins; ties resolve by server preference br > zstd > gzip (Brotli gives
// the best ratio for text and is the most widely supported of the three). A
// coding with q=0 or one the client never accepts is excluded.
[[nodiscard]] inline HttpContentCoding httpSelectResponseCodingFromQualities(
    const HttpAcceptedEncodingQuality& gzip,
    const HttpAcceptedEncodingQuality& brotli,
    const HttpAcceptedEncodingQuality& zstd) noexcept {
    struct Candidate final {
        HttpContentCoding coding;
        int score;
    };
    const Candidate candidates[] = {
        {HttpContentCoding::kBrotli, httpAcceptedEncodingScore(brotli)},
        {HttpContentCoding::kZstd, httpAcceptedEncodingScore(zstd)},
        {HttpContentCoding::kGzip, httpAcceptedEncodingScore(gzip)},
    };
    HttpContentCoding best = HttpContentCoding::kIdentity;
    int bestScore = 0;
    for (const auto& candidate : candidates) {
        if (candidate.score > bestScore) {
            bestScore = candidate.score;
            best = candidate.coding;
        }
    }
    return best;
}

// Single-pass Accept-Encoding scan that updates the gzip/br/zstd qualities
// together. Equivalent to gzip.update()/brotli.update()/zstd.update() with the
// same header value, but walks the comma-separated list once instead of three
// times ; this runs on the per-request header-parse hot path. Like
// HttpAcceptedEncodingQuality::update it accumulates across calls (a later
// matching item overwrites), so repeated Accept-Encoding header lines compose.
inline void httpUpdateResponseCodingQualities(
    std::string_view acceptEncoding,
    HttpAcceptedEncodingQuality& gzip,
    HttpAcceptedEncodingQuality& brotli,
    HttpAcceptedEncodingQuality& zstd) noexcept {
    httpVisitCommaSeparatedQuoted(
        acceptEncoding,
        [&gzip, &brotli, &zstd](std::string_view item) noexcept {
            const auto token = httpHeaderTokenBeforeParameters(item);
            if (httpAsciiEqualsIgnoreCase(token, "gzip")) {
                gzip.explicitQuality = httpQualityParameter(item);
            } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                brotli.explicitQuality = httpQualityParameter(item);
            } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                zstd.explicitQuality = httpQualityParameter(item);
            } else if (token == "*") {
                const auto wildcard = httpQualityParameter(item);
                gzip.wildcardQuality = wildcard;
                brotli.wildcardQuality = wildcard;
                zstd.wildcardQuality = wildcard;
            }
            return true;
        });
}

[[nodiscard]] inline HttpContentCoding httpSelectResponseCoding(std::string_view acceptEncoding) noexcept {
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
    httpUpdateResponseCodingQualities(acceptEncoding, gzip, brotli, zstd);
    return httpSelectResponseCodingFromQualities(gzip, brotli, zstd);
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

    if (rangeParts.type == "*" && rangeParts.subtype == "*") {
        return true;
    }
    if (!httpAsciiEqualsIgnoreCase(rangeParts.type, offeredParts.type)) {
        return false;
    }
    return rangeParts.subtype == "*" ||
        httpAsciiEqualsIgnoreCase(rangeParts.subtype, offeredParts.subtype);
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
            const auto specificity = httpMediaRangeSpecificity(item);
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
