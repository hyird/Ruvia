#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"

namespace ruvia {

// Returns the media type before any parameters. The view borrows the input;
// owning string temporaries cannot be passed through borrowed_text.
[[nodiscard]] std::string_view http_media_type_only(borrowed_text value) noexcept;

// Validates one Content-Type field value, including RFC 9110's optional empty
// semicolon-delimited parameter slots. Nonempty parameters require name=value
// without whitespace around '='. A value with more than 64 nonempty parameters
// is invalid.
[[nodiscard]] bool is_valid_http_content_type_field_value(std::string_view value) noexcept;

enum class http_accept_token_match_mode : std::uint8_t {
    exact,
    language_prefix,
};

// Accumulates one offered representation's match across multiple Accept field
// lines, equivalent to a comma-joined field without allocating or copying it.
// More specific matches override less specific ones, including q=0 exclusions.
// Language-prefix mode uses the longest matching range; equal specificity takes
// the highest quality, independently of field order. Token fields allow only
// an optional q weight; unknown or additional parameters are unacceptable.
// Media ranges permit empty parameter slots; these do not affect specificity
// or quality. Nonempty parameters constrain the offered type, even after q.
// A media range, or an offered type, with more than 64 nonempty parameters
// (q included) is invalid and never matches.
class http_accept_match final {
public:
    void update_media_type(std::string_view field, std::string_view offered) noexcept;
    void update_token(std::string_view field, std::string_view offered,
        http_accept_token_match_mode mode) noexcept;

    [[nodiscard]] bool matched() const noexcept {
        return specificity_ >= 0 && quality_ > 0;
    }

    // 0 means no acceptable match. The value otherwise uses HTTP qvalue's
    // thousand-point scale (1 through 1000).
    [[nodiscard]] int quality() const noexcept {
        return matched() ? quality_ : 0;
    }

private:
    int specificity_{-1};
    int quality_{0};
    std::size_t parameter_specificity_{0};
};

}  // namespace ruvia

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.
// gzip and its deprecated x-gzip spelling share one case-insensitive weight.

namespace ruvia {

class http_request;

struct http_accepted_encoding_quality {
    int explicit_quality_{-1};
    int wildcard_quality_{-1};

    void update(std::string_view accept_encoding, std::string_view coding) noexcept;

    // HTTP qvalue on a thousand-point scale, with 0 for an unacceptable coding.
    // Explicit weights override the wildcard. Without an explicit identity
    // weight, identity defaults to 1000 unless a zero wildcard excludes it.
    [[nodiscard]] int quality(bool is_identity = false) const noexcept {
        if (explicit_quality_ >= 0) {
            return explicit_quality_;
        }
        if (is_identity) {
            return wildcard_quality_ == 0 ? 0 : 1000;
        }
        return wildcard_quality_ > 0 ? wildcard_quality_ : 0;
    }

    [[nodiscard]] bool accepts() const noexcept {
        return accepts(false);
    }

    [[nodiscard]] bool accepts(bool is_identity) const noexcept {
        return quality(is_identity) > 0;
    }
};

[[nodiscard]] bool http_accepts_encoding(
    std::string_view accept_encoding, std::string_view coding) noexcept;

struct http_response_coding_qualities final {
    // A missing field and an explicitly empty field have different RFC 9110
    // semantics: absence accepts any coding, while an empty value accepts only
    // the identity/no-encoding representation.
    bool field_present_{false};
    bool has_non_empty_item_{false};
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality deflate_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;

    void update(std::string_view accept_encoding) noexcept;

    [[nodiscard]] bool accepts(http_content_coding coding) const noexcept {
        return score(coding) >= 0;
    }

private:
    friend class http_response_coding_selection;

    [[nodiscard]] int score(http_content_coding coding) const noexcept {
        const http_accepted_encoding_quality* coding_quality = nullptr;
        switch (coding) {
            case http_content_coding::identity:
                if (field_present_ && !has_non_empty_item_) {
                    return 1000;
                }
                return identity_.quality(true) > 0 ? identity_.quality(true) : -1;
            case http_content_coding::gzip:
                coding_quality = &gzip_;
                break;
            case http_content_coding::deflate:
                coding_quality = &deflate_;
                break;
            case http_content_coding::brotli:
                coding_quality = &brotli_;
                break;
            case http_content_coding::zstd:
                coding_quality = &zstd_;
                break;
        }
        if (coding_quality == nullptr) {
            return -1;
        }
        if (!field_present_) {
            return 999;
        }
        return coding_quality->quality() > 0 ? coding_quality->quality() : -1;
    }
};

// A representation policy supplies the codings it can actually produce or
// retrieve. Keeping this set typed prevents callers from reimplementing
// Accept-Encoding ranking with raw q-value integers.
class http_response_coding_candidates final {
public:
    [[nodiscard]] static constexpr http_response_coding_candidates empty() noexcept {
        return http_response_coding_candidates(0);
    }

