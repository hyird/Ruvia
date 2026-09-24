#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

enum class HttpAcceptTokenMatchMode : std::uint8_t {
    kExact,
    kLanguagePrefix,
};

// Accumulates one offered representation's match across multiple Accept field
// lines, equivalent to a comma-joined field without allocating or copying it.
// More specific matches override less specific ones, including q=0 exclusions.
class HttpAcceptMatch final {
public:
    void updateMediaType(std::string_view field, std::string_view offered) noexcept;
    void updateToken(std::string_view field, std::string_view offered,
        HttpAcceptTokenMatchMode mode) noexcept;

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
