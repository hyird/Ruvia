#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"

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
    if (weight.size() < 3 || httpAsciiToLower(static_cast<unsigned char>(weight[0])) != 'q' ||
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

inline void httpUpdateAcceptedEncodingQuality(std::string_view acceptEncoding,
    std::string_view coding, int& explicitQuality, int& wildcardQuality) noexcept {
    httpVisitCommaSeparatedQuoted(acceptEncoding,
        [coding, &explicitQuality, &wildcardQuality](std::string_view item) noexcept {
            const auto token = httpHeaderTokenBeforeParameters(item);
            if (httpAsciiEqualsIgnoreCase(token, coding)) {
                httpAccumulateAcceptedQuality(httpEncodingQualityParameter(item), explicitQuality);
            } else if (token == "*") {
                httpAccumulateAcceptedQuality(httpEncodingQualityParameter(item), wildcardQuality);
            }
            return true;
        });
}

[[nodiscard]] inline bool httpAcceptedEncodingAllows(
    int explicitQuality, int wildcardQuality) noexcept {
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

[[nodiscard]] inline bool httpAcceptsEncoding(
    std::string_view acceptEncoding, std::string_view coding) noexcept {
    if (acceptEncoding.empty()) {
        return httpAsciiEqualsIgnoreCase(coding, "identity");
    }
    HttpAcceptedEncodingQuality quality;
    quality.update(acceptEncoding, coding);
    return quality.accepts();
}

struct HttpResponseCodingQualities final {
    // A missing field and an explicitly empty field have different RFC 9110
    // semantics: absence accepts any coding, while an empty value accepts only
    // the identity/no-encoding representation.
    bool fieldPresent{false};
    bool hasNonEmptyItem{false};
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
    HttpAcceptedEncodingQuality identity;

    void update(std::string_view acceptEncoding) noexcept {
        fieldPresent = true;
        httpVisitCommaSeparatedQuoted(acceptEncoding, [this](std::string_view item) noexcept {
            const auto token = httpHeaderTokenBeforeParameters(item);
            hasNonEmptyItem = true;
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

    [[nodiscard]] bool accepts(HttpContentCoding coding) const noexcept {
        return score(coding) >= 0;
    }

private:
    friend class HttpResponseCodingSelection;

    [[nodiscard]] int score(HttpContentCoding coding) const noexcept {
        switch (coding) {
            case HttpContentCoding::kIdentity:
                if (fieldPresent && !hasNonEmptyItem) {
                    return 1000;
                }
                if (identity.explicitQuality >= 0) {
                    return identity.explicitQuality > 0 ? identity.explicitQuality : -1;
                }
                // RFC 9110 section 12.5.3: identity is acceptable by
                // default. A wildcard only excludes it when q=0.
                return identity.wildcardQuality == 0 ? -1 : 1000;
            case HttpContentCoding::kGzip:
                if (!fieldPresent) {
                    return 999;
                }
                return gzip.accepts() ? (gzip.explicitQuality >= 0 ? gzip.explicitQuality
                                                                   : gzip.wildcardQuality)
                                      : -1;
            case HttpContentCoding::kBrotli:
                if (!fieldPresent) {
                    return 999;
                }
                return brotli.accepts() ? (brotli.explicitQuality >= 0 ? brotli.explicitQuality
                                                                       : brotli.wildcardQuality)
                                        : -1;
            case HttpContentCoding::kZstd:
                if (!fieldPresent) {
                    return 999;
                }
                return zstd.accepts() ? (zstd.explicitQuality >= 0 ? zstd.explicitQuality
                                                                   : zstd.wildcardQuality)
                                      : -1;
        }
        return -1;
    }
};

// A representation policy supplies the codings it can actually produce or
// retrieve. Keeping this set typed prevents callers from reimplementing
// Accept-Encoding ranking with raw q-value integers.
class HttpResponseCodingCandidates final {
public:
    [[nodiscard]] static constexpr HttpResponseCodingCandidates empty() noexcept {
        return HttpResponseCodingCandidates(0);
    }

    [[nodiscard]] static constexpr HttpResponseCodingCandidates identityOnly() noexcept {
        return HttpResponseCodingCandidates(bit(HttpContentCoding::kIdentity));
    }

    [[nodiscard]] static constexpr HttpResponseCodingCandidates all() noexcept {
        return HttpResponseCodingCandidates(
            bit(HttpContentCoding::kIdentity) | bit(HttpContentCoding::kGzip) |
            bit(HttpContentCoding::kBrotli) | bit(HttpContentCoding::kZstd));
    }

    constexpr HttpResponseCodingCandidates& include(HttpContentCoding coding) noexcept {
        bits_ = static_cast<std::uint8_t>(bits_ | bit(coding));
        return *this;
    }

    [[nodiscard]] constexpr bool contains(HttpContentCoding coding) const noexcept {
        return (bits_ & bit(coding)) != 0;
    }

private:
    explicit constexpr HttpResponseCodingCandidates(std::uint8_t bits) noexcept
        : bits_(bits) {}

    [[nodiscard]] static constexpr std::uint8_t bit(HttpContentCoding coding) noexcept {
        switch (coding) {
            case HttpContentCoding::kIdentity:
                return 1u;
            case HttpContentCoding::kGzip:
                return 2u;
            case HttpContentCoding::kBrotli:
                return 4u;
            case HttpContentCoding::kZstd:
                return 8u;
        }
        return 0u;
    }

    std::uint8_t bits_;
};

// The selected coding and the identity fallback decision come from the same
// Accept-Encoding snapshot. Keeping them together prevents a runtime from
// selecting one coding and separately observing a stale or differently parsed
// identity quality.
class HttpResponseCodingSelectionResult;

class HttpResponseCodingSelection final {
public:
    [[nodiscard]] static HttpResponseCodingSelectionResult select(
        const HttpResponseCodingQualities& qualities) noexcept;
    [[nodiscard]] static HttpResponseCodingSelectionResult select(
        const HttpResponseCodingQualities& qualities,
        HttpResponseCodingCandidates candidates) noexcept;

    [[nodiscard]] constexpr HttpContentCoding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] constexpr bool identityAccepted() const noexcept {
        return identityAccepted_;
    }

    // The selected coding is the server's preference, while this predicate
    // retains the complete client acceptability snapshot for a response that
    // was already encoded by application code or a representation store. A
    // missing Accept-Encoding field accepts every coding; an explicitly
    // present field uses the parsed q-value set, including wildcard rules.
    [[nodiscard]] constexpr bool accepts(HttpContentCoding coding) const noexcept {
        return !acceptEncodingPresent_ || (acceptableBits_ & bit(coding)) != 0;
    }

private:
    constexpr HttpResponseCodingSelection(HttpContentCoding coding, bool identityAccepted,
        bool acceptEncodingPresent, std::uint8_t acceptableBits) noexcept
        : coding_(coding),
          identityAccepted_(identityAccepted),
          acceptEncodingPresent_(acceptEncodingPresent),
          acceptableBits_(acceptableBits) {}

    [[nodiscard]] static constexpr std::uint8_t bit(HttpContentCoding coding) noexcept {
        switch (coding) {
            case HttpContentCoding::kIdentity:
                return 1u;
            case HttpContentCoding::kGzip:
                return 2u;
            case HttpContentCoding::kBrotli:
                return 4u;
            case HttpContentCoding::kZstd:
                return 8u;
        }
        return 0u;
    }

    HttpContentCoding coding_;
    bool identityAccepted_;
    bool acceptEncodingPresent_;
    std::uint8_t acceptableBits_;
};

enum class HttpResponseCodingSelectionError : std::uint8_t {
    kNoAcceptableCoding,
};

class HttpResponseCodingSelectionFailure final {
public:
    [[nodiscard]] constexpr HttpResponseCodingSelectionError error() const noexcept {
        return error_;
    }

private:
    friend class HttpResponseCodingSelection;
    friend class HttpResponseCodingSelectionResult;

    explicit constexpr HttpResponseCodingSelectionFailure(
        HttpResponseCodingSelectionError error) noexcept
        : error_(error) {}

    HttpResponseCodingSelectionError error_;
};

// Response content negotiation has two valid protocol outcomes. Making the
// rejection explicit prevents callers from confusing a 406 negotiation result
// with an uninitialized selection or an intentionally disabled response policy.
class HttpResponseCodingSelectionResult final {
public:
    [[nodiscard]] const HttpResponseCodingSelection* selected() const& noexcept {
        return std::get_if<HttpResponseCodingSelection>(&value_);
    }
    const HttpResponseCodingSelection* selected() const&& = delete;

    [[nodiscard]] const HttpResponseCodingSelectionFailure* failure() const& noexcept {
        return std::get_if<HttpResponseCodingSelectionFailure>(&value_);
    }
    const HttpResponseCodingSelectionFailure* failure() const&& = delete;

private:
    friend class HttpResponseCodingSelection;

    explicit HttpResponseCodingSelectionResult(HttpResponseCodingSelection selection) noexcept
        : value_(selection) {}

    explicit HttpResponseCodingSelectionResult(HttpResponseCodingSelectionFailure failure) noexcept
        : value_(failure) {}

    using Value = std::variant<HttpResponseCodingSelection, HttpResponseCodingSelectionFailure>;
    Value value_;
};

// Picks the best response coding from the supplied representation candidates.
// The highest client q-value wins; ties resolve by server preference br > zstd
// > gzip > identity. A coding with q=0 or one the client never accepts is
// excluded. A failure result means the request has no acceptable response
// content coding and must be answered with 406 Not Acceptable by the Web layer.
inline HttpResponseCodingSelectionResult HttpResponseCodingSelection::select(
    const HttpResponseCodingQualities& qualities) noexcept {
    return select(qualities, HttpResponseCodingCandidates::all());
}

inline HttpResponseCodingSelectionResult HttpResponseCodingSelection::select(
    const HttpResponseCodingQualities& qualities,
    HttpResponseCodingCandidates candidates) noexcept {
    const int identityScore = qualities.score(HttpContentCoding::kIdentity);
    const HttpContentCoding availableCodings[] = {
        HttpContentCoding::kBrotli,
        HttpContentCoding::kZstd,
        HttpContentCoding::kGzip,
        HttpContentCoding::kIdentity,
    };
    std::uint8_t acceptableBits = 0;
    for (const auto coding : availableCodings) {
        if (qualities.accepts(coding)) {
            acceptableBits = static_cast<std::uint8_t>(acceptableBits | bit(coding));
        }
    }
    HttpContentCoding best = HttpContentCoding::kIdentity;
    bool found = false;
    int bestScore = -1;
    for (const auto coding : availableCodings) {
        if (!candidates.contains(coding)) {
            continue;
        }
        const int score = qualities.score(coding);
        if (score > bestScore) {
            bestScore = score;
            best = coding;
            found = true;
        }
    }
    if (!found) {
        return HttpResponseCodingSelectionResult(HttpResponseCodingSelectionFailure(
            HttpResponseCodingSelectionError::kNoAcceptableCoding));
    }
    return HttpResponseCodingSelectionResult(HttpResponseCodingSelection(
        best, identityScore >= 0, qualities.fieldPresent, acceptableBits));
}

}  // namespace ruvia::detail
