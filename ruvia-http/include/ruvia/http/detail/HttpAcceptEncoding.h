#pragma once

#include <string_view>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/detail/HttpQualityValue.h"

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.

namespace ruvia::detail {

// Accept-Encoding uses `codings [ weight ]`, not the arbitrary parameter list
// accepted by media ranges. Validate that optional weight without normalizing
// whitespace around '='; malformed items are explicitly unacceptable.
[[nodiscard]] inline int httpEncodingQualityParameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = value.substr(semicolon + 1);
    while (!weight.empty() && (weight.front() == ' ' || weight.front() == '\t')) {
        weight.remove_prefix(1);
    }
    if (weight.size() < 3 ||
        httpAsciiToLower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != httpTrimOws(qvalue)) {
        return 0;
    }
    const auto parsed = httpParseQualityValue(qvalue);
    return parsed < 0 ? 0 : parsed;
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
                    httpEncodingQualityParameter(item), explicitQuality);
            } else if (token == "*") {
                httpAccumulateAcceptedQuality(
                    httpEncodingQualityParameter(item), wildcardQuality);
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
                        httpEncodingQualityParameter(item), gzip.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                    httpAccumulateAcceptedQuality(
                        httpEncodingQualityParameter(item), brotli.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                    httpAccumulateAcceptedQuality(
                        httpEncodingQualityParameter(item), zstd.explicitQuality);
                } else if (httpAsciiEqualsIgnoreCase(token, "identity")) {
                    httpAccumulateAcceptedQuality(
                        httpEncodingQualityParameter(item), identity.explicitQuality);
                } else if (token == "*") {
                    const auto wildcard = httpEncodingQualityParameter(item);
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

}  // namespace ruvia::detail