    [[nodiscard]] static constexpr http_response_coding_candidates identity_only() noexcept {
        return http_response_coding_candidates(bit(http_content_coding::identity));
    }

    [[nodiscard]] static constexpr http_response_coding_candidates all() noexcept {
        return http_response_coding_candidates(
            bit(http_content_coding::identity) | bit(http_content_coding::gzip) |
            bit(http_content_coding::deflate) | bit(http_content_coding::brotli) |
            bit(http_content_coding::zstd));
    }

    constexpr http_response_coding_candidates& include(http_content_coding coding) noexcept {
        bits_ = static_cast<std::uint8_t>(bits_ | bit(coding));
        return *this;
    }

    [[nodiscard]] constexpr bool contains(http_content_coding coding) const noexcept {
        return (bits_ & bit(coding)) != 0;
    }

private:
    explicit constexpr http_response_coding_candidates(std::uint8_t bits) noexcept
        : bits_(bits) {}

    [[nodiscard]] static constexpr std::uint8_t bit(http_content_coding coding) noexcept {
        switch (coding) {
            case http_content_coding::identity:
                return 1u;
            case http_content_coding::gzip:
                return 2u;
            case http_content_coding::deflate:
                return 16u;
            case http_content_coding::brotli:
                return 4u;
            case http_content_coding::zstd:
                return 8u;
        }
        return 0u;
    }

    std::uint8_t bits_;
};

// The selected coding and the identity fallback decision come from the same
// Accept-Encoding snapshot. Keeping them together prevents a runtime from
// selecting one coding and separately observing a stale or differently parsed
// identity quality. A selection exists even when the client accepts none of
// the candidates: acceptable() then reports the failed negotiation, which the
// Web layer answers with 406 Not Acceptable once a response with content needs
// it, and the snapshot still answers accepts() for a coding a handler applied.
class http_response_coding_selection final {
public:
    [[nodiscard]] static http_response_coding_selection select(
        const http_response_coding_qualities& qualities) noexcept;
    [[nodiscard]] static http_response_coding_selection select(
        const http_response_coding_qualities& qualities,
        http_response_coding_candidates candidates) noexcept;
    [[nodiscard]] static constexpr http_response_coding_selection select(
        const http_response_coding_qualities& qualities,
        std::span<const http_content_coding> candidates) noexcept;

    // Ranks the same Accept-Encoding snapshot over the codings one
    // representation store actually holds (for example indexed precompressed
    // files), so that store never parses the request's fields again.
    [[nodiscard]] constexpr http_response_coding_selection select(
        http_response_coding_candidates candidates) const noexcept {
        return rank(scores_, candidates, explicit_identity_);
    }

    [[nodiscard]] constexpr http_response_coding_selection select(
        std::span<const http_content_coding> candidates) const noexcept {
        return rank_ordered(scores_, candidates, explicit_identity_);
    }
    // False when the client accepts none of the candidates.
    [[nodiscard]] constexpr bool acceptable() const noexcept {
        return acceptable_;
    }

    // The selected coding; identity, as a placeholder only, when !acceptable().
    [[nodiscard]] constexpr http_content_coding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] constexpr bool identity_accepted() const noexcept {
        return accepts(http_content_coding::identity);
    }

    // The selected coding is the server's preference, while this predicate
    // retains the complete client acceptability snapshot for a response that
    // was already encoded by application code or a representation store. A
    // missing Accept-Encoding field accepts every coding; an explicitly
    // present field uses the parsed q-value set, including wildcard rules.
    [[nodiscard]] constexpr bool accepts(http_content_coding coding) const noexcept {
        const auto index = static_cast<std::size_t>(coding);
        return index < scores_.size() && scores_[index] >= 0;
    }

    // Extension tokens are evaluated from a borrowed request without retaining
    // it or allocating extension storage in every negotiation snapshot.
    [[nodiscard]] bool accepts(std::string_view coding, const http_request& request) const noexcept;

private:
    // Client score per http_content_coding value; -1 marks an unacceptable coding.
    using scores_type = std::array<std::int16_t, 5>;

    constexpr http_response_coding_selection(http_content_coding coding, bool acceptable,
        scores_type scores, bool explicit_identity = false) noexcept
        : scores_(scores),
          coding_(coding),
          acceptable_(acceptable),
          explicit_identity_(explicit_identity) {}

