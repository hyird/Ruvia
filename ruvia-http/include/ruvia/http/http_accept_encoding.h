#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/http_content_coding.h"

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.
// gzip and its deprecated x-gzip spelling share one case-insensitive weight.

namespace ruvia {

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
        return quality() > 0;
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
// identity quality.
class http_response_coding_selection_result;

class http_response_coding_selection final {
public:
    [[nodiscard]] static http_response_coding_selection_result select(
        const http_response_coding_qualities& qualities) noexcept;
    [[nodiscard]] static http_response_coding_selection_result select(
        const http_response_coding_qualities& qualities,
        http_response_coding_candidates candidates) noexcept;

    [[nodiscard]] constexpr http_content_coding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] constexpr bool identity_accepted() const noexcept {
        return identity_accepted_;
    }

    // The selected coding is the server's preference, while this predicate
    // retains the complete client acceptability snapshot for a response that
    // was already encoded by application code or a representation store. A
    // missing Accept-Encoding field accepts every coding; an explicitly
    // present field uses the parsed q-value set, including wildcard rules.
    [[nodiscard]] constexpr bool accepts(http_content_coding coding) const noexcept {
        return http_response_coding_candidates::all().contains(coding) &&
               (!accept_encoding_present_ || acceptable_codings_.contains(coding));
    }

private:
    constexpr http_response_coding_selection(http_content_coding coding, bool identity_accepted,
        bool accept_encoding_present, http_response_coding_candidates acceptable_codings) noexcept
        : coding_(coding),
          identity_accepted_(identity_accepted),
          accept_encoding_present_(accept_encoding_present),
          acceptable_codings_(acceptable_codings) {}

    http_content_coding coding_;
    bool identity_accepted_;
    bool accept_encoding_present_;
    http_response_coding_candidates acceptable_codings_;
};

enum class http_response_coding_selection_error : std::uint8_t {
    no_acceptable_coding,
};

class http_response_coding_selection_failure final {
public:
    [[nodiscard]] constexpr http_response_coding_selection_error error() const noexcept {
        return error_;
    }

private:
    friend class http_response_coding_selection;
    friend class http_response_coding_selection_result;

    explicit constexpr http_response_coding_selection_failure(
        http_response_coding_selection_error error) noexcept
        : error_(error) {}

    http_response_coding_selection_error error_;
};

// Response content negotiation has two valid protocol outcomes. Making the
// rejection explicit prevents callers from confusing a 406 negotiation result
// with an uninitialized selection or an intentionally disabled response policy.
class http_response_coding_selection_result final {
public:
    [[nodiscard]] const http_response_coding_selection* selected() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_response_coding_selection* selected() const&& = delete;

    [[nodiscard]] const http_response_coding_selection_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http_response_coding_selection_failure* failure() const&& = delete;

private:
    friend class http_response_coding_selection;

    explicit http_response_coding_selection_result(http_response_coding_selection selection) noexcept
        : value_(selection) {}

    explicit http_response_coding_selection_result(http_response_coding_selection_failure failure) noexcept
        : value_(failure) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_selection_failure>;
    value_type value_;
};

// Picks the best response coding from the supplied representation candidates.
// The highest client q-value wins; ties resolve by server preference br > zstd
// > gzip > deflate > identity. A coding with q=0 or one the client never accepts is
// excluded. A failure result means the request has no acceptable response
// content coding and must be answered with 406 Not Acceptable by the Web layer.
inline http_response_coding_selection_result http_response_coding_selection::select(
    const http_response_coding_qualities& qualities) noexcept {
    return select(qualities, http_response_coding_candidates::all());
}

inline http_response_coding_selection_result http_response_coding_selection::select(
    const http_response_coding_qualities& qualities,
    http_response_coding_candidates candidates) noexcept {
    const int identity_score = qualities.score(http_content_coding::identity);
    const http_content_coding available_codings[] = {
        http_content_coding::brotli,
        http_content_coding::zstd,
        http_content_coding::gzip,
        http_content_coding::deflate,
        http_content_coding::identity,
    };
    auto acceptable_codings = http_response_coding_candidates::empty();
    for (const auto coding : available_codings) {
        if (qualities.accepts(coding)) {
            acceptable_codings.include(coding);
        }
    }
    http_content_coding best = http_content_coding::identity;
    bool found = false;
    int best_score = -1;
    for (const auto coding : available_codings) {
        if (!candidates.contains(coding)) {
            continue;
        }
        const int score = qualities.score(coding);
        if (score > best_score) {
            best_score = score;
            best = coding;
            found = true;
        }
    }
    if (!found) {
        return http_response_coding_selection_result(http_response_coding_selection_failure(
            http_response_coding_selection_error::no_acceptable_coding));
    }
    return http_response_coding_selection_result(http_response_coding_selection(
        best, identity_score >= 0, qualities.field_present_, acceptable_codings));
}

}  // namespace ruvia
