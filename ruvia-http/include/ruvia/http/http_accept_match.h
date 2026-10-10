#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

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
};

}  // namespace ruvia
