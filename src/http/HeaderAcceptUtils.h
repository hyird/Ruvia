#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "HeaderTokenUtils.h"

namespace ruvia::detail {

// Response content-codings the server can negotiate (RFC 9110 §8.4.1). gzip is
// RFC 1952, br (Brotli) is RFC 7932, zstd is RFC 8878.
enum class HttpContentCoding : std::uint8_t {
    kNone,
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
    // parameter (RFC 7231 §5.3.1: token "=" (token / quoted-string)) is not mistaken for a
    // parameter separator — the same helper multipart Content-Type parsing uses. The leading
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
    HttpContentCoding best = HttpContentCoding::kNone;
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
// times — this runs on the per-request header-parse hot path. Like
// HttpAcceptedEncodingQuality::update it accumulates across calls (a later
// matching item overwrites), so repeated Accept-Encoding header lines compose.
inline void httpUpdateResponseCodingQualities(
    std::string_view acceptEncoding,
    HttpAcceptedEncodingQuality& gzip,
    HttpAcceptedEncodingQuality& brotli,
    HttpAcceptedEncodingQuality& zstd) noexcept {
    while (!acceptEncoding.empty()) {
        const auto comma = acceptEncoding.find(',');
        const auto item = httpTrimOws(
            comma == std::string_view::npos ? acceptEncoding : acceptEncoding.substr(0, comma));
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

        if (comma == std::string_view::npos) {
            break;
        }
        acceptEncoding.remove_prefix(comma + 1);
    }
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

[[nodiscard]] inline bool httpMediaRangeMatches(std::string_view range, std::string_view offered) noexcept {
    range = httpMediaTypeOnly(range);
    offered = httpMediaTypeOnly(offered);
    const auto offeredSlash = offered.find('/');
    const auto rangeSlash = range.find('/');
    if (offeredSlash == std::string_view::npos || rangeSlash == std::string_view::npos) {
        return false;
    }

    const auto offeredType = offered.substr(0, offeredSlash);
    const auto offeredSubtype = offered.substr(offeredSlash + 1);
    const auto rangeType = range.substr(0, rangeSlash);
    const auto rangeSubtype = range.substr(rangeSlash + 1);
    if (rangeType == "*" && rangeSubtype == "*") {
        return true;
    }
    if (!httpAsciiEqualsIgnoreCase(rangeType, offeredType)) {
        return false;
    }
    return rangeSubtype == "*" || httpAsciiEqualsIgnoreCase(rangeSubtype, offeredSubtype);
}

[[nodiscard]] inline int httpMediaRangeSpecificity(std::string_view range) noexcept {
    range = httpMediaTypeOnly(range);
    const auto slash = range.find('/');
    if (slash == std::string_view::npos) {
        return -1;
    }
    const auto type = range.substr(0, slash);
    const auto subtype = range.substr(slash + 1);
    if (type == "*" && subtype == "*") {
        return 0;
    }
    if (subtype == "*") {
        return 1;
    }
    return 2;
}

[[nodiscard]] inline bool httpAcceptsMediaType(std::string_view accept, std::string_view offered) noexcept {
    if (accept.empty()) {
        return true;
    }

    int bestSpecificity = -1;
    int bestQuality = 0;
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

    return bestSpecificity >= 0 && bestQuality > 0;
}

}  // namespace ruvia::detail
