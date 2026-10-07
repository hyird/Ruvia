#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "ruvia/http/HttpContentCoding.h"

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.
// gzip and its deprecated x-gzip spelling share one case-insensitive weight.

namespace ruvia {

struct HttpAcceptedEncodingQuality {
    int explicitQuality{-1};
    int wildcardQuality{-1};

    void update(std::string_view acceptEncoding, std::string_view coding) noexcept;

    [[nodiscard]] bool accepts() const noexcept {
        return explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality > 0;
    }

    [[nodiscard]] bool accepts(bool isIdentity) const noexcept {
        if (isIdentity) {
            return explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality != 0;
        }
        return accepts();
    }
};

[[nodiscard]] bool httpAcceptsEncoding(
    std::string_view acceptEncoding, std::string_view coding) noexcept;

struct HttpResponseCodingQualities final {
    // A missing field and an explicitly empty field have different RFC 9110
    // semantics: absence accepts any coding, while an empty value accepts only
    // the identity/no-encoding representation.
    bool fieldPresent{false};
    bool hasNonEmptyItem{false};
    HttpAcceptedEncodingQuality gzip;
    HttpAcceptedEncodingQuality deflate;
    HttpAcceptedEncodingQuality brotli;
    HttpAcceptedEncodingQuality zstd;
    HttpAcceptedEncodingQuality identity;

    void update(std::string_view acceptEncoding) noexcept;

    [[nodiscard]] bool accepts(HttpContentCoding coding) const noexcept {
        return score(coding) >= 0;
    }

private:
    friend class HttpResponseCodingSelection;

    [[nodiscard]] int score(HttpContentCoding coding) const noexcept {
        const HttpAcceptedEncodingQuality* coding_quality = nullptr;
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
                coding_quality = &gzip;
                break;
            case HttpContentCoding::deflate:
                coding_quality = &deflate;
                break;
            case HttpContentCoding::kBrotli:
                coding_quality = &brotli;
                break;
            case HttpContentCoding::kZstd:
                coding_quality = &zstd;
                break;
        }
        if (coding_quality == nullptr) {
            return -1;
        }
        if (!fieldPresent) {
            return 999;
        }
        return coding_quality->accepts() ? (coding_quality->explicitQuality >= 0 ? coding_quality->explicitQuality : coding_quality->wildcardQuality) : -1;
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
            bit(HttpContentCoding::deflate) | bit(HttpContentCoding::kBrotli) |
            bit(HttpContentCoding::kZstd));
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
            case HttpContentCoding::deflate:
                return 16u;
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
        return HttpResponseCodingCandidates::all().contains(coding) &&
               (!acceptEncodingPresent_ || acceptable_codings_.contains(coding));
    }

private:
    constexpr HttpResponseCodingSelection(HttpContentCoding coding, bool identityAccepted,
        bool acceptEncodingPresent, HttpResponseCodingCandidates acceptable_codings) noexcept
        : coding_(coding),
          identityAccepted_(identityAccepted),
          acceptEncodingPresent_(acceptEncodingPresent),
          acceptable_codings_(acceptable_codings) {}

    HttpContentCoding coding_;
    bool identityAccepted_;
    bool acceptEncodingPresent_;
    HttpResponseCodingCandidates acceptable_codings_;
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
        return value_ ? &*value_ : nullptr;
    }
    const HttpResponseCodingSelection* selected() const&& = delete;

    [[nodiscard]] const HttpResponseCodingSelectionFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const HttpResponseCodingSelectionFailure* failure() const&& = delete;

private:
    friend class HttpResponseCodingSelection;

    explicit HttpResponseCodingSelectionResult(HttpResponseCodingSelection selection) noexcept
        : value_(selection) {}

    explicit HttpResponseCodingSelectionResult(HttpResponseCodingSelectionFailure failure) noexcept
        : value_(std::unexpected(failure)) {}

    using Value = std::expected<HttpResponseCodingSelection, HttpResponseCodingSelectionFailure>;
    Value value_;
};

// Picks the best response coding from the supplied representation candidates.
// The highest client q-value wins; ties resolve by server preference br > zstd
// > gzip > deflate > identity. A coding with q=0 or one the client never accepts is
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
        HttpContentCoding::deflate,
        HttpContentCoding::kIdentity,
    };
    auto acceptable_codings = HttpResponseCodingCandidates::empty();
    for (const auto coding : availableCodings) {
        if (qualities.accepts(coding)) {
            acceptable_codings.include(coding);
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
        best, identityScore >= 0, qualities.fieldPresent, acceptable_codings));
}

}  // namespace ruvia