    [[nodiscard]] static constexpr scores_type make_scores(
        const http_response_coding_qualities& qualities) noexcept {
        scores_type scores{};
        for (std::size_t index = 0; index < scores.size(); ++index) {
            scores[index] =
                static_cast<std::int16_t>(qualities.score(static_cast<http_content_coding>(index)));
        }
        return scores;
    }

    [[nodiscard]] static constexpr http_response_coding_selection rank(
        scores_type scores, http_response_coding_candidates candidates,
        bool explicit_identity = false) noexcept;

    [[nodiscard]] static constexpr http_response_coding_selection rank_ordered(
        scores_type scores, std::span<const http_content_coding> candidates,
        bool explicit_identity) noexcept;

    scores_type scores_;
    http_content_coding coding_;
    bool acceptable_;
    bool explicit_identity_{false};
};

// Picks the best response coding from the supplied representation candidates
// (every coding the server can produce for the one-argument form). The highest
// client q-value wins; ties resolve by server preference br > zstd > gzip >
// deflate > identity. A coding with q=0 or one the client never accepts is
// excluded.
inline http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities) noexcept {
    return select(qualities, http_response_coding_candidates::all());
}

inline http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities,
    http_response_coding_candidates candidates) noexcept {
    return rank(make_scores(qualities), candidates, qualities.identity_.explicit_quality_ >= 0);
}

constexpr http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities,
    std::span<const http_content_coding> candidates) noexcept {
    return rank_ordered(
        make_scores(qualities), candidates, qualities.identity_.explicit_quality_ >= 0);
}

constexpr http_response_coding_selection http_response_coding_selection::rank(
    scores_type scores, http_response_coding_candidates candidates, bool explicit_identity) noexcept {
    constexpr http_content_coding preference[] = {
        http_content_coding::brotli,
        http_content_coding::zstd,
        http_content_coding::gzip,
        http_content_coding::deflate,
        http_content_coding::identity,
    };
    http_content_coding best = http_content_coding::identity;
    bool found = false;
    int best_score = -1;
    for (const auto coding : preference) {
        if (!candidates.contains(coding)) {
            continue;
        }
        const int score = scores[static_cast<std::size_t>(coding)];
        if (score > best_score) {
            best_score = score;
            best = coding;
            found = true;
        }
    }
    return http_response_coding_selection(best, found, scores, explicit_identity);
}

constexpr http_response_coding_selection http_response_coding_selection::rank_ordered(
    scores_type scores, std::span<const http_content_coding> candidates,
    bool explicit_identity) noexcept {
    http_content_coding best = http_content_coding::identity;
    bool found = false;
    int best_score = -1;
    bool identity_in_candidates = false;
    for (const auto coding : candidates) {
        const auto index = static_cast<std::size_t>(coding);
        if (index >= scores.size()) {
            continue;
        }
        if (coding == http_content_coding::identity) {
            identity_in_candidates = true;
        }
        const int raw_score = scores[index];
        if (raw_score < 0) {
            continue;
        }
        const int score =
            (coding == http_content_coding::identity && !explicit_identity) ? 0 : raw_score;
        if (score > best_score) {
            best_score = score;
            best = coding;
            found = true;
        }
    }
    const int identity_score =
        scores[static_cast<std::size_t>(http_content_coding::identity)];
    if (!identity_in_candidates && identity_score >= 0) {
        if (!found || (explicit_identity && identity_score > best_score)) {
            best = http_content_coding::identity;
            best_score = identity_score;
            found = true;
        }
    }
    return http_response_coding_selection(best, found, scores, explicit_identity);
}

class http_response;

// Custom-coding queries borrow the request only during the call; negotiation
// snapshots never allocate storage for unknown extension tokens.
[[nodiscard]] bool http_accepts_encoding(
    const http_request& request, std::string_view coding) noexcept;

enum class http_response_content_coding_status : std::uint8_t {
    absent,
    accepted,
    not_acceptable,
    invalid,
};

// Parses all Content-Encoding lines once and evaluates the required custom
// codings together in one Accept-Encoding walk. Only stacks with more than
// eight distinct extension codings require temporary storage.
[[nodiscard]] http_response_content_coding_status evaluate_http_response_content_coding(
    const http_response_coding_selection& selection, const http_response& response,
    const http_request* request = nullptr);

// Folds every Accept-Encoding field line of a received request into one
// quality snapshot and selects among all producible codings. This is the one
// Accept-Encoding preference authority for HTTP/1, HTTP/2 and subrequests.
[[nodiscard]] http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept;

// Selects a content coding from Accept-Encoding using the caller's ordered
// candidate list for tie-breaking, comparing explicit/implicit identity weight
// and returning std::nullopt when every candidate (and identity) is rejected.
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept;

[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept;

}  // namespace ruvia
